/*
 * Sony Ericsson P910i board (-M p910i): display path and DMA (docs/research/p910i-display.md).
 *
 * The 208x320 panel has its own controller chip on the external bus: index port phys 0x18000000,
 * data port 0x18200000 (byte writes) [V]; register writes go through the "LCD FIFO" block at phys
 * 0x58000700 (videodriver_ext waits for +0x04 <= 0x78, polls +0x00 bit 4) [V]. Pixels go to the chip
 * by DMA (PL080 at phys 0x70000000 [I: register layout]) from the video RAM at phys 0xD0000000
 * (16 bpp, stride 416, two frames of 0x20800 bytes, the first one shown) [V].
 *
 * H-p910-lcd: the console shows the video RAM itself (RGB565 [I]) once the panel is on (chip register
 * 0 bit 6, written by the driver after the first full-frame transfer [V]); the chip's own memory and
 * window registers are not modelled, so DMA to the data port moves nothing.
 * H-p910-dma: a PL080 that finishes a channel as soon as it is enabled: memory-to-memory channels
 * copy (with the linked list), peripheral channels only complete; terminal count -> IrqDma (0).
 * DispSync (IRQ 47): a 60 Hz pulse.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "hw/core/irq.h"
#include "ui/console.h"
#include "p910i.h"

#define LCD_W        208
#define LCD_H        320
#define VRAM_BASE    0xD0000000
#define FIFO_BASE    0x58000700
#define CHIP_INDEX   0x18000000
#define CHIP_DATA    0x18200000
#define DMA_BASE     0x70000000
#define IRQ_DMA      0
#define IRQ_DISPSYNC 47

typedef struct {
    /* LCD FIFO block */
    MemoryRegion fifo_mr;
    uint32_t fifo[0x40];
    /* panel controller chip */
    MemoryRegion idx_mr, data_mr;
    uint8_t index;
    uint8_t reg[256];
    unsigned chip_logged;
    /* console */
    QemuConsole *con;
    uint16_t shown[LCD_H][LCD_W];
    bool dirty, was_on;
    /* DMA */
    MemoryRegion dma_mr;
    uint32_t ch[8][5];              /* src, dst, lli, control, config */
    uint32_t tc_raw, err_raw, dma_cfg;
    qemu_irq dma_irq;
    unsigned dma_logged;
    /* DispSync */
    QEMUTimer *sync;
    qemu_irq sync_irq;
    bool sync_level;
} P910Lcd;

/* ---- LCD FIFO block (phys 0x58000700) --------------------------------------------------------- */

static uint64_t fifo_read(void *opaque, hwaddr a, unsigned size)
{
    P910Lcd *l = opaque;
    unsigned i = a >> 2;
    uint32_t v;

    if (i == 0) {
        v = l->fifo[0] | 0x10;               /* ready [V: polled by the bootstrap and the driver] */
    } else if (i == 1) {
        v = 0;                               /* FIFO level: empty */
    } else if (a >= 0x20 && a < 0x60) {
        v = 0;                               /* dirty bitmap of 512-byte blocks: nothing [I] */
    } else {
        v = l->fifo[i];
    }
    return (v >> ((a & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
}

static void fifo_write(void *opaque, hwaddr a, uint64_t val, unsigned size)
{
    P910Lcd *l = opaque;
    unsigned i = a >> 2, sh = (a & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;

    l->fifo[i] = (l->fifo[i] & ~mask) | (((uint32_t)val << sh) & mask);
}

static const MemoryRegionOps fifo_ops = {
    .read = fifo_read,
    .write = fifo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- panel controller chip ---------------------------------------------------------------------- */

static uint64_t chip_read(void *opaque, hwaddr a, unsigned size)
{
    return 0;                                /* only the bootstrap reads it, and wants 0 [V] */
}

static void chip_idx_write(void *opaque, hwaddr a, uint64_t val, unsigned size)
{
    ((P910Lcd *)opaque)->index = val;
}

static void chip_data_write(void *opaque, hwaddr a, uint64_t val, unsigned size)
{
    P910Lcd *l = opaque;

    if (l->index != 0x0b && l->chip_logged < 64) {   /* 0x0B = pixel data */
        l->chip_logged++;
        qemu_log_mask(LOG_UNIMP, "p910i-lcd: chip reg %02x = %02x pc=%08x\n", l->index,
                      (unsigned)val & 0xff, p910_pc());
    }
    l->reg[l->index] = val;
    l->dirty = true;
}

static const MemoryRegionOps chip_idx_ops = {
    .read = chip_read,
    .write = chip_idx_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps chip_data_ops = {
    .read = chip_read,
    .write = chip_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- console ------------------------------------------------------------------------------------ */

static bool lcd_on(P910Lcd *l)
{
    return l->reg[0] & 0x40;
}

static bool lcd_gfx_update(void *opaque)
{
    P910Lcd *l = opaque;
    static uint16_t vram[LCD_H][LCD_W];
    DisplaySurface *surf = qemu_console_surface(l->con);
    bool on = lcd_on(l);
    uint8_t *dst;
    int stride;

    if (on) {
        address_space_read(&address_space_memory, VRAM_BASE, MEMTXATTRS_UNSPECIFIED, vram, sizeof(vram));
    } else {
        memset(vram, 0, sizeof(vram));
    }
    if (!l->dirty && on == l->was_on && !memcmp(vram, l->shown, sizeof(vram))) {
        return true;
    }
    memcpy(l->shown, vram, sizeof(vram));
    l->was_on = on;
    l->dirty = false;
    if (surface_width(surf) != LCD_W || surface_height(surf) != LCD_H) {
        qemu_console_resize(l->con, LCD_W, LCD_H);
        surf = qemu_console_surface(l->con);
    }
    dst = surface_data(surf);
    stride = surface_stride(surf);
    for (int y = 0; y < LCD_H; y++) {
        uint32_t *row = (uint32_t *)(dst + y * stride);
        for (int x = 0; x < LCD_W; x++) {
            uint16_t p = le16_to_cpu(l->shown[y][x]);
            uint32_t r = p >> 11, g = (p >> 5) & 0x3f, b = p & 0x1f;
            row[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
        }
    }
    qemu_console_update_full(l->con);
    return true;
}

static void lcd_invalidate(void *opaque)
{
    ((P910Lcd *)opaque)->dirty = true;
}

static const GraphicHwOps lcd_gfx_ops = {
    .gfx_update = lcd_gfx_update,
    .invalidate = lcd_invalidate,
};

/* ---- DMA (PL080-like) ------------------------------------------------------------------------ */

static void dma_update(P910Lcd *l)
{
    uint32_t itc = 0, ie = 0;

    for (int n = 0; n < 8; n++) {
        if (l->ch[n][4] & (1 << 15)) {
            itc |= 1 << n;
        }
        if (l->ch[n][4] & (1 << 14)) {
            ie |= 1 << n;
        }
    }
    qemu_set_irq(l->dma_irq, (l->tc_raw & itc) || (l->err_raw & ie));
}

/* one linked-list item of a memory-to-memory channel */
static void dma_copy(uint32_t src, uint32_t dst, uint32_t ctl)
{
    unsigned sw = 1u << ((ctl >> 18) & 7), dw = 1u << ((ctl >> 21) & 7);
    unsigned bytes = (ctl & 0xfff) * sw;
    bool si = ctl & (1u << 26), di = ctl & (1u << 27);
    uint8_t buf[4];

    if (si && di) {
        g_autofree uint8_t *b = g_malloc(bytes);
        address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED, b, bytes);
        address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED, b, bytes);
        return;
    }
    for (unsigned i = 0; i < bytes; i += sw) {        /* a fixed address on one side */
        address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED, buf, sw);
        address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED, buf, MIN(sw, dw));
        src += si ? sw : 0;
        dst += di ? sw : 0;
    }
}

static void dma_run(P910Lcd *l, int n)
{
    uint32_t *c = l->ch[n];
    unsigned flow = (c[4] >> 11) & 7, guard = 0;

    if (l->dma_logged < 64) {
        l->dma_logged++;
        qemu_log_mask(LOG_UNIMP, "p910i-dma: ch%d %08x -> %08x ctl %08x cfg %08x lli %08x pc=%08x\n",
                      n, c[0], c[1], c[3], c[4], c[2], p910_pc());
    }
    for (;;) {
        if (flow == 0) {
            dma_copy(c[0], c[1], c[3]);
        }
        if (!c[2] || ++guard > 4096) {
            break;
        }
        uint32_t lli[4];
        address_space_read(&address_space_memory, c[2] & ~3u, MEMTXATTRS_UNSPECIFIED, lli, sizeof(lli));
        c[0] = le32_to_cpu(lli[0]);
        c[1] = le32_to_cpu(lli[1]);
        c[2] = le32_to_cpu(lli[2]);
        c[3] = le32_to_cpu(lli[3]);
    }
    c[3] &= ~0xfffu;                 /* transfer size counts down to 0 */
    c[4] &= ~1u;                     /* channel disabled when done */
    l->tc_raw |= 1u << n;
    dma_update(l);
}

static uint64_t dma_read(void *opaque, hwaddr a, unsigned size)
{
    P910Lcd *l = opaque;
    uint32_t itc = 0, ie = 0, en = 0;

    for (int n = 0; n < 8; n++) {
        itc |= (l->ch[n][4] & (1 << 15)) ? 1 << n : 0;
        ie |= (l->ch[n][4] & (1 << 14)) ? 1 << n : 0;
        en |= (l->ch[n][4] & 1) << n;
    }
    if (a >= 0x100 && a < 0x200) {
        return l->ch[(a - 0x100) >> 5][((a & 0x1f) >> 2) % 5];
    }
    switch (a) {
    case 0x00: return (l->tc_raw & itc) | (l->err_raw & ie);
    case 0x04: return l->tc_raw & itc;
    case 0x0c: return l->err_raw & ie;
    case 0x14: return l->tc_raw;
    case 0x18: return l->err_raw;
    case 0x1c: return en;
    case 0x30: return l->dma_cfg;
    }
    return 0;
}

static void dma_write(void *opaque, hwaddr a, uint64_t val, unsigned size)
{
    P910Lcd *l = opaque;

    if (a >= 0x100 && a < 0x200 && (a & 0x1f) < 0x14) {
        int n = (a - 0x100) >> 5, r = (a & 0x1f) >> 2;

        l->ch[n][r] = val;
        if (r == 4) {
            if ((val & 1) && !(val & (1 << 18))) {
                dma_run(l, n);
            }
            dma_update(l);
        }
        return;
    }
    switch (a) {
    case 0x08: l->tc_raw &= ~val; break;
    case 0x10: l->err_raw &= ~val; break;
    case 0x30: l->dma_cfg = val; break;
    }
    dma_update(l);
}

static const MemoryRegionOps dma_ops = {
    .read = dma_read,
    .write = dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- DispSync ---------------------------------------------------------------------------------- */

static void sync_tick(void *opaque)
{
    P910Lcd *l = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    l->sync_level = !l->sync_level;
    qemu_set_irq(l->sync_irq, l->sync_level);
    /* high for 1 ms of every 1/60 s [I] */
    timer_mod(l->sync, now + (l->sync_level ? 1000000 : NANOSECONDS_PER_SECOND / 60 - 1000000));
}

static void lcd_reset(void *opaque)
{
    P910Lcd *l = opaque;

    memset(l->fifo, 0, sizeof(l->fifo));
    memset(l->reg, 0, sizeof(l->reg));
    memset(l->ch, 0, sizeof(l->ch));
    l->tc_raw = l->err_raw = l->dma_cfg = 0;
    l->dirty = true;
    l->sync_level = false;
    qemu_set_irq(l->sync_irq, 0);
    timer_mod(l->sync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);
    dma_update(l);
}

void p910_lcd_init(MemoryRegion *sys)
{
    P910Lcd *l = g_new0(P910Lcd, 1);

    l->dma_irq = p910_irq(IRQ_DMA);
    l->sync_irq = p910_irq(IRQ_DISPSYNC);
    l->sync = timer_new_ns(QEMU_CLOCK_VIRTUAL, sync_tick, l);
    memory_region_init_io(&l->fifo_mr, NULL, &fifo_ops, l, "p910i.lcd-fifo", 0x100);
    memory_region_add_subregion_overlap(sys, FIFO_BASE, &l->fifo_mr, 10);
    memory_region_init_io(&l->idx_mr, NULL, &chip_idx_ops, l, "p910i.lcd-index", 0x1000);
    memory_region_add_subregion_overlap(sys, CHIP_INDEX, &l->idx_mr, 10);
    memory_region_init_io(&l->data_mr, NULL, &chip_data_ops, l, "p910i.lcd-data", 0x1000);
    memory_region_add_subregion_overlap(sys, CHIP_DATA, &l->data_mr, 10);
    memory_region_init_io(&l->dma_mr, NULL, &dma_ops, l, "p910i.dma", 0x1000);
    memory_region_add_subregion_overlap(sys, DMA_BASE, &l->dma_mr, 10);
    l->con = qemu_graphic_console_create(NULL, 0, &lcd_gfx_ops, l);
    qemu_console_resize(l->con, LCD_W, LCD_H);
    qemu_register_reset(lcd_reset, l);
}
