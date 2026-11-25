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
    c->regs[1] = 0x01;              /* sensor type 1, variant 1 [V tables; the part itself: I] */
    c->ccp[CAM_CCP_MASK / 4] = 0x3ff;
    c->scl = c->sda = true;
}

/* ---- I2C slave -------------------------------------------------------------------------------- */

/* a whole byte came in (8 bits, before the acknowledge clock); true = acknowledge it */
static bool cam_i2c_byte(NgageCam *c, uint8_t b)
{
    if (c->tx == 0xfe) {                        /* the address byte of this transfer */
        c->tx = 0;
        if (b >> 1 != CAM_I2C_ADDR) {
            return false;
        }
        c->rd = b & 1;
        c->first = !c->rd;
        return true;
    }
    if (c->first) {
        c->ptr = b & (CAM_NREGS - 1);
        c->first = false;
    } else {
        c->regs[c->ptr] = b;
        cam_log(c, "sensor reg %02x = %02x", c->ptr, b);
        c->ptr = (c->ptr + 1) & (CAM_NREGS - 1);
    }
    return true;
}

static void cam_i2c_load(NgageCam *c)
{
    c->shift = c->regs[c->ptr];
    c->ptr = (c->ptr + 1) & (CAM_NREGS - 1);
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
    for (unsigned y = 0; y < h; y++) {
        uint8_t *row = dst + y * w * 2;
        if (rgb444) {
            for (unsigned x = 0; x < w; x++) {
                uint8_t a[3];
                cam_pixel(x, y, w, h, frame, a);
                uint16_t p = (a[0] >> 4) << 8 | (a[1] >> 4) << 4 | a[2] >> 4;
                row[x * 2] = p;
                row[x * 2 + 1] = p >> 8;
            }
            continue;
        }
        for (unsigned x = 0; x + 1 < w; x += 2) {
            uint8_t a[3], b[3], y0, y1, cb0, cr0, cb1, cr1;
            cam_pixel(x, y, w, h, frame, a);
            cam_pixel(x + 1, y, w, h, frame, b);
            cam_rgb_to_yuv(a, &y0, &cb0, &cr0);
            cam_rgb_to_yuv(b, &y1, &cb1, &cr1);
            row[x * 2] = y1;
            row[x * 2 + 1] = (cr0 + cr1) / 2;
            row[x * 2 + 2] = y0;
            row[x * 2 + 3] = (cb0 + cb1) / 2;
        }
    }
}
