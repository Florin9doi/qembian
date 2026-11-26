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
    printf("ngage_cam_test: ok\n");
    return 0;
}
