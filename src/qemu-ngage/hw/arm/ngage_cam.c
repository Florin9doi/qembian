/*
 * Nokia 7650 board: the camera as a test-pattern source (QEMU-free core, ngage_cam.h).
 *
 * The phone's own driver stack runs unchanged: Camera.app -> CameraServer -> cameraldd.ldd -> camerapdd.pdd.
 * camerapdd [V, static RE of 0x503d5fc4-0x503d8af4; docs/devices/nokia-7650-nhl2/camera.md]:
 *   - talks to the sensor over a bit-banged I2C bus (GPIO 0x0C15020C in / 0x0C150210 pull-low, SCL bit 7,
 *     SDA bit 8), address 0x3C; at power-up it reads registers 0..0x7F and identifies the sensor from
 *     register 1: 0x01 / 0x11 / .. / 0xF1 = type 1, variant (reg >> 4) + 1 [V tables 0x503d8738 / 0x503d8740];
 *     we answer 0x01: type 1, variant 1 (variant 0 discards 12 viewfinder frames at each start, 1-3 one);
 *   - writes its mode settings back over I2C (stored, never interpreted: the pattern is the same in every mode);
 *   - programs the UPP's CCP receiver (0x0C121000: CCP2 sync codes FF000002/03/00/01 at +0x0C..+0x24) and
 *     moves each frame by DMA from the receiver's FIFO (+0x2C, the engine's address 0x1012102C) into the
 *     variant's camera buffer (physical 0x0A000000, 0x96000 bytes; count = bytes / 4 - 1) [V run];
 *   - its ISR (IrqCCPStatus, source 86) reads the status, stops the DMA, checks the DMA's remaining count
 *     (must read -1: a whole frame) and re-arms the DMA for the next buffer.
 * Two frame formats, by mode:
 *   - viewfinder (CCP control +0x3C bits 1-2 = 3, sensor reg 3 bit 6 = 1: mode table entry 6): 160x120, one 16-bit
 *     word 0x0RGB (4444) per pixel, which the app draws as it is [V run: YUV bytes showed white as cyan, i.e. 0x80FF
 *     read as 0x0FF; whether the sensor or the receiver converts is not known, the model keys on +0x3C];
 *   - still capture (+0x3C = 0, entry 5): YUV 4:2:2 as Csu.dll converts it (0x50a606cc / 0x50a6097c) [V], per
 *     pixel pair the bytes Y1 Cr Y0 Cb, R = Y + 2(Cr - 128), B = Y + 2(Cb - 128),
 *     G = Y - (7(Cb - 128) + 15(Cr - 128)) / 16.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "ngage_cam.h"

enum { CAM_I2C_IDLE, CAM_I2C_RX, CAM_I2C_ACK, CAM_I2C_TX, CAM_I2C_TXACK, CAM_I2C_IGNORE };

static void cam_log(NgageCam *c, const char *fmt, unsigned a, unsigned b)
{
    char buf[96];

    if (c->log) {
        snprintf(buf, sizeof(buf), fmt, a, b);
        c->log(c->opaque, buf);
    }
}

void ngage_cam_init(NgageCam *c)
{
    void (*log)(void *, const char *) = c->log;
    void *opaque = c->opaque;

    memset(c, 0, sizeof(*c));
    c->log = log;
    c->opaque = opaque;
    c->regs[1] = 0x01;              /* 7650: sensor type 1, variant 1 [V tables; the part itself: I] */
    c->ccp[CAM_CCP_MASK / 4] = 0x3ff;
    c->scl = c->sda = true;
    c->addr = CAM_I2C_ADDR;
    c->cur = -1;
}

void ngage_cam_set_sensor(NgageCam *c, uint8_t addr, bool reg16)
{
    c->addr = addr;
    c->reg16 = reg16;
}

uint8_t *ngage_cam_add_aux(NgageCam *c, uint8_t addr, uint8_t regw)
{
    NgageCamAux *a;

    if (c->naux >= CAM_AUX_MAX) {
        return NULL;
    }
    a = &c->aux[c->naux++];
    memset(a, 0, sizeof(*a));
    a->addr = addr;
    a->regw = regw;
    return a->mem;
}

static uint16_t cam_reg_mask(const NgageCam *c)
{
    return c->reg16 ? 0xffff : 0xff;
}

/* ---- 3230 camera module commands ------------------------------------------------------------- */

/* the answer to a command (resp[0] = cmd[0] already, the rest 0) [I: learnt command by command] */
void ngage_cam_cmd(NgageCam *c, const uint8_t *cmd, unsigned n, uint8_t resp[8])
{
    (void)c; (void)cmd; (void)n; (void)resp;
}

/* ---- I2C slave -------------------------------------------------------------------------------- */

/* a whole byte came in (8 bits, before the acknowledge clock); true = acknowledge it */
static bool cam_i2c_byte(NgageCam *c, uint8_t b)
{
    if (c->tx == 0xfe) {                        /* the address byte of this transfer */
        c->tx = 0;
        c->cur = -1;
        if (b >> 1 != c->addr) {
            for (unsigned i = 0; i < c->naux; i++) {
                if (c->aux[i].addr == b >> 1) {
                    c->cur = i;
                }
            }
            if (c->cur < 0) {
                return false;
            }
        }
        c->rd = b & 1;
        if (c->cur >= 0) {
            c->naddr = c->rd ? 0 : c->aux[c->cur].regw;
            if (c->naddr || !c->aux[c->cur].regw) {
                c->ptr = 0;                     /* a register-less part starts at 0 every transfer */
            }
            return true;
        }
        c->naddr = c->rd ? 0 : c->reg16 ? 2 : 1;
        if (c->naddr) {
            c->ptr = 0;
        }
        return true;
    }
    if (c->cur >= 0) {                          /* another device on the bus */
        NgageCamAux *a = &c->aux[c->cur];

        if (c->naddr) {
            c->ptr = b;
            c->naddr--;
        } else {
            a->mem[c->ptr & 0xff] = b;
            c->ptr = (c->ptr + 1) & 0xff;
        }
        return true;
    }
    if (c->naddr) {                             /* the register pointer, high byte first */
        c->ptr = (uint16_t)(c->ptr << 8 | b) & cam_reg_mask(c);
        c->naddr--;
        if (c->cmdport && c->ptr == CAM_CMD_PORT) {
            c->ncmd = 0;
            c->nresp = 0;                       /* an answer is read from its first byte */
        }
    } else if (c->cmdport && c->ptr == CAM_CMD_PORT) {
        if (c->ncmd < sizeof(c->cmd)) {
            c->cmd[c->ncmd++] = b;
        }
    } else {
        c->regs[c->ptr] = b;
        cam_log(c, c->reg16 ? "sensor reg %04x = %02x" : "sensor reg %02x = %02x", c->ptr, b);
        c->ptr = (c->ptr + 1) & cam_reg_mask(c);
    }
    return true;
}

/* 3230 module: a whole command arrived (STOP) */
static void cam_cmd_exec(NgageCam *c)
{
    char m[96];

    snprintf(m, sizeof(m), "module cmd %02x %02x %02x %02x %02x %02x %02x %02x (%u bytes)", c->cmd[0], c->cmd[1],
             c->cmd[2], c->cmd[3], c->cmd[4], c->cmd[5], c->cmd[6], c->cmd[7], c->ncmd);
    if (c->log) {
        c->log(c->opaque, m);
    }
    memset(c->resp, 0, sizeof(c->resp));        /* byte 0 bit 7 clear: done [V wait loop 0x5040b9c0] */
    c->resp[0] = c->cmd[0] & 0x7f;             /* bit 7 = busy [V 0x5040b9c0]; the rest of byte 0 [I] */
    ngage_cam_cmd(c, c->cmd, c->ncmd, c->resp);
    c->ncmd = 0;
}

static void cam_i2c_load(NgageCam *c)
{
    if (c->cur >= 0) {
        c->shift = c->aux[c->cur].mem[c->ptr & 0xff];
        c->ptr = (c->ptr + 1) & 0xff;
        c->nbits = 0;
        c->s_sda_low = !(c->shift & 0x80);
        return;
    }
    if (c->cmdport && c->ptr == CAM_CMD_PORT) {
        c->shift = c->resp[c->nresp & 7];
        c->nresp++;
        c->nbits = 0;
        c->s_sda_low = !(c->shift & 0x80);
        return;
    }
    c->shift = c->regs[c->ptr];
    c->ptr = (c->ptr + 1) & cam_reg_mask(c);
    c->nbits = 0;
    c->s_sda_low = !(c->shift & 0x80);
}

static void cam_i2c_update(NgageCam *c)
{
    bool scl = !c->m_scl_low;
    bool sda = !(c->m_sda_low || c->s_sda_low);

    if (scl && c->scl && sda != c->sda) {       /* SDA moved while SCL high: START or STOP */
        c->s_sda_low = false;
        if (!sda) {
            c->state = CAM_I2C_RX;
            c->tx = 0xfe;                       /* the next byte is an address */
            c->nbits = 0;
            c->shift = 0;
        } else {
            if (c->cmdport && c->ncmd && !c->rd) {
                cam_cmd_exec(c);
            }
            c->state = CAM_I2C_IDLE;
        }
        sda = !c->m_sda_low;
    } else if (scl && !c->scl) {                /* rising SCL: the receiver samples */
        if (c->state == CAM_I2C_RX && c->nbits < 8) {
            c->shift = c->shift << 1 | sda;
            c->nbits++;
        } else if (c->state == CAM_I2C_TXACK) {
            c->rd = !sda;                       /* master acknowledged: it wants another byte */
        }
    } else if (!scl && c->scl) {                /* falling SCL: the transmitter moves on */
        switch (c->state) {
        case CAM_I2C_RX:
            if (c->nbits == 8) {
                if (cam_i2c_byte(c, c->shift)) {
                    c->s_sda_low = true;        /* acknowledge during the 9th clock */
                    c->state = CAM_I2C_ACK;
                } else {
                    c->state = CAM_I2C_IGNORE;  /* not us: wait for the next START */
                }
            }
            break;
        case CAM_I2C_ACK:
            c->s_sda_low = false;
            if (c->rd) {
                cam_i2c_load(c);
                c->state = CAM_I2C_TX;
            } else {
                c->state = CAM_I2C_RX;
                c->nbits = 0;
                c->shift = 0;
            }
            break;
        case CAM_I2C_TX:
            if (++c->nbits == 8) {
                c->s_sda_low = false;
                c->state = CAM_I2C_TXACK;
            } else {
                c->s_sda_low = !(c->shift & (0x80 >> c->nbits));
            }
            break;
        case CAM_I2C_TXACK:
            if (c->rd) {
                cam_i2c_load(c);
                c->state = CAM_I2C_TX;
            } else {
                c->state = CAM_I2C_IGNORE;      /* not acknowledged: the last byte */
            }
            break;
        }
        sda = !(c->m_sda_low || c->s_sda_low);
    }
    c->scl = scl;
    c->sda = sda;
}

void ngage_cam_gpio_drive(NgageCam *c, uint16_t drive)
{
    bool scl_low = drive & CAM_GPIO_SCL, sda_low = drive & CAM_GPIO_SDA;

    if (scl_low == c->m_scl_low && sda_low == c->m_sda_low) {
        return;
    }
    /* one line at a time, as the driver moves them (it never changes both in one write) */
    if (sda_low != c->m_sda_low) {
        c->m_sda_low = sda_low;
        cam_i2c_update(c);
    }
    if (scl_low != c->m_scl_low) {
        c->m_scl_low = scl_low;
        cam_i2c_update(c);
    }
}

uint16_t ngage_cam_gpio_lines(const NgageCam *c)
{
    return (c->scl ? CAM_GPIO_SCL : 0) | (c->sda ? CAM_GPIO_SDA : 0);
}

/* ---- CCP receiver ----------------------------------------------------------------------------- */

uint32_t ngage_cam_ccp_read(NgageCam *c, unsigned off)
{
    off &= CAM_CCP_SIZE - 4;
    if (off == CAM_CCP_STATUS) {
        uint32_t v = c->status;
        c->status = 0;
        return v;
    }
    return c->ccp[off / 4];
}

void ngage_cam_ccp_write(NgageCam *c, unsigned off, uint32_t val)
{
    off &= CAM_CCP_SIZE - 4;
    if (off == CAM_CCP_STATUS) {
        c->status &= ~val;                      /* [I] write-1-to-clear; the driver never writes it */
        return;
    }
    if (off == CAM_CCP_ENABLE && (val & 1) != (c->ccp[off / 4] & 1)) {
        cam_log(c, (val & 1) ? "receiver on" : "receiver off", 0, 0);
    }
    c->ccp[off / 4] = val;
}

bool ngage_cam_rgb444(const NgageCam *c)
{
    return (c->ccp[CAM_CCP_CTRL / 4] & 6) == 6;
}

bool ngage_cam_streaming(const NgageCam *c)
{
    return c->ccp[CAM_CCP_ENABLE / 4] & 1;
}

bool ngage_cam_irq(const NgageCam *c)
{
    return (c->status & ~c->ccp[CAM_CCP_MASK / 4] & 0x7ff) != 0;
}

void ngage_cam_frame_start(NgageCam *c)
{
    if (!(c->ccp[CAM_CCP_MASK / 4] & CAM_ST_FRAME_START)) {
        c->status |= CAM_ST_FRAME_START;
    }
}

void ngage_cam_frame_end(NgageCam *c)
{
    c->status |= CAM_ST_FRAME_END;
    c->frames++;
}

/* ---- the test pattern --------------------------------------------------------------------------- */

/* the inverse of Csu.dll's converter: R - Y = 2(Cr - 128), B - Y = 2(Cb - 128), so Y = (15R + 32G + 7B) / 54 */
static void cam_rgb_to_yuv(const uint8_t rgb[3], uint8_t *y, uint8_t *cb, uint8_t *cr)
{
    int r = rgb[0], g = rgb[1], b = rgb[2];
    int yy = (15 * r + 32 * g + 7 * b + 27) / 54;
    int u = (b - yy) / 2 + 128, v = (r - yy) / 2 + 128;

    *y = yy;
    *cb = u < 0 ? 0 : u > 255 ? 255 : u;
    *cr = v < 0 ? 0 : v > 255 ? 255 : v;
}

static uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

void ngage_cam_yuv_to_rgb(const uint8_t q[4], uint8_t rgb0[3], uint8_t rgb1[3])
{
    int cr = q[1] - 128, cb = q[3] - 128;
    int g = (7 * cb + 15 * cr) >> 4;
    int y0 = q[2], y1 = q[0];

    rgb0[0] = clamp8(y0 + 2 * cr); rgb0[1] = clamp8(y0 - g); rgb0[2] = clamp8(y0 + 2 * cb);
    rgb1[0] = clamp8(y1 + 2 * cr); rgb1[1] = clamp8(y1 - g); rgb1[2] = clamp8(y1 + 2 * cb);
}

static const uint8_t cam_bars[8][3] = {
    { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
    { 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 },
};

/* the colour of pixel (x, y): colour bars over the top three quarters, a grey ramp below with a marker
   that moves every frame; a red square in the top-left corner shows the orientation */
static void cam_pixel(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t frame, uint8_t rgb[3])
{
    unsigned sq = h / 8 ? h / 8 : 1;

    if (x < sq && y < sq) {
        rgb[0] = 255; rgb[1] = 0; rgb[2] = 0;
        if (x < sq / 2 && y < sq / 2) {             /* a black inner corner: red bar vs this square */
            rgb[0] = 0;
        }
        return;
    }
    if (y < h * 3 / 4) {
        memcpy(rgb, cam_bars[x * 8 / w], 3);
        return;
    }
    unsigned mw = w / 10 ? w / 10 : 1;
    unsigned mx = (frame * (w / 40 ? w / 40 : 1)) % (w - mw + 1);
    uint8_t v = x * 255 / (w > 1 ? w - 1 : 1);
    if (x >= mx && x < mx + mw) {
        v = 255 - v;                               /* the marker: the ramp inverted */
    }
    rgb[0] = rgb[1] = rgb[2] = v;
}

/*
 * 3650/3660 raw frames (the small preview after a capture): Csu.dll 0x50abfee0 reads Cr Y1 Cb Y0 per pixel pair and
 * converts with its own matrix (16.16 / 3.13 fixed point) [V]:
 *   R = 1.1282 Y + 1.5691 Cr' - 0.0098 Cb'    G = 1.1571 Y - 0.8141 Cr' - 0.3925 Cb'    B = 1.1076 Y - 0.0330 Cr' + 1.9875 Cb'
 * (Cr' = Cr - 128, Cb' = Cb - 128). The pattern uses the exact inverse.
 */
static const double cam_m8[3][3] = {
    { 0x2421 / 8192.0, 0x191b0 / 65536.0, -0x27f / 65536.0 },
    { 0x1283a / 65536.0, -0xd06c / 65536.0, -0x6478 / 65536.0 },
    { 0x11b8f / 65536.0, -0x872 / 65536.0, 0x1fcdf / 65536.0 },
};

static void cam_rgb_to_yuv8(const uint8_t rgb[3], uint8_t *y, uint8_t *cb, uint8_t *cr)
{
    static double inv[3][3];
    static bool init;

    if (!init) {                                /* Cramer: inverse of cam_m8 */
        const double (*m)[3] = cam_m8;
        double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                   + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                int a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, d = (i + 2) % 3;
                inv[i][j] = (m[a][c] * m[b][d] - m[a][d] * m[b][c]) / det;
            }
        }
        init = true;
    }
    double v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = inv[i][0] * rgb[0] + inv[i][1] * rgb[1] + inv[i][2] * rgb[2];
    }
    *y = clamp8((int)floor(v[0] + 0.5));
    *cr = clamp8((int)floor(v[1] + 128.5));
    *cb = clamp8((int)floor(v[2] + 128.5));
}

void ngage_cam_yuv8_to_rgb(const uint8_t q[4], uint8_t rgb0[3], uint8_t rgb1[3])
{
    int cr = q[0] - 128, cb = q[2] - 128, y0 = q[3], y1 = q[1];
    int r = (cr * 0x191b0 + cb * -0x27f) >> 16, g = (cr * 0xd06c + cb * 0x6478) >> 16;
    int b = (cb * 0x1fcdf + cr * -0x872) >> 16;

    rgb0[0] = clamp8(r + (y0 * 0x2421 >> 13)); rgb0[1] = clamp8(((y0 * 0x1283a) >> 16) - g);
    rgb0[2] = clamp8(b + ((y0 * 0x11b8f) >> 16));
    rgb1[0] = clamp8(r + (y1 * 0x2421 >> 13)); rgb1[1] = clamp8(((y1 * 0x1283a) >> 16) - g);
    rgb1[2] = clamp8(b + ((y1 * 0x11b8f) >> 16));
}

enum { CAM_FMT_RGB444, CAM_FMT_RGB565, CAM_FMT_YUV, CAM_FMT_YUV8 };

/*
 * Pixel (x, y) of a w x h frame. rot90: the sensor lies on its side (6260): the phone turns every frame 90 degrees
 * anticlockwise for display (the frame's right edge at the top, its bottom row on the right) [V run], so the frame
 * carries the upright h x w picture turned 90 degrees clockwise.
 */
static void cam_pixel_at(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t frame, bool rot90, uint8_t rgb[3])
{
    if (rot90) {
        cam_pixel(y, w - 1 - x, h, w, frame, rgb);
    } else {
        cam_pixel(x, y, w, h, frame, rgb);
    }
}

/* w x h pixels of the pattern at dst (w * h * 2 bytes): little-endian 0x0RGB (4444) or RGB565 words, or YUV 4:2:2 in
   Csu.dll's byte order (YUV8: the 3650/3660's Cr Y1 Cb Y0; YUV: the 7650/6600's Y1 Cr Y0 Cb) */
static void cam_pattern_wh(uint8_t *dst, unsigned w, unsigned h, uint32_t frame, int fmt, bool rot90)
{
    bool yuv8 = fmt == CAM_FMT_YUV8;

    for (unsigned y = 0; y < h; y++) {
        uint8_t *row = dst + y * w * 2;
        if (fmt == CAM_FMT_RGB444 || fmt == CAM_FMT_RGB565) {
            for (unsigned x = 0; x < w; x++) {
                uint8_t a[3];
                cam_pixel_at(x, y, w, h, frame, rot90, a);
                uint16_t p = fmt == CAM_FMT_RGB444 ? (a[0] >> 4) << 8 | (a[1] >> 4) << 4 | a[2] >> 4
                                                   : (a[0] >> 3) << 11 | (a[1] >> 2) << 5 | a[2] >> 3;
                row[x * 2] = p;
                row[x * 2 + 1] = p >> 8;
            }
            continue;
        }
        for (unsigned x = 0; x + 1 < w; x += 2) {
            uint8_t a[3], b[3], y0, y1, cb0, cr0, cb1, cr1;
            cam_pixel_at(x, y, w, h, frame, rot90, a);
            cam_pixel_at(x + 1, y, w, h, frame, rot90, b);
            if (yuv8) {
                cam_rgb_to_yuv8(a, &y0, &cb0, &cr0);
                cam_rgb_to_yuv8(b, &y1, &cb1, &cr1);
                row[x * 2] = (cr0 + cr1) / 2;
                row[x * 2 + 1] = y1;
                row[x * 2 + 2] = (cb0 + cb1) / 2;
                row[x * 2 + 3] = y0;
                continue;
            }
            cam_rgb_to_yuv(a, &y0, &cb0, &cr0);
            cam_rgb_to_yuv(b, &y1, &cb1, &cr1);
            row[x * 2] = y1;
            row[x * 2 + 1] = (cr0 + cr1) / 2;
            row[x * 2 + 2] = y0;
            row[x * 2 + 3] = (cb0 + cb1) / 2;
        }
    }
}

void ngage_cam_pattern(uint8_t *dst, uint32_t bytes, uint32_t frame, bool rgb444)
{
    unsigned w, h;

    switch (bytes) {
    case 160 * 120 * 2: w = 160; h = 120; break;    /* viewfinder (mode table size 0x9600) [V] */
    case 176 * 144 * 2: w = 176; h = 144; break;    /* small capture (0xC600) [V] */
    case 640 * 480 * 2: w = 640; h = 480; break;    /* capture (0x96000) [V] */
    default: w = 160; h = bytes / (w * 2); break;
    }
    memset(dst, rgb444 ? 0 : 0x80, bytes);
    cam_pattern_wh(dst, w, h, frame, rgb444 ? CAM_FMT_RGB444 : CAM_FMT_YUV, false);
}

/* ---- JPEG (3650/3660 stills: the sensor compresses them) ------------------------------------------ */
/* A minimal baseline encoder: JFIF, YCbCr 4:2:2 (H 2, V 1), the Annex K quantisation tables at quality 75 and Huffman
   tables. 4:2:2 because the phone's decoder shows 4:2:0 with the wrong colours [V run, 3650]: the sensor's own format. */

static const uint8_t jz[64] = {             /* zigzag position -> natural index */
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63,
};
static const uint8_t jq_luma[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
static const uint8_t jq_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};
static const uint8_t jh_dc_l_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t jh_dc_c_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t jh_dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t jh_ac_l_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t jh_ac_l_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t jh_ac_c_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t jh_ac_c_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

typedef struct {
    uint16_t code[256];
    uint8_t len[256];
} JHuff;

typedef struct {
    uint8_t *out;
    uint32_t cap, n;
    uint32_t acc;
    int nacc;
    bool full;
    uint8_t q[2][64];               /* natural order */
    JHuff dc[2], ac[2];
} JEnc;

static void jput(JEnc *e, uint8_t b)
{
    if (e->n < e->cap) {
        e->out[e->n++] = b;
    } else {
        e->full = true;
    }
}

static void jbits(JEnc *e, uint32_t v, int n)
{
    e->acc = e->acc << n | (v & ((1u << n) - 1));
    e->nacc += n;
    while (e->nacc >= 8) {
        uint8_t b = e->acc >> (e->nacc - 8);
        jput(e, b);
        if (b == 0xff) {
            jput(e, 0);                         /* byte stuffing */
        }
        e->nacc -= 8;
    }
}

static void jhuff_build(JHuff *h, const uint8_t bits[16], const uint8_t *vals)
{
    unsigned code = 0, k = 0;

    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++) {
            h->code[vals[k]] = code++;
            h->len[vals[k]] = l;
            k++;
        }
        code <<= 1;
    }
}

static void jmarker_dht(JEnc *e, int cls, int id, const uint8_t bits[16], const uint8_t *vals)
{
    unsigned n = 0;

    for (int i = 0; i < 16; i++) {
        n += bits[i];
    }
    jput(e, 0xff); jput(e, 0xc4);
    jput(e, (3 + 16 + n) >> 8); jput(e, 3 + 16 + n);
    jput(e, cls << 4 | id);
    for (int i = 0; i < 16; i++) {
        jput(e, bits[i]);
    }
    for (unsigned i = 0; i < n; i++) {
        jput(e, vals[i]);
    }
}

static int jcat(int v)
{
    int n = 0;

    v = v < 0 ? -v : v;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

/* one 8x8 block (level-shifted samples, natural order): forward DCT, quantise, entropy-code */
static void jblock(JEnc *e, const float in[64], int t, int *pred)
{
    static float cosv[8][8];
    static bool init;
    float tmp[64], co[64];
    int z[64];

    if (!init) {
        for (int x = 0; x < 8; x++) {
            for (int u = 0; u < 8; u++) {
                cosv[x][u] = cosf((2 * x + 1) * u * 3.14159265f / 16);
            }
        }
        init = true;
    }
    for (int y = 0; y < 8; y++) {               /* rows */
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++) {
                s += in[y * 8 + x] * cosv[x][u];
            }
            tmp[y * 8 + u] = s * (u ? 0.5f : 0.35355339f);
        }
    }
    for (int u = 0; u < 8; u++) {               /* columns */
        for (int v = 0; v < 8; v++) {
            float s = 0;
            for (int y = 0; y < 8; y++) {
                s += tmp[y * 8 + u] * cosv[y][v];
            }
            co[v * 8 + u] = s * (v ? 0.5f : 0.35355339f);
        }
    }
    for (int k = 0; k < 64; k++) {
        float q = co[jz[k]] / e->q[t][jz[k]];
        z[k] = (int)(q < 0 ? q - 0.5f : q + 0.5f);
    }
    int d = z[0] - *pred, c = jcat(d);
    *pred = z[0];
    jbits(e, e->dc[t].code[c], e->dc[t].len[c]);
    if (c) {
        jbits(e, d < 0 ? d - 1 : d, c);
    }
    int run = 0;
    for (int k = 1; k < 64; k++) {
        if (!z[k]) {
            run++;
            continue;
        }
        while (run > 15) {
            jbits(e, e->ac[t].code[0xf0], e->ac[t].len[0xf0]);
            run -= 16;
        }
        c = jcat(z[k]);
        jbits(e, e->ac[t].code[run << 4 | c], e->ac[t].len[run << 4 | c]);
        jbits(e, z[k] < 0 ? z[k] - 1 : z[k], c);
        run = 0;
    }
    if (run) {
        jbits(e, e->ac[t].code[0], e->ac[t].len[0]);        /* EOB */
    }
}

uint32_t ngage_cam_jpeg(uint8_t *out, uint32_t cap, unsigned w, unsigned h, uint32_t frame)
{
    static JEnc e;
    static const uint8_t app0[] = { 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
    int pred[3] = { 0, 0, 0 };

    memset(&e, 0, sizeof(e));
    e.out = out;
    e.cap = cap;
    for (int i = 0; i < 64; i++) {              /* quality 75: base * 50 / 100 */
        int a = (jq_luma[i] * 50 + 50) / 100, b = (jq_chroma[i] * 50 + 50) / 100;
        e.q[0][i] = a < 1 ? 1 : a > 255 ? 255 : a;
        e.q[1][i] = b < 1 ? 1 : b > 255 ? 255 : b;
    }
    jhuff_build(&e.dc[0], jh_dc_l_bits, jh_dc_vals);
    jhuff_build(&e.dc[1], jh_dc_c_bits, jh_dc_vals);
    jhuff_build(&e.ac[0], jh_ac_l_bits, jh_ac_l_vals);
    jhuff_build(&e.ac[1], jh_ac_c_bits, jh_ac_c_vals);

    jput(&e, 0xff); jput(&e, 0xd8);
    for (unsigned i = 0; i < sizeof(app0); i++) {
        jput(&e, app0[i]);
    }
    for (int t = 0; t < 2; t++) {               /* DQT, zigzag order */
        jput(&e, 0xff); jput(&e, 0xdb); jput(&e, 0); jput(&e, 67); jput(&e, t);
        for (int k = 0; k < 64; k++) {
            jput(&e, e.q[t][jz[k]]);
        }
    }
    static const uint8_t sof[] = { 0xff, 0xc0, 0, 17, 8 };
    for (unsigned i = 0; i < sizeof(sof); i++) {
        jput(&e, sof[i]);
    }
    jput(&e, h >> 8); jput(&e, h); jput(&e, w >> 8); jput(&e, w); jput(&e, 3);
    jput(&e, 1); jput(&e, 0x21); jput(&e, 0);    /* Y 2x1, table 0 */
    jput(&e, 2); jput(&e, 0x11); jput(&e, 1);    /* Cb */
    jput(&e, 3); jput(&e, 0x11); jput(&e, 1);    /* Cr */
    jmarker_dht(&e, 0, 0, jh_dc_l_bits, jh_dc_vals);
    jmarker_dht(&e, 1, 0, jh_ac_l_bits, jh_ac_l_vals);
    jmarker_dht(&e, 0, 1, jh_dc_c_bits, jh_dc_vals);
    jmarker_dht(&e, 1, 1, jh_ac_c_bits, jh_ac_c_vals);
    static const uint8_t sos[] = { 0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
    for (unsigned i = 0; i < sizeof(sos); i++) {
        jput(&e, sos[i]);
    }
    for (unsigned my = 0; my < h; my += 8) {
        for (unsigned mx = 0; mx < w; mx += 16) {
            float Y[2][64], Cb[64], Cr[64];
            memset(Cb, 0, sizeof(Cb));
            memset(Cr, 0, sizeof(Cr));
            for (unsigned yy = 0; yy < 8; yy++) {
                for (unsigned xx = 0; xx < 16; xx++) {
                    unsigned x = mx + xx < w ? mx + xx : w - 1, y = my + yy < h ? my + yy : h - 1;
                    uint8_t p[3];
                    cam_pixel(x, y, w, h, frame, p);
                    float r = p[0], g = p[1], b = p[2];
                    Y[xx / 8][yy * 8 + xx % 8] = 0.299f * r + 0.587f * g + 0.114f * b - 128;
                    Cb[yy * 8 + xx / 2] += (-0.168736f * r - 0.331264f * g + 0.5f * b) / 2;
                    Cr[yy * 8 + xx / 2] += (0.5f * r - 0.418688f * g - 0.081312f * b) / 2;
                }
            }
            for (int i = 0; i < 2; i++) {
                jblock(&e, Y[i], 0, &pred[0]);
            }
            jblock(&e, Cb, 1, &pred[1]);
            jblock(&e, Cr, 1, &pred[2]);
        }
    }
    if (e.nacc) {
        jbits(&e, 0x7f, 8 - e.nacc);            /* pad with 1 bits */
    }
    jput(&e, 0xff); jput(&e, 0xd9);
    return e.full ? 0 : e.n;
}

/* ---- a frame for a DMA transfer -------------------------------------------------------------------- */

/* the decoder's escape window (camxpdd 0x503e6290): b = 2^n - 1 and a | b = 0xFF */
static bool camx_trigger(uint8_t a, uint8_t b)
{
    uint32_t r = a | b | (uint32_t)b << 8;
    return b && ((r + 1) & r) == 0;
}

uint32_t ngage_cam_camx_jpeg(uint8_t *dst, uint32_t cap, uint32_t frame, uint32_t *jpeg_len)
{
    return ngage_cam_camx_jpeg_wh(dst, cap, CAM_CAMX_W, CAM_CAMX_H, frame, true, jpeg_len);
}

uint32_t ngage_cam_camx_jpeg_wh(uint8_t *dst, uint32_t cap, unsigned w, unsigned h, uint32_t frame, bool swap,
                                uint32_t *jpeg_len)
{
    static uint8_t jpg[CAM_CAMX_STILL];
    uint32_t n = ngage_cam_jpeg(jpg, sizeof(jpg), w, h, frame), o = 0;
    uint8_t p1 = 0, p2 = 0;                     /* the decoder's last two bytes (r1 / r2 start at 0) */

    *jpeg_len = n;
    if (!n) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t ch = jpg[i];
        if (o + 2 > cap) {
            return 0;
        }
        dst[o++] = ch;
        if (ch == 0 && camx_trigger(p2, p1)) {
            dst[o++] = 0xa5;                    /* dropped by the driver; then it is the previous byte */
            p2 = 0;
            p1 = 0xa5;
        } else {
            p2 = p1;
            p1 = ch;
        }
    }
    while (o & 3) {
        if (o >= cap) {
            return 0;
        }
        dst[o++] = 0xa5;
    }
    for (uint32_t i = 0; swap && i < o; i += 4) {  /* each word byte-reversed (0x503e6258 swaps them back) */
        uint8_t t = dst[i]; dst[i] = dst[i + 3]; dst[i + 3] = t;
        t = dst[i + 1]; dst[i + 1] = dst[i + 2]; dst[i + 2] = t;
    }
    return o;
}

/* the frame header of the camx layout: 44 bytes (7610, 6670, 6630) unless the phone sets another (N90: 296) */
static unsigned cam_hdr(const NgageCam *c)
{
    return c->hdr ? c->hdr : CAM_HDR_CAMX;
}

uint32_t ngage_cam_fill(NgageCam *c, uint8_t *dst, uint32_t bytes)
{
    static const unsigned sizes[][2] = { { 160, 120 }, { 176, 144 }, { 640, 480 },
                                         /* 128 x 96: the 6260 viewfinder in swivel position 5 [V run] */
                                         { 128, 96 },
                                         /* the N90's other raw modes (campdd mode table 0x500d0a88) [V] */
                                         { 320, 240 }, { 352, 288 }, { 800, 600 } };
    unsigned hdr = cam_hdr(c);
    bool rgb444 = ngage_cam_rgb444(c);
    int fmt = rgb444 ? CAM_FMT_RGB444
            : c->reg16 && c->regs[CAM_REG_MODE] == 7 ? CAM_FMT_RGB565
            : c->reg16 ? CAM_FMT_YUV8 : CAM_FMT_YUV;

    if (!rgb444 && fmt != CAM_FMT_RGB565 && c->reg16 && c->regs[CAM_REG_JPEG] == 6) {
        static uint8_t jpg[640 * 482];
        uint32_t n = ngage_cam_jpeg(jpg, sizeof(jpg), 640, 480, c->frames);

        memset(dst, 0, bytes);
        if (!n || n * 2 > bytes) {
            cam_log(c, "JPEG frame: no room (%u bytes of %u)", n, bytes);
            return bytes;
        }
        for (uint32_t i = 0; i < n; i += 2) {       /* two bytes per word, at offsets 3 and 2 */
            dst[i * 2 + 3] = jpg[i];
            dst[i * 2 + 2] = i + 1 < n ? jpg[i + 1] : 0;
        }
        cam_log(c, "JPEG frame: %u bytes", n, 0);
        return bytes;
    }
    if (c->camx && bytes == (c->still_bytes ? c->still_bytes : CAM_CAMX_STILL)) {  /* the sensor's JPEG still */
        uint32_t len, o;

        memset(dst, 0, hdr);
        o = ngage_cam_camx_jpeg_wh(dst + hdr, bytes - hdr, c->still_w ? c->still_w : CAM_CAMX_W,
                                   c->still_h ? c->still_h : CAM_CAMX_H, c->frames, !c->still_raw, &len);
        c->regs[CAM_REG_CAMX_LEN] = len >> 16;
        c->regs[CAM_REG_CAMX_LEN + 1] = len >> 8;
        c->regs[CAM_REG_CAMX_LEN + 2] = len;
        c->regs[CAM_REG_CAMX_LEN + 4] = 0xc0;
        cam_log(c, "camx JPEG still: %u bytes, %u sent", len, o);
        return o ? hdr + o : bytes;
    }
    memset(dst, fmt <= CAM_FMT_RGB565 ? 0 : 0x80, bytes);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned w = sizes[i][0], h = sizes[i][1];
        if (bytes == w * h * 2) {
            cam_pattern_wh(dst, w, h, c->frames, fmt, c->rot90);
            return bytes;
        }
        if (bytes == w * (h + 2) * 2) {             /* 3650/3660: a header line, the picture, a spare line */
            cam_pattern_wh(dst + w * 2, w, h, c->frames, fmt, c->rot90);
            return bytes;
        }
        if (bytes == w * h * 2 + hdr) {             /* 7610/6670/6630: a 44-byte header (N90: 296), then the picture */
            memset(dst, 0, hdr);
            cam_pattern_wh(dst + hdr, w, h, c->frames, fmt, c->rot90);
            return bytes;
        }
    }
    cam_pattern_wh(dst, 160, bytes / 320, c->frames, fmt, c->rot90);
    return bytes;
}

/* ---- Nokia 6630 (campdd.pdd on the OMAP1610 board, nokia6630_cam.c) -------------------------------------------- */

void ngage_cam_rm1_setup(NgageCam *c)
{
    c->camx = true;                 /* the 7610's frame layout: a 44-byte header, then the picture or the JPEG */
    c->still_bytes = CAM_RM1_STILL;
    c->still_w = CAM_RM1_W;
    c->still_h = CAM_RM1_H;
    c->still_raw = true;
    /*
     * Power-on values of the gain registers: campdd reads registers 1..0xEA once into a shadow (0x500a1200) and fills
     * header bytes +8/+9 from shadow 0x5E/0x5F when it rewrites the header (0x500a09ac); CamServerCore takes their
     * 10-bit value into the EXIF data (0x501fbf98). Zero there panicked CamServerCore after a still (USER-EXEC 3, the
     * logarithm of 0) [V run]. Values: what the driver programs later (0x18, 0x02) [I].
     */
    c->regs[0x5e] = 0x18;
    c->regs[0x5f] = 0x02;
}

void ngage_cam_n90_setup(NgageCam *c)
{
    uint8_t *adc;

    ngage_cam_rm1_setup(c);
    c->hdr = CAM_HDR_N90;
    c->still_bytes = CAM_N90_STILL;
    c->still_w = CAM_N90_W;
    c->still_h = CAM_N90_H;
    ngage_cam_set_sensor(c, CAM_I2C_ADDR, true);    /* 0x78 / 0x79, register high byte first [V campdd 0x500c6240] */
    ngage_cam_add_aux(c, 0x3e, 1);                  /* 0x7C: one register byte [V 0x500c6290] */
    adc = ngage_cam_add_aux(c, 0x64, 0);            /* 0xC8: no register [V 0x500c629c] */
    /* campdd 0x500c4ec4 writes it 2 bytes, reads 4 and takes their rounded mean; 0 = failure [V]. What it measures: [I] */
    memset(adc, 0x80, 4);
    for (uint8_t page = 0; page < 4; page++) {      /* 0xA0..0xA6 by register >> 8 (table 0x500cd380) */
        ngage_cam_add_aux(c, 0x50 + page, 1);
    }
}

/*
 * A DMA transfer of dma bytes carries a frame of n bytes: the 6630 arms the whole 16-byte DMA frames below n (0x9620
 * for 0x962C) and reads the tail from the FIFO; the N90 arms up to 0x20 more than n (0x9740 for 0x9728) [V runs].
 */
static bool cam_rm1_dma_fits(uint32_t n, uint32_t dma)
{
    return (n & ~(CAM_RM1_DMA_FRAME - 1)) == dma || (dma > n && dma - n <= 2 * CAM_RM1_DMA_FRAME);
}

uint32_t ngage_cam_rm1_frame_len(const NgageCam *c, uint32_t dma)
{
    static const unsigned sizes[][2] = { { 160, 120 }, { 320, 240 }, { 640, 480 }, { 128, 96 }, { 176, 144 },
                                         { 352, 288 }, { 800, 600 } };
    uint32_t still = c->still_bytes ? c->still_bytes : CAM_RM1_STILL;

    if (cam_rm1_dma_fits(still, dma)) {
        return still;
    }
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint32_t n = cam_hdr(c) + sizes[i][0] * sizes[i][1] * 2;

        if (cam_rm1_dma_fits(n, dma)) {
            return n;
        }
    }
    return dma;
}

void ngage_cam_rm1_header(uint8_t *h)
{
    static const uint8_t words[3][4] = {
        { 0x80, 0x81, 0x81, 0x80 },     /* +0 flag (bit 0 clear), +1..+3 frame counter (constant) */
        { 0xb2, 0x81, 0x81, 0xb2 },     /* +4..+6 exposure (+6 & 7) << 14 | (+5 & 0x7F) << 7 | (+4 & 0x7F) */
        { 0x98, 0x81, 0x81, 0x98 },     /* +8/+9 gain (+9 & 7) << 7 | (+8 & 0x7F), +10 */
    };

    memset(h, 0x80, CAM_HDR_CAMX);
    memcpy(h, words, sizeof(words));
}

uint32_t ngage_cam_rm1_fill(NgageCam *c, uint8_t *dst, uint32_t dma)
{
    uint32_t n = ngage_cam_rm1_frame_len(c, dma);

    memset(dst, 0, n);
    n = ngage_cam_fill(c, dst, n);
    if (n > cam_hdr(c)) {
        memset(dst, 0x80, cam_hdr(c));
        ngage_cam_rm1_header(dst);
    }
    return n;
}

bool ngage_cam_rm1_drq(uint32_t left)
{
    return left >= 2 * CAM_RM1_DMA_FRAME;
}
