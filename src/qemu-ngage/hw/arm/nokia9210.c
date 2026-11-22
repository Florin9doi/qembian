/*
 * Nokia 9210 Communicator (RAE-3, Series 80 1st edition, Symbian 6.0 / EKA1) bring-up board: -M nokia9210.
 * docs/devices/nokia-9210-rae3/bring-up.md has the evidence for every value here ([V] read in the
 * phone's code or seen in a run, [I] inferred).
 *
 * The SoC has no public datasheet: the board is modelled from the phone's own code, as the P910i's was.
 *   CPU         TI925T: the bootstrap's CPU table accepts Main ID 0x54009250 & 0xFF00FFF0 [V 0x50000A50]
 *   0x00000000  flash bank 0: Intel ID 0x0089 / 0x8864, ROM offsets 0..8 MB [V bootstrap 0x50000B30]
 *   0x01000000  flash bank 1: the rest of the ROM (bank table {0, 0x01000000, 0x02000000} [V 0x50000DD0])
 *   0x03000000  SDRAM, 16 MB (stack at 0x03FFFFF0, size probe at 0x03800000; RAM test 0x03000000..0x037FFFFF [V])
 *   0x04000000  memory controller (EMIF-like; written by the bootstrap's CPU init 0x50000AB8)
 *   0x05000000  peripherals (kernel window VA 0x58000000, 256 KB [V 0x50000E10]), on-chip SRAM at
 *               0x05011000 (the DSP's API RAM; the bootstrap runs its flash-ID routine there [V])
 *   everything else: a catch-all that logs ("nokia9210: R/W addr size value pc"), keeps written values and
 *   ORs in the "ready" bits of nokia9210_ready[].
 * Options: -M nokia9210,ppm=DIR[,flash=FILE]  DIR = eka1/rae3/core (ROM.bin: eka1/tools/rae3_extract.py),
 *          FILE = the DataFlash (C:, 4 MB), created blank and kept
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "target/arm/cpu.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "nokia9210_dataflash.h"
#include "ui/console.h"
#include "ui/input.h"
#include "nokia9210_keys.h"

#define RAE3_BANK0      0x00000000u
#define RAE3_BANK0_SIZE (8 * MiB)
#define RAE3_BANK1      0x01000000u
#define RAE3_BANK1_SIZE (4 * MiB)
#define RAE3_BANK_WIN   (16 * MiB)       /* address window per chip select: the chip repeats in it */
#define RAE3_RAM_BASE   0x03000000u
#define RAE3_SRAM_BASE  0x05011000u
#define RAE3_SRAM_SIZE  0x1000u          /* the DSP API RAM [V cmt.ext], 4 KB [I]; 0x05012000 is registers [V] */

static char *rae3_dir;                  /* -M nokia9210,ppm=DIR */
static char *rae3_flash_file;           /* -M nokia9210,flash=FILE: the DataFlash (C:), kept across boots */

/* ---- catch-all for unmodelled addresses ---------------------------------------------------------- */

typedef struct {
    MemoryRegion mr;
    hwaddr base;
    GHashTable *mem;
    GHashTable *seen;
} Rae3Unknown;

uint32_t rae3_pc(void);
uint32_t rae3_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void rae3_unknown_log(Rae3Unknown *u, hwaddr addr, bool wr, uint64_t val, unsigned size)
{
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(u->seen, k));

    if (n < 16) {
        g_hash_table_insert(u->seen, k, GUINT_TO_POINTER(n + 1));
        qemu_log_mask(LOG_UNIMP, "nokia9210: %s %08" HWADDR_PRIx " %u %08" PRIx64 " pc=%08x%s\n",
                      wr ? "W" : "R", u->base + addr, size, val, rae3_pc(),
                      n + 1 == 16 ? " (further accesses not logged)" : "");
    }
}

/*
 * Status bits polled before the device behind them is modelled: they read as set, always. Each line: the
 * physical word address, the bits, and the poll that needs them (docs/devices/nokia-9210-rae3/bring-up.md).
 */
static const struct { uint32_t addr, bits; } nokia9210_ready[] = {
    { 0x05003000, 0x800 },      /* cmt.ext 0x50A8F4A8: after 0x05002004 = 0x1000, 0x800 (a clock enable) waits for
                                 * bit 11 (its lock / ready) [V] */
    { 0x050121b4, 0x06 },       /* cmt.ext 0x50A1DEF2 / 0x50A1DF64: a byte serial interface (+0xB0 out, +0x1B0 in),
                                 * status bit 1 = sent, bit 2 = received [V] */
};

/* Start/busy bits that the hardware clears when done: they read back as 0 at once. */
static const struct { uint32_t addr, bits; } nokia9210_selfclear[] = {
    { 0x05012060, 0x80 },       /* cmt.ext 0x50A1F59E: 0x80 starts a conversion (channel in 0x80049BBC), waits for
                                 * bit 7 to clear [V]; an ADC [I] */
};

static uint64_t rae3_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    Rae3Unknown *u = opaque;
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem, GUINT_TO_POINTER((guint)(addr & ~3u))));
    uint64_t v;

    for (unsigned i = 0; i < ARRAY_SIZE(nokia9210_ready); i++) {
        if (nokia9210_ready[i].addr == u->base + (addr & ~3u)) {
            w |= nokia9210_ready[i].bits;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(nokia9210_selfclear); i++) {
        if (nokia9210_selfclear[i].addr == u->base + (addr & ~3u)) {
            w &= ~nokia9210_selfclear[i].bits;
        }
    }
    v = (w >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
    rae3_unknown_log(u, addr, false, v, size);
    return v;
}

static void rae3_unknown_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rae3Unknown *u = opaque;
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem, k));
    unsigned sh = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;

    w = (w & ~mask) | (((uint32_t)val << sh) & mask);
    g_hash_table_insert(u->mem, k, GUINT_TO_POINTER(w));
    rae3_unknown_log(u, addr, true, val, size);
}

static const MemoryRegionOps rae3_unknown_ops = {
    .read = rae3_unknown_read,
    .write = rae3_unknown_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void rae3_unknown(MemoryRegion *sys, hwaddr base, uint64_t size, const char *name, int prio)
{
    Rae3Unknown *u = g_new0(Rae3Unknown, 1);

    u->base = base;
    u->mem = g_hash_table_new(g_direct_hash, g_direct_equal);
    u->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&u->mr, NULL, &rae3_unknown_ops, u, name, size);
    memory_region_add_subregion_overlap(sys, base, &u->mr, prio);
}

/* ---- flash: Intel command set --------------------------------------------------------------------- */

/*
 * 16-bit Intel flash, the commands the phone uses [V]:
 * - the bootstrap's ID probe (copied to 0x05011000, run per chip select 0, 0x01000000, 0x02000000): strb 0x90
 *   (read identifier), ldr a word, strb 0xFF (read array); bank 0 = 0x88640089 (r6, literal 0x50000B6C) ends
 *   the probe; otherwise every chip select up to 0x02000000 must answer 0x88960089 (r3, 0x50000B68), each one
 *   that does not sets an error bit and the bootstrap blinks its error LED (0x05007000) for ever
 *   [V 0x50000B30..0x50000B6C + run];
 * - pmmwrite.dll (the cellular side's permanent memory, PMM, at the top of chip select 2): 0x50 clear status,
 *   0x40 program one halfword, 0x20 + 0xD0 block erase, 0xB0 suspend / 0xD0 resume, 0x70 read status (bit 7 =
 *   ready, bits 3-5 errors), 0xFF read array [V 0x500BA400..0x500BA560].
 * Operations complete at once (status always ready). Erase block 64 KB [I]. A chip repeats over its chip
 * select's 16 MB window (the bootstrap sizes banks by finding the mirror).
 */
typedef struct {
    MemoryRegion mr;
    uint8_t *data;
    uint32_t size;
    uint16_t device;
    enum { RAE3_ARRAY, RAE3_ID, RAE3_STATUS, RAE3_PROGRAM, RAE3_ERASE } mode;
} Rae3Flash;

#define RAE3_FLASH_BLOCK 0x10000u

static uint64_t rae3_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    Rae3Flash *f = opaque;
    uint32_t w;

    switch (f->mode) {
    case RAE3_ID:
        if (size == 4) {
            return 0x0089 | (uint32_t)f->device << 16;
        }
        return (addr & 2) ? f->device : 0x0089;            /* a halfword per identifier address */
    case RAE3_ARRAY:
        w = ldl_le_p(f->data + ((addr & ~3u) % f->size));
        return (w >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
    default:
        return 0x80;                                     /* status: ready, no error */
    }
}

static void rae3_flash_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Rae3Flash *f = opaque;
    uint32_t off = addr % f->size;

    if (f->mode == RAE3_PROGRAM) {                      /* the halfword (programming clears bits only) */
        uint16_t old = lduw_le_p(f->data + (off & ~1u));

        stw_le_p(f->data + (off & ~1u), old & (uint16_t)val);
        memory_region_set_dirty(&f->mr, off & ~1u, 2);
        f->mode = RAE3_STATUS;
    } else if (f->mode == RAE3_ERASE && (val & 0xff) == 0xd0) {
        memset(f->data + (off & ~(RAE3_FLASH_BLOCK - 1)), 0xff, RAE3_FLASH_BLOCK);
        memory_region_set_dirty(&f->mr, off & ~(RAE3_FLASH_BLOCK - 1), RAE3_FLASH_BLOCK);
        f->mode = RAE3_STATUS;
    } else {
        switch (val & 0xff) {
        case 0x90: f->mode = RAE3_ID; break;
        case 0xff: f->mode = RAE3_ARRAY; break;
        case 0x40: case 0x10: f->mode = RAE3_PROGRAM; break;
        case 0x20: f->mode = RAE3_ERASE; break;
        case 0x50: case 0x70: case 0xb0: case 0xd0: f->mode = RAE3_STATUS; break;
        default:
            qemu_log_mask(LOG_UNIMP, "nokia9210: flash command %02x at %08x pc=%08x (not modelled)\n",
                          (unsigned)(val & 0xff), off, rae3_pc());
            f->mode = RAE3_STATUS;
            break;
        }
    }
    memory_region_rom_device_set_romd(&f->mr, f->mode == RAE3_ARRAY);
}

static const MemoryRegionOps rae3_flash_ops = {
    .read = rae3_flash_read,
    .write = rae3_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void rae3_flash(MemoryRegion *sys, hwaddr base, const uint8_t *img, uint32_t len, uint32_t size,
                       uint16_t device, const char *name)
{
    Rae3Flash *f = g_new0(Rae3Flash, 1);

    memory_region_init_rom_device(&f->mr, NULL, &rae3_flash_ops, f, name, size, &error_fatal);
    f->data = memory_region_get_ram_ptr(&f->mr);
    f->size = size;
    f->device = device;
    memset(f->data, 0xff, size);
    memcpy(f->data, img, MIN(len, size));
    memory_region_add_subregion(sys, base, &f->mr);
    for (hwaddr a = base + size; a < base + RAE3_BANK_WIN; a += size) {
        MemoryRegion *alias = g_new(MemoryRegion, 1);

        memory_region_init_alias(alias, NULL, name, &f->mr, 0, size);
        memory_region_add_subregion(sys, a, alias);
    }
}

/* ---- the DSP's shared API RAM (0x05011000) --------------------------------------------------------- */

/*
 * H-rae3-dsp: no DSP is emulated. The 4 KB at 0x05011000 are the DSP's API RAM (the bootstrap only borrows
 * it to run its flash-ID routine). cmt.ext boots the DSP through it [V 0x50A1EF9E..0x50A1F06A]: 0x05012008
 * bit 0 set (DSP reset released), then 127 blocks of 512 halfwords to API+0x200, after each the MCU clears
 * one flag and waits for the DSP's boot loader to set the other (MCU 0 -> +0x100, DSP -> +0xFE; MCU 0 ->
 * +0xFE, DSP -> +0x100), then it waits for API+0x002 != 0xFFFF and keeps API+0x000 / +0x002 (the DSP code's
 * version [I]). The stand-in answers each handshake at once and leaves a version 0x0001 / 0x0100 [I].
 */
static uint8_t rae3_api[0x1000];

static uint64_t rae3_api_read(void *opaque, hwaddr a, unsigned size)
{
    return size == 4 ? ldl_le_p(rae3_api + a) : size == 2 ? lduw_le_p(rae3_api + a) : rae3_api[a];
}

static void rae3_api_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    if (size == 4) {
        stl_le_p(rae3_api + a, v);
    } else if (size == 2) {
        stw_le_p(rae3_api + a, v);
    } else {
        rae3_api[a] = v;
    }
    if (size == 2 && !v && (a == 0x100 || a == 0xfe)) {     /* the boot loader's handshake */
        stw_le_p(rae3_api + (a == 0x100 ? 0xfe : 0x100), 1);
        stw_le_p(rae3_api + 0x000, 0x0001);
        stw_le_p(rae3_api + 0x002, 0x0100);
    }
}

static const MemoryRegionOps rae3_api_ops = {
    .read = rae3_api_read,
    .write = rae3_api_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- interrupt controllers and timers ----------------------------------------------------------- */

/*
 * Two controllers, 64 sources each [V kernel accessors 0x50002408..0x500025A0, dispatchers 0x50003CA0 /
 * 0x50003CF8]: 0x05006000 = the kernel's lines 0..56 (its name table's "Fiq..." set: Timer0-4, DMA0-3, Lid,
 * IrDA, Media, SerialFlash, Ctsi, Acc, Key, Battery, LCD, Gpt32k ...) to the CPU's FIQ, 0x05005000 = lines
 * 57..113 ("Irq...", the same sources in the same order, index + 0x39) to the IRQ [I: which pin]. Per
 * controller: +0x08 / +0x28 enable bits (write 1 sets), +0x0C / +0x2C (write 1 clears), +0x10 / +0x30
 * pending (raw & enabled), +0x14 / +0x34 the pending source + 1 among 0..31 / 32..63 (0 = none). Sources are levels from the
 * devices (a timer's expired flag) [I]; the lowest pending source wins [I].
 */
typedef struct {
    uint32_t enable[2];
} Rae3Intc;

static Rae3Intc rae3_intc[2];              /* 0 = 0x05006000 (FIQ), 1 = 0x05005000 (IRQ) */
static uint64_t rae3_src;                  /* source levels, bit = source */
static qemu_irq rae3_cpu_fiq, rae3_cpu_irq;

static void rae3_intc_update(void)
{
    for (int c = 0; c < 2; c++) {
        uint64_t en = rae3_intc[c].enable[0] | (uint64_t)rae3_intc[c].enable[1] << 32;

        qemu_set_irq(c ? rae3_cpu_irq : rae3_cpu_fiq, (rae3_src & en) != 0);
    }
}

void rae3_set_source(int n, bool level);
void rae3_set_source(int n, bool level)
{
    if (level) {
        rae3_src |= 1ull << n;
    } else {
        rae3_src &= ~(1ull << n);
    }
    rae3_intc_update();
}

static uint64_t rae3_intc_read(void *opaque, hwaddr a, unsigned size)
{
    Rae3Intc *c = opaque;
    int hi = (a >> 5) & 1;
    uint32_t pend = (uint32_t)(rae3_src >> (32 * hi)) & c->enable[hi];

    switch (a & 0x1f) {
    case 0x08: return c->enable[hi];
    case 0x10: return pend;         /* pending = raw & enabled: pmmwrite suspends an erase while it is
                                     * non-zero, and a raw never-enabled source would hold it for ever [V run] */
    case 0x14: return pend ? ctz32(pend) + 1 : 0;
    }
    return 0;
}

static void rae3_intc_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    Rae3Intc *c = opaque;
    int hi = (a >> 5) & 1;

    switch (a & 0x1f) {
    case 0x08: c->enable[hi] |= v; break;
    case 0x0c: c->enable[hi] &= ~v; break;
    default:
        qemu_log_mask(LOG_UNIMP, "nokia9210: intc %08x write %02x %08x pc=%08x\n",
                      c == &rae3_intc[0] ? 0x05006000 : 0x05005000, (unsigned)a, (unsigned)v, rae3_pc());
        return;
    }
    rae3_intc_update();
}

static const MemoryRegionOps rae3_intc_ops = {
    .read = rae3_intc_read,
    .write = rae3_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * Timers [V accessors 0x500025A4..0x50002668, users 0x50003B38 (timer 3: run 2, reload and count 0x1FBD0,
 * expired cleared, waits for it), 0x50003D6C (timer 1 free-running from 0x7FFFFFFF), 0x50004DF4 (timer 1
 * one-shot delay)]: timer n at 0x05007000 + 0x20 * n (n = 0..4, sources 0..4), the 32 kHz timer at
 * 0x05016000 (sources 55 "Gpt32k" and 56 "Gpt32kWb" together: the kernel's start-up waits on 55, later it
 * enables only 56 [V intc in runs]). +0x04 run: 0 stop, 1 one shot, 2 periodic [I from the users]; +0x08
 * expired (set when the count reaches 0; written 0 to clear); +0x0C kept; +0x10 count (write loads, read =
 * current, counting down); +0x14 the next period: loaded when the count reaches 0, or at once into a stopped
 * timer (the 32 kHz tick handler 0x500038F4 writes 0x200 there each tick [V]; without it the tick stopped
 * after one period and every kernel timer with it); +0x18 the reload value for periodic mode. Clock 13 MHz (0x1FBD0 = 10 ms)
 * and 32768 Hz [I].
 */
typedef struct {
    uint32_t reg[8];
    int64_t start_ns;          /* when the count had the value in reg[4] */
    uint32_t hz;
    int source;
    QEMUTimer *t;
} Rae3Timer;

static Rae3Timer rae3_timers[6];

static uint32_t rae3_timer_count(Rae3Timer *t)
{
    int64_t ticks;

    if (!(t->reg[1] & 3)) {
        return t->reg[4];
    }
    ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - t->start_ns, t->hz, NANOSECONDS_PER_SECOND);
    return ticks >= t->reg[4] ? 0 : t->reg[4] - ticks;
}

static void rae3_timer_arm(Rae3Timer *t)
{
    if ((t->reg[1] & 3) && t->reg[4]) {
        timer_mod(t->t, t->start_ns + (int64_t)muldiv64(t->reg[4], NANOSECONDS_PER_SECOND, t->hz));
    } else {
        timer_del(t->t);
    }
}

static void rae3_timer_expire(void *opaque)
{
    Rae3Timer *t = opaque;

    t->reg[2] = 1;
    rae3_set_source(t->source, true);
    if (t->source == 55) {
        rae3_set_source(56, true);
    }
    if ((t->reg[1] & 3) == 2 && t->reg[6]) {        /* periodic: reload */
        t->start_ns += muldiv64(t->reg[4], NANOSECONDS_PER_SECOND, t->hz);
        t->reg[4] = t->reg[6];
        rae3_timer_arm(t);
    } else if (t->reg[5]) {                         /* +0x14: the next period (the tick handler's) */
        t->start_ns += muldiv64(t->reg[4], NANOSECONDS_PER_SECOND, t->hz);
        t->reg[4] = t->reg[5];
        t->reg[5] = 0;
        rae3_timer_arm(t);
    } else {
        t->reg[4] = 0;
        t->reg[1] &= ~3u;
    }
}

static uint64_t rae3_timer_read(void *opaque, hwaddr a, unsigned size)
{
    Rae3Timer *t = opaque ? &rae3_timers[5] : &rae3_timers[(a >> 5) % 5];
    unsigned r = (a & 0x1f) >> 2;

    return r == 4 ? rae3_timer_count(t) : t->reg[r];
}

static void rae3_timer_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    Rae3Timer *t = opaque ? &rae3_timers[5] : &rae3_timers[(a >> 5) % 5];
    unsigned r = (a & 0x1f) >> 2;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (r) {
    case 1:                                         /* run */
        t->reg[4] = rae3_timer_count(t);
        t->start_ns = now;
        t->reg[1] = v;
        break;
    case 2:                                         /* expired: write 0 clears */
        t->reg[2] = v;
        if (!v) {
            rae3_set_source(t->source, false);
            if (t->source == 55) {
                rae3_set_source(56, false);
            }
        }
        return;
    case 4:                                         /* count: load */
        t->reg[4] = v;
        t->start_ns = now;
        break;
    case 5:                                         /* the next period: restarts a stopped timer */
        t->reg[5] = v;
        if (!(t->reg[1] & 3) && v) {
            t->reg[1] |= 1;
            t->reg[4] = v;
            t->reg[5] = 0;
            t->start_ns = now;
            break;
        }
        return;
    default:
        t->reg[r] = v;
        return;
    }
    rae3_timer_arm(t);
}

static const MemoryRegionOps rae3_timer_ops = {
    .read = rae3_timer_read,
    .write = rae3_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * CTSI (the cellular side's timing block, kernel sources 33 "CtsiIrq" / 34 "CtsiFiq") at 0x05012020..0x0501204F
 * [V cmt.ext 0x50A6DBC0..0x50A6E10C]: +0x40 / +0x44 a 16-bit counter (high, low byte), +0x48 / +0x4C a compare
 * value (written count + 2 and waited for), +0x20 raw status, bit 4 = compare matched (polled during
 * start-up, write 1 clears), +0x24 interrupt status (same bits, write 1 clears) and +0x2C its mask: the
 * handler (0x50A1D9DE) loops while +0x24 & ~+0x2C, masking, acknowledging and unmasking each bit [V], +0x3C
 * written 0xF9 and polled until the hardware changes it (a synchronisation step [I]). Model: the counter runs
 * at 32768 Hz [I], a match sets bit 4 in +0x20 and +0x24; source 33 = +0x24 & ~+0x2C; +0x3C reads 0 [I].
 */
typedef struct {
    uint8_t reg[0x30];
    int64_t base_ns;
    QEMUTimer *t;
} Rae3Ctsi;

static Rae3Ctsi rae3_ctsi;

static uint16_t rae3_ctsi_count(void)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - rae3_ctsi.base_ns, 32768, NANOSECONDS_PER_SECOND);
}

static void rae3_ctsi_arm(void)
{
    uint16_t cmp = rae3_ctsi.reg[0x28] << 8 | rae3_ctsi.reg[0x2c];
    uint16_t delta = cmp - rae3_ctsi_count();

    timer_mod(rae3_ctsi.t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(delta ? delta : 0x10000, NANOSECONDS_PER_SECOND, 32768));
}

static void rae3_ctsi_update(void)
{
    rae3_set_source(33, (rae3_ctsi.reg[0x04] & ~rae3_ctsi.reg[0x0c]) != 0);
}

static void rae3_ctsi_match(void *opaque)
{
    rae3_ctsi.reg[0x00] |= 0x10;
    rae3_ctsi.reg[0x04] |= 0x10;
    rae3_ctsi_update();
}

static uint64_t rae3_ctsi_read(void *opaque, hwaddr a, unsigned size)
{
    switch (a) {
    case 0x20: return rae3_ctsi_count() >> 8;
    case 0x24: return rae3_ctsi_count() & 0xff;
    case 0x1c: return 0;
    }
    return a < sizeof(rae3_ctsi.reg) ? rae3_ctsi.reg[a] : 0;
}

static void rae3_ctsi_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    static unsigned logged;

    if (logged < 64) {
        logged++;
        qemu_log_mask(LOG_UNIMP, "nokia9210: ctsi W %02x %02x pc=%08x\n", (unsigned)a + 0x20, (unsigned)v,
                      rae3_pc());
    }
    switch (a) {
    case 0x00:                                      /* raw status / interrupt status: write 1 clears */
    case 0x04:
        rae3_ctsi.reg[a] &= ~v;
        rae3_ctsi_update();
        return;
    case 0x0c:                                      /* interrupt mask */
        rae3_ctsi.reg[a] = v;
        rae3_ctsi_update();
        return;
    case 0x28: case 0x2c:
        rae3_ctsi.reg[a] = v;
        rae3_ctsi_arm();
        return;
    }
    if (a < sizeof(rae3_ctsi.reg)) {
        rae3_ctsi.reg[a] = v;
    }
}

static const MemoryRegionOps rae3_ctsi_ops = {
    .read = rae3_ctsi_read,
    .write = rae3_ctsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

/* ---- the serial DataFlash (C:) and its controller at 0x0500B000 ------------------------------------- */

static Rae3DataFlash rae3_df;
static int rae3_df_fd = -1;
static QEMUTimer *rae3_df_saver;

static void rae3_df_save(void *opaque)
{
    if (rae3_df_fd >= 0 && rae3_df.dirty) {
        if (pwrite(rae3_df_fd, rae3_df.mem + rae3_df.dirty_lo, rae3_df.dirty_hi - rae3_df.dirty_lo,
                   rae3_df.dirty_lo) < 0) {
            warn_report("nokia9210: cannot write %s", rae3_flash_file);
        }
        rae3_df.dirty = false;
    }
    timer_mod(rae3_df_saver, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
}

static uint64_t rae3_sf_read(void *opaque, hwaddr a, unsigned size)
{
    switch (a) {
    case 0x08: return rae3_df_rx(&rae3_df);
    case 0x0c: return !rae3_df.selected;
    case 0x10: return 0;                            /* transmit never busy */
    case 0x14: return 1;                            /* a byte is always there (or clocked on read) */
    }
    return 0;
}

static void rae3_sf_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    switch (a) {
    case 0x04: rae3_df_tx(&rae3_df, v); break;
    case 0x0c:
        if ((v & 1) && rae3_df.selected) {
            static unsigned logged, reads;

            if (rae3_df.cmd == 0x52 && ++reads % 1024) {
                /* page reads: every 1024th */
            } else if (logged++ < 2000) {
                qemu_log_mask(LOG_UNIMP, "nokia9210: dataflash %02x addr %06x (%u bytes, %u reads) pc=%08x\n",
                              rae3_df.cmd, rae3_df.addr, rae3_df.n, reads, rae3_pc());
            }
        }
        rae3_df_select(&rae3_df, !(v & 1));
        break;
    default: break;                                 /* +0x18 / +0x1C: set-up, kept nowhere */
    }
}

static const MemoryRegionOps rae3_sf_ops = {
    .read = rae3_sf_read,
    .write = rae3_sf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void rae3_dataflash_init(MemoryRegion *sys)
{
    MemoryRegion *m = g_new(MemoryRegion, 1);

    rae3_df.mem = g_malloc(RAE3_DF_SIZE);
    memset(rae3_df.mem, 0xff, RAE3_DF_SIZE);
    rae3_df_reset(&rae3_df);
    if (rae3_flash_file) {
        rae3_df_fd = open(rae3_flash_file, O_RDWR | O_CREAT, 0644);
        if (rae3_df_fd < 0) {
            error_report("nokia9210: cannot open %s", rae3_flash_file);
            exit(1);
        }
        if (pread(rae3_df_fd, rae3_df.mem, RAE3_DF_SIZE, 0) != RAE3_DF_SIZE) {
            memset(rae3_df.mem, 0xff, RAE3_DF_SIZE);       /* new: a blank chip */
            rae3_df_touch(&rae3_df, 0, RAE3_DF_SIZE);
        }
        rae3_df_saver = timer_new_ms(QEMU_CLOCK_REALTIME, rae3_df_save, NULL);
        timer_mod(rae3_df_saver, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
    }
    /*
     * the chip's RDY/BUSY pin = source 51, "SerialFlashReady": a level, high while the chip is ready; the
     * driver enables it only while it waits for a program/erase and its handler masks it again [V intc
     * enables in a run; the kernel never clears pending bits]. Operations finish at once: always high.
     * (Raised at release and lowered at the next select, the format stopped after 63 pages.)
     */
    rae3_set_source(51, true);
    memory_region_init_io(m, NULL, &rae3_sf_ops, NULL, "nokia9210.sflash", 0x20);
    memory_region_add_subregion_overlap(sys, 0x0500b000, m, 10);
}

/*
 * DMA, 4 channels at 0x05009000 + 0x40 * n, sources 5..8 [V kernel 0x5000130C..0x50001518 and accessors
 * 0x50002EA4..0x50003030]: +0x04 enable, +0x08 1 / 2 = start (reads non-zero while running; written 0 and
 * polled to stop), +0x0C write clears the done status, +0x10 done status, +0x14 source, +0x18 source
 * increment (4) or fixed (0), +0x1C source width (8, 0x10, 0x20 bits), +0x20 destination, +0x24 / +0x28 its
 * increment / width, +0x2C the byte count - 1 (16 bytes from a 32-bit source: 0x0F [V run]). Bus addresses are 24-bit: bit 23 = the peripherals
 * (0x05000000 + the rest, e.g. 0x0080B008 = the DataFlash receive register), else RAM (0x03000000 + it).
 * A start moves the data at once (bytes in order, device registers read / written per unit) and raises the
 * channel's source until +0x0C [I: completion timing and status bit].
 */
static uint32_t rae3_dma[4][16];

static hwaddr rae3_dma_pa(uint32_t a)
{
    return (a & 0x800000) ? 0x05000000 + (a & 0x7fffff) : 0x03000000 + (a & 0xffffff);
}

static void rae3_dma_run(int ch)
{
    uint32_t *r = rae3_dma[ch];
    unsigned sw = MAX(1, r[7] / 8), n = (r[11] + 1 + sw - 1) / sw;    /* +0x2C: bytes - 1 [V 0x0F for 16] */
    hwaddr src = rae3_dma_pa(r[5]), dst = rae3_dma_pa(r[8]);
    bool sinc = r[6] != 0, dinc = r[9] != 0;
    uint8_t buf[4];

    for (unsigned i = 0; i < n; i++) {
        address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED, buf, sw);
        if (dinc) {
            address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED, buf, sw);
            dst += sw;
        } else {
            for (unsigned k = 0; k < sw; k++) {         /* to a device register: unit by unit */
                address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED, &buf[k], 1);
            }
        }
        if (sinc) {
            src += sw;
        }
    }
    r[2] = 0;
    r[4] = 1;
    rae3_set_source(5 + ch, true);
}

static uint64_t rae3_dma_read(void *opaque, hwaddr a, unsigned size)
{
    return rae3_dma[(a >> 6) & 3][(a & 0x3f) >> 2];
}

static void rae3_dma_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    int ch = (a >> 6) & 3, r = (a & 0x3f) >> 2;

    if (r == 3) {                                   /* clear done */
        rae3_dma[ch][4] = 0;
        rae3_set_source(5 + ch, false);
        return;
    }
    rae3_dma[ch][r] = v;
    if (r == 2 && v) {
        rae3_dma_run(ch);
    }
}

static const MemoryRegionOps rae3_dma_ops = {
    .read = rae3_dma_read,
    .write = rae3_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * The memory-card (MMC) host at 0x0500A000, as EpBus.dll drives it [V 0x5005DE70, 0x5005EDF8]: a command
 * index written to +0x04 (0x40 | n: CMD0 = 0x40) starts a command; +0x18 status: bit 3 command busy, bit 2
 * data busy, bit 8 response there, bit 6 response timeout (the stack's KMMCErrResponseTimeOut), bits 1 / 9
 * other errors; sources 27-30 (MediaData, MediaDataIdle, MediaResponse, MediaCmdIdle). H-rae3-no-card: no card
 * is modelled: every command ends at once with a response timeout and pulses source 29 (MediaResponse); the
 * host is always idle: MediaDataIdle (28) and MediaCmdIdle (30) are levels, always high (the stack enables
 * them only while it waits, e.g. CmdIdle after its reset write +0x04 = 0x40 [V 0x5005DE74 + 0x5005EF20]).
 * EpBus binds IrqMediaCmdIdle, IrqMediaDataIdle, IrqMediaResponse, IrqMediaLidOpen, IrqMediaLidClosed [V].
 * The other registers keep what is written.
 */
static uint32_t rae3_mmc[0x40];
static QEMUTimer *rae3_mmc_pulse;

static void rae3_mmc_pulse_end(void *opaque)
{
    rae3_set_source(29, false);
}

static unsigned rae3_mmc_logged;

static uint64_t rae3_mmc_read(void *opaque, hwaddr a, unsigned size)
{
    if (rae3_mmc_logged < 200) {
        rae3_mmc_logged++;
        qemu_log_mask(LOG_UNIMP, "nokia9210: mmc R %02x %08x pc=%08x\n", (unsigned)a, rae3_mmc[(a >> 2) & 0x3f],
                      rae3_pc());
    }
    return rae3_mmc[(a >> 2) & 0x3f];
}

static void rae3_mmc_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    if (rae3_mmc_logged < 200) {
        rae3_mmc_logged++;
        qemu_log_mask(LOG_UNIMP, "nokia9210: mmc W %02x %08x pc=%08x\n", (unsigned)a, (unsigned)v, rae3_pc());
    }
    rae3_mmc[(a >> 2) & 0x3f] = v;
    if (a == 0x04 && v) {
        rae3_mmc[0x18 >> 2] = 0x40;                 /* idle, no response: timeout */
        rae3_set_source(29, true);
        timer_mod(rae3_mmc_pulse, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

static const MemoryRegionOps rae3_mmc_ops = {
    .read = rae3_mmc_read,
    .write = rae3_mmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * The system/LCD block at 0x04000000 (VA 0x59000000) and the inner display (docs/research/rae3-display.md):
 * a register file (the bootstrap's memory/clock set-up at 0x00..0x18 [V]; ECust.dll's LCD: +0x20 frame start
 * = 0x03000000, +0x24 its last halfword, +0x40 bit 0 enable, +0x50 / +0x54 timings 640 x 200, +0x58 clock
 * and panel power; nothing is polled [V]). The frame buffer: 640 x 200 at +0x20 from the frame start (a
 * 32-byte header first [I]), 16-bit EColor4K 0000RRRRGGGGBBBB (Wsini.ini COLOR4096 [V]), 1280 bytes a line.
 * The board shows it whatever the enable bit says [I].
 */
#define RAE3_LCD_W 640
#define RAE3_LCD_H 200
static uint32_t rae3_sysreg[0x400];
static QemuConsole *rae3_con;
static uint16_t rae3_shown[RAE3_LCD_H][RAE3_LCD_W];
static bool rae3_lcd_dirty = true;

static uint64_t rae3_sys_read(void *opaque, hwaddr a, unsigned size)
{
    return a == 0x44 ? 0 : rae3_sysreg[(a >> 2) & 0x3ff];
}

static void rae3_sys_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    rae3_sysreg[(a >> 2) & 0x3ff] = v;
    rae3_lcd_dirty = true;
}

static const MemoryRegionOps rae3_sys_ops = {
    .read = rae3_sys_read,
    .write = rae3_sys_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static bool rae3_lcd_update(void *opaque)
{
    static uint16_t fb[RAE3_LCD_H][RAE3_LCD_W];
    uint32_t start = rae3_sysreg[0x20 >> 2] ? rae3_sysreg[0x20 >> 2] : RAE3_RAM_BASE;
    DisplaySurface *surf = qemu_console_surface(rae3_con);
    uint8_t *dst;
    int stride;

    address_space_read(&address_space_memory, start + 0x20, MEMTXATTRS_UNSPECIFIED, fb, sizeof(fb));
    if (!rae3_lcd_dirty && !memcmp(fb, rae3_shown, sizeof(fb))) {
        return true;
    }
    memcpy(rae3_shown, fb, sizeof(fb));
    rae3_lcd_dirty = false;
    if (surface_width(surf) != RAE3_LCD_W || surface_height(surf) != RAE3_LCD_H) {
        qemu_console_resize(rae3_con, RAE3_LCD_W, RAE3_LCD_H);
        surf = qemu_console_surface(rae3_con);
    }
    dst = surface_data(surf);
    stride = surface_stride(surf);
    for (int y = 0; y < RAE3_LCD_H; y++) {
        uint32_t *row = (uint32_t *)(dst + y * stride);

        for (int x = 0; x < RAE3_LCD_W; x++) {
            uint16_t p = le16_to_cpu(rae3_shown[y][x]);
            uint32_t r = (p >> 8) & 15, g = (p >> 4) & 15, b = p & 15;

            row[x] = (r * 17) << 16 | (g * 17) << 8 | (b * 17);
        }
    }
    qemu_console_update_full(rae3_con);
    return true;
}

static void rae3_lcd_invalidate(void *opaque)
{
    rae3_lcd_dirty = true;
}

static const GraphicHwOps rae3_lcd_ops = {
    .gfx_update = rae3_lcd_update,
    .invalidate = rae3_lcd_invalidate,
};

/* ---- the inner keyboard (nokia9210_keys.h, docs/research/rae3-keys.md) -------------------------------- */

#define RAE3_Q(k) Q_KEY_CODE_##k
static const Rae3Key rae3_keys[] = RAE3_KEYS(RAE3_Q);
static uint16_t rae3_kbd_pressed[10];          /* per column: the rows down */
static uint32_t rae3_kbd[0x10];

static void rae3_kbd_update(void)
{
    bool any = false;

    for (int c = 0; c < 10; c++) {
        any |= rae3_kbd_pressed[c] != 0;
    }
    rae3_set_source(39, any);                    /* "KeyPress": a level, any key down [I] */
}

static uint64_t rae3_kbd_read(void *opaque, hwaddr a, unsigned size)
{
    if (a == 0x04) {                             /* rows of the driven columns, active low */
        uint32_t drive = rae3_kbd[0x20 >> 2], rows = 0;

        for (int c = 0; c < 10; c++) {
            if (!(drive & (1u << c))) {
                rows |= rae3_kbd_pressed[c];
            }
        }
        return ~rows & 0x3ff;
    }
    return rae3_kbd[(a >> 2) & 0xf];
}

static void rae3_kbd_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    rae3_kbd[(a >> 2) & 0xf] = v;
}

static const MemoryRegionOps rae3_kbd_ops = {
    .read = rae3_kbd_read,
    .write = rae3_kbd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rae3_key_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    int q;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    q = qemu_input_linux_to_qcode(evt->key.key);       /* this tree passes Linux key codes */
    for (unsigned i = 0; i < ARRAY_SIZE(rae3_keys); i++) {
        if (rae3_keys[i].qcode == q) {
            if (evt->key.down) {
                rae3_kbd_pressed[rae3_keys[i].col] |= 1u << rae3_keys[i].row;
            } else {
                rae3_kbd_pressed[rae3_keys[i].col] &= ~(1u << rae3_keys[i].row);
            }
            rae3_kbd_update();
            return;
        }
    }
}

static const QemuInputHandler rae3_key_handler = {
    .name  = "nokia9210 keyboard",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = rae3_key_event,
};

static void rae3_soc_init(MemoryRegion *sys, ARMCPU *cpu)
{
    MemoryRegion *m;

    rae3_cpu_fiq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ);
    rae3_cpu_irq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ);
    for (int c = 0; c < 2; c++) {
        m = g_new(MemoryRegion, 1);
        memory_region_init_io(m, NULL, &rae3_intc_ops, &rae3_intc[c], "nokia9210.intc", 0x40);
        memory_region_add_subregion(sys, c ? 0x05005000 : 0x05006000, m);
    }
    for (int i = 0; i < 6; i++) {
        rae3_timers[i].hz = i < 5 ? 13000000 : 32768;
        rae3_timers[i].source = i < 5 ? i : 55;     /* "Gpt32k" + "Gpt32kWb" (56), see above */
        rae3_timers[i].t = timer_new_ns(QEMU_CLOCK_VIRTUAL, rae3_timer_expire, &rae3_timers[i]);
    }
    m = g_new(MemoryRegion, 1);
    memory_region_init_io(m, NULL, &rae3_timer_ops, NULL, "nokia9210.timers", 0xa0);
    memory_region_add_subregion(sys, 0x05007000, m);
    m = g_new(MemoryRegion, 1);
    memory_region_init_io(m, NULL, &rae3_dma_ops, NULL, "nokia9210.dma", 0x100);
    memory_region_add_subregion_overlap(sys, 0x05009000, m, 10);
    m = g_new(MemoryRegion, 1);
    memory_region_init_io(m, NULL, &rae3_sys_ops, NULL, "nokia9210.sys-lcd", 0x1000);
    memory_region_add_subregion_overlap(sys, 0x04000000, m, 10);
    rae3_con = qemu_graphic_console_create(NULL, 0, &rae3_lcd_ops, NULL);
    qemu_console_resize(rae3_con, RAE3_LCD_W, RAE3_LCD_H);
    m = g_new(MemoryRegion, 1);
    memory_region_init_io(m, NULL, &rae3_kbd_ops, NULL, "nokia9210.kbd", 0x40);
    memory_region_add_subregion_overlap(sys, 0x05013000, m, 10);
    qemu_input_handler_activate(qemu_input_handler_register(NULL, &rae3_key_handler));
    rae3_dataflash_init(sys);
    rae3_mmc_pulse = timer_new_ns(QEMU_CLOCK_VIRTUAL, rae3_mmc_pulse_end, NULL);
    rae3_set_source(28, true);
    rae3_set_source(30, true);
    /*
     * Lids, as levels whose handlers re-enable the opposite line [V EKeyb (docs/research/rae3-keys.md), EpBus
     * binds both media-lid lines]: the Communicator's lid open (source 10 "LidOpen" high, 9 low; GPIO
     * 0x05003000 bit 9 reads 0 = open), the memory-card door open (11 "MediaLidOpen" high, 12 low: no card).
     * With neither door line high the card stack waited for a door state for ever; with the door closed it
     * powered the (absent) card up, timed out, and the file server still waited for the card drive's
     * reconnect; with the door open the drive is simply not ready and the boot goes on [V runs].
     */
    rae3_set_source(10, true);
    rae3_set_source(11, true);
    m = g_new(MemoryRegion, 1);
    memory_region_init_io(m, NULL, &rae3_mmc_ops, NULL, "nokia9210.mmc", 0x100);
    memory_region_add_subregion_overlap(sys, 0x0500a000, m, 10);
    rae3_ctsi.t = timer_new_ns(QEMU_CLOCK_VIRTUAL, rae3_ctsi_match, NULL);
    m = g_new(MemoryRegion, 1);
    memory_region_init_io(m, NULL, &rae3_ctsi_ops, NULL, "nokia9210.ctsi", 0x30);
    memory_region_add_subregion_overlap(sys, 0x05012020, m, 10);
    m = g_new(MemoryRegion, 1);
    memory_region_init_io(m, NULL, &rae3_timer_ops, (void *)1, "nokia9210.gpt32k", 0x20);
    memory_region_add_subregion(sys, 0x05016000, m);
}

/* ---- the machine -------------------------------------------------------------------------------- */

static void nokia9210_init(MachineState *machine)
{
    MemoryRegion *sys = get_system_memory();
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    g_autofree char *path = NULL;
    g_autofree gchar *rom = NULL;
    gsize len;

    if (!rae3_dir) {
        error_report("nokia9210: give the images with -M nokia9210,ppm=eka1/rae3/core (ROM.bin)");
        exit(1);
    }
    path = g_build_filename(rae3_dir, "ROM.bin", NULL);
    if (!g_file_get_contents(path, &rom, &len, NULL) || len <= RAE3_BANK0_SIZE ||
        len > RAE3_BANK0_SIZE + RAE3_BANK1_SIZE) {
        error_report("nokia9210: cannot read %s (or not an 8..12 MB core ROM)", path);
        exit(1);
    }
    ARMCPU *cpu = ARM_CPU(cpu_create(machine->cpu_type));

    rae3_unknown(sys, 0, 0x100000000ULL, "nokia9210.unknown", -100);
    rae3_flash(sys, RAE3_BANK0, (uint8_t *)rom, RAE3_BANK0_SIZE, RAE3_BANK0_SIZE, 0x8864, "nokia9210.flash0");
    rae3_flash(sys, RAE3_BANK1, (uint8_t *)rom + RAE3_BANK0_SIZE, len - RAE3_BANK0_SIZE, RAE3_BANK1_SIZE,
               0x8864, "nokia9210.flash1");
    /*
     * H-rae3-cs2: the ROM's upper part (offset 0x800000..) is at 0x02000000 + its offset: the kernel's page
     * table maps VA 0x50BCF000 to 0x023CF000 [V gva2gpa], and the flash file's records for ROM 0x9D0100..
     * carry bus address 0x021D0100.. [V]; chip select 1 holds the same bytes so that any split the
     * bootstrap's bank sizing makes still finds ROM offset X at base + X [I]. Chip select 2 is 8 MB: the
     * cellular side's PMM sits at its top (VA 0x5A040000 = 0x023F0000, then 0x02400000..) [V gva2gpa]. (An empty chip select 2 had
     * to read as a floating data bus for the bootstrap's bank test 0x500006F0: the instruction word after
     * the load; with a chip there the test passes.)
     */
    rae3_flash(sys, 0x02000000, (uint8_t *)rom + RAE3_BANK0_SIZE, len - RAE3_BANK0_SIZE, 8 * MiB,
               0x8896, "nokia9210.flash2");
    memory_region_add_subregion(sys, RAE3_RAM_BASE, machine->ram);
    /* the DSP API RAM; the bootstrap runs code from it (MMU off), so it is RAM-like I/O plus the stand-in */
    memory_region_init_io(sram, NULL, &rae3_api_ops, NULL, "nokia9210.dsp-api", RAE3_SRAM_SIZE);
    memory_region_add_subregion(sys, RAE3_SRAM_BASE, sram);
    rae3_soc_init(sys, cpu);
}

static char *rae3_get_ppm(Object *obj, Error **errp)
{
    return g_strdup(rae3_dir ? rae3_dir : "");
}

static void rae3_set_ppm(Object *obj, const char *value, Error **errp)
{
    g_free(rae3_dir);
    rae3_dir = g_strdup(value);
}

static char *rae3_get_flash(Object *obj, Error **errp)
{
    return g_strdup(rae3_flash_file ? rae3_flash_file : "");
}

static void rae3_set_flash(Object *obj, const char *value, Error **errp)
{
    g_free(rae3_flash_file);
    rae3_flash_file = *value ? g_strdup(value) : NULL;
}

static void nokia9210_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "ppm", rae3_get_ppm, rae3_set_ppm);
    object_class_property_add_str(oc, "flash", rae3_get_flash, rae3_set_flash);
    object_class_property_set_description(oc, "flash", "the DataFlash (C:), kept across boots");
    object_class_property_set_description(oc, "ppm", "eka1/rae3/core (ROM.bin)");
    mc->desc = "Nokia 9210 Communicator (RAE-3, Series 80 1st, Symbian 6.0 EKA1) bring-up board";
    mc->init = nokia9210_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("ti925t");
    mc->default_ram_size = 16 * MiB;        /* the bootstrap stacks at 0x03FFFFF0 and sizes RAM by 0x03800000 [V run] */
    mc->default_ram_id = "nokia9210.sdram";
}

static const TypeInfo nokia9210_type = {
    .name = MACHINE_TYPE_NAME("nokia9210"),
    .parent = TYPE_MACHINE,
    .class_init = nokia9210_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokia9210_register(void)
{
    type_register_static(&nokia9210_type);
}

type_init(nokia9210_register)
