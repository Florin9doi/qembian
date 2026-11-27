/*
 * Host unit test of the Nokia 7650 camera model (src/qemu-ngage/hw/arm/ngage_cam.c): the sensor's I2C bus driven
 * with camerapdd.pdd's own pin sequences, the CCP receiver registers, and the test pattern read back through
 * Csu.dll's converter.
 *   cc -std=gnu11 -Wall -o build/ngage_cam_test tests/ngage_cam_test.c \
 *      src/qemu-ngage/hw/arm/ngage_cam.c -lm && build/ngage_cam_test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/qemu-ngage/hw/arm/ngage_cam.h"

static NgageCam cam;
static uint16_t drive;          /* 0x0C150210 */

/* the PDD's pin helpers (0x503d6bc8 .. 0x503d6c04): modify16(0x150210, set, clear) */
static void pins(uint16_t set, uint16_t clr)
{
    drive = (drive & ~clr) | set;
    ngage_cam_gpio_drive(&cam, drive);
}
static void scl_low(void)  { pins(CAM_GPIO_SCL, 0); }
static void sda_low(void)  { pins(CAM_GPIO_SDA, 0); }
static void scl_rel(void)  { pins(0, CAM_GPIO_SCL); }
static void sda_rel(void)  { pins(0, CAM_GPIO_SDA); }
static int scl(void) { return !!(ngage_cam_gpio_lines(&cam) & CAM_GPIO_SCL); }
static int sda(void) { return !!(ngage_cam_gpio_lines(&cam) & CAM_GPIO_SDA); }

/* 0x503d67a8: START (SCL high and SDA high, then SDA low, then SCL low) */
static int start(void)
{
    if (!scl() || !sda()) {
        return -1;
    }
    sda_low();
    scl_low();
    return 0;
}

/* 0x503d6810: STOP */
static int stop(void)
{
    scl_rel();
    if (!scl()) {
        return -1;
    }
    sda_rel();
    return sda() ? 0 : -1;
}

/* 0x503d6874 + 0x503d692c: a byte, then the slave's acknowledge (SDA low in the 9th clock) */
static int put(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        if (b >> i & 1) {
            sda_rel();
        } else {
            sda_low();
        }
        scl_rel();
        if ((b >> i & 1) && !sda()) {
            return -1;                      /* arbitration: someone holds SDA */
        }
        scl_low();
    }
    sda_low();
    sda_rel();                              /* 0x503d692c */
    scl_rel();
    int ack = !sda();
    scl_low();
    sda_low();
    return ack ? 0 : -1;
}

/* 0x503d69ac + 0x503d6a4c (ack) / 0x503d6aa4 (nack) */
static int get(int last)
{
    int v = 0;
    sda_rel();
    for (int i = 7; i >= 0; i--) {
        scl_rel();
        v |= sda() << i;
        scl_low();
    }
    if (last) {
        sda_rel();
        scl_rel();
        if (!sda()) {
            return -1;                      /* the slave still drives: it did not see the NACK */
        }
        scl_low();
        sda_low();
    } else {
        sda_low();
        scl_rel();
        scl_low();
    }
    return v;
}

/* 0x503d666c: read n registers from reg: START 0x78 reg STOP START 0x79 data... STOP */
static int read_regs(uint8_t reg, uint8_t *out, int n)
{
    if (start() || put(0x78) || put(reg) || stop() || start() || put(0x79)) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        int v = get(i == n - 1);
        if (v < 0) {
            return -1;
        }
        out[i] = v;
    }
    return stop();
}

/* 0x503d762c: write one register */
static int write_reg(uint8_t reg, uint8_t v)
{
    return (start() || put(0x78) || put(reg) || put(v) || stop()) ? -1 : 0;
}

static void reset(void)
{
    memset(&cam, 0, sizeof(cam));
    ngage_cam_init(&cam);
    drive = 0;
    ngage_cam_gpio_drive(&cam, 0);
}

/* the sensor answers the identification read: register 1 = 0x01 picks type 1, variant 1 */
static void test_identify(void)
{
    uint8_t r[CAM_NREGS];
    reset();
    assert(scl() && sda());
    assert(read_regs(0, r, CAM_NREGS) == 0);
    assert(r[1] == 0x01);
    /* camerapdd's type-1 table entry 0x01014001: (reg1 << 8 | reg2) << 16 >> 24 == 1 */
    assert((((r[1] << 8 | r[2]) << 16) >> 24) == 1);
    assert(scl() && sda());                 /* bus idle afterwards */
}

/* a write is stored and read back, at any register, pointer auto-increments */
static void test_write_read(void)
{
    uint8_t r[3];
    reset();
    assert(write_reg(0x03, 0x18) == 0);
    assert(write_reg(0xff, 0xa5) == 0);
    assert(write_reg(0x9f, 0x5a) == 0);               /* the 6600's campdd reads 0..0x9F */
    assert(read_regs(0x03, r, 1) == 0 && r[0] == 0x18);
    assert(read_regs(0x9f, r, 1) == 0 && r[0] == 0x5a);
    assert(read_regs(0xfe, r, 3) == 0);
    assert(r[1] == 0xa5 && r[2] == cam.regs[0]);     /* wraps at 0x100 */
    /* multi-byte write */
    assert(start() == 0 && put(0x78) == 0 && put(0x10) == 0 && put(0x11) == 0 && put(0x22) == 0 && stop() == 0);
    assert(cam.regs[0x10] == 0x11 && cam.regs[0x11] == 0x22);
}

/* another address is not acknowledged and leaves the bus free (camerapdd then reports KErrNotReady -44) */
static void test_wrong_address(void)
{
    reset();
    assert(start() == 0);
    assert(put(0x42) == -1);
    assert(stop() == 0);
    assert(scl() && sda());
    uint8_t r;
    assert(read_regs(1, &r, 1) == 0 && r == 0x01);   /* still answers afterwards */
}

/* a read aborted mid-byte (the driver's error path 0x503d6c18 releases both pins) leaves the sensor holding SDA
   for its 0 bit, as a real slave does; clocking until SDA is high and a STOP recover the bus */
static void test_abort_mid_read(void)
{
    reset();
    cam.regs[0] = 0x00;
    assert(start() == 0 && put(0x78) == 0 && put(0x00) == 0 && stop() == 0);
    assert(start() == 0 && put(0x79) == 0);
    sda_rel();
    scl_rel();
    scl_low();                              /* one 0 bit clocked: the sensor presents the next 0 */
    pins(0, CAM_GPIO_SCL | CAM_GPIO_SDA);
    assert(scl() && !sda());
    for (int i = 0; i < 9 && !sda(); i++) {
        scl_low();
        scl_rel();
    }
    assert(sda());
    scl_low();
    sda_low();
    assert(stop() == 0);
    uint8_t r;
    assert(read_regs(1, &r, 1) == 0 && r == 0x01);
}

/* CCP: the status reads once (read-to-clear), the mask gates the interrupt */
static void test_ccp(void)
{
    reset();
    assert(!ngage_cam_streaming(&cam));
    ngage_cam_ccp_write(&cam, CAM_CCP_ENABLE, 1);
    assert(ngage_cam_streaming(&cam));
    ngage_cam_frame_end(&cam);
    assert(!ngage_cam_irq(&cam));           /* masked: 0x3FF after reset */
    ngage_cam_ccp_write(&cam, CAM_CCP_MASK, 0x3ff & ~CAM_ST_FRAME_END);
    assert(ngage_cam_irq(&cam));
    assert(ngage_cam_ccp_read(&cam, CAM_CCP_STATUS) == CAM_ST_FRAME_END);
    assert(!ngage_cam_irq(&cam));
    assert(ngage_cam_ccp_read(&cam, CAM_CCP_STATUS) == 0);
    /* the sync codes the mode table writes read back */
    ngage_cam_ccp_write(&cam, 0x0c, 0xff000002);
    assert(ngage_cam_ccp_read(&cam, 0x0c) == 0xff000002);
    ngage_cam_ccp_write(&cam, CAM_CCP_ENABLE, 0);
    assert(!ngage_cam_streaming(&cam));
}

static int near(const uint8_t a[3], int r, int g, int b)
{
    return abs(a[0] - r) <= 6 && abs(a[1] - g) <= 6 && abs(a[2] - b) <= 6;
}

/* the pattern comes out of Csu.dll's converter as colour bars, the red corner, and a moving marker */
static void test_pattern(void)
{
    static uint8_t f[640 * 480 * 2], g[160 * 120 * 2];
    static const int bars[8][3] = { { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
                                    { 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 } };
    unsigned sizes[3][2] = { { 160, 120 }, { 176, 144 }, { 640, 480 } };

    for (int s = 0; s < 3; s++) {
        unsigned w = sizes[s][0], h = sizes[s][1];
        ngage_cam_pattern(f, w * h * 2, 0, false);
        for (int b = 0; b < 8; b++) {
            unsigned x = (b * w / 8 + w / 16) & ~1u, y = h / 2;
            uint8_t p0[3], p1[3];
            ngage_cam_yuv_to_rgb(f + (y * w + x) * 2, p0, p1);
            if (!near(p0, bars[b][0], bars[b][1], bars[b][2]) || !near(p1, bars[b][0], bars[b][1], bars[b][2])) {
                fprintf(stderr, "%ux%u bar %d: %d,%d,%d\n", w, h, b, p0[0], p0[1], p0[2]);
                assert(0);
            }
        }
        uint8_t p0[3], p1[3];
        unsigned sq = h / 8;
        ngage_cam_yuv_to_rgb(f + ((sq * 3 / 4) * w + (sq * 3 / 4 & ~1u)) * 2, p0, p1);
        assert(near(p0, 255, 0, 0));        /* the corner square, top-left */
        ngage_cam_yuv_to_rgb(f + ((sq / 4) * w + (sq / 4 & ~1u)) * 2, p0, p1);
        assert(near(p0, 0, 0, 0));          /* its black inner corner */
    }
    /* the marker moves between frames: the bottom band differs, the bars do not */
    ngage_cam_pattern(f, 160 * 120 * 2, 0, false);
    ngage_cam_pattern(g, 160 * 120 * 2, 1, false);
    assert(memcmp(f, g, 160 * 90 * 2) == 0);
    assert(memcmp(f + 160 * 100 * 2, g + 160 * 100 * 2, 160 * 2) != 0);
    /* an unknown size fills what it is given (160 wide), nothing more */
    memset(f, 0xee, sizeof(f));
    ngage_cam_pattern(f, 160 * 10 * 2, 3, false);
    assert(f[160 * 10 * 2] == 0xee);
}

/*
 * Regression "viewfinder_colours_wrong": the viewfinder showed white as cyan, yellow as cyan, magenta as pink. Its
 * frames are 0x0RGB 4444 words drawn as they are, not YUV (YUV white 0x80FF read as 0x0FF = cyan). The format
 * follows the CCP control register: the viewfinder mode writes 6 to +0x3C, a still capture 0.
 */
static void test_viewfinder_rgb444(void)
{
    static uint8_t f[160 * 120 * 2];
    static const uint16_t bars[8] = { 0xfff, 0xff0, 0x0ff, 0x0f0, 0xf0f, 0xf00, 0x00f, 0x000 };

    reset();
    assert(!ngage_cam_rgb444(&cam));
    ngage_cam_ccp_write(&cam, CAM_CCP_CTRL, 6);         /* mode table entry 6: the viewfinder */
    assert(ngage_cam_rgb444(&cam));
    ngage_cam_ccp_write(&cam, CAM_CCP_CTRL, 7);         /* the restart pulse keeps the mode */
    assert(ngage_cam_rgb444(&cam));
    ngage_cam_ccp_write(&cam, CAM_CCP_CTRL, 0);         /* entry 5: a still capture */
    assert(!ngage_cam_rgb444(&cam));

    ngage_cam_pattern(f, sizeof(f), 0, true);
    for (int b = 0; b < 8; b++) {
        unsigned x = b * 160 / 8 + 10, y = 60;
        uint16_t p = f[(y * 160 + x) * 2] | f[(y * 160 + x) * 2 + 1] << 8;
        if (p != bars[b]) {
            fprintf(stderr, "bar %d: %03x\n", b, p);
            assert(0);
        }
    }
    uint16_t corner = f[(2 * 160 + 12) * 2] | f[(2 * 160 + 12) * 2 + 1] << 8;
    assert(corner == 0xf00);                            /* the red orientation square */
    uint16_t ramp0 = f[(110 * 160 + 0) * 2] | f[(110 * 160) * 2 + 1] << 8;
    assert(ramp0 == 0xfff || ramp0 == 0x000);           /* grey: the marker may sit on it */
    uint16_t ramp = f[(110 * 160 + 120) * 2] | f[(110 * 160 + 120) * 2 + 1] << 8;
    assert((ramp & 0xf) == (ramp >> 4 & 0xf) && (ramp & 0xf) == (ramp >> 8));
}

/* 3650/3660 camerapdd (0x5040c780 / 0x5040c6b0): address 0x08/0x09, register number high byte first */
static int write_reg16(uint16_t reg, uint8_t v)
{
    return (start() || put(0x08) || put(reg >> 8) || put(reg & 0xff) || put(v) || stop()) ? -1 : 0;
}

static int read_reg16(uint16_t reg, uint8_t *v)
{
    if (start() || put(0x08) || put(reg >> 8) || put(reg & 0xff) || stop() || start() || put(0x09)) {
        return -1;
    }
    int r = get(1);
    if (r < 0) {
        return -1;
    }
    *v = r;
    return stop();
}

/* the 3650/3660 sensor: 7-bit address 0x04, 16-bit register numbers; the 7650's address is not answered */
static void test_sensor_nhl8(void)
{
    uint8_t v;

    reset();
    ngage_cam_set_sensor(&cam, CAM_I2C_ADDR_NHL8, true);
    assert(write_reg16(CAM_REG_JPEG, 0x06) == 0);
    assert(cam.regs[0xc102] == 0x06 && cam.regs[0x02] == 0);
    assert(write_reg16(0x8165, 0xc0) == 0);              /* the firmware upload range (mode table entry 2) */
    assert(read_reg16(0x8165, &v) == 0 && v == 0xc0);
    assert(read_reg16(CAM_REG_JPEG, &v) == 0 && v == 0x06);
    /* the self-test (0x5040d... reads, inverts, writes, reads back) */
    assert(write_reg16(0x0010, 0x5a) == 0 && read_reg16(0x0010, &v) == 0 && v == 0x5a);
    assert(start() == 0 && put(0x78) == -1 && stop() == 0);     /* 0x3C: not this sensor */
    /* the 7650's 8-bit sensor still wraps at 0x100 */
    reset();
    assert(write_reg(0xff, 0x11) == 0 && cam.regs[0xff] == 0x11 && cam.regs[0x100] == 0);
}

/* 3650/3660 viewfinder: 160x122 transfers (0x9880): a header line, 160x120 RGB444, a spare line; the driver hands
   out buffer + 0x140 [V 0x5040db2c] */
static void test_fill_header_line(void)
{
    static uint8_t f[160 * 122 * 2], g[160 * 120 * 2];

    reset();
    ngage_cam_set_sensor(&cam, CAM_I2C_ADDR_NHL8, true);
    ngage_cam_ccp_write(&cam, CAM_CCP_CTRL, 6);
    ngage_cam_fill(&cam, f, sizeof(f));
    ngage_cam_pattern(g, sizeof(g), 0, true);
    assert(memcmp(f + 0x140, g, sizeof(g)) == 0);
    /* the 7650's 0x9600 transfer: no header line */
    ngage_cam_fill(&cam, f, sizeof(g));
    assert(memcmp(f, g, sizeof(g)) == 0);
}

/*
 * 3650/3660 stills: the sensor sends a JPEG (register 0xC102 = 6), two bytes per 32-bit word at byte offsets 3 and 2;
 * camerapdd 0x5040e87c checks FF D8 in the first word, compacts the bytes and finds FF D9 within the buffer [V].
 */
static void test_fill_jpeg(void)
{
    static uint8_t f[0x96a00], j[0x96a00 / 2];
    uint32_t n = 0;
    bool eoi = false;

    reset();
    ngage_cam_set_sensor(&cam, CAM_I2C_ADDR_NHL8, true);
    cam.regs[CAM_REG_JPEG] = 6;
    ngage_cam_fill(&cam, f, sizeof(f));
    uint32_t w0 = f[0] | f[1] << 8 | f[2] << 16 | (uint32_t)f[3] << 24;
    assert(w0 >> 24 == 0xff && (w0 >> 16 & 0xff) == 0xd8);          /* the driver's own test */
    for (uint32_t i = 0; i + 3 < sizeof(f) && !eoi; i += 4) {        /* the driver's compaction */
        j[n++] = f[i + 3];
        j[n++] = f[i + 2];
        eoi = n >= 2 && j[n - 2] == 0xff && j[n - 1] == 0xd9;
        if (!eoi && n >= 3 && j[n - 3] == 0xff && j[n - 2] == 0xd9) {
            eoi = true;
            n--;
        }
    }
    assert(eoi && n > 1000 && n < 100000);
    /* a JPEG: SOI, JFIF, SOF0 640x480 with 3 components; Huffman tables and quantisation tables present */
    assert(j[0] == 0xff && j[1] == 0xd8 && j[2] == 0xff && j[3] == 0xe0 && !memcmp(j + 6, "JFIF", 4));
    const uint8_t *sof = memmem(j, n, "\xff\xc0", 2);
    assert(sof && (sof[5] << 8 | sof[6]) == 480 && (sof[7] << 8 | sof[8]) == 640 && sof[9] == 3);
    assert(memmem(j, n, "\xff\xc4", 2) && memmem(j, n, "\xff\xdb", 2) && memmem(j, n, "\xff\xda", 2));
    /* the same file as the encoder makes; a full decode is checked with PIL in tests/ngage-cam-jpeg-check.py */
    static uint8_t ref[0x96a00 / 2];
    assert(ngage_cam_jpeg(ref, sizeof(ref), 640, 480, 0) == n && !memcmp(ref, j, n));
    /* raw stills (0xC102 = 4) are YUV again, with the header line */
    cam.regs[CAM_REG_JPEG] = 4;
    ngage_cam_fill(&cam, f, sizeof(f));
    assert(!(f[3] == 0xff && f[2] == 0xd8));
    assert(ngage_cam_jpeg(ref, 100, 640, 480, 0) == 0);              /* too small: 0, no overrun */
}

/*
 * Regression "preview_magenta_green" (3650): the still's preview showed white as magenta and black as green. The
 * 3650/3660 Csu.dll reads raw frames as Cr Y1 Cb Y0 with its own matrix (0x50abfee0), not the 7650's Y1 Cr Y0 Cb, so
 * the luma landed in the chroma. The 16-bit sensor's raw frames (CCP control 0, 0xC102 = 4) use its order.
 */
static void test_fill_yuv_nhl8(void)
{
    static uint8_t f[160 * 122 * 2];
    static const int bars[8][3] = { { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
                                    { 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 } };

    reset();
    ngage_cam_set_sensor(&cam, CAM_I2C_ADDR_NHL8, true);
    cam.regs[CAM_REG_JPEG] = 4;
    ngage_cam_fill(&cam, f, sizeof(f));
    for (int b = 0; b < 8; b++) {
        unsigned x = (b * 160 / 8 + 10) & ~1u, y = 60;
        uint8_t p0[3], p1[3];
        ngage_cam_yuv8_to_rgb(f + 0x140 + (y * 160 + x) * 2, p0, p1);
        if (!near(p0, bars[b][0], bars[b][1], bars[b][2]) || !near(p1, bars[b][0], bars[b][1], bars[b][2])) {
            fprintf(stderr, "3650 bar %d: %d,%d,%d\n", b, p0[0], p0[1], p0[2]);
            assert(0);
        }
    }
    /* the grey ramp stays grey */
    uint8_t p0[3], p1[3];
    ngage_cam_yuv8_to_rgb(f + 0x140 + (110 * 160 + 80) * 2, p0, p1);
    assert(abs(p0[0] - p0[1]) <= 6 && abs(p0[1] - p0[2]) <= 6);
}

/*
 * Regression "viewfinder_3660_colours": the 3660 (16-bit screen) runs its viewfinder in sensor mode 0xC005 = 7 with
 * CCP control 0; Csu.dll (0x50aa0c88) copies those frames into a 64K-colour bitmap as they are, so they are little-
 * endian RGB565 (the model sent YUV: odd colours). The 3650 (mode 2, control 6) stays RGB 4444.
 */
static void test_fill_rgb565_3660(void)
{
    static uint8_t f[160 * 122 * 2];
    static const uint16_t bars[8] = { 0xffff, 0xffe0, 0x07ff, 0x07e0, 0xf81f, 0xf800, 0x001f, 0x0000 };

    reset();
    ngage_cam_set_sensor(&cam, CAM_I2C_ADDR_NHL8, true);
    cam.regs[CAM_REG_MODE] = 7;
    ngage_cam_fill(&cam, f, sizeof(f));
    for (int b = 0; b < 8; b++) {
        unsigned x = b * 160 / 8 + 10, y = 60;
        const uint8_t *p = f + 0x140 + (y * 160 + x) * 2;
        assert((p[0] | p[1] << 8) == bars[b]);
    }
    cam.regs[CAM_REG_MODE] = 2;                        /* the 3650's viewfinder: control 6 = RGB 4444 */
    ngage_cam_ccp_write(&cam, CAM_CCP_CTRL, 6);
    ngage_cam_fill(&cam, f, sizeof(f));
    assert((f[0x140 + (60 * 160 + 10) * 2] | f[0x140 + (60 * 160 + 10) * 2 + 1] << 8) == 0x0fff);
}

/*
 * Regression "camx_no_frame_start" (7610/6670): the camera stayed blank and closed itself every 5 s. camxpdd arms its
 * buffer from a DFC its ISR queues on the frame-start event (status bit 4, unmasked by the mask 0xFFE7); the model
 * only raised frame end, so no buffer was ever armed and the driver's 5 s timeout reported an error. Frame start is
 * raised only when unmasked: the 7650's family masks it (0x3F7) and must not see it with the next frame end.
 */
static void test_frame_start(void)
{
    reset();
    ngage_cam_ccp_write(&cam, CAM_CCP_ENABLE, 1);
    ngage_cam_ccp_write(&cam, CAM_CCP_MASK, 0x3ff & ~CAM_ST_FRAME_END);     /* 7650 */
    ngage_cam_frame_start(&cam);
    assert(!ngage_cam_irq(&cam));
    ngage_cam_frame_end(&cam);
    assert(ngage_cam_ccp_read(&cam, CAM_CCP_STATUS) == CAM_ST_FRAME_END);
    ngage_cam_ccp_write(&cam, CAM_CCP_MASK, 0xffe7);                        /* 7610 */
    ngage_cam_frame_start(&cam);
    assert(ngage_cam_irq(&cam));
    assert(ngage_cam_ccp_read(&cam, CAM_CCP_STATUS) == CAM_ST_FRAME_START);  /* alone: both = an error */
    ngage_cam_frame_end(&cam);
    assert(ngage_cam_ccp_read(&cam, CAM_CCP_STATUS) == CAM_ST_FRAME_END);
}

/*
 * Regression "camx_header_shift" (7610/6670): the viewfinder showed the bars 22 pixels to the left (white wrapped to
 * the right edge). Each camxpdd frame is a 44-byte header and then the picture (38444 = 44 + 160x120x2, stills
 * 614444); the model wrote the picture from byte 0.
 */
static void test_fill_camx_header(void)
{
    static uint8_t f[CAM_HDR_CAMX + 640 * 480 * 2], g[640 * 480 * 2];
    unsigned sizes[2][2] = { { 160, 120 }, { 640, 480 } };

    for (int i = 0; i < 2; i++) {
        unsigned n = sizes[i][0] * sizes[i][1] * 2;
        reset();
        memset(f, 0xee, sizeof(f));
        ngage_cam_fill(&cam, f, CAM_HDR_CAMX + n);
        ngage_cam_pattern(g, n, 0, false);
        for (int k = 0; k < CAM_HDR_CAMX; k++) {
            assert(f[k] == 0);
        }
        assert(memcmp(f + CAM_HDR_CAMX, g, n) == 0);
    }
}

/*
 * Regression "rm25_viewfinder_sideways" (6260): the viewfinder and the stills showed the bars horizontal, white at the
 * bottom. The phone turns each frame 90 degrees anticlockwise for display; with rot90 the frame carries the upright
 * picture turned clockwise, so frame pixel (x, y) is upright pixel (y, w - 1 - x) of an h x w picture.
 */
static void test_fill_rot90(void)
{
    static uint8_t f[176 * 144 * 2];
    const unsigned w = 176, h = 144;
    uint8_t p0[3], p1[3];

    reset();
    cam.rot90 = true;
    ngage_cam_fill(&cam, f, sizeof(f));
    /* upright: 144 wide, 176 tall; bar b spans x 18b..18b+17. White (bar 0) is upright x < 18 = frame row y < 18;
       black (bar 7) frame rows y >= 126. Frame column x maps to upright row w - 1 - x: the bars are in the top three
       quarters (upright row < 132), i.e. frame x > 43 */
    ngage_cam_yuv_to_rgb(f + (8 * w + 100) * 2, p0, p1);
    assert(near(p0, 255, 255, 255));
    ngage_cam_yuv_to_rgb(f + (135 * w + 100) * 2, p0, p1);
    assert(near(p0, 0, 0, 0));
    /* the red corner square (upright top-left: x, y < 18) = frame y < 18, x near w - 1 */
    ngage_cam_yuv_to_rgb(f + (14 * w + (w - 16)) * 2, p0, p1);
    assert(near(p0, 255, 0, 0));
    /* the grey ramp (upright rows >= 132) = frame columns x < 44, along y */
    ngage_cam_yuv_to_rgb(f + (40 * w + 20) * 2, p0, p1);    /* upright x 40: past the frame-0 marker (x < 14) */
    uint8_t dark = p0[0];
    ngage_cam_yuv_to_rgb(f + ((h - 4) * w + 20) * 2, p0, p1);
    assert(p0[0] > dark + 100 && abs(p0[0] - p0[1]) <= 6);
    /* not rotated: the plain pattern */
    static uint8_t g[176 * 144 * 2];
    cam.rot90 = false;
    ngage_cam_fill(&cam, f, sizeof(f));
    ngage_cam_pattern(g, sizeof(g), 0, false);
    assert(memcmp(f, g, sizeof(g)) == 0);
}

/*
 * Regression "rm25_swivel5_viewfinder_garbled" (6260): in swivel position 5 Camera asks for 128x96 viewfinder frames
 * (24576 bytes); the fill knew only 160x120, 176x144 and 640x480 and wrote 160-pixel rows, so the picture came out as
 * slanted thin stripes. A 128x96 frame must be the pattern at that size (turned, as every 6260 frame).
 */
static void test_fill_128x96(void)
{
    static uint8_t f[128 * 96 * 2];
    const unsigned w = 128;
    uint8_t p0[3], p1[3];

    reset();
    cam.rot90 = true;
    assert(ngage_cam_fill(&cam, f, sizeof(f)) == sizeof(f));
    /* upright 96 x 128, bar b spans x 12b..12b+11: white = frame rows y < 12, black = frame rows y >= 84 */
    ngage_cam_yuv_to_rgb(f + (6 * w + 80) * 2, p0, p1);
    assert(near(p0, 255, 255, 255));
    ngage_cam_yuv_to_rgb(f + (90 * w + 80) * 2, p0, p1);
    assert(near(p0, 0, 0, 0));
    /* one bar along a whole frame row (an upright column): the same colour at both ends of the bars' span */
    ngage_cam_yuv_to_rgb(f + (40 * w + 40) * 2, p0, p1);
    uint8_t a[3] = { p0[0], p0[1], p0[2] };
    ngage_cam_yuv_to_rgb(f + (40 * w + 120) * 2, p0, p1);
    assert(near(p0, a[0], a[1], a[2]));
    cam.rot90 = false;                      /* plain: bars 16 pixels wide, white on the left, black on the right */
    ngage_cam_fill(&cam, f, sizeof(f));
    ngage_cam_yuv_to_rgb(f + (40 * w + 8) * 2, p0, p1);
    assert(near(p0, 255, 255, 255));
    ngage_cam_yuv_to_rgb(f + (40 * w + 120) * 2, p0, p1);
    assert(near(p0, 0, 0, 0));
}

/* camxpdd 0x503e6258 + 0x503e6290, ported instruction by instruction: the driver's view of a 7610 still. buf has
   size bytes of stream and room for the 3-byte terminator it appends; returns the JPEG length it finds */
static uint32_t camx_decode_order(uint8_t *buf, uint32_t size, bool swap)
{
    for (int32_t i = (int32_t)size - 4; swap && i >= 0; i -= 4) {     /* 6258: words big-endian -> little-endian */
        uint8_t t = buf[i]; buf[i] = buf[i + 3]; buf[i + 3] = t;
        t = buf[i + 1]; buf[i + 1] = buf[i + 2]; buf[i + 2] = t;
    }
    uint8_t *e = buf + size;
    if (*--e == 0xa5 && *--e == 0xa5 && *--e == 0xa5) {
        e--;
    }
    e++;
    e[0] = 0xff; e[1] = 0; e[2] = 0;
    uint8_t *rd = buf, *wr = buf, r0, r1 = 0, r2 = 0;
#define TRIG(a, b) ((b) && ((((a) | (b) | (b) << 8) + 1) & ((a) | (b) | (b) << 8)) == 0)
    goto l62ec;
l62e8:
    *wr++ = r1;
l62ec:
    r0 = *rd++;
    if (r0 == 0 && TRIG(r2, r1)) {
        r2 = *rd++;
        if (r2 == 0xa5) { *wr++ = r0; goto l637c; }
        if (r2 == 0) goto end;
        rd--;
    }
    *wr++ = r0;
l6334:
    r2 = *rd++;
    if (r2 == 0 && TRIG(r1, r0)) {
        r1 = *rd++;
        if (r1 == 0xa5) { *wr++ = r2; goto l62ec; }
        if (r1 == 0) goto end;
        rd--;
    }
    *wr++ = r2;
l637c:
    r1 = *rd++;
    if (r1 != 0 || !TRIG(r0, r2)) goto l62e8;
    r0 = *rd++;
    if (r0 == 0xa5) { *wr++ = r1; goto l6334; }
    if (r0 == 0) goto end;
    rd--;
    goto l62e8;
end:
#undef TRIG
    return wr - buf - 1;
}

/* camxpdd's view (words swapped first); the 6630's campdd runs the same decoder (0x500a0d6c) without the swap */
static uint32_t camx_decode(uint8_t *buf, uint32_t size)
{
    return camx_decode_order(buf, size, true);
}

/*
 * Regression "camx_still_rejected" (7610/6670): taking a picture blanked the viewfinder and nothing was saved. Stills
 * are a JPEG in the sensor's transport; the driver decodes it in place and compares the length with registers
 * 0xF6-0xF8 (and wants 0xFA bits 7/6). The stream must decode to exactly the JPEG.
 */
static void test_camx_still(void)
{
    static uint8_t f[CAM_CAMX_STILL + 4], jpg[CAM_CAMX_STILL];
    uint32_t n = ngage_cam_jpeg(jpg, sizeof(jpg), CAM_CAMX_W, CAM_CAMX_H, 0), len;

    reset();
    cam.camx = true;
    uint32_t sent = ngage_cam_fill(&cam, f, CAM_CAMX_STILL);
    assert(n && sent > CAM_HDR_CAMX && sent < CAM_CAMX_STILL && sent % 4 == 0);
    uint32_t got = cam.regs[CAM_REG_CAMX_LEN] << 16 | cam.regs[CAM_REG_CAMX_LEN + 1] << 8 | cam.regs[CAM_REG_CAMX_LEN + 2];
    assert(got == n);
    assert((cam.regs[CAM_REG_CAMX_LEN + 4] & 0xc0) == 0xc0);
    assert(camx_decode(f + CAM_HDR_CAMX, sent - CAM_HDR_CAMX) == n);
    assert(memcmp(f + CAM_HDR_CAMX, jpg, n) == 0);
    /* the escapes were needed: the stream is longer than the JPEG (entropy data has FF 00) */
    assert(ngage_cam_camx_jpeg(f, sizeof(f), 0, &len) > n && len == n);
    /* the viewfinder and the raw 640x480 mode (the camera server's start-up frames) stay whole pictures; regression
       "camx_raw_vga_as_jpeg": the 640x480 transfer was sent as a JPEG and the driver wanted all 0x9602C bytes */
    assert(ngage_cam_fill(&cam, f, CAM_HDR_CAMX + 160 * 120 * 2) == CAM_HDR_CAMX + 160 * 120 * 2);
    assert(ngage_cam_fill(&cam, f, CAM_HDR_CAMX + 640 * 480 * 2) == CAM_HDR_CAMX + 640 * 480 * 2);
}


/* ---- Nokia 6630 (campdd.pdd) ------------------------------------------------------------------------------------- */

/* the frame sizes from the DMA transfers seen in runs: whole 16-byte DMA frames, the ISR reads the rest */
static void test_rm1_frame_len(void)
{
    reset();
    ngage_cam_rm1_setup(&cam);
    assert(ngage_cam_rm1_frame_len(&cam, 0x962 * 16) == CAM_HDR_CAMX + 160 * 120 * 2);    /* viewfinder, 0x962C */
    assert(ngage_cam_rm1_frame_len(&cam, 0xc802 * 16) == CAM_RM1_STILL);                  /* the JPEG still */
    assert(ngage_cam_rm1_frame_len(&cam, CAM_HDR_CAMX + 640 * 480 * 2) == CAM_HDR_CAMX + 640 * 480 * 2);
    assert(ngage_cam_rm1_frame_len(&cam, 0x1000) == 0x1000);                              /* unknown: as it is */
}

/*
 * Regression "rm1_still_word_order": the 6630's still was taken again and again and never saved; campdd decodes the
 * JPEG in stream order (it byte-reverses only the 44-byte header), and the 7610's byte-reversed words decoded to a
 * wrong length (its check against registers 0xF6-0xF8 failed).
 */
static void test_rm1_still(void)
{
    static uint8_t f[CAM_RM1_STILL + 4], jpg[CAM_CAMX_STILL];
    uint32_t n = ngage_cam_jpeg(jpg, sizeof(jpg), CAM_RM1_W, CAM_RM1_H, 0);

    reset();
    ngage_cam_rm1_setup(&cam);
    uint32_t sent = ngage_cam_rm1_fill(&cam, f, CAM_RM1_STILL & ~15u);
    assert(n && sent > CAM_HDR_CAMX && sent < CAM_RM1_STILL && sent % 4 == 0);
    uint32_t got = cam.regs[CAM_REG_CAMX_LEN] << 16 | cam.regs[CAM_REG_CAMX_LEN + 1] << 8 | cam.regs[CAM_REG_CAMX_LEN + 2];
    assert(got == n && (cam.regs[CAM_REG_CAMX_LEN + 4] & 0xc0) == 0xc0);
    assert(memcmp(f + CAM_HDR_CAMX, jpg, 4) == 0);                  /* FF D8 in stream order */
    assert(camx_decode_order(f + CAM_HDR_CAMX, sent - CAM_HDR_CAMX, false) == n);
    assert(memcmp(f + CAM_HDR_CAMX, jpg, n) == 0);
    /* the 7610 keeps its byte-reversed words */
    reset();
    cam.camx = true;
    sent = ngage_cam_fill(&cam, f, CAM_CAMX_STILL);
    assert(f[CAM_HDR_CAMX] != 0xff && f[CAM_HDR_CAMX + 3] == 0xff);
}

/* CamServerCore's EXIF fields from the header (0x501fbce0): the exposure +4..+6 and the gain +8/+9 */
static uint32_t rm1_exposure(const uint8_t *h)
{
    return (h[6] & 7u) << 14 | (h[5] & 0x7fu) << 7 | (h[4] & 0x7fu);
}

static uint32_t rm1_gain(const uint8_t *h)
{
    return (h[9] & 7u) << 7 | (h[8] & 0x7fu);
}

/*
 * Regression "rm1_exif_zero_header": CamServerCore panicked (USER-EXEC 3, EUSER's TRealX: the logarithm of 0) right
 * after a still; it takes the exposure and the gain from the frame header (all zero), and campdd rewrites +8/+9 from
 * the sensor's power-on gain registers 0x5E/0x5F (zero too). campdd byte-reverses the header's words only sometimes
 * (when CCP +0x60 reads 1), so the fields must survive either order.
 */
static void test_rm1_header(void)
{
    uint8_t h[CAM_HDR_CAMX], r[CAM_HDR_CAMX];

    ngage_cam_rm1_header(h);
    for (unsigned i = 0; i < CAM_HDR_CAMX; i++) {
        assert(h[i] & 0x80);                                         /* embedded data: bit 7 set */
        r[i] = h[(i & ~3u) + 3 - (i & 3)];                           /* each word byte-reversed */
    }
    assert(memcmp(h, r, sizeof(h)) == 0);
    assert(rm1_exposure(h) && rm1_gain(h) && (h[10] & 0x7f));
    assert(!(h[0] & 1));
    /* the frames carry it (the viewfinder's and the still's) */
    static uint8_t f[CAM_RM1_STILL + 4];
    reset();
    ngage_cam_rm1_setup(&cam);
    assert(ngage_cam_rm1_fill(&cam, f, 0x962 * 16) == 0x962c && memcmp(f, h, sizeof(h)) == 0);
    assert(ngage_cam_rm1_fill(&cam, f, CAM_RM1_STILL & ~15u) > CAM_HDR_CAMX && memcmp(f, h, sizeof(h)) == 0);
    /* campdd's rewrite (0x500a09ac): +8 = shadow 0x5E | 0x80 (bit 6 clear), +9 = 0x80 | (0x5F & 3) << 1 | 0x5E >> 7 */
    uint8_t s5e = cam.regs[0x5e], s5f = cam.regs[0x5f];
    uint8_t b8 = (s5e & 0x40) ? s5e & 0x7f : s5e | 0x80, b9 = 0x80 | (s5f & 3) << 1 | s5e >> 7;
    assert(((b9 & 7u) << 7 | (b8 & 0x7fu)) != 0);
}

/*
 * QEMU's omap_dma as campdd uses it: frame-synchronised, 4 words a frame; at the set-up of each frame the channel stays
 * active for the next one while the request line is high. Returns the bytes the driver counts: DMA bytes (words past
 * the frame read as 0) plus the FIFO words the ISR reads itself (up to 4, while the status shows them).
 */
static uint32_t rm1_dma_run(uint32_t len, bool (*drq)(uint32_t))
{
    uint32_t pos = 0, moved = 0;
    bool more = true;

    while (more) {
        more = drq(pos < len ? len - pos : 0);
        for (int w = 0; w < 4; w++) {
            pos += 4;
            moved += 4;
        }
    }
    for (int w = 0; w < 4 && pos < len; w++) {
        pos += 4;
        moved += 4;
    }
    return moved;
}

static bool rm1_drq_one_frame(uint32_t left)
{
    return left >= CAM_RM1_DMA_FRAME;
}

/*
 * Regression "rm1_dma_one_frame_past": with the request held while one DMA frame was left, QEMU moved one frame past
 * the data and campdd counted 12-16 bytes too many: the still failed its length check, again and again.
 */
static void test_rm1_drq(void)
{
    for (uint32_t len = 2 * CAM_RM1_DMA_FRAME; len < 4096; len += 4) {
        assert(rm1_dma_run(len, ngage_cam_rm1_drq) == len);
    }
    assert(rm1_dma_run(0x962c, ngage_cam_rm1_drq) == 0x962c);
    assert(rm1_dma_run(32156, rm1_drq_one_frame) != 32156);          /* the old rule */
}

int main(void)
{
    test_identify();
    test_write_read();
    test_wrong_address();
    test_abort_mid_read();
    test_ccp();
    test_pattern();
    test_viewfinder_rgb444();
    test_sensor_nhl8();
    test_fill_header_line();
    test_fill_jpeg();
    test_fill_yuv_nhl8();
    test_fill_rgb565_3660();
    test_frame_start();
    test_fill_camx_header();
    test_fill_rot90();
    test_fill_128x96();
    test_camx_still();
    test_rm1_frame_len();
    test_rm1_still();
    test_rm1_header();
    test_rm1_drq();
    printf("ngage_cam_test: ok\n");
    return 0;
}
