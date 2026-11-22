/*
 * Sony Ericsson P910i board (-M p910i): the PNX4000's devices, modelled from what the phone's code does
 * with them (docs/research/p910i-soc-map.md; [V] read from code, [I] inferred).
 *
 * Physical 0x50000000 page (the kernel sees it at virtual 0x58000000), "sys" below:
 *   +0x000 clock generation / power (+0x00 bit 2 = PLL locked, polled by the bootstrap) [V poll]
 *   +0x100 GPIO / pin mux [I]
 *   +0x200 IC0 (IRQ 0..31), +0x300 IC1 (IRQ 32..63; cascaded into IC0 bit 19 = IRQ, bit 20 = FIQ):
 *          +00 enable, +04 write-1-to-clear, +08 pending (& enable), +0C config, +10 edge select (the
 *          kernel writes 6 = HsTimer0/1, the lines it acknowledges at +04; other lines are levels [I]),
 *          +14 FIQ select [V]
 *   +0x400 HsTimer: 32768 Hz up-counter, three match channels = IRQ 1..3 [V]
 *   +0x700 a block written in a slow clock domain: the kernel waits for an edge of +0x10 bit 10 before
 *          each write (EKern 0x50014C54) [V]; bit 10 is modelled as a 32768 Hz square wave [I]
 *   rest   registers kept (read back what was written), first accesses logged
 * Physical 0x58000000: the UART bank, 0x20 per unit, 16550 registers on a 4-byte stride [V]; unit 4
 * (0x58000080) is the kernel's debug port [V] -> the first -serial.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "system/system.h"
#include "system/reset.h"
#include "hw/core/irq.h"
#include "hw/char/serial-mm.h"
#include "qemu/timer.h"
#include "p910i.h"
#include "qemu/error-report.h"
#include "hw/core/cpu.h"

#define SYS_BASE     0x50000000
#define UART_BASE    0x58000000
#define HST_HZ       32768

typedef struct {
    MemoryRegion mr;
    uint32_t reg[0x400];            /* every word of the page, as written */
    uint8_t seen[0x400];            /* accesses logged per word */
    /* interrupt controllers */
    uint32_t en[2], latch[2], level[2], fiqsel[2];
    uint32_t gpio_in;               /* input pins (p910_gpio_set) */
    qemu_irq cpu_irq, cpu_fiq;
    /* HsTimer */
    QEMUTimer *hst;
    int64_t hst_base_ns;            /* virtual time of counter 0 */
    uint32_t hst_last;              /* counter at the last update */
    uint8_t hst_status, hst_ctrl;
    uint16_t hst_ien;
    uint32_t hst_match[3];
} P910Sys;

static P910Sys *p910_sys;

/* ---- interrupt controllers ------------------------------------------------------------------- */

/* GPIO input pins (sys page +0x100) and the interrupt line each one drives */
static const struct { uint32_t bit; int line; } p910_pins[] = {
    { 1u << 1, 42 },                /* jog dial phase A: IrqIOPort1 [I: bit] */
    { 1u << 18, 43 },               /* flip: IrqFlip [V] */
    { 1u << 2, 54 },                /* side key: IrqIOPort2 [V] */
};
static const uint32_t p910_pin_lines[2] = {
    0, (1u << (42 - 32)) | (1u << (43 - 32)) | (1u << (54 - 32)),
};
#define GPIO_INMASK ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 18))

static uint32_t ic_pending(P910Sys *s, int n)
{
    uint32_t edge = s->reg[(0x210 + n * 0x100) >> 2];
    /*
     * Pin lines (GPIO inputs) carry the pin's level; +0x0C is their polarity: 1 = active high,
     * 0 = active low (the jog, side-key and flip drivers flip it after every edge) [I, strong:
     * docs/research/p910i-input.md s. 0]. Device lines are modelled active high.
     */
    uint32_t pol = s->reg[(0x20c + n * 0x100) >> 2];
    uint32_t level = s->level[n] ^ (p910_pin_lines[n] & ~pol);
    uint32_t p = ((s->latch[n] & edge) | (level & ~edge)) & s->en[n];

    if (n == 0) {
        uint32_t p1 = ic_pending(s, 1);

        p &= ~((1u << 19) | (1u << 20));
        if (p1 & ~s->fiqsel[1]) {
            p |= (1u << 19) & s->en[0];
        }
        if (p1 & s->fiqsel[1]) {
            p |= (1u << 20) & s->en[0];
        }
    }
    return p;
}

static void ic_update(P910Sys *s)
{
    uint32_t p = ic_pending(s, 0);

    qemu_set_irq(s->cpu_irq, !!(p & ~s->fiqsel[0]));
    qemu_set_irq(s->cpu_fiq, !!(p & s->fiqsel[0]));
}

/* a device's interrupt line (0..63) */
static void p910_irq_set(void *opaque, int line, int level)
{
    P910Sys *s = opaque;
    int n = line >> 5;
    uint32_t bit = 1u << (line & 31);

    if (level) {
        if (!(s->level[n] & bit)) {
            s->latch[n] |= bit;             /* an edge is latched until cleared at +04 [I] */
        }
        s->level[n] |= bit;
    } else {
        s->level[n] &= ~bit;
    }
    ic_update(s);
}

/* set the input pins in mask to value (p910i_input.c) and drive their interrupt lines */
void p910_gpio_set(uint32_t mask, uint32_t value)
{
    P910Sys *s = p910_sys;

    s->gpio_in = (s->gpio_in & ~mask) | (value & mask);
    for (unsigned i = 0; i < ARRAY_SIZE(p910_pins); i++) {
        p910_irq_set(s, p910_pins[i].line, !!(s->gpio_in & p910_pins[i].bit));
    }
}

qemu_irq p910_irq(int line)
{
    return qemu_allocate_irq(p910_irq_set, p910_sys, line);
}

/* ---- HsTimer -------------------------------------------------------------------------------- */

static uint32_t hst_count(P910Sys *s)
{
    int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->hst_base_ns;

    return (uint32_t)muldiv64(ns, HST_HZ, NANOSECONDS_PER_SECOND);
}

static const uint16_t hst_ien_bit[3] = { 1 << 0, 1 << 3, 1 << 6 };

/* latch the matches passed since the last update, raise IRQ 1..3, arm the QEMU timer for the next */
static void hst_update(P910Sys *s)
{
    uint32_t now = hst_count(s), passed = now - s->hst_last;
    uint32_t next = UINT32_MAX;

    for (int i = 0; i < 3; i++) {
        uint32_t d = s->hst_match[i] - s->hst_last - 1;     /* counts until the match, minus one */

        if (d < passed) {
            s->hst_status |= 1 << i;
        }
        if (s->hst_ien & hst_ien_bit[i]) {
            uint32_t to = s->hst_match[i] - now;
            if (to && to < next) {
                next = to;
            }
        }
    }
    s->hst_last = now;
    for (int i = 0; i < 3; i++) {
        p910_irq_set(s, 1 + i, (s->hst_status & (1 << i)) && (s->hst_ien & hst_ien_bit[i]));
    }
    if (next != UINT32_MAX) {
        int64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->hst_base_ns;
        uint64_t target = muldiv64(ns, HST_HZ, NANOSECONDS_PER_SECOND) + next;
        int64_t at = s->hst_base_ns + muldiv64(target, NANOSECONDS_PER_SECOND, HST_HZ);
        timer_mod(s->hst, at + 1);
    } else {
        timer_del(s->hst);
    }
}

static void hst_tick(void *opaque)
{
    hst_update(opaque);
}

static uint64_t hst_read(P910Sys *s, hwaddr a)
{
    hst_update(s);
    switch (a) {
    case 0x00: return s->hst_status;
    case 0x04: return s->hst_ctrl;
    case 0x08: return hst_count(s);
    case 0x14: return s->hst_ien;
    case 0x18: return s->hst_match[0];
    case 0x1c: return s->hst_match[1];
    case 0x20: return s->hst_match[2];
    }
    return s->reg[(0x400 + a) >> 2];
}

static void hst_write(P910Sys *s, hwaddr a, uint32_t v)
{
    hst_update(s);
    switch (a) {
    case 0x00:
        s->hst_status &= ~v;
        break;
    case 0x04:
        if (v & 2) {                        /* reset the counter [I] */
            s->hst_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->hst_last = 0;
        }
        s->hst_ctrl = v;
        break;
    case 0x14:
        s->hst_ien = v;
        break;
    case 0x18: case 0x1c: case 0x20:
        s->hst_match[(a - 0x18) / 4] = v;
        break;
    }
    hst_update(s);
}

/* ---- the page ------------------------------------------------------------------------------- */

static void sys_log(P910Sys *s, hwaddr a, bool wr, uint64_t v, unsigned size)
{
    if (s->seen[a >> 2] < 8) {
        s->seen[a >> 2]++;
        qemu_log_mask(LOG_UNIMP, "p910i-sys: %s %03" HWADDR_PRIx " %u %08" PRIx64 " pc=%08x\n",
                      wr ? "W" : "R", a, size, v, p910_pc());
    }
}

static uint64_t sys_read32(P910Sys *s, hwaddr a)
{
    if (a >= 0x200 && a < 0x400) {
        int n = (a - 0x200) >> 8;
        switch (a & 0xff) {
        case 0x00: return s->en[n];
        case 0x08: return ic_pending(s, n);
        case 0x14: return s->fiqsel[n];
        }
    } else if (a >= 0x400 && a < 0x500) {
        return hst_read(s, a - 0x400);
    } else if (a == 0x100) {
        return (s->reg[a >> 2] & ~GPIO_INMASK) | (s->gpio_in & GPIO_INMASK);
    } else if (a == 0x000) {
        return s->reg[0] | 4;               /* PLL locked [V: bootstrap 0x900007A4 waits for bit 2] */
    } else if (a == 0x710) {
        /* bit 10: the slow clock the kernel synchronises its writes to [V poll, I meaning] */
        uint32_t c = (uint32_t)muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 2 * HST_HZ,
                                        NANOSECONDS_PER_SECOND);
        return (s->reg[a >> 2] & ~0x400u) | ((c & 1) << 10);
    }
    return s->reg[a >> 2];
}

static void sys_write32(P910Sys *s, hwaddr a, uint32_t v)
{
    s->reg[a >> 2] = v;
    if (a >= 0x200 && a < 0x400) {
        int n = (a - 0x200) >> 8;
        switch (a & 0xff) {
        case 0x00: s->en[n] = v; break;
        case 0x04: s->latch[n] &= ~v; break;
        case 0x14: s->fiqsel[n] = v; break;
        }
        ic_update(s);
    } else if (a >= 0x400 && a < 0x500) {
        hst_write(s, a - 0x400, v);
    }
}

static uint64_t sys_read(void *opaque, hwaddr a, unsigned size)
{
    P910Sys *s = opaque;
    unsigned sh = (a & 3) * 8;
    uint64_t v = (sys_read32(s, a & ~3) >> sh) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);

    if ((a & ~0xffu) != 0x400 && (a & ~0x1ffu) != 0x200 && a != 0x710) {
        sys_log(s, a, false, v, size);
    }
    return v;
}

static void sys_write(void *opaque, hwaddr a, uint64_t val, unsigned size)
{
    P910Sys *s = opaque;
    unsigned sh = (a & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;
    uint32_t w = sys_read32(s, a & ~3);

    /* write-1-to-clear and status registers: only the written bits count */
    if (((a & ~3) & 0xff) == 0x04 && a >= 0x200 && a < 0x400) {
        w = 0;
    }
    if ((a & ~3) == 0x400) {
        w = 0;
    }
    w = (w & ~mask) | (((uint32_t)val << sh) & mask);
    sys_log(s, a, true, val, size);
    sys_write32(s, a & ~3, w);
}

static const MemoryRegionOps sys_ops = {
    .read = sys_read,
    .write = sys_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void sys_reset(void *opaque)
{
    P910Sys *s = opaque;

    memset(s->reg, 0, sizeof(s->reg));
    memset(s->en, 0, sizeof(s->en));
    memset(s->latch, 0, sizeof(s->latch));
    memset(s->fiqsel, 0, sizeof(s->fiqsel));
    s->hst_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->hst_last = 0;
    s->hst_status = s->hst_ctrl = 0;
    s->hst_ien = 0;
    memset(s->hst_match, 0, sizeof(s->hst_match));
    timer_del(s->hst);
    ic_update(s);
}

/*
 * ---- DiskOnChip window (physical 0xB0000000, 8 KB; the kernel's 0x5C000000) ----------------------
 * The DiskOnChip and TrueFFS are not modelled (H-p910-doc): the stand-in in udiskonchip.dll asks this
 * block device for sectors of C: (eka1/p910i/c.img, the factory FAT16 image; with -M p910i,flash=FILE
 * the phone's own copy, created from it on the first boot, every write going through to FILE).
 *   +0x1020  reads 0xC0: DocSupportDrv.ldd faults unless bits 7..6 are set [V]
 * Block device at window + 0x1800 = physical 0xB0001800. udiskonchip runs in EFile's thread in user
 * mode and reaches the window through the user-accessible 32 MB alias at 0x5E000000 [V: TrueFFS writes
 * 0x5E001006]; the 0x5C000000 mapping is kernel-only (a write from user mode aborted):
 *            +00 W command (1 read, 2 write, 3 info), +04 partition (0 only), +08 sector,
 *            +0C count, +10 buffer (virtual: the caller's mapping), +14 R result (0 ok, -1 bad
 *            partition, -2 out of range, -3 buffer not mapped), +18 R size in sectors
 * Everything else reads 0.
 */
#define DOC_BASE  0xB0000000
#define DOC_BLK   (DOC_BASE + 0x1800)

typedef struct {
    MemoryRegion mr, blk;
    uint32_t r[8];
    uint8_t *disk;
    uint32_t sectors;
    unsigned logged;
    int fd;                         /* flash=FILE: writes go through to it, -1 = memory only */
} P910Doc;

static void doc_cmd(P910Doc *d, uint32_t cmd)
{
    uint32_t sec = d->r[2], cnt = d->r[3], va = d->r[4];

    if (d->logged < 200) {
        d->logged++;
        qemu_log_mask(LOG_UNIMP, "p910i-doc: cmd %u part %u sector %u count %u buf %08x\n",
                      cmd, d->r[1], sec, cnt, va);
    }
    if (d->r[1] != 0) {
        d->r[5] = -1;
        return;
    }
    if (cmd == 3) {
        d->r[6] = d->sectors;
        d->r[5] = 0;
        return;
    }
    if (sec > d->sectors || cnt > d->sectors - sec) {
        d->r[5] = -2;
        return;
    }
    if (cmd == 1 || cmd == 2) {
        bool wr = cmd == 2;
        if (cpu_memory_rw_debug(current_cpu, va, d->disk + (uint64_t)sec * 512, cnt * 512, !wr)) {
            d->r[5] = -3;
            return;
        }
        if (wr && d->fd >= 0 &&
            pwrite(d->fd, d->disk + (uint64_t)sec * 512, cnt * 512, (off_t)sec * 512) != cnt * 512) {
            warn_report("p910i: C: write to %s failed", p910_flash_file());
        }
        d->r[5] = 0;
    }
}

static uint64_t doc_read(void *opaque, hwaddr a, unsigned size)
{
    P910Doc *d = opaque;

    if (a == 0x1020) {
        return 0xc0;
    }
    return 0;
}

static uint64_t blk_read(void *opaque, hwaddr a, unsigned size)
{
    P910Doc *d = opaque;

    return a < sizeof(d->r) ? d->r[a >> 2] : 0;
}

static void blk_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    P910Doc *d = opaque;

    if (a < sizeof(d->r)) {
        d->r[a >> 2] = v;
        if (a == 0) {
            doc_cmd(d, v);
        }
    }
}

static const MemoryRegionOps blk_ops = {
    .read = blk_read,
    .write = blk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void doc_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
}

static const MemoryRegionOps doc_ops = {
    .read = doc_read,
    .write = doc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void doc_init(MemoryRegion *sys)
{
    P910Doc *d = g_new0(P910Doc, 1);
    g_autofree char *path = g_build_filename(p910_images(), "c.img", NULL);
    const char *flash = p910_flash_file();
    gchar *buf;
    gsize len;

    /* the phone's own C: when it has one (flash=FILE), else the factory image */
    if (flash && g_file_test(flash, G_FILE_TEST_EXISTS)) {
        g_free(path);
        path = g_strdup(flash);
    }
    if (!g_file_get_contents(path, &buf, &len, NULL) || len % 512) {
        error_report("p910i: cannot load C: from %s", path);
        exit(1);
    }
    d->disk = (uint8_t *)buf;
    d->sectors = len / 512;
    d->fd = -1;
    if (flash) {
        d->fd = open(flash, O_RDWR | O_CREAT, 0644);
        if (d->fd < 0 || (strcmp(path, flash) && pwrite(d->fd, buf, len, 0) != (ssize_t)len)) {
            error_report("p910i: cannot write C: to %s", flash);
            exit(1);
        }
        info_report("p910i: C: kept in %s", flash);
    }
    memory_region_init_io(&d->mr, NULL, &doc_ops, d, "p910i.doc", 0x2000);
    memory_region_add_subregion_overlap(sys, DOC_BASE, &d->mr, 10);
    memory_region_init_io(&d->blk, NULL, &blk_ops, d, "p910i.blk", 0x20);
    memory_region_add_subregion_overlap(sys, DOC_BLK, &d->blk, 20);
}

void p910_board_init(MachineState *machine, MemoryRegion *sys, ARMCPU *cpu)
{
    P910Sys *s = g_new0(P910Sys, 1);

    p910_sys = s;
    s->cpu_irq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ);
    s->cpu_fiq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ);
    s->hst = timer_new_ns(QEMU_CLOCK_VIRTUAL, hst_tick, s);
    memory_region_init_io(&s->mr, NULL, &sys_ops, s, "p910i.sys", 0x1000);
    memory_region_add_subregion_overlap(sys, SYS_BASE, &s->mr, 10);
    qemu_register_reset(sys_reset, s);
    if (p910_phone->doc) {
        doc_init(sys);
    }
    p910_gsm_init(sys);
    p910_lcd_init(sys);
    p910_i2c_init(sys);
    p910_input_init(machine, sys);

    /* UART unit 4: the kernel's debug port (IrqUart5 = 9 [I: unit n -> IRQ 5 + n]) */
    serial_mm_init(sys, UART_BASE + 4 * 0x20, 2, p910_irq(9), 115200, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);
    /*
     * The other units (unit 2 is the GSM link, p910i_gsm.c) as plain 16550s with nothing attached
     * unless a -serial is given (units 0, 1 -> the 2nd and 3rd -serial): the comms server polls unit 1's
     * LSR for "transmitter empty" (Euart1.pdd 0x500B84CC) and spun for ever on the catch-all [V by run].
     */
    serial_mm_init(sys, UART_BASE + 0 * 0x20, 2, p910_irq(5), 115200, serial_hd(1), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(sys, UART_BASE + 1 * 0x20, 2, p910_irq(6), 115200, serial_hd(2), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(sys, UART_BASE + 3 * 0x20, 2, p910_irq(8), 115200, NULL, DEVICE_LITTLE_ENDIAN);
    serial_mm_init(sys, UART_BASE + 5 * 0x20, 2, p910_irq(10), 115200, NULL, DEVICE_LITTLE_ENDIAN);
}
