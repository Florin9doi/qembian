/*
 * Nokia 6630 (-M nokia6630) camera: the sensor behind campdd.pdd, as a test-pattern source (ngage_cam.c core:
 * the I2C sensor, the receiver registers, the frames). docs/devices/nokia-7650-nhl2/camera.md "Nokia 6630".
 *
 * - Sensor I2C, bit-banged and open drain: SCL = GPIO1 line 11, SDA = GPIO4 line 14. The PDD pulls a line low by
 *   making it an output (DIRECTION bit cleared, the latch cleared once) and releases it by making it an input; it reads
 *   DATAIN (campdd 0x5009fac8 / 0x5009fadc / 0x5009fb1c..0x5009fb9c) [V]. Address 0x78 / 0x79 [V 0x5009f5e4].
 * - CCP receiver at physical 0x2007D000 (the PDD's VA 0x58105000) [V run], interrupt "Lv2_CCPStatus" = kernel id
 *   0x52 = level-2 line 50 [V name table].
 * - Frames go through the system DMA: channel 2, source the FIFO (0x2007D070, constant), 32-bit elements, 4 per frame,
 *   frame-synchronised on request 3, armed from the frame-start interrupt's DFC [V run]. The ISR reads the last words
 *   (< 16 bytes) from the FIFO itself while the status shows them (bit 10) [V 0x500a0344].
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/range.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "system/reset.h"
#include "hw/arm/omap.h"
#include "system/address-spaces.h"
#include "nokia6630_soc.h"
#include "ngage_cam.h"

#define RM1_CAM_CCP_BASE 0x2007d000
#define RM1_CAM_CCP_SIZE 0x100
#define RM1_CAM_FIFO     (RM1_CAM_CCP_BASE + 0x70)
#define RM1_CAM_IRQ      50         /* level 2 */
#define RM1_CAM_DRQ      3          /* the channel's CCR sync [V run] */
#define N90_CAM_DRQ      5          /* the N90's: channel 4, CCR 0x40E5, 16 x 32-bit frame-synchronised [V run] */
#define RM1_CAM_SCL      11         /* GPIO1 line */
#define RM1_CAM_SDA      14         /* GPIO4 line */
#define RM1_CAM_ST_FIFO  0x400      /* status: words wait in the FIFO */
#define RM1_CAM_FRAME_NS (50 * 1000 * 1000)     /* 20 frames/s, as the DCT4 boards (ngage.c) */
#define RM1_CAM_START_NS (5 * 1000 * 1000)      /* frame start to frame end */
#define RM1_CAM_WAIT_NS  (1000 * 1000)          /* frame end waits for the DMA (QEMU runs it in steps) */
#define RM1_CAM_WAITS    40
#define RM1_DMA_BASE     0xfffed800
#define RM1_DMA_CHANS    12         /* the channels QEMU's omap_dma decodes (0x000-0x2FF) */

typedef struct Rm1Cam {
    MemoryRegion mr;
    qemu_irq irq, drq;
    NgageCam cam;
    int variant;                    /* RM1_CAM_6630 / RM1_CAM_N90 */
    bool scl_low, sda_low;          /* what the PDD drives */
    QEMUTimer *timer;
    bool in_frame;                  /* between the frame-start and frame-end events */
    unsigned waits;                 /* frame end postponed while the DMA still reads the frame */
    /* the frame in the FIFO: built at its first read, sized from the DMA channel reading the FIFO */
    uint8_t *buf;
    uint32_t len, pos;
    bool built;
} Rm1Cam;

static void rm1_cam_log(void *opaque, const char *msg)
{
    qemu_log_mask(LOG_UNIMP, "rm1-cam: %s\n", msg);
}

static void rm1_cam_bus(Rm1Cam *s)
{
    uint16_t lines;

    ngage_cam_gpio_drive(&s->cam, (s->scl_low ? CAM_GPIO_SCL : 0) | (s->sda_low ? CAM_GPIO_SDA : 0));
    lines = ngage_cam_gpio_lines(&s->cam);
    rm1_gpio_set_input(0, RM1_CAM_SCL, lines & CAM_GPIO_SCL);
    rm1_gpio_set_input(3, RM1_CAM_SDA, lines & CAM_GPIO_SDA);
}

static void rm1_cam_scl_dir(void *opaque, uint16_t dir)
{
    Rm1Cam *s = opaque;

    s->scl_low = !(dir & (1u << RM1_CAM_SCL));
    rm1_cam_bus(s);
}

static void rm1_cam_sda_dir(void *opaque, uint16_t dir)
{
    Rm1Cam *s = opaque;

    s->sda_low = !(dir & (1u << RM1_CAM_SDA));
    rm1_cam_bus(s);
}

/*
 * The receiver is the 7650's (ngage_cam.c) with its registers 0x10 apart [V campdd: the same set-up values, table
 * 0x500a5064]: +10/+20/+30/+40 the CCP2 sync codes (7650 +0C/+14/+1C/+24), +50 = 0x3FF (+28), +60 bit 0 receiver on
 * (+38; pulsed 0/1 to restart, 0x5009ff50), +70 the FIFO (+2C; read 4 times to drain it), +80 status (+30), +90 mask
 * (+34; 0xFFFFFFE7 while running: frame end and frame start, the 7610's design), +A0 control (+3C) [V run].
 */
static int rm1_ccp_off(hwaddr addr)
{
    static const int map[11] = { -1, 0x0c, 0x14, 0x1c, 0x24, 0x28, CAM_CCP_ENABLE, CAM_CCP_DATA, CAM_CCP_STATUS,
                                 CAM_CCP_MASK, CAM_CCP_CTRL };

    return (addr & 0xf) || addr / 0x10 >= 11 ? -1 : map[addr / 0x10];
}

/* the system DMA channel armed on the FIFO (enabled, source address): its transfer in bytes, 0 = none */
static uint32_t rm1_cam_dma_len(void)
{
    for (int ch = 0; ch < RM1_DMA_CHANS; ch++) {
        hwaddr b = RM1_DMA_BASE + 0x40 * ch;
        uint16_t csdp, ccr, sl, su, cen, cfn;

        address_space_read(&address_space_memory, b + 0x02, MEMTXATTRS_UNSPECIFIED, &ccr, 2);
        address_space_read(&address_space_memory, b + 0x08, MEMTXATTRS_UNSPECIFIED, &sl, 2);
        address_space_read(&address_space_memory, b + 0x0a, MEMTXATTRS_UNSPECIFIED, &su, 2);
        if (!(ccr & 0x80) || ((uint32_t)su << 16 | sl) != RM1_CAM_FIFO) {
            continue;
        }
        address_space_read(&address_space_memory, b + 0x00, MEMTXATTRS_UNSPECIFIED, &csdp, 2);
        address_space_read(&address_space_memory, b + 0x10, MEMTXATTRS_UNSPECIFIED, &cen, 2);
        address_space_read(&address_space_memory, b + 0x12, MEMTXATTRS_UNSPECIFIED, &cfn, 2);
        return (uint32_t)cen * cfn << (csdp & 3);
    }
    return 0;
}

static void rm1_cam_build(Rm1Cam *s)
{
    uint32_t dma = rm1_cam_dma_len();

    s->built = true;
    s->pos = s->len = 0;
    if (dma) {
        s->buf = g_realloc(s->buf, ngage_cam_rm1_frame_len(&s->cam, dma));
        s->len = ngage_cam_rm1_fill(&s->cam, s->buf, dma);
    }
}

/* the FIFO's bytes not yet read */
static uint32_t rm1_cam_fifo_left(const Rm1Cam *s)
{
    return s->built && s->pos < s->len ? s->len - s->pos : 0;
}

static void rm1_cam_update(Rm1Cam *s)
{
    qemu_set_irq(s->irq, ngage_cam_irq(&s->cam));
    if (!ngage_cam_streaming(&s->cam)) {
        timer_del(s->timer);
        s->in_frame = s->built = false;
    } else if (!timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (s->in_frame ? RM1_CAM_START_NS : RM1_CAM_FRAME_NS - RM1_CAM_START_NS));
    }
    /*
     * before the frame's first read the channel may still be arming (the frame-start DFC): high. The 6630 reads the
     * tail (< 16 bytes) itself; the N90's ISR reads at most 4 words but its frames end mid-way through a 64-byte DMA
     * frame (0x9728 = 604 x 64 + 40), so its DMA drains the FIFO and the count hook drops the padding.
     */
    qemu_set_irq(s->drq, s->in_frame && (!s->built || (s->variant == RM1_CAM_N90 ? rm1_cam_fifo_left(s) > 0
                                                       : ngage_cam_rm1_drq(rm1_cam_fifo_left(s)))));
}

static void rm1_cam_frame(void *opaque)
{
    Rm1Cam *s = opaque;

    if (!s->in_frame) {
        s->in_frame = true;
        s->built = false;
        s->waits = 0;
        ngage_cam_frame_start(&s->cam);
    } else if ((rm1_cam_fifo_left(s) >= (s->variant == RM1_CAM_N90 ? 4 : CAM_RM1_DMA_FRAME) ||
                (!s->built && rm1_cam_dma_len())) &&
               ++s->waits < RM1_CAM_WAITS) {
        /* the DMA still moves the frame: end it when the DMA has */
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RM1_CAM_WAIT_NS);
        return;
    } else {
        s->in_frame = false;
        ngage_cam_frame_end(&s->cam);
    }
    rm1_cam_update(s);
}

/* QEMU's omap_dma (patch 0029): the elements a channel reading the FIFO really got, the bytes served this frame */
extern uint32_t (*omap_dma_count_hook)(hwaddr src, uint32_t elements);
static Rm1Cam *rm1_cam_counted;

static uint32_t rm1_cam_count(hwaddr src, uint32_t elements)
{
    Rm1Cam *s = rm1_cam_counted;

    if (!s || src != RM1_CAM_FIFO || !s->built) {
        return elements;
    }
    return MIN(elements, s->pos / 4);
}

/*
 * DMA port 2 is the OMAP16xx's OCP-T1 (QEMU: "imif", IMIF RAM only); the N90's camera channel reads the FIFO through it
 * (CSDP 0x800A) [V run: "Bus time-out in DMA4"]. The port also reaches the receiver's window.
 */
static int (*rm1_ocp_t1_valid)(struct omap_mpu_state_s *s, hwaddr addr);

static int rm1_cam_ocp_t1_valid(struct omap_mpu_state_s *s, hwaddr addr)
{
    return range_covers_byte(RM1_CAM_CCP_BASE, RM1_CAM_CCP_SIZE, addr) || rm1_ocp_t1_valid(s, addr);
}

static uint64_t rm1_ccp_read(void *opaque, hwaddr addr, unsigned size)
{
    Rm1Cam *s = opaque;
    int off = rm1_ccp_off(addr);
    uint32_t v = 0;

    if (off == CAM_CCP_DATA) {
        if (!s->built && s->in_frame) {
            rm1_cam_build(s);
        }
        if (rm1_cam_fifo_left(s)) {
            v = ldl_le_p(s->buf + s->pos);
            s->pos += 4;
            rm1_cam_update(s);
        }
        return v;
    }
    v = off < 0 ? 0 : ngage_cam_ccp_read(&s->cam, off);
    if (off == CAM_CCP_STATUS) {
        if (!s->in_frame && rm1_cam_fifo_left(s)) {
            v |= RM1_CAM_ST_FIFO;
        }
        rm1_cam_update(s);
    } else if (off < 0) {
        qemu_log_mask(LOG_UNIMP, "rm1-cam: ccp read %02x\n", (unsigned)addr);
    }
    return v;
}

static void rm1_ccp_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rm1Cam *s = opaque;
    int off = rm1_ccp_off(addr);

    if (off < 0) {
        qemu_log_mask(LOG_UNIMP, "rm1-cam: ccp write %02x %08x\n", (unsigned)addr, (uint32_t)val);
        return;
    }
    ngage_cam_ccp_write(&s->cam, off, val);
    rm1_cam_update(s);
}

static const MemoryRegionOps rm1_ccp_ops = {
    .read = rm1_ccp_read,
    .write = rm1_ccp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void rm1_cam_reset(void *opaque)
{
    Rm1Cam *s = opaque;

    ngage_cam_init(&s->cam);
    if (s->variant == RM1_CAM_N90) {
        ngage_cam_n90_setup(&s->cam);
    } else if (s->variant == RM1_CAM_N70) {
        ngage_cam_n70_setup(&s->cam);
    } else {
        ngage_cam_rm1_setup(&s->cam);
    }
    timer_del(s->timer);
    s->in_frame = s->built = false;
    qemu_set_irq(s->irq, 0);
    qemu_set_irq(s->drq, 0);
    s->scl_low = s->sda_low = false;
    rm1_cam_bus(s);
}

void rm1_cam_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu, int variant)
{
    Rm1Cam *s = g_new0(Rm1Cam, 1);

    s->variant = variant;
    s->cam.log = rm1_cam_log;
    s->irq = qdev_get_gpio_in(mpu->ih[1], RM1_CAM_IRQ);
    s->drq = mpu->drq[variant == RM1_CAM_N90 ? N90_CAM_DRQ : RM1_CAM_DRQ];
    if (!rm1_ocp_t1_valid) {
        rm1_ocp_t1_valid = mpu->port[imif].addr_valid;
        mpu->port[imif].addr_valid = rm1_cam_ocp_t1_valid;
    }
    if (variant == RM1_CAM_N90) {
        rm1_cam_counted = s;
        omap_dma_count_hook = rm1_cam_count;
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rm1_cam_frame, s);
    memory_region_init_io(&s->mr, NULL, &rm1_ccp_ops, s, "omap1610.ccp", RM1_CAM_CCP_SIZE);
    memory_region_add_subregion_overlap(sys, RM1_CAM_CCP_BASE, &s->mr, 10);
    rm1_gpio_set_dir_hook(0, rm1_cam_scl_dir, s);
    rm1_gpio_set_dir_hook(3, rm1_cam_sda_dir, s);
    qemu_register_reset(rm1_cam_reset, s);
    rm1_cam_reset(s);
}
