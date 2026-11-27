/*
 * OMAP1610 blocks the Nokia 6630 (-M nokia6630) needs on top of QEMU's OMAP310/1510 model.
 * docs/devices/nokia-6630-rm1/bring-up.md has the evidence.
 *
 * GP timers (stall 3). The 1610 has eight 32-bit "GP timers" (the OMAP2 dmtimer register layout)
 * at 0xFFFB1400 + 0x800 * i. The 6630's kernel takes its tick from GP timer 2 (0xFFFB1C00) [V
 * ekern 0x5000e05c]: TIOCP_CFG = 0x34, TWER = 3, TLDR = 0, TTGR, TMAR = 0x200, TIER = 3 (match,
 * overflow), TCLR |= 0x43 (start, auto-reload, compare), polling TWPS for the posted writes; it
 * unmasks level-1 line 18 and, after each interrupt, reads TCRR [V 0x5000ea80]. Lines: GP timer 1
 * and 2 are level-1 17 and 18, 3..8 level-2 34..39 (the 1610 interrupt map; 18 [V MIR]).
 * The functional clock is taken as 32768 Hz [I: TMAR 0x200 = 1/64 s, the EKA1 tick].
 *
 * Registers: 00 TIDR, 10 TIOCP_CFG, 14 TISTAT, 18 TISR, 1C TIER, 20 TWER, 24 TCLR, 28 TCRR,
 * 2C TLDR, 30 TTGR, 34 TWPS, 38 TMAR, 3C TCAR, 40 TSICR. TCLR: 0 ST, 1 AR, 2-4 PTV, 5 PRE, 6 CE.
 * TISR/TIER: 0 match, 1 overflow, 2 capture. Posted writes complete at once (TWPS reads 0).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "system/reset.h"
#include "nokia6630_soc.h"
#include "nokia6630_gpt.h"
#include "nokia6630_nand.h"
#include "nokia6630_lcd.h"
#include "nokia6630_ssi.h"
#include "ngage_isi.h"
#include "nokia6630_keys.h"
#include "ui/input.h"
#include "ui/console.h"
#include "system/address-spaces.h"
#include "qemu/error-report.h"

#define GPT_HZ 32768

typedef struct {
    MemoryRegion mr;
    QEMUTimer *timer;
    qemu_irq irq;
    int index;
    GptCore c;              /* TCLR, TLDR, TMAR, TISR, TCRR */
    uint32_t tiocp, tier, twer, tcar, tsicr;
    int64_t last_ns;        /* virtual time of c.cnt */
} Gpt;

static uint32_t gpt_hz(Gpt *g)
{
    return (g->c.tclr & 0x20) ? GPT_HZ >> (((g->c.tclr >> 2) & 7) + 1) : GPT_HZ;
}

/* Bring the counter up to now, raising match/overflow status on the way. */
static void gpt_advance(Gpt *g)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t hz = gpt_hz(g);
    uint64_t ticks;

    if (!(g->c.tclr & 1)) {
        g->last_ns = now;
        return;
    }
    if (now <= g->last_ns) {
        return;
    }
    ticks = muldiv64(now - g->last_ns, hz, NANOSECONDS_PER_SECOND);
    g->last_ns += muldiv64(ticks, NANOSECONDS_PER_SECOND, hz);
    gpt_core_step(&g->c, ticks);
}

static void gpt_update(Gpt *g)
{
    uint64_t next = gpt_core_next(&g->c);

    qemu_set_irq(g->irq, (g->c.tisr & g->tier & 7) != 0);
    if (next) {
        timer_mod(g->timer, g->last_ns + muldiv64(next, NANOSECONDS_PER_SECOND, gpt_hz(g)) + 1);
    } else {
        timer_del(g->timer);
    }
}

static void gpt_tick(void *opaque)
{
    Gpt *g = opaque;

    gpt_advance(g);
    gpt_update(g);
}

static void gpt_reset(Gpt *g)
{
    g->tiocp = g->tier = g->twer = g->tcar = g->tsicr = 0;
    memset(&g->c, 0, sizeof(g->c));
    g->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(g->timer);
    qemu_set_irq(g->irq, 0);
}

static uint64_t gpt_read(void *opaque, hwaddr addr, unsigned size)
{
    Gpt *g = opaque;
    uint32_t v;

    switch (addr & ~3) {
    case 0x00: v = 0x21; break;            /* TIDR: a revision, not checked [I] */
    case 0x10: v = g->tiocp; break;
    case 0x14: v = 1; break;               /* TISTAT: reset done */
    case 0x18: gpt_advance(g); v = g->c.tisr; gpt_update(g); break;
    case 0x1c: v = g->tier; break;
    case 0x20: v = g->twer; break;
    case 0x24: gpt_advance(g); v = g->c.tclr; gpt_update(g); break;
    case 0x28: gpt_advance(g); v = g->c.cnt; gpt_update(g); break;
    case 0x2c: v = g->c.tldr; break;
    case 0x30: v = 0xffffffff; break;      /* TTGR reads all ones */
    case 0x34: v = 0; break;               /* TWPS: no write pending */
    case 0x38: v = g->c.tmar; break;
    case 0x3c: v = g->tcar; break;
    case 0x40: v = g->tsicr; break;
    default:
        qemu_log_mask(LOG_UNIMP, "nokia6630: GP timer %d read %02x\n", g->index + 1, (unsigned)addr);
        return 0;
    }
    return (v >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
}

static void gpt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Gpt *g = opaque;
    uint32_t v = val;

    if (size != 4) {
        /* halfword writes into the 32-bit registers: merge with the current value */
        unsigned sh = (addr & 3) * 8;
        uint32_t mask = ((1u << (size * 8)) - 1) << sh;
        uint32_t cur = gpt_read(opaque, addr & ~3, 4);

        if ((addr & ~3) == 0x18) {
            cur = 0;        /* TISR is write-1-to-clear: keep the other bits untouched */
        }
        v = (cur & ~mask) | (((uint32_t)val << sh) & mask);
        addr &= ~3;
    }
    gpt_advance(g);
    switch (addr) {
    case 0x10:
        g->tiocp = v & ~2;
        if (v & 2) {        /* soft reset */
            gpt_reset(g);
            return;
        }
        break;
    case 0x18: g->c.tisr &= ~v; break;
    case 0x1c: g->tier = v & 7; break;
    case 0x20: g->twer = v & 7; break;
    case 0x24:
        if ((v & 1) && !(g->c.tclr & 1)) {
            g->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        g->c.tclr = v & 0x7fff;
        break;
    case 0x28: g->c.cnt = v; break;
    case 0x2c: g->c.tldr = v; break;
    case 0x30: g->c.cnt = g->c.tldr; break;    /* TTGR: reload now */
    case 0x38: g->c.tmar = v; break;
    case 0x40: g->tsicr = v; break;
    default:
        qemu_log_mask(LOG_UNIMP, "nokia6630: GP timer %d write %02x %08x\n", g->index + 1,
                      (unsigned)addr, v);
        return;
    }
    gpt_update(g);
}

static const MemoryRegionOps gpt_ops = {
    .read = gpt_read,
    .write = gpt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
};

static void gpt_reset_cb(void *opaque)
{
    gpt_reset(opaque);
}

void rm1_gptimers_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu)
{
    for (int i = 0; i < 8; i++) {
        Gpt *g = g_new0(Gpt, 1);
        g_autofree char *name = g_strdup_printf("omap1610.gptimer%d", i + 1);

        g->index = i;
        g->irq = i < 2 ? qdev_get_gpio_in(mpu->ih[0], 17 + i) : qdev_get_gpio_in(mpu->ih[1], 32 + i);
        g->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gpt_tick, g);
        memory_region_init_io(&g->mr, NULL, &gpt_ops, g, name, 0x80);
        /* above QEMU's 1510 blocks there (McBSP1 at 0xFFFB1000, uWire at 0xFFFB3000) */
        memory_region_add_subregion_overlap(sys, 0xfffb1400 + 0x800 * i, &g->mr, 10);
        qemu_register_reset(gpt_reset_cb, g);
    }
}

/*
 * Display (H-rm1-panel): SoSSI (0xFFFBAC00) + the 176x208 DCS panel + the LCD DMA channel (0xFFFEE3C0,
 * past QEMU's 1510 DMA block), nokia6630_lcd.h. The panel's frame memory is console 0 (patch 0023 leaves
 * the 1610 path without QEMU's LCD controller). The DMA runs whole when CCR bit 7 is set and completes
 * 1 ms later: CCR bit 7 cleared, CTRL bit 3 set, level-1 line 25 raised while CTRL bits 1 and 3 are set
 * [I: the line, Linux INT_DMA_LCD; lcd.ldd binds "Lv1_DMAChLCD"].
 */
typedef struct {
    Rm1Lcd l;
    MemoryRegion sossi_mr, dma_mr;
    QemuConsole *con;
    QEMUTimer *done;
    qemu_irq irq;
    bool busy;
} Rm1Display;

static void lcd_mem_read(Rm1Lcd *l, uint64_t addr, void *buf, unsigned len)
{
    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
}

static void lcd_dma_irq(Rm1Display *s)
{
    qemu_set_irq(s->irq, (RM1_D(&s->l, 0xbc4) & 0x0a) == 0x0a);
}

static uint64_t sossi_read(void *opaque, hwaddr addr, unsigned size)
{
    Rm1Display *s = opaque;
    uint32_t v = rm1_sossi_read(&s->l, addr & ~3);

    return (v >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
}

static void sossi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rm1Display *s = opaque;

    rm1_sossi_write(&s->l, addr & ~3, val);
}

static const MemoryRegionOps sossi_ops = {
    .read = sossi_read,
    .write = sossi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t lcddma_read(void *opaque, hwaddr addr, unsigned size)
{
    Rm1Display *s = opaque;
    uint32_t v = s->l.d[(addr >> 1) & 31] | (uint32_t)s->l.d[((addr >> 1) + 1) & 31] << 16;

    return size == 4 ? v : v & 0xffff;
}

static void lcddma_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rm1Display *s = opaque;

    s->l.d[(addr >> 1) & 31] = val;
    if (size == 4) {
        s->l.d[((addr >> 1) + 1) & 31] = val >> 16;
    }
    if ((addr & ~1) == 0x02 && (val & 0x80) && !s->busy) {
        s->busy = true;
        rm1_lcd_dma_run(&s->l);
        RM1_D(&s->l, 0xbc2) |= 0x80;        /* busy until the completion timer */
        RM1_D(&s->l, 0xbc4) &= ~0x08;
        timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
    lcd_dma_irq(s);
}

static void lcddma_done(void *opaque)
{
    Rm1Display *s = opaque;

    s->busy = false;
    RM1_D(&s->l, 0xbc2) &= ~0x80;
    RM1_D(&s->l, 0xbc4) |= 0x08;
    lcd_dma_irq(s);
}

static const MemoryRegionOps lcddma_ops = {
    .read = lcddma_read,
    .write = lcddma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 4 },
};

static bool panel_update(void *opaque)
{
    Rm1Display *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    unsigned y0, y1;

    if (!surf || !rm1_lcd_take_dirty(&s->l, &y0, &y1)) {
        return true;
    }
    unsigned w = rm1_lcd_w(&s->l);

    memcpy((uint32_t *)surface_data(surf) + y0 * w, s->l.fb + y0 * w, (y1 - y0) * w * 4);
    qemu_console_update(s->con, 0, y0, w, y1 - y0);
    return true;
}

static void panel_invalidate(void *opaque)
{
    Rm1Display *s = opaque;

    s->l.dirty_y0 = 0;
    s->l.dirty_y1 = rm1_lcd_h(&s->l);
}

static const GraphicHwOps panel_ops = {
    .invalidate = panel_invalidate,
    .gfx_update = panel_update,
};

static void display_reset(void *opaque)
{
    Rm1Display *s = opaque;

    rm1_lcd_reset(&s->l);
    s->busy = false;
    timer_del(s->done);
    qemu_set_irq(s->irq, 0);
    panel_invalidate(s);
}

void rm1_display_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu, unsigned w, unsigned h, bool ctrl)
{
    Rm1Display *s = g_new0(Rm1Display, 1);

    assert(w <= RM1_LCD_MAX_W && h <= RM1_LCD_MAX_H);
    s->l.w = w;
    s->l.h = h;
    s->l.ctrl = ctrl;

    s->l.mem_read = lcd_mem_read;
    rm1_lcd_reset(&s->l);
    s->irq = qdev_get_gpio_in(mpu->ih[0], 25);
    s->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcddma_done, s);
    s->con = qemu_graphic_console_create(NULL, 0, &panel_ops, s);
    qemu_console_resize(s->con, rm1_lcd_w(&s->l), rm1_lcd_h(&s->l));
    memory_region_init_io(&s->sossi_mr, NULL, &sossi_ops, s, "omap1610.sossi", 0x40);
    memory_region_add_subregion_overlap(sys, 0xfffbac00, &s->sossi_mr, 10);
    memory_region_init_io(&s->dma_mr, NULL, &lcddma_ops, s, "omap1610.lcddma", 0x40);
    memory_region_add_subregion_overlap(sys, 0xfffee3c0, &s->dma_mr, 10);
    qemu_register_reset(display_reset, s);
}

/*
 * GPIO (the 1610's four 16-line modules at 0xFFFBE400, 0xFFFBEC00, 0xFFFBB400, 0xFFFBBC00). Interrupts:
 * module 1 level-1 line 14, 2 and 3 level-2 40 and 41, 4 level-2 48 (the kernel's dispatcher, 0x5000e95c
 * and 0x5000e62c, gives their lines interrupt numbers 0xA0.. 0xB0.. 0xC0.. 0xD0..) [V]. Registers (16 bits;
 * SYSCONFIG 32): 00 REVISION, 10 SYSCONFIG, 14 SYSSTATUS, 18 IRQSTATUS1, 1C IRQENABLE1, 20 IRQSTATUS2,
 * 24 IRQENABLE2, 28 WAKEUPENABLE, 2C DATAIN, 30 DATAOUT, 34 DIRECTION (1 = input), 38/3C EDGE_CTRL1/2
 * (2 bits a line: 1 falling, 2 rising, 3 both), 9C/A4/A8/B0 CLEAR_IRQENABLE1/2, CLEAR_WAKEUPENA,
 * CLEAR_DATAOUT, DC/E4/E8/F0 the SET_ ones. Input levels come from rm1_gpio_set_input (none yet: 0).
 */
typedef struct {
    MemoryRegion mr;
    qemu_irq irq;
    int index;
    uint32_t sysconfig;
    uint16_t irqstatus[2], irqenable[2], wakeup, in, out, dir, edge[2];
} Gpio;

static Gpio *rm1_gpio[4];
static void (*rm1_gpio_hook[4])(void *opaque, uint16_t out);
static void *rm1_gpio_hook_opaque[4];
static void (*rm1_gpio_dir_hook[4])(void *opaque, uint16_t dir);
static void *rm1_gpio_dir_hook_opaque[4];

void rm1_gpio_set_dir_hook(int module, void (*fn)(void *opaque, uint16_t dir), void *opaque)
{
    rm1_gpio_dir_hook[module] = fn;
    rm1_gpio_dir_hook_opaque[module] = opaque;
}

void rm1_gpio_set_output_hook(int module, void (*fn)(void *opaque, uint16_t out), void *opaque)
{
    rm1_gpio_hook[module] = fn;
    rm1_gpio_hook_opaque[module] = opaque;
}

static uint16_t gpio_datain(Gpio *g)
{
    return (g->in & g->dir) | (g->out & ~g->dir);
}

static void gpio_update(Gpio *g)
{
    qemu_set_irq(g->irq, (g->irqstatus[0] & g->irqenable[0]) != 0);
}

static void gpio_edge(Gpio *g, uint16_t before)
{
    uint16_t after = gpio_datain(g), changed = before ^ after;

    for (int i = 0; i < 16; i++) {
        unsigned mode = (g->edge[i >> 3] >> ((i & 7) * 2)) & 3;
        bool rose = (after >> i) & 1;

        if (!((changed >> i) & 1) || !mode) {
            continue;
        }
        if ((rose && (mode & 2)) || (!rose && (mode & 1))) {
            g->irqstatus[0] |= 1u << i;
            g->irqstatus[1] |= 1u << i;
        }
    }
    gpio_update(g);
}

void rm1_gpio_set_input(int module, int line, bool level)
{
    Gpio *g = rm1_gpio[module];
    uint16_t before = gpio_datain(g);

    g->in = (g->in & ~(1u << line)) | ((uint16_t)level << line);
    gpio_edge(g, before);
}

static uint64_t gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Gpio *g = opaque;

    switch (addr & ~3) {
    case 0x00: return 0x10;            /* REVISION [I] */
    case 0x10: return g->sysconfig;
    case 0x14: return 1;               /* SYSSTATUS: reset done */
    case 0x18: return g->irqstatus[0];
    case 0x1c: return g->irqenable[0];
    case 0x20: return g->irqstatus[1];
    case 0x24: return g->irqenable[1];
    case 0x28: return g->wakeup;
    case 0x2c: return gpio_datain(g);
    case 0x30: return g->out;
    case 0x34: return g->dir;
    case 0x38: return g->edge[0];
    case 0x3c: return g->edge[1];
    }
    qemu_log_mask(LOG_UNIMP, "nokia6630: GPIO%d read %02x\n", g->index + 1, (unsigned)addr);
    return 0;
}

static void gpio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Gpio *g = opaque;
    uint16_t v = val, before = gpio_datain(g);

    switch (addr & ~3) {
    case 0x10:
        g->sysconfig = val & ~2u;
        if (val & 2) {                 /* soft reset */
            memset(g->irqstatus, 0, sizeof(g->irqstatus));
            memset(g->irqenable, 0, sizeof(g->irqenable));
            memset(g->edge, 0, sizeof(g->edge));
            g->wakeup = g->out = 0;
            g->dir = 0xffff;
        }
        break;
    case 0x18: g->irqstatus[0] &= ~v; break;
    case 0x1c: g->irqenable[0] = v; break;
    case 0x20: g->irqstatus[1] &= ~v; break;
    case 0x24: g->irqenable[1] = v; break;
    case 0x28: g->wakeup = v; break;
    case 0x30: g->out = v; break;
    case 0x34: g->dir = v; break;
    case 0x38: g->edge[0] = v; break;
    case 0x3c: g->edge[1] = v; break;
    case 0x9c: g->irqenable[0] &= ~v; break;
    case 0xa4: g->irqenable[1] &= ~v; break;
    case 0xa8: g->wakeup &= ~v; break;
    case 0xb0: g->out &= ~v; break;
    case 0xdc: g->irqenable[0] |= v; break;
    case 0xe4: g->irqenable[1] |= v; break;
    case 0xe8: g->wakeup |= v; break;
    case 0xf0: g->out |= v; break;
    default:
        qemu_log_mask(LOG_UNIMP, "nokia6630: GPIO%d write %02x %04x\n", g->index + 1, (unsigned)addr, v);
        return;
    }
    gpio_edge(g, before);
    if (rm1_gpio_dir_hook[g->index] && (addr & ~3) == 0x34) {
        /* open-drain lines driven through the direction register (the 6630 camera's I2C, nokia6630_cam.c) */
        rm1_gpio_dir_hook[g->index](rm1_gpio_dir_hook_opaque[g->index], g->dir);
    }
    if (rm1_gpio_hook[g->index] && ((addr & ~3) == 0x30 || (addr & ~3) == 0xb0 || (addr & ~3) == 0xf0)) {
        /*
         * The output latch, whatever the direction: the Nokia N70's ssi.pdd sets AP wake (GPIO4 bit 3,
         * SET_DATAOUT in its Start 0x50150208) without the 6630 PDD's make-output call (its +0x44), and the
         * pin stays an input in the direction register (0x6F7F) [V Ghidra + run]; the 6630 made it an output
         * first, so the latch is what both phones mean.
         */
        rm1_gpio_hook[g->index](rm1_gpio_hook_opaque[g->index], g->out);
    }
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 4 },
};

static void gpio_reset_cb(void *opaque)
{
    Gpio *g = opaque;

    g->sysconfig = 0;
    memset(g->irqstatus, 0, sizeof(g->irqstatus));
    memset(g->irqenable, 0, sizeof(g->irqenable));
    memset(g->edge, 0, sizeof(g->edge));
    g->wakeup = g->out = 0;
    g->dir = 0xffff;
    qemu_set_irq(g->irq, 0);
}

void rm1_gpio_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu)
{
    static const hwaddr base[4] = { 0xfffbe400, 0xfffbec00, 0xfffbb400, 0xfffbbc00 };

    for (int i = 0; i < 4; i++) {
        Gpio *g = g_new0(Gpio, 1);
        g_autofree char *name = g_strdup_printf("omap1610.gpio%d", i + 1);

        g->index = i;
        g->irq = i == 0 ? qdev_get_gpio_in(mpu->ih[0], 14)
                        : qdev_get_gpio_in(mpu->ih[1], i == 1 ? 40 : i == 2 ? 41 : 48);
        g->dir = 0xffff;
        memory_region_init_io(&g->mr, NULL, &gpio_ops, g, name, 0x100);
        memory_region_add_subregion_overlap(sys, base[i], &g->mr, 10);
        qemu_register_reset(gpio_reset_cb, g);
        rm1_gpio[i] = g;
    }
}

/*
 * NAND (H-rm1-nand): the controller and chip of nokia6630_nand.h on a RAM copy of the image. PRISTINE is
 * eka1/tools/rm1_nand.py's output; with FLASH (machine option flash=) the phone's own NAND file is used
 * and every program/erase is written through (created from PRISTINE when missing). At reset the board
 * does what the boot loader leaves behind: the partition table (TOC) in NAND pages 0x100-0x101 is copied
 * to OCP SRAM 0x20000100 (mednand looks up "SOS-USER" and "SOS-ROFS" there [V rm1-nand.md]).
 */
static Rm1Nand rm1_nand;
static int rm1_nand_fd = -1;
static uint8_t *rm1_sram;

static void nand_flush(Rm1Nand *n, uint32_t first, uint32_t pages)
{
    if (rm1_nand_fd >= 0 &&
        pwrite(rm1_nand_fd, rm1_nand_page(n, first), (size_t)pages * RM1_NAND_RAW,
               (off_t)first * RM1_NAND_RAW) != (ssize_t)pages * RM1_NAND_RAW) {
        error_report("nokia6630: NAND write-through failed");
    }
}

static uint64_t nand_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    return rm1_nand_read(&rm1_nand, addr, size);
}

static void nand_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    rm1_nand_write(&rm1_nand, addr, val, size);
}

static const MemoryRegionOps nand_ops = {
    .read = nand_mmio_read,
    .write = nand_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void nand_reset_cb(void *opaque)
{
    Rm1Nand *n = &rm1_nand;

    n->mode = RM1_NAND_IDLE;
    n->ptr_area = 0;
    for (int i = 0; i < 2; i++) {
        memcpy(rm1_sram + 0x100 + i * RM1_NAND_PAGE, rm1_nand_page(n, 0x100 + i), RM1_NAND_PAGE);
    }
}

void rm1_nand_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu, const char *pristine,
                   const char *flash)
{
    MemoryRegion *mr = g_new(MemoryRegion, 1);
    g_autofree gchar *img = NULL;
    gsize len = 0;

    rm1_nand.store = g_malloc(RM1_NAND_SIZE);
    memset(rm1_nand.store, 0xff, RM1_NAND_SIZE);
    if (flash) {
        rm1_nand_fd = open(flash, O_RDWR | O_CREAT, 0644);
        if (rm1_nand_fd < 0) {
            error_report("nokia6630: cannot open the NAND file %s", flash);
            exit(1);
        }
        if (pread(rm1_nand_fd, rm1_nand.store, RM1_NAND_SIZE, 0) == (ssize_t)RM1_NAND_SIZE) {
            pristine = NULL;            /* the kept NAND */
        }
    }
    if (pristine) {
        if (!g_file_get_contents(pristine, &img, &len, NULL) || len != RM1_NAND_SIZE) {
            error_report("nokia6630: %s is not a %llu-byte NAND image (eka1/tools/rm1_nand.py)", pristine,
                         (unsigned long long)RM1_NAND_SIZE);
            exit(1);
        }
        memcpy(rm1_nand.store, img, RM1_NAND_SIZE);
        if (rm1_nand_fd >= 0) {
            nand_flush(&rm1_nand, 0, RM1_NAND_PAGES);
        }
    }
    rm1_nand.flush = nand_flush;
    rm1_sram = memory_region_get_ram_ptr(&mpu->imif_ram);
    memory_region_init_io(mr, NULL, &nand_ops, NULL, "omap1610.nand", 0x100);
    memory_region_add_subregion_overlap(sys, 0xfffbcc00, mr, 10);
    qemu_register_reset(nand_reset_cb, NULL);
}

/*
 * OMAP1710 SPI (0xFFFB0C00; VA 0x580B0C00 in the Nokia N90's espi.dll). Layout from the kernel's accessors
 * espi imports (ekern 0x5000F778..0x5000F7FC: base + offset) and espi's use of them [V Ghidra]:
 *   10 SYSCONFIG (bit 1 soft reset)  14 SYSSTATUS (bit 0 reset done: espi polls it 50 times, else
 *   Kern::Fault "Hw-error:SPI reset failed")  18 ISR (read, then written back to clear: bit 0 read done,
 *   1 write done, 2 and 3 errors -9/-10)  1C IER (5 for a read, 10 for a write)  24 SET1 (bit 0 clock on,
 *   bits 1-3 prescaler, bit 5 a flag)  28 SET2  2C CTRL (bit 0 read, bit 1 write, bits 2-6 word length - 1,
 *   bits 7-9 the device)  30 STATUS (bit 0 read done, bit 1 write done)  34 TX  38 RX.
 * Interrupt "Lv2_SPI": entry 81 of the kernel's name table = level-2 line 49 [V]. DMA mode (SET1 bit 5, espi's
 * mode 2): espi starts system DMA channel 0 (CSDP 0x602, CDSA 0xFFFB0C34 = TX, CCR sync 1) and then writes CTRL;
 * the SPI raises DMA request 1 until CTRL or SET1 bit 5 is cleared and sends each word the DMA writes to TX [V
 * catches on the kernel's DMA write helper 0x5000E640].
 * H-rm42-spi: a transfer completes at once. The devices are the N90's DCS panels over 9-bit SPI (bit 8 = data,
 * genericlcd_lcd.ldd sends DCS 11 Sleep Out first [V trace]): a read answers RDDID (04) with an ID the LCD
 * driver knows (0x290015: its panel type 0x2000, which is also its default for an unknown ID; 0x5004C4C8),
 * anything else 0 [I].
 */
typedef struct {
    MemoryRegion mr;
    qemu_irq irq, drq_tx;
    uint32_t r[0x40 / 4];
    uint8_t last_cmd[8];                /* per device: the last DCS command byte */
} Rm1Spi;

#define RM1_SPI_ID 0x290015u            /* RDDID as genericlcd_lcd.ldd reads it (low byte 0x15, bits 16-23 0x29) */

static uint32_t rm1_spi_answer(uint8_t last_cmd)
{
    return last_cmd == 0x04 ? RM1_SPI_ID : 0;
}

static void spi_update(Rm1Spi *s)
{
    qemu_set_irq(s->irq, (s->r[0x18 / 4] & s->r[0x1c / 4] & 0xf) != 0);
    qemu_set_irq(s->drq_tx, (s->r[0x24 / 4] & 0x20) && (s->r[0x2c / 4] & 2));
}

/* one word out to device DEV (BITS long): DCS command bytes (9-bit words with bit 8 clear) are remembered */
static void spi_send(Rm1Spi *s, unsigned dev, unsigned bits, uint32_t tx)
{
    tx &= bits >= 32 ? 0xffffffffu : (1u << bits) - 1;     /* DMA elements are 32 bits wide */
    qemu_log_mask(LOG_UNIMP, "nokia6630: SPI dev%u tx %u bits %x\n", dev, bits, tx);
    if (bits == 9 && !(tx & 0x100)) {
        s->last_cmd[dev & 7] = tx;
    }
    s->r[0x30 / 4] |= 2;
    s->r[0x18 / 4] |= 2;
}

static void spi_reset(void *opaque)
{
    Rm1Spi *s = opaque;

    memset(s->r, 0, sizeof(s->r));
    memset(s->last_cmd, 0, sizeof(s->last_cmd));
    spi_update(s);
}

static uint64_t spi_read(void *opaque, hwaddr addr, unsigned size)
{
    Rm1Spi *s = opaque;

    addr &= 0x3c;
    switch (addr) {
    case 0x14: return 1;                                /* SYSSTATUS: reset done */
    }
    return s->r[addr / 4];
}

static void spi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rm1Spi *s = opaque;
    unsigned dev = (val >> 7) & 7, bits = ((val >> 2) & 0x1f) + 1;

    addr &= 0x3c;
    switch (addr) {
    case 0x10:
        if (val & 2) {
            spi_reset(s);
        }
        s->r[addr / 4] = val & ~2u;
        return;
    case 0x14:
    case 0x30:
        return;                                         /* read-only */
    case 0x18:
        s->r[addr / 4] &= ~val;
        spi_update(s);
        return;
    case 0x2c:
        s->r[addr / 4] = val;
        if ((val & 2) && !(s->r[0x24 / 4] & 0x20)) {   /* write: TX to the device (DMA mode: spi_update) */
            spi_send(s, dev, bits, s->r[0x34 / 4]);
        }
        if (val & 1) {                                  /* read: the device's answer to RX */
            s->r[0x38 / 4] = rm1_spi_answer(s->last_cmd[dev]);
            qemu_log_mask(LOG_UNIMP, "nokia6630: SPI dev%u rx %u bits -> %x\n", dev, bits, s->r[0x38 / 4]);
            s->r[0x30 / 4] |= 1;
            s->r[0x18 / 4] |= 1;
        }
        spi_update(s);
        return;
    }
    s->r[addr / 4] = val;
    if (addr == 0x34 && (s->r[0x24 / 4] & 0x20) && (s->r[0x2c / 4] & 2)) {   /* a word from the DMA */
        uint32_t ctl = s->r[0x2c / 4];

        spi_send(s, (ctl >> 7) & 7, ((ctl >> 2) & 0x1f) + 1, val);
    }
    if (addr == 0x1c || addr == 0x24 || addr == 0x34) {
        spi_update(s);
    }
}

static const MemoryRegionOps spi_ops = {
    .read = spi_read,
    .write = spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void rm1_spi_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu)
{
    Rm1Spi *s = g_new0(Rm1Spi, 1);

    s->irq = qdev_get_gpio_in(mpu->ih[1], 49);
    s->drq_tx = mpu->drq[1];
    memory_region_init_io(&s->mr, NULL, &spi_ops, s, "omap1710.spi", 0x40);
    memory_region_add_subregion_overlap(sys, 0xfffb0c00, &s->mr, 10);
    spi_reset(s);
    qemu_register_reset(spi_reset, s);
}

/*
 * 32 kHz watchdog (0xFFFEB000, the OMAP2 WDT layout): wd32khz.ext soft-resets it (SYSCONFIG 0x10 bit 1)
 * and polls SYSSTATUS (0x14) for 1 [V 0x500aba0c]. H-rm1-wdt: a register file; it never expires.
 * 14 SYSSTATUS reads 1, 34 WWPS (write pending) reads 0, the rest read back what was written.
 */
static uint32_t rm1_wdt_reg[0x20];

static uint64_t wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr & ~3) {
    case 0x14: return 1;
    case 0x34: return 0;
    }
    return rm1_wdt_reg[(addr >> 2) & 0x1f];
}

static void wdt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    rm1_wdt_reg[(addr >> 2) & 0x1f] = (addr & ~3) == 0x10 ? val & ~2u : val;
}

static const MemoryRegionOps wdt_ops = {
    .read = wdt_read,
    .write = wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void rm1_wdt_init(MemoryRegion *sys)
{
    MemoryRegion *mr = g_new(MemoryRegion, 1);

    memory_region_init_io(mr, NULL, &wdt_ops, NULL, "omap1610.wdt32k", 0x80);
    memory_region_add_subregion_overlap(sys, 0xfffeb000, mr, 10);
}

/*
 * Modem link (H-rm1-modem): the SSI block at 0x30000000 (SST transmitter +0x000, SSR receiver +0x800, its
 * GDD DMA channels at +0x1800 + 0x40 * n [V run: the PDD writes CCR at +0x1802/+0x1842; rm1-ssi-xbus.md says 0x2800, a VA offset]) with the modem's end of xbus.ldd's word protocol
 * (nokia6630_ssi.h) and the ISI peer of ngage_isi.c (isi=FILE). Interrupts [V numbers from the kernel's
 * name table, rm1-ssi-xbus.md]: Lv1_SSRFullCh0 = level-1 29 (a word waits in SSR channel 0),
 * Lv2_SSRCtrl = level-2 90 (raised once when the phone raises AP wake, GPIO4 bit 3), Lv2_GDDChn =
 * level-2 95 + n [I]. The modem's wake line (GPIO1 bit 12) is held high: always awake [I].
 */
typedef struct {
    MemoryRegion mr;
    Rm1SsiLink link;
    qemu_irq full0, ctrl, gdd[8];
    uint32_t sst[0x40], ssr[0x40], gdd_global[0x40];  /* +0x1100.. (the kernel writes 0x1100 = 8) */
    uint16_t gddr[8][0x20];
    bool ctrl_sent;
    QEMUTimer *poll, *gdd_done;
    uint8_t gdd_pending;            /* channels whose completion is due */
    unsigned logged;
} Rm1Ssi;

#define SST_CH3 0x3000008cu
#define SSR_CH3 0x3000088cu

static void ssi_irqs(Rm1Ssi *s)
{
    qemu_set_irq(s->full0, s->link.rxn > 0);
}

static void ssi_gdd_run(Rm1Ssi *s, int ch)
{
    uint16_t *r = s->gddr[ch];
    uint32_t src = r[4] | (uint32_t)r[5] << 16, dst = r[6] | (uint32_t)r[7] << 16;
    uint32_t bytes = MIN(r[8] * 4u, RM1_SSI_MAX);
    uint8_t buf[RM1_SSI_MAX];

    if (dst == SST_CH3) {                   /* phone -> modem */
        address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED, buf, bytes);
        ngage_isi_isa_send(buf, rm1_ssi_isi_len(buf, bytes));
        s->link.want_words = 0;
    } else if (src == SSR_CH3) {            /* modem -> phone */
        bytes = MIN(bytes, s->link.out_len);
        address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED, s->link.out, bytes);
        s->link.out_len = 0;
    } else {
        qemu_log_mask(LOG_UNIMP, "nokia6630: SSI GDD%d src %08x dst %08x: not a channel-3 copy\n",
                      ch, src, dst);
    }
    s->gdd_pending |= 1u << ch;
    timer_mod(s->gdd_done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50000);
}

static void ssi_gdd_done(void *opaque)
{
    Rm1Ssi *s = opaque;

    for (int ch = 0; ch < 8; ch++) {
        if (s->gdd_pending & (1u << ch)) {
            s->gddr[ch][1] &= ~0x80;        /* CCR: off */
            s->gddr[ch][3] = 0x20;          /* CSR: block done (bit 0 would panic the PDD) */
            qemu_irq_raise(s->gdd[ch]);
        }
    }
    s->gdd_pending = 0;
}

static void ssi_poll(void *opaque)
{
    Rm1Ssi *s = opaque;
    uint8_t m[RM1_SSI_MAX];

    rm1_ssi_tick(&s->link);
    if (s->link.up && !s->link.out_len && !s->link.out_wait_ack) {
        uint32_t n = ngage_isi_isa_take(m, sizeof(m));

        if (n) {
            rm1_ssi_modem_send(&s->link, m, n);
        }
    }
    ssi_irqs(s);
    timer_mod(s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

static void ssi_log(Rm1Ssi *s, const char *what, hwaddr a, uint32_t v)
{
    if (s->logged < 200) {
        s->logged++;
        qemu_log_mask(LOG_UNIMP, "nokia6630: SSI %s %04x %08x\n", what, (unsigned)a, v);
    }
}

static uint64_t ssi_read(void *opaque, hwaddr a, unsigned size)
{
    Rm1Ssi *s = opaque;
    uint32_t w;

    if (a >= 0x1800 && a < 0x1a00) {
        int ch = (a - 0x1800) / 0x40, r = (a & 0x3f) / 2;
        uint32_t v = s->gddr[ch][r] | (size == 4 ? (uint32_t)s->gddr[ch][(r + 1) & 0x1f] << 16 : 0);

        if (r == 3) {
            s->gddr[ch][3] = 0;
            qemu_irq_lower(s->gdd[ch]);
        }
        return v;
    }
    switch (a) {
    case 0x010: return 0;                   /* SST buffer state: never full */
    case 0x820: return 0;                   /* SSR error */
    case 0x880:                             /* SSR channel 0 */
        w = 0;
        if (rm1_ssi_pop(&s->link, &w)) {
            ssi_log(s, "modem->", a, w);
        }
        ssi_irqs(s);
        return w;
    }
    if (a < 0x100) {
        return s->sst[a / 4];
    }
    if (a >= 0x800 && a < 0x900) {
        return s->ssr[(a - 0x800) / 4];
    }
    if (a >= 0x1100 && a < 0x1200) {
        return s->gdd_global[(a - 0x1100) / 4];
    }
    ssi_log(s, "R", a, 0);
    return 0;
}

static void ssi_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    Rm1Ssi *s = opaque;

    if (a >= 0x1800 && a < 0x1a00) {
        int ch = (a - 0x1800) / 0x40, r = (a & 0x3f) / 2;

        s->gddr[ch][r] = v;
        if (size == 4) {
            s->gddr[ch][(r + 1) & 0x1f] = v >> 16;
        }
        if (r == 1 && (v & 0x80)) {
            ssi_gdd_run(s, ch);
        }
        return;
    }
    switch (a) {
    case 0x080:                             /* SST channel 0: a control word */
        ssi_log(s, "phone->", a, v);
        rm1_ssi_from_phone(&s->link, v);
        ssi_irqs(s);
        return;
    case 0x81c:                             /* SSRCtrl acknowledge */
        qemu_irq_lower(s->ctrl);
        break;
    }
    if (a < 0x100) {
        s->sst[a / 4] = v;
    } else if (a >= 0x800 && a < 0x900) {
        s->ssr[(a - 0x800) / 4] = v;
    } else if (a >= 0x1100 && a < 0x1200) {
        s->gdd_global[(a - 0x1100) / 4] = v;
    } else {
        ssi_log(s, "W", a, v);
    }
}

static const MemoryRegionOps ssi_ops = {
    .read = ssi_read,
    .write = ssi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 4 },
};

static void ssi_ap_wake(void *opaque, uint16_t out)
{
    Rm1Ssi *s = opaque;

    if ((out & 8) && !s->ctrl_sent) {       /* GPIO4 bit 3 up: the modem starts the link once */
        s->ctrl_sent = true;
        qemu_irq_raise(s->ctrl);
    }
}

static void ssi_reset(void *opaque)
{
    Rm1Ssi *s = opaque;

    memset(&s->link, 0, sizeof(s->link));
    memset(s->sst, 0, sizeof(s->sst));
    memset(s->ssr, 0, sizeof(s->ssr));
    memset(s->gddr, 0, sizeof(s->gddr));
    s->ctrl_sent = false;
    s->gdd_pending = 0;
    qemu_irq_lower(s->ctrl);
    for (int i = 0; i < 8; i++) {
        qemu_irq_lower(s->gdd[i]);
    }
    rm1_gpio_set_input(0, 12, true);
    timer_mod(s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

void rm1_ssi_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu, const char *isi)
{
    Rm1Ssi *s = g_new0(Rm1Ssi, 1);

    s->full0 = qdev_get_gpio_in(mpu->ih[0], 29);
    s->ctrl = qdev_get_gpio_in(mpu->ih[1], 90);
    for (int i = 0; i < 8; i++) {
        s->gdd[i] = qdev_get_gpio_in(mpu->ih[1], 95 + i);
    }
    s->poll = timer_new_ns(QEMU_CLOCK_VIRTUAL, ssi_poll, s);
    s->gdd_done = timer_new_ns(QEMU_CLOCK_VIRTUAL, ssi_gdd_done, s);
    memory_region_init_io(&s->mr, NULL, &ssi_ops, s, "omap1610.ssi", 0x4000);
    memory_region_add_subregion_overlap(sys, 0x30000000, &s->mr, 10);
    rm1_gpio_set_output_hook(3, ssi_ap_wake, s);
    if (isi) {
        ngage_isi_load(isi);
    }
    qemu_register_reset(ssi_reset, s);
}

/* Keypad: host keys -> the MPUIO matrix (nokia6630_keys.h); QEMU's omap_mpuio raises the keyboard line */
#define RM1_Q(k) Q_KEY_CODE_##k
static const Rm1Key rm1_keys_6630[] = RM1_KEYS(RM1_Q);
static const Rm1Key rm1_keys_n70[] = RM84_KEYS(RM1_Q);
static const Rm1Key *rm1_keys;
static unsigned rm1_nkeys;
static struct omap_mpuio_s *rm1_mpuio;

static void rm1_key_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    int q;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    q = qemu_input_linux_to_qcode(evt->key.key);       /* this tree passes Linux key codes */
    for (unsigned i = 0; i < rm1_nkeys; i++) {
        if (rm1_keys[i].qcode == q) {
            omap_mpuio_key(rm1_mpuio, rm1_keys[i].row, rm1_keys[i].col, evt->key.down);
            return;
        }
    }
}

static const QemuInputHandler rm1_key_handler = {
    .name  = "nokia6630 keypad",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = rm1_key_event,
};

void rm1_keys_init(struct omap_mpu_state_s *mpu, bool n70)
{
    rm1_keys = n70 ? rm1_keys_n70 : rm1_keys_6630;
    rm1_nkeys = n70 ? ARRAY_SIZE(rm1_keys_n70) : ARRAY_SIZE(rm1_keys_6630);
    rm1_mpuio = mpu->mpuio;
    qemu_input_handler_activate(qemu_input_handler_register(NULL, &rm1_key_handler));
}
