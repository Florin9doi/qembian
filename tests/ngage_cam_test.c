/*
 * Host unit test of the Nokia 7650 camera model (src/qemu-ngage/hw/arm/ngage_cam.c): the sensor's I2C bus driven
 * with camerapdd.pdd's own pin sequences, the CCP receiver registers, and the test pattern read back through
 * Csu.dll's converter.
 *   cc -std=gnu11 -Wall -o build/ngage_cam_test tests/ngage_cam_test.c \
 *      src/qemu-ngage/hw/arm/ngage_cam.c && build/ngage_cam_test
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
    assert(write_reg(0x7f, 0xa5) == 0);
    assert(read_regs(0x03, r, 1) == 0 && r[0] == 0x18);
    assert(read_regs(0x7e, r, 3) == 0);
    assert(r[1] == 0xa5 && r[2] == cam.regs[0]);     /* wraps at 0x80 */
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

int main(void)
{
    test_identify();
    test_write_read();
    test_wrong_address();
    test_abort_mid_read();
    test_ccp();
    test_pattern();
    test_viewfinder_rgb444();
    printf("ngage_cam_test: ok\n");
    return 0;
}
