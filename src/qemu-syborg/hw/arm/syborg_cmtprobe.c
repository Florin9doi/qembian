/*
 * n95-cmt: a probe machine for the N95 8GB modem (RAP3G, ARM1136) firmware (docs/research/n95-cmt-emulation.md).
 *
 * The first step of running the modem CPU's own software (MCUSW) instead of reply tables: an ARM1136 with
 * the MCUSW image (its 1 KB header + code, linked at 0xf0100000) in ROM, and everything else backed by a
 * "learning" memory that keeps what is written (reads of unwritten bytes give 0) and logs each distinct
 * (pc, address, size, direction) once. What the firmware touches, and where it then spins, tells which
 * hardware it needs. Known regions are declared in a map file instead of being learned:
 *
 *   ram  BASE SIZE [name]       plain RAM (fast, not logged)
 *   rom  BASE FILE [name]       a file in ROM
 *   mirror WINDOW               repeat the previous ram region up to BASE + WINDOW (address aliasing, as an
 *                               SDRAM controller decodes too few address lines: size probes depend on it)
 *   val  ADDR VALUE             reads of ADDR (32 bit) always give VALUE; writes are logged and dropped
 *   orbits ADDR MASK            reads of ADDR (32 bit) give what was written with MASK set (ready/lock bits)
 *   latch ADDR BIT SET CLR MASK  bit BIT of ADDR reads as a latch that a write with MASK to SET turns on and
 *                               to CLR turns off (enable/disable register pairs with a status bit; starts on)
 *   setclr SET CLR              a 32-bit output latch (GenIO-style pin outputs): a write to SET turns on the bits
 *                               written as 1, a write to CLR turns them off; SET reads back the latch
 *   loop ADDR BIT TERM...       board wiring between outputs and an input (the start-up self-tests' loopbacks):
 *                               bit BIT of ADDR (32 bit) reads 1 while every TERM holds, 0 otherwise; several
 *                               lines for one ADDR/BIT are OR-ed. TERM = m:ADDR:MASK[=VALUE] (a memory word, as
 *                               last written or latched) or r:REG:MASK[=VALUE] (Retu register); without =VALUE
 *                               every MASK bit must be set
 *   counter ADDR HZ             reads of ADDR (32 bit) give the virtual time in ticks of HZ (free-running timer)
 *   hwsem BASE SIZE             32-bit hardware semaphores: a read returns 1 and takes it when free, 0 when
 *                               taken; any write frees it (MCUSW: 1 at start-up, 0xffffffff on release)
 *   stream BASE SIZE            every write in the range, in order, to LOG.stream ("offset size value"):
 *                               trace ports (the XTI at 0x401ef000)
 *   quiet BASE SIZE             learning memory, but not logged (stacks, heaps once known)
 *   bootrom ENTRY [ENTRY...]    the on-chip boot ROM, high-level: a Thumb stub at each ENTRY (odd address) hands
 *                               r0-r3 and its own address to C. 0xffff1209 is the security service: r0
 *                               points to a 16-byte service id whose u16 at +0xe is the function (0x14 =
 *                               authenticate image: answered 1 = OK); other entries are logged, answered 0
 *   bootrom-pa ENTRY            the boot ROM's secure-call gate (MCUSW FUN_f049c540, MMU off): an ARM stub at
 *                               ENTRY (even) hands r4 (physical pointer to the 16-byte service id), r5
 *                               (physical pointer to the arguments {a1, a2, a3}) and sp to C, then returns as
 *                               the ROM does: r0 = result, r4 = 1 (done), pop the resume address, bx to it
 *   pflash BASE SIZE ID0 ID1    Intel-command-set NOR (QEMU pflash_cfi01, 16-bit bus, 128 KB blocks) at BASE,
 *                               contents from -drive if=pflash (index 0), device ids ID0/ID1
 *   cbus BASE                   the CBUS controller (serial bus to the power chips: Retu = device 1): +0x04
 *                               enable, +0x08 busy (0), +0x18 read command (dev << 6 | reg | 0x20), +0x1c
 *                               write command (dev << 6 | reg), +0x20 data, +0x2c bit count; each transfer
 *                               logged as "CBUS R/W dev reg value"
 *   cbusreg DEV REG VALUE       a CBUS device register's value at reset
 *   retuirq LINE HZ             Retu interrupts (hw_retu.c): reg 0x01 status (write 1 to clear), reg 0x02 mask (1 = masked);
 *                               intc LINE while status & ~mask; the SIM timer: card power on (reg 0x1c bit 3) with
 *                               reg 0x1d = N starts it, N ticks at HZ later status bit 0x0400 (event 5, "SIM timer
 *                               expired"); reg 0x1c = 0 stops it (docs/research/n95-cmt-sim-interface.md)
 *   retortc                     Retu's real-time clock, running with the virtual clock: reg 0x03 = days << 8 | seconds,
 *                               reg 0x04 = hours << 8 | minutes (writes set it) [I: layout as the Linux retu-rtc driver]
 *   retuadc CH VALUE            Retu's ADC: a read of Retu reg 0x08 gives its channel bits [14:10] (as last
 *                               written) | the 10-bit value of that channel
 *   intc BASE                   the interrupt controller (hw_irq_1.c): +0x04 on, +0x0c enable n, +0x10 disable n
 *                               (0xffffffff: all), +0x20 select n, +0x24 its config, +0x2c its pending flag,
 *                               +0x08 read: the lowest pending enabled n, -1 none (a status read: the idle code
 *                               polls it too; lines are levels, each source clears its own);
 *                               +0x88 lock (nests), +0x8c one level / +0x90 = value (end of critical section / of interrupt): no IRQ
 *                               reaches the CPU while locked; drives the CPU's IRQ line
 *   oneshot BASE HZ IRQ         a count-down timer (the OS timer, hal_timer.c's): +0x28 write N starts counting
 *                               down N ticks at HZ, read gives the count (signed: it goes on below zero); +0x48
 *                               write D adds D (signed) to the count; +0x68 (write pending) reads 0; reaching 0
 *                               raises IRQ (a level, cleared by the next +0x28 write)
 *   ssi CHARDEV|-               the SSI to the APE with a built-in APE peer (docs/research/n95-cmt-ssi-protocol.md):
 *                               SSR 0x20024600 / SST 0x20024700 (control words on channel 0: SST +0x80 out,
 *                               SSR +0x80 in with interrupt 0x3c), the GDMA at 0x20040780 (channel 3 data:
 *                               request 0x42 into the modem, 0x4a out, done interrupt = the DMA channel); the
 *                               peer brings the link up (wake test) and carries ISI messages to/from CHARDEV
 *                               (u16 LE length + PhoNet message, as syborg-modem's link); "-" = log only
 *   ssisend HEX...              the APE peer queues this PhoNet message (hex bytes) for the modem at start (tests
 *                               without a phone; needs an "ssi" line before it)
 *   ssisendat MS HEX...         the same at virtual time MS
 *   dsp                         a fake modem DSP on the MCU-DSP interface (docs/research/n95-cmt-mdi-protocol.md):
 *                               starts on 0x20032d18 = 1, boot handshake on interrupt 0x17, mailbox rings in pdram0
 *                               (0x30003800 MCU->DSP, 0x30003c00 DSP->MCU), doorbell 0x20040284, interrupt clear
 *                               0x20040288 (lines 0x12..0x17 = bits 0..5), DSP->MCU buffers in mdi0 (0x08140000);
 *                               announces PhoNet device 0x7c (media 0x12, resource 0xDA) and logs what it gets;
 *                               "dsp answer" also replies to every request with id + 1 and status 0 [I: experiment
 *                               only; the MCU rejects those replies with f0 01 <id>]
 *   dspreply RES ID RID [HEX...] the fake DSP answers a request RES/ID with message RID (same resource, transaction
 *                               echoed, objects swapped) and data HEX: the DSP's replies the MCU waits for, one line
 *                               per message, each with its evidence in the map (an unlisted request gets no answer)
 *   i2c BASE IRQ                an I2C controller, TI OMAP2 layout (hal_i2c_rap.c): IE +0x04, STAT +0x08 (write 1
 *                               to clear: AL 0, NACK 1, ARDY 2, RRDY 3, XRDY 4), SYSS +0x10 (reset done), CNT +0x18,
 *                               DATA +0x1c (16 bits: two bytes per access, low first; one when one is left
 *                               [V: hal_i2c_rap.c XRDY handler FUN_f01baa1a]), SYSC +0x20, CON +0x24 (STT 0, STP 1, TRX 9, MST
 *                               10, EN 15), SA +0x2c; interrupt IRQ while STAT & IE; REV +0x00 = 0x36
 *   i2cdev BUS ADDR             a register-file slave (first byte written = register, then auto-increment) at ADDR
 *                               on the BUS-th "i2c" line (0 = first); other addresses NACK (logged)
 *   pokev MS VADDR VALUE SIZE   at virtual time MS, write VALUE (SIZE 1/2/4 bytes) at the guest VIRTUAL address VADDR
 *                               through the CPU's current mapping (workarounds: state the firmware would reach
 *                               by a path not emulated yet; each use documented in the map)
 *   sham BASE                   the SHA-1/MD5 hash accelerator (syborg_cmt_sham.h: digest +0x00, byte count +0x14,
 *                               control +0x18, data +0x1c..+0x58); the PRNG seeding hashes with it
 *   dev  TYPE BASE              a QEMU sysbus device (its MMIO region 0) at BASE, e.g. "dev l2x0 0x50000000"
 *
 * -M n95-cmt[,image=FILE],map=FILE,log=FILE[,entry=ADDR][,midr=0x4107b366]
 * image= puts MCUSW alone at 0xf0100000; without it, the map's "rom" lines give the flash (NOLO runs).
 * Exact PCs in the log need -accel tcg,one-insn-per-tb=on (otherwise a chained TB leaves regs[15] stale), but
 * one instruction per TB also defeats the SCTLR prefetch (patch 0011): MCUSW then faults when it enables its MMU.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/block/flash.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "chardev/char-fe.h"
#include "system/address-spaces.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "exec/cpu-common.h"
#include "qom/object.h"
#include "syborg_cmt_sham.h"

#define CMT_IMAGE_BASE  0xf0100000u
#define CMT_ENTRY       0xf01003d0u     /* header +0x148 / +0x1c: 0x3d0 */
#define CMT_MIDR        0x4107b366u     /* ARM1136 r0p6, as the N95's flashing log reports the RAPIDO CPU */
#define LOG_CAP         400000

static char *cmt_image, *cmt_map, *cmt_log;
static uint64_t cmt_entry = CMT_ENTRY, cmt_midr = CMT_MIDR;

typedef struct { uint32_t addr, value; } CmtVal;
typedef struct { uint32_t addr, bit, set, clr, mask; bool on; } CmtLatch;
typedef struct { bool retu; uint32_t a, mask, value; } CmtTerm;
typedef struct { uint32_t addr, bit; unsigned n; CmtTerm t[8]; } CmtLoop;
static uint16_t cbus_regs[8][32];

static struct {
    ARMCPU *cpu;
    FILE *log;
    unsigned lines;
    GHashTable *pages;      /* page number -> uint8_t[4096] */
    GHashTable *seen;       /* (pc, addr, size, dir) key -> 1 */
    GHashTable *vals;       /* addr -> CmtVal* */
    GHashTable *orbits;     /* addr -> CmtVal* (value = mask) */
    GHashTable *counters;   /* addr -> CmtVal* (value = Hz) */
    GArray *latches;        /* CmtLatch */
    GArray *setclr;         /* pairs of (set, clr) addresses */
    GArray *loops;          /* CmtLoop */
    GArray *quiet;          /* pairs of (base, end) */
    GArray *stream;         /* pairs of (base, end) */
    GArray *hwsem;          /* pairs of (base, end) */
    FILE *slog;
} cmt;

static uint32_t cmt_pc(void)
{
    if (!current_cpu) {
        return 0;
    }
    CPUARMState *env = cpu_env(current_cpu);
    return env->regs[15] | (env->thumb ? 1 : 0);
}

static bool cmt_is_quiet(uint64_t a)
{
    for (guint i = 0; i < cmt.quiet->len; i += 2) {
        if (a >= g_array_index(cmt.quiet, uint64_t, i) && a < g_array_index(cmt.quiet, uint64_t, i + 1)) {
            return true;
        }
    }
    return false;
}

static void cmt_note(char dir, uint64_t addr, unsigned size, uint64_t value)
{
    if (!cmt.log || cmt.lines >= LOG_CAP || cmt_is_quiet(addr)) {
        return;
    }
    uint32_t pc = cmt_pc();
    /* key: pc (32) ^ addr (32) ^ size/dir: a 64-bit hash key in a string is simplest and exact */
    char key[48];
    snprintf(key, sizeof key, "%08x:%08" PRIx64 ":%u%c", pc, addr, size, dir);
    if (g_hash_table_contains(cmt.seen, key)) {
        return;
    }
    g_hash_table_add(cmt.seen, g_strdup(key));
    fprintf(cmt.log, "%c pc=%08x a=%08" PRIx64 " s=%u v=%08" PRIx64 "\n", dir, pc, addr, size, value);
    if (++cmt.lines == LOG_CAP) {
        fprintf(cmt.log, "# log cap reached\n");
    }
    fflush(cmt.log);
}

static bool cmt_in(GArray *ranges, uint64_t a)
{
    for (guint i = 0; ranges && i < ranges->len; i += 2) {
        if (a >= g_array_index(ranges, uint64_t, i) && a < g_array_index(ranges, uint64_t, i + 1)) {
            return true;
        }
    }
    return false;
}

static uint8_t *cmt_page(uint64_t addr, bool create)
{
    gpointer k = GUINT_TO_POINTER((guint)(addr >> 12));
    uint8_t *p = g_hash_table_lookup(cmt.pages, k);
    if (!p && create) {
        p = g_malloc0(4096);
        g_hash_table_insert(cmt.pages, k, p);
    }
    return p;
}

static uint64_t wake_read(uint32_t addr, uint64_t v);
static bool wake_write(uint32_t addr, uint32_t v);

static uint32_t cmt_word(uint32_t addr)
{
    uint32_t w = 0;
    uint8_t *p = cmt_page(addr, false);
    if (p) {
        memcpy(&w, p + (addr & 0xffc), 4);
    }
    return w;
}

/* map "loop": the input bits wired to outputs; returns v with those bits of the word at addr recomputed */
static uint64_t cmt_loops(uint32_t addr, uint64_t v)
{
    uint32_t touched = 0, on = 0;
    for (guint i = 0; i < cmt.loops->len; i++) {
        CmtLoop *l = &g_array_index(cmt.loops, CmtLoop, i);
        if (l->addr != (addr & ~3u)) {
            continue;
        }
        bool all = true;
        for (unsigned k = 0; k < l->n && all; k++) {
            CmtTerm *t = &l->t[k];
            uint32_t x = t->retu ? cbus_regs[1][t->a & 31] : cmt_word(t->a);
            all = (x & t->mask) == t->value;
        }
        touched |= l->bit;
        on |= all ? l->bit : 0;
    }
    unsigned sh = (addr & 3) * 8;
    return (v & ~(uint64_t)(touched >> sh)) | (on >> sh);
}

static uint64_t cmt_learn_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t v = 0;
    CmtVal *fv = g_hash_table_lookup(cmt.vals, GUINT_TO_POINTER((guint)(addr & ~3u)));
    if (fv) {
        v = fv->value >> ((addr & 3) * 8);
    } else {
        uint8_t *p = cmt_page(addr, false);
        if (p) {
            unsigned o = addr & 0xfff;
            for (unsigned i = 0; i < size && o + i < 4096; i++) {
                v |= (uint64_t)p[o + i] << (8 * i);
            }
        }
    }
    if (cmt_in(cmt.hwsem, addr)) {        /* a read takes a free semaphore */
        uint8_t *sp = cmt_page(addr, true) + (addr & 0xffc);
        uint32_t cur, zero = 0;
        memcpy(&cur, sp, 4);
        memcpy(sp, &zero, 4);
        v = cur ? 1 : 0;
    }
    CmtVal *ct = g_hash_table_lookup(cmt.counters, GUINT_TO_POINTER((guint)(addr & ~3u)));
    if (ct) {
        v = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), ct->value, NANOSECONDS_PER_SECOND) >> ((addr & 3) * 8);
    }
    for (guint i = 0; i < cmt.latches->len; i++) {
        CmtLatch *l = &g_array_index(cmt.latches, CmtLatch, i);
        if ((addr & ~3u) == l->addr) {
            uint32_t b = l->bit >> ((addr & 3) * 8);
            v = l->on ? (v | b) : (v & ~(uint64_t)b);
        }
    }
    CmtVal *ob = g_hash_table_lookup(cmt.orbits, GUINT_TO_POINTER((guint)(addr & ~3u)));
    if (ob) {
        v |= ob->value >> ((addr & 3) * 8);
    }
    v = cmt_loops(addr, v);
    if (size == 4) {
        v = wake_read(addr, v);
    }
    if (size < 8) {
        v &= (1ull << (size * 8)) - 1;
    }
    cmt_note('R', addr, size, v);
    return v;
}

static void dsp_start(void);
static bool dsp_on;                         /* map "dsp": 0x20032d18 = 1 starts the fake DSP */

static void cmt_learn_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    if (addr == 0x20032d18 && v == 1 && dsp_on) {     /* the DSP is released from reset */
        dsp_start();
    }
    if (addr == 0x20032d14 && cmt.log) {    /* the reset request (hw_reset_generate) */
        fprintf(cmt.log, "RESET %8.3f request %#x (pc %08x)\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                (unsigned)v, cmt_pc());
    }
    for (guint i = 0; cmt.slog && i < cmt.stream->len; i += 2) {
        if (addr >= g_array_index(cmt.stream, uint64_t, i) && addr < g_array_index(cmt.stream, uint64_t, i + 1)) {
            fprintf(cmt.slog, "%08" PRIx64 " %u %08" PRIx64 " %08x\n", addr, size, v, cmt_pc());
            break;
        }
    }
    cmt_note('W', addr, size, v);
    for (guint i = 0; size == 4 && i < cmt.latches->len; i++) {
        CmtLatch *l = &g_array_index(cmt.latches, CmtLatch, i);
        if (addr == l->set && (v & l->mask)) {
            l->on = true;
        } else if (addr == l->clr && (v & l->mask)) {
            l->on = false;
        }
    }
    if (g_hash_table_contains(cmt.vals, GUINT_TO_POINTER((guint)(addr & ~3u)))) {
        return;
    }
    if (size == 4 && wake_write(addr, v)) {
        return;
    }
    for (guint i = 0; size == 4 && i < cmt.setclr->len; i += 2) {
        uint32_t set = g_array_index(cmt.setclr, uint32_t, i), clr = g_array_index(cmt.setclr, uint32_t, i + 1);
        if (addr == set || addr == clr) {
            uint32_t w = cmt_word(set);
            w = addr == set ? (w | (uint32_t)v) : (w & ~(uint32_t)v);
            memcpy(cmt_page(set, true) + (set & 0xffc), &w, 4);
            return;
        }
    }
    if (cmt_in(cmt.hwsem, addr)) {        /* any write frees the semaphore */
        uint32_t one = 1;
        memcpy(cmt_page(addr, true) + (addr & 0xffc), &one, 4);
        return;
    }
    uint8_t *p = cmt_page(addr, true);
    unsigned o = addr & 0xfff;
    for (unsigned i = 0; i < size && o + i < 4096; i++) {
        p[o + i] = v >> (8 * i);
    }
}

static const MemoryRegionOps cmt_learn_ops = {
    .read = cmt_learn_read,
    .write = cmt_learn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

/* ---- interrupt controller ---- */
#define SSI_IRQ_RX_DBG 0x3c
#define INTC_N 256
static struct {
    bool on, locked;
    unsigned lock_depth;            /* +0x88 nesting level (locked = depth > 0) */
    uint32_t lock_pc, unlock_pc;    /* who wrote +0x88 / +0x8c last (diagnostics in the SSI log) */
    uint32_t sel, regs[0x40];
    uint8_t enabled[INTC_N], pending[INTC_N], config[INTC_N];
    qemu_irq cpu_irq;
    bool present;
} intc;

static int intc_next(void)
{
    for (int n = 0; n < INTC_N; n++) {
        if (intc.pending[n] && intc.enabled[n]) {
            return n;
        }
    }
    return -1;
}

static void intc_update(void)
{
    if (intc.present) {
        /* +0x88 locks (the IRQ entry code 0xf049c25c and critical sections FUN_f010dbfc), +0x8c (end of a
           critical section) and +0x90 (end of interrupt) unlock; no interrupt reaches the CPU while locked [V].
           The lock nests [I]: a critical section inside an interrupt handler must not reopen interrupts
           (bug: with a plain flag its +0x8c did, a second interrupt entered the handler and the OS crashed in
           its IRQ dispatch, STR to 0x32 at 0xf0110134, in linked runs after ~10 s).
           Bugs on the way: gating without the +0x90 unlock left the lock set after the first interrupt (0x3c never
           read again); not gating at all let interrupts into the OS's critical sections (the linked run jumped
           to 0xf0c20000 from the IRQ path at ~12 s) */
        qemu_set_irq(intc.cpu_irq, intc.on && !intc.locked && intc_next() >= 0);
    }
}

static void intc_raise(int n)
{
    if (n >= 0 && n < INTC_N) {
        intc.pending[n] = 1;
        intc_update();
    }
}

static uint64_t intc_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case 0x08: {
        return (uint32_t)intc_next();
    }
    case 0x24:
        return intc.config[intc.sel & 0xff];
    case 0x2c:
        return intc.pending[intc.sel & 0xff];
    default:
        return addr < sizeof intc.regs ? intc.regs[addr / 4] : 0;
    }
}

static void intc_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    if (addr < sizeof intc.regs) {
        intc.regs[addr / 4] = v;
    }
    switch (addr) {
    case 0x04:
        intc.on = v & 1;
        break;
    case 0x0c:
        if (v < INTC_N) {
            intc.enabled[v] = 1;
        }
        break;
    case 0x10:
        if (v == 0xffffffff) {
            memset(intc.enabled, 0, sizeof intc.enabled);
        } else if (v < INTC_N) {
            intc.enabled[v] = 0;
        }
        break;
    case 0x20:
        intc.sel = v;
        break;
    case 0x24:
        intc.config[intc.sel & 0xff] = v;
        break;
    case 0x88:      /* lock: the IRQ entry code and every critical section (FUN_f010dbfc) */
        intc.lock_depth++;
        intc.locked = true;
        intc.lock_pc = cmt_pc();
        break;
    case 0x8c:      /* end of a critical section (FUN_f010dbf4): one level */
        if (intc.lock_depth) {
            intc.lock_depth--;
        }
        intc.locked = intc.lock_depth > 0;
        intc.unlock_pc = cmt_pc();
        break;
    case 0x90:      /* end of interrupt (both exits of the IRQ entry code, 0xf049c364 and the scheduler path,
                       write 0): the level written */
        intc.lock_depth = v;
        intc.locked = intc.lock_depth > 0;
        intc.unlock_pc = cmt_pc();
        break;
    }
    intc_update();
}

static void intc_lower(int n)
{
    if (n >= 0 && n < INTC_N) {
        intc.pending[n] = 0;
        intc_update();
    }
}

/* ---- SHA-1/MD5 accelerator (syborg_cmt_sham.h) ---- */
static CmtSham sham;

static uint64_t sham_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    return sham_read(&sham, off & ~3u);
}

static void sham_mmio_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    sham_write(&sham, off & ~3u, v);
}

static const MemoryRegionOps sham_ops = {
    .read = sham_mmio_read,
    .write = sham_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps intc_ops = {
    .read = intc_read,
    .write = intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- one-shot OS timer ---- */
typedef struct {
    QEMUTimer *timer;
    uint32_t hz, irq, regs[0x20];
    int64_t start_ns;
    uint32_t load;
    bool running;
} CmtOneshot;

static int32_t oneshot_left(CmtOneshot *t)
{
    if (!t->running) {
        return 0;
    }
    uint64_t done = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - t->start_ns, t->hz, NANOSECONDS_PER_SECOND);
    return (int32_t)(t->load - (uint32_t)done);     /* goes on below zero, as the hardware's count does */
}

static void oneshot_arm(CmtOneshot *t)
{
    int32_t left = oneshot_left(t);
    if (left > 0) {
        timer_mod(t->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + muldiv64(left, NANOSECONDS_PER_SECOND, t->hz));
    } else {
        timer_del(t->timer);
        intc_raise(t->irq);
    }
}

static void oneshot_expire(void *opaque)
{
    CmtOneshot *t = opaque;
    intc_raise(t->irq);                     /* the count goes on (negative) until reloaded */
}

static uint64_t oneshot_read(void *opaque, hwaddr addr, unsigned size)
{
    CmtOneshot *t = opaque;
    if (addr == 0x28) {
        return (uint32_t)oneshot_left(t);
    }
    if (addr == 0x68) {
        return 0;
    }
    return addr < sizeof t->regs ? t->regs[addr / 4] : 0;
}

static void oneshot_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CmtOneshot *t = opaque;
    if (addr < sizeof t->regs) {
        t->regs[addr / 4] = v;
    }
    if (addr == 0x28) {
        intc_lower(t->irq);                 /* a new count clears the expiry */
        t->load = v;
        t->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        t->running = true;
        oneshot_arm(t);
    } else if (addr == 0x48 && t->running) {    /* adjust the running count */
        t->load += (int32_t)v;
        oneshot_arm(t);
    }
}

static const MemoryRegionOps oneshot_ops = {
    .read = oneshot_read,
    .write = oneshot_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- SSI + GDMA + the APE peer ---- */
#define SSI_SSR      0x20024600u
#define SST_BASE     0x20024700u
#define GDMA_BASE    0x20040780u
#define SSI_IRQ_RX   0x3c
#define SSI_MSGQ     64
#define SSI_MAXMSG   4096

typedef struct { uint32_t len; uint8_t data[SSI_MAXMSG]; } SsiMsg;

static struct {
    bool present, link_up, started;
    uint32_t ssr[0x40], sst[0x40], gdma[0x1200 / 4];
    uint32_t ctrl_q[32], ctrl_head, ctrl_count;     /* control words to the modem (SSR channel 0) */
    /* APE -> CMT: queue of ISI messages; [0] is the one in flight once OPEN_CONN went out */
    SsiMsg *to_cmt[SSI_MSGQ];
    uint32_t to_cmt_head, to_cmt_count;
    bool open_sent, ready;                          /* our OPEN_CONN sent / CONN_READY received */
    uint32_t expect_words;                          /* the modem's OPEN_CONN: words it will send */
    SsiMsg *to_ape[SSI_MSGQ];                       /* CMT -> APE while the chardev is not connected */
    uint32_t to_ape_head, to_ape_count;
    CharFrontend chr;
    bool chr_on, chr_open;
    uint8_t rx[2 + SSI_MAXMSG];
    uint32_t rx_len;
    QEMUTimer *kick, *dma_timer, *activate_timer;
    bool media_active;                              /* the modem answered our media activate (d7 04) */
    int dma_pending_ch;                             /* -1 none */
    uint32_t sent, received;
} ssi;

static void ssi_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* ---- wake lines (docs/research/n95-cmt-ssi-protocol.md "Wake lines") ----
 * CAWake (CMT -> APE): 0x20030800, +0x04 write 1 = high, +0x08 write 1 = low, +0x00 read = level [V].
 * ACWake (APE -> CMT): GenIO bank 0 (0x20031d40) pin 29: level at +0x08, enable mask +0x0c (read/write), set
 * +0x10 / clear +0x14, status +0x20 (write 1 to clear), +0x3c = the lowest pending enabled pin or -1 (the
 * dispatcher FUN_f010e2b8 reads it, with +0x0c narrowed to the pins that have handlers); GenIO pins 0x18..0x1e
 * interrupt on intc line 0xf6 (pin table 0xf04a024c, hw_io_event.c FUN_f010e332) [V]. Only pin 29 is modelled
 * (the others have no source: +0x3c gives -1 for them). Any change of ACWake sets the status bit (edge select
 * not modelled). Bug fixed with this: +0x3c read as learned memory (0), so the dispatcher never saw pin 29.
 * The peer holds ACWake high, except during the cold-boot wake test (after its 0x00000000): there it follows
 * CAWake (CMT drops CAWake -> APE drops ACWake -> CMT raises CAWake -> APE raises ACWake -> CMT 0x10000fff).
 * ST_CMT_APE_WAKEUP_TEST (FUN_f016582c) reports the result (link state 0xd80104d0 must be 3) [V]. */
#define CAWAKE_BASE     0x20030800u
#define GENIO0_BASE     0x20031d40u
#define ACWAKE_BIT      0x20000000u
#define GENIO_WAKE_IRQ  0xf6
static struct {
    bool ca, ac, test;
    uint32_t en, st;
    QEMUTimer *follow;
} wake = { .ca = true, .ac = true };

static void wake_irq_update(void)
{
    if (wake.st & wake.en) {
        intc_raise(GENIO_WAKE_IRQ);
    } else {
        intc_lower(GENIO_WAKE_IRQ);
    }
}

static void wake_set_ac(bool level)
{
    if (wake.ac != level) {
        wake.ac = level;
        wake.st |= ACWAKE_BIT;
        ssi_log("ACWake %s", level ? "high" : "low");
        wake_irq_update();
    }
}

static void wake_follow(void *opaque)
{
    if (wake.test) {
        wake_set_ac(wake.ca);
    }
}

static uint64_t wake_read(uint32_t addr, uint64_t v)
{
    if (!ssi.present) {
        return v;
    }
    if (addr == GENIO0_BASE + 0x08) {
        return (v & ~(uint64_t)ACWAKE_BIT) | (wake.ac ? ACWAKE_BIT : 0);
    }
    if (addr == GENIO0_BASE + 0x20) {
        return wake.st;
    }
    if (addr == GENIO0_BASE + 0x0c) {
        return wake.en;
    }
    if (addr == GENIO0_BASE + 0x3c) {
        return (wake.st & wake.en & ACWAKE_BIT) ? 29 : 0xffffffffu;
    }
    if (addr == CAWAKE_BASE) {
        return wake.ca;
    }
    return v;
}

static bool wake_write(uint32_t addr, uint32_t v)
{
    if (!ssi.present) {
        return false;
    }
    if ((addr == CAWAKE_BASE + 0x04 || addr == CAWAKE_BASE + 0x08) && (v & 1)) {
        bool level = addr == CAWAKE_BASE + 0x04;
        if (wake.ca != level) {
            wake.ca = level;
            ssi_log("CAWake %s", level ? "high" : "low");
            if (wake.test) {                /* the APE answers within microseconds: the modem's wake-test timeout
                                               (FUN_f0109db6(.., 1000)) is about 1 ms [V: a 1 ms answer failed] */
                timer_mod_ns(wake.follow, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50 * SCALE_US);
            }
        }
        return false;
    }
    if (addr == GENIO0_BASE + 0x0c) {
        wake.en = v & ACWAKE_BIT;
    } else if (addr == GENIO0_BASE + 0x10) {
        wake.en |= v & ACWAKE_BIT;
    } else if (addr == GENIO0_BASE + 0x14) {
        wake.en &= ~(v & ACWAKE_BIT);
    } else if (addr == GENIO0_BASE + 0x20) {
        wake.st &= ~v;
        wake_irq_update();
        return true;                        /* the status register is the model's, not memory */
    } else {
        return false;
    }
    wake_irq_update();
    return false;
}

static void ssi_log(const char *fmt, ...)
{
    if (!cmt.log) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(cmt.log, "SSI %8.3f ", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    vfprintf(cmt.log, fmt, ap);
    fputc('\n', cmt.log);
    va_end(ap);
    fflush(cmt.log);
}

static void ssi_log_msg(const char *dir, const uint8_t *m, uint32_t n)
{
    if (!cmt.log) {
        return;
    }
    fprintf(cmt.log, "ISI %s res=%02x tid=%02x id=%02x robj=%02x sobj=%02x len=%u:", dir,
            n > 3 ? m[3] : 0, n > 8 ? m[8] : 0, n > 9 ? m[9] : 0, n > 6 ? m[6] : 0, n > 7 ? m[7] : 0, n);
    for (uint32_t i = 0; i < n && i < 64; i++) {
        fprintf(cmt.log, " %02x", m[i]);
    }
    fputc('\n', cmt.log);
    fflush(cmt.log);
}

/* a control word to the modem: queued in SSR channel 0, interrupt 0x3c */
static void ssi_ctrl_to_cmt(uint32_t w)
{
    if (ssi.ctrl_count == ARRAY_SIZE(ssi.ctrl_q)) {
        ssi_log("ctrl queue full, %08x dropped", w);
        return;
    }
    ssi.ctrl_q[(ssi.ctrl_head + ssi.ctrl_count++) % ARRAY_SIZE(ssi.ctrl_q)] = w;
    ssi_log("ctrl -> CMT %08x (intc 0x3c enabled %d, on %d, locked %d [lock pc %08x, unlock pc %08x]; SSR mode %u)",
            w, intc.enabled[SSI_IRQ_RX], intc.on, intc.locked, intc.lock_pc, intc.unlock_pc, ssi.ssr[1]);
    intc_raise(SSI_IRQ_RX);
}

static uint32_t ssi_open_conn(uint32_t words)
{
    return 0x73000000u | (words & 0xffff) << 8 | ((0x73 ^ (words >> 8) ^ words) & 0xff);
}

/* the next queued APE message: announce it with OPEN_CONN */
static void ssi_try_send(void)
{
    if (!ssi.link_up || ssi.open_sent || !ssi.to_cmt_count) {
        return;
    }
    SsiMsg *m = ssi.to_cmt[ssi.to_cmt_head];
    ssi.open_sent = true;
    ssi.ready = false;
    ssi_ctrl_to_cmt(ssi_open_conn((m->len + 3) / 4));
}

/* the SOS media activate, until the modem answers it (it drops everything before its SOS proxy registers,
   which happens on MTC_STATE_INFO_IND some time after link-up) */
static void ssi_from_ape(const uint8_t *m, uint32_t n);
static void ssi_activate(void *opaque)
{
    static const uint8_t activate[] = { 0x1b, 0x00, 0x6c, 0xd7, 0x06, 0x00, 0x00, 0x00, 0x01, 0x03, 0x1b, 0x00 };
    /* then "media active" (0x09, byte 11 = 3): FUN_f01e5d10 case 3 moves the SOS media from 1 (activating) to 2
       (active) [V]; the modem's own answers can only leave once it is active, so it is sent without waiting */
    static const uint8_t active[] = { 0x1b, 0x00, 0x6c, 0xd7, 0x06, 0x00, 0x00, 0x00, 0x02, 0x09, 0x1b, 0x03 };
    if (ssi.media_active) {
        return;
    }
    ssi_from_ape(activate, sizeof activate);
    ssi_from_ape(active, sizeof active);
    timer_mod(ssi.activate_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

/* a whole ISI message from the modem: to the chardev (or held until it connects) */
static void ssi_to_ape(const uint8_t *m, uint32_t n)
{
    if (n >= 10 && m[3] == 0xd7) {         /* media control answers (04 activate, 0a state): ours */
        ssi.media_active = true;
        ssi_log("SOS media control from the modem: id %02x status %02x", m[9], n > 11 ? m[11] : 0);
        return;                             /* not for the phone */
    }
    ssi.received++;
    ssi_log_msg("CMT->APE", m, n);
    if (ssi.chr_on && ssi.chr_open) {
        uint8_t hdr[2] = { n & 0xff, n >> 8 };
        qemu_chr_fe_write_all(&ssi.chr, hdr, 2);
        qemu_chr_fe_write_all(&ssi.chr, m, n);
    } else if (ssi.chr_on && ssi.to_ape_count < SSI_MSGQ) {
        SsiMsg *h = g_new(SsiMsg, 1);
        h->len = n;
        memcpy(h->data, m, n);
        ssi.to_ape[(ssi.to_ape_head + ssi.to_ape_count++) % SSI_MSGQ] = h;
    }
}

/* a whole ISI message from the APE (chardev): queued for the modem */
static void ssi_from_ape(const uint8_t *m, uint32_t n)
{
    if (n < 10 || n > SSI_MAXMSG || ssi.to_cmt_count == SSI_MSGQ) {
        ssi_log("APE message dropped (%u bytes, queue %u)", n, ssi.to_cmt_count);
        return;
    }
    SsiMsg *q = g_new0(SsiMsg, 1);
    q->len = n;
    memcpy(q->data, m, n);
    /* the media byte: 0x1b = the SOS media in the modem's router (media table 0xd8001714, descriptor 0xf04b99a0;
       the SOS proxy's receive FUN_f01e5f76 sends media-0x1b messages to task 0x2d, into the router's receive path
       where the sender device is learned) [V]; the APE's own link driver fills it, ours leaves 0 */
    q->data[0] = 0x1b;
    ssi.to_cmt[(ssi.to_cmt_head + ssi.to_cmt_count++) % SSI_MSGQ] = q;
    ssi_log_msg("APE->CMT queued", m, n);
    ssi_try_send();
}

/* the modem wrote a control word (SST channel 0) */
static void ssi_ctrl_from_cmt(uint32_t w)
{
    unsigned cmd = w >> 28;
    ssi_log("ctrl <- CMT %08x", w);
    if (w == 0xcaaccaaa) {
        ssi_log("BREAK handshake asked: not modelled (case B)");
    } else if (cmd == 1) {                  /* 0x10000fff / 0x10000ffe: link up */
        ssi.link_up = true;
        ssi_log("link up%s", w == 0x10000fff ? " (wake test passed)" : " (wake test failed or timed out)");
        wake.test = false;
        wake_set_ac(true);                  /* from now on the peer keeps ACWake high */
        /* activate the SOS media, as the APE's link driver does after link-up [I]: router media control
           (resource 0xD7) message 0x03 for media 0x1b; the modem's SOS media task (sos_usb_t.c FUN_f01e5e7a)
           answers 0x04 and only sends on the media once it is active (state 0xd801a568 = 2, FUN_f01e5be4) [V] */
        timer_mod(ssi.activate_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 200);
        ssi_try_send();
    } else if (cmd == 7) {                  /* the modem wants to send */
        uint32_t words = (w >> 8) & 0xffff;
        if ((w & 0xff) != ((0x73 ^ (words >> 8) ^ words) & 0xff) || ((w >> 24) & 0xf) != 3) {
            ssi_log("bad OPEN_CONN: NAK");
            ssi_ctrl_to_cmt(0xc3000000);
            return;
        }
        ssi.expect_words = words;
        ssi_ctrl_to_cmt(0x83000000);        /* CONN_READY: send */
    } else if (cmd == 8) {                  /* CONN_READY for our OPEN_CONN */
        ssi.ready = true;
    } else if (cmd == 0xc) {                /* NAK: offer it again a little later */
        ssi.open_sent = false;
        timer_mod(ssi.kick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }
}

static void ssi_kick(void *opaque)
{
    if (!ssi.started) {                     /* the SSI is set up: start the link (wake test, case A) */
        ssi.started = true;
        wake.test = true;                   /* follow CAWake until the modem reports the link */
        ssi_ctrl_to_cmt(0x00000000);
        return;
    }
    ssi_try_send();
}

/* ---- GDMA: channel c registers at GDMA_BASE + 0x400 c + 0x484.., request line at GDMA_BASE + 0x88 + 4 c */
#define GD(c, off) ssi.gdma[(0x400 * (c) + (off)) / 4]

static void gdma_run(int c)
{
    uint32_t req = ssi.gdma[(0x88 + 4 * c) / 4] & 0xff;
    uint32_t words = GD(c, 0x4ac) + 1;
    ssi_log("DMA ch %d request %#x words %u src %08x dst %08x (intc %d enabled %d)", c, req, words, GD(c, 0x494),
            GD(c, 0x4a0), c, intc.enabled[c]);
    if (req == 0x42) {                      /* APE -> CMT data into SSR channel 3 */
        uint32_t dst = GD(c, 0x4a0);
        if (!ssi.to_cmt_count || !ssi.open_sent) {
            ssi_log("rx DMA %d with nothing to send (%u words)", c, words);
        } else {
            SsiMsg *m = ssi.to_cmt[ssi.to_cmt_head];
            uint8_t buf[SSI_MAXMSG + 4] = { 0 };
            memcpy(buf, m->data, m->len);
            address_space_write(&address_space_memory, dst, MEMTXATTRS_UNSPECIFIED, buf, MIN(words * 4, sizeof buf));
            ssi_log_msg("APE->CMT", m->data, m->len);
            ssi.to_cmt_head = (ssi.to_cmt_head + 1) % SSI_MSGQ;
            ssi.to_cmt_count--;
            g_free(m);
            ssi.open_sent = ssi.ready = false;
            ssi.sent++;
        }
    } else if (req == 0x4a) {               /* CMT -> APE data from SST channel 3 */
        uint32_t src = GD(c, 0x494);
        uint8_t buf[SSI_MAXMSG + 4];
        uint32_t n = MIN(words * 4, sizeof buf);
        address_space_read(&address_space_memory, src, MEMTXATTRS_UNSPECIFIED, buf, n);
        uint32_t len = n >= 6 ? (buf[4] | buf[5] << 8) + 6u : n;
        ssi_to_ape(buf, MIN(len, n));
    } else {
        ssi_log("DMA %d: request %#x not modelled (%u words)", c, req, words);
    }
    GD(c, 0x488) = 0;
    intc_raise(c);                          /* the channel's done interrupt */
    timer_mod(ssi.kick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);    /* the next message, if any */
}

static void gdma_fire(void *opaque)
{
    for (int c = 0; c < 4; c++) {
        if (GD(c, 0x488) == 2) {
            gdma_run(c);
        }
    }
}

static uint64_t gdma_read(void *opaque, hwaddr addr, unsigned size)
{
    return addr < sizeof ssi.gdma ? ssi.gdma[addr / 4] : 0;
}

static void gdma_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    if (addr >= sizeof ssi.gdma) {
        return;
    }
    ssi.gdma[addr / 4] = v;
    for (int c = 0; c < 4; c++) {
        if (addr == 0x400u * c + 0x488 && v == 2) {     /* start: complete a little later (as hardware) */
            timer_mod(ssi.dma_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20000);
        } else if (addr == 0x400u * c + 0x488) {        /* stop (the done handler): clears the done line */
            intc_lower(c);
        }
    }
}

static const MemoryRegionOps gdma_ops = {
    .read = gdma_read,
    .write = gdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t ssr_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == 0x80) {                     /* channel 0: the next control word (the read pops it) */
        if (!ssi.ctrl_count) {
            return 0;
        }
        uint32_t w = ssi.ctrl_q[ssi.ctrl_head];
        ssi_log("CMT read ctrl %08x (pc %08x)", w, cmt_pc());
        ssi.ctrl_head = (ssi.ctrl_head + 1) % ARRAY_SIZE(ssi.ctrl_q);
        if (!--ssi.ctrl_count) {
            intc_lower(SSI_IRQ_RX);         /* channel 0 empty */
        }
        return w;
    }
    if (addr == 0x20 || addr == 0x1c) {     /* no errors, no BREAK */
        return 0;
    }
    return addr < sizeof ssi.ssr ? ssi.ssr[addr / 4] : 0;
}

static void ssr_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    if (addr < sizeof ssi.ssr) {
        ssi.ssr[addr / 4] = v;
    }
}

static uint64_t sst_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == 0x10) {
        return 0;                           /* buffer empty: a control word goes out at once */
    }
    return addr < sizeof ssi.sst ? ssi.sst[addr / 4] : 0;
}

static void sst_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    if (addr < sizeof ssi.sst) {
        ssi.sst[addr / 4] = v;
    }
    if (addr == 0x80) {
        ssi_ctrl_from_cmt(v);
    } else if (addr == 0x2c && v == 4 && !ssi.started) {   /* SSI set up: start the link shortly */
        timer_mod(ssi.kick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 20);
    } else if (addr == 0x20 && (v & 1)) {
        ssi_log("BREAK from the modem");
    }
}

static const MemoryRegionOps ssr_ops = {
    .read = ssr_read, .write = ssr_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps sst_ops = {
    .read = sst_read, .write = sst_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static int ssi_can_receive(void *opaque)
{
    return sizeof(ssi.rx) - ssi.rx_len;
}

static void ssi_receive(void *opaque, const uint8_t *buf, int size)
{
    memcpy(ssi.rx + ssi.rx_len, buf, size);
    ssi.rx_len += size;
    while (ssi.rx_len >= 2) {
        uint32_t n = ssi.rx[0] | ssi.rx[1] << 8;
        if (n > SSI_MAXMSG) {
            ssi.rx_len = 0;
            return;
        }
        if (ssi.rx_len < 2 + n) {
            return;
        }
        ssi_from_ape(ssi.rx + 2, n);
        memmove(ssi.rx, ssi.rx + 2 + n, ssi.rx_len - 2 - n);
        ssi.rx_len -= 2 + n;
    }
}

static void ssi_chr_event(void *opaque, QEMUChrEvent ev)
{
    if (ev == CHR_EVENT_OPENED) {
        ssi.chr_open = true;
        ssi_log("APE connected");
        while (ssi.to_ape_count) {          /* what the modem said before the APE was there */
            SsiMsg *h = ssi.to_ape[ssi.to_ape_head];
            ssi.to_ape_head = (ssi.to_ape_head + 1) % SSI_MSGQ;
            ssi.to_ape_count--;
            uint8_t hdr[2] = { h->len & 0xff, h->len >> 8 };
            qemu_chr_fe_write_all(&ssi.chr, hdr, 2);
            qemu_chr_fe_write_all(&ssi.chr, h->data, h->len);
            g_free(h);
        }
    } else if (ev == CHR_EVENT_CLOSED) {
        ssi.chr_open = false;
        ssi_log("APE disconnected");
    }
}

static void ssi_install(MemoryRegion *sysmem, const char *chr)
{
    MemoryRegion *r = g_new0(MemoryRegion, 3);
    wake.follow = timer_new_ns(QEMU_CLOCK_VIRTUAL, wake_follow, NULL);
    memory_region_init_io(&r[0], NULL, &ssr_ops, NULL, "cmt.ssr", 0x100);
    memory_region_add_subregion(sysmem, SSI_SSR, &r[0]);
    memory_region_init_io(&r[1], NULL, &sst_ops, NULL, "cmt.sst", 0x100);
    memory_region_add_subregion(sysmem, SST_BASE, &r[1]);
    memory_region_init_io(&r[2], NULL, &gdma_ops, NULL, "cmt.gdma", sizeof ssi.gdma);
    memory_region_add_subregion(sysmem, GDMA_BASE, &r[2]);
    ssi.kick = timer_new_ms(QEMU_CLOCK_VIRTUAL, ssi_kick, NULL);
    ssi.dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gdma_fire, NULL);
    ssi.activate_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ssi_activate, NULL);
    ssi.present = true;
    if (strcmp(chr, "-")) {
        Chardev *c = qemu_chr_find(chr);
        if (!c) {
            error_report("n95-cmt: ssi: no chardev '%s' (-chardev ...,id=%s)", chr, chr);
            exit(1);
        }
        qemu_chr_fe_init(&ssi.chr, c, &error_fatal);
        qemu_chr_fe_set_handlers(&ssi.chr, ssi_can_receive, ssi_receive, ssi_chr_event, NULL, NULL, NULL, true);
        ssi.chr_on = true;
    }
}

/* ---- timed APE messages (tests) ---- */
typedef struct { uint8_t m[256]; unsigned n; QEMUTimer *t; } CmtLater;

static void ssi_from_ape(const uint8_t *m, uint32_t n);
static void later_fire(void *opaque)
{
    CmtLater *l = opaque;
    ssi_from_ape(l->m, l->n);
}

/* ---- timed writes to guest virtual memory (workarounds) ---- */
typedef struct { uint64_t vaddr, value; unsigned size; QEMUTimer *t; } CmtPoke;

static void poke_fire(void *opaque)
{
    CmtPoke *p = opaque;
    uint8_t b[4] = { p->value, p->value >> 8, p->value >> 16, p->value >> 24 };
    int r = cpu_memory_rw_debug(CPU(cmt.cpu), p->vaddr, b, p->size, true);
    if (cmt.log) {
        fprintf(cmt.log, "POKE %8.3f %08" PRIx64 " = %" PRIx64 " (%u bytes)%s\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, p->vaddr, p->value, p->size, r ? " FAILED" : "");
        fflush(cmt.log);
    }
}

/* ---- I2C controller (OMAP2 layout) with register-file slaves ---- */
#define I2C_MAX 4
typedef struct {
    uint8_t addr, reg, regs[256];
    bool present, first;
} CmtI2cDev;

typedef struct {
    uint32_t irq, ie, stat, cnt, con, sa, other[0x20];
    uint32_t left;                       /* bytes left in the transfer */
    int dev;                             /* index of the addressed slave, -1 none */
    CmtI2cDev devs[8];
} CmtI2c;

static CmtI2c *i2c_bus[I2C_MAX];
static int i2c_count;

static void i2c_update(CmtI2c *b)
{
    /* completion (AL, NACK, ARDY) interrupts whatever IE says: hal_i2c_rap.c polls XRDY/RRDY in the data phase,
       never writes IE (+0x04), and finishes a transfer in its interrupt handler (FUN_f01ba974: STAT bits ->
       handlers, each acknowledged by writing it back) [V: register log; IE semantics I] */
    if ((b->stat & 0x07) || (b->stat & b->ie & 0x1f)) {
        intc_raise(b->irq);
    } else {
        intc_lower(b->irq);
    }
}

static void i2c_start(CmtI2c *b)
{
    b->dev = -1;
    for (int i = 0; i < 8; i++) {
        if (b->devs[i].present && b->devs[i].addr == (b->sa & 0x7f)) {
            b->dev = i;
        }
    }
    b->left = b->cnt ? b->cnt : 1;
    if (b->dev < 0) {
        if (cmt.log) {
            fprintf(cmt.log, "I2C irq %#x: no device at %#x (NACK)\n", b->irq, b->sa & 0x7f);
        }
        b->stat |= 0x2 | 0x4;            /* NACK + ARDY */
        b->con &= ~3u;
    } else {
        b->devs[b->dev].first = true;
        b->stat |= (b->con & (1 << 9)) ? 0x10 : 0x08;     /* TX: XRDY, RX: RRDY */
    }
    i2c_update(b);
}

/* a byte moved through DATA: the next XRDY/RRDY (or ARDY at the end) follows the driver's acknowledge of this
   one (STAT write), as the driver reads/writes DATA first and acknowledges after [V: register log] */
static void i2c_byte_done(CmtI2c *b)
{
    if (b->left) {
        b->left--;
    }
}

static void i2c_next(CmtI2c *b)
{
    if (b->dev < 0 || !(b->con & 1 << 10)) {
        return;
    }
    if (!b->left) {
        if (!(b->stat & 0x4) && (b->con & 3)) {
            b->stat |= 0x4;              /* ARDY: the transfer is complete */
            b->con &= ~3u;
        }
    } else if (!(b->stat & 0x18)) {
        b->stat |= (b->con & (1 << 9)) ? 0x10 : 0x08;
    }
}

static uint64_t i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    CmtI2c *b = opaque;
    if (cmt.log && getenv("CMT_I2C_LOG") && addr != 0x08) {
        fprintf(cmt.log, "I2C %#x R +%02x (stat %04x)\n", b->irq, (unsigned)addr, b->stat);
    }
    switch (addr) {
    case 0x00: return 0x36;
    case 0x04: return b->ie;
    case 0x08: return b->stat;
    case 0x10: return 1;                 /* reset done */
    case 0x18: return b->cnt;
    case 0x1c: {
        uint32_t v = 0;
        if (b->dev >= 0 && (b->stat & 0x08)) {
            CmtI2cDev *d = &b->devs[b->dev];
            v = d->regs[d->reg++];
            i2c_byte_done(b);
            if (b->left) {               /* the second byte of the 16-bit DATA register */
                v |= (uint32_t)d->regs[d->reg++] << 8;
                i2c_byte_done(b);
            }
        }
        return v;
    }
    case 0x24: return b->con;
    case 0x2c: return b->sa;
    default: return addr < sizeof b->other ? b->other[addr / 4] : 0;
    }
}

static void i2c_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    CmtI2c *b = opaque;
    if (cmt.log && getenv("CMT_I2C_LOG")) {
        fprintf(cmt.log, "I2C %#x W +%02x = %04x (stat %04x)\n", b->irq, (unsigned)addr, (unsigned)v, b->stat);
    }
    switch (addr) {
    case 0x04: b->ie = v; i2c_update(b); break;
    case 0x08:
        b->stat &= ~v;
        if (v & 0x18) {
            i2c_next(b);                 /* XRDY/RRDY acknowledged: the next byte, or ARDY */
        }
        i2c_update(b);
        break;
    case 0x18: b->cnt = v & 0xffff; break;
    case 0x1c:
        if (b->dev >= 0 && (b->stat & 0x10)) {
            CmtI2cDev *d = &b->devs[b->dev];
            for (int k = 0; k < 2 && b->left; k++) {     /* up to two bytes, low first */
                uint8_t byte = v >> (8 * k);
                if (d->first) {
                    d->reg = byte;
                    d->first = false;
                } else {
                    d->regs[d->reg++] = byte;
                }
                i2c_byte_done(b);
            }
        }
        break;
    case 0x24:
        b->con = v;
        if ((v & 1) && (v & (1 << 10))) {      /* STT in master mode */
            i2c_start(b);
        } else if (v & 2) {                     /* STP alone: the stop condition goes out at once */
            b->con &= ~2u;
        }
        break;
    case 0x2c: b->sa = v; break;
    default:
        if (addr < sizeof b->other) {
            b->other[addr / 4] = v;
        }
    }
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read, .write = i2c_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- fake DSP on the MCU-DSP interface (MDI) ---- */
#define MDI_BASE     0x20040280u
#define PDRAM        0x30000000u
#define SDRAM        0x08000000u
#define MDI0_DSP     0x140000u                       /* mdi0 in DSP addresses (SDRAM offsets) */
#define MDI0_SIZE    0x80000u
#define MDI_M2D      (PDRAM + 0x3800)
#define MDI_D2M      (PDRAM + 0x3c00)

static struct {
    bool present, started;
    uint32_t pending, regs[0x20], hs_step, m2d_rd, d2m_wr, alloc;
    QEMUTimer *timer;
    uint32_t raise_later;                             /* bits to raise when the timer fires */
    bool answer;                                      /* map "dsp answer": generic replies to every request */
    GArray *replies;                                  /* map "dspreply": CmtDspReply */
} dsp;

static uint32_t dsp_rd32(uint32_t a)
{
    uint32_t v = 0;
    address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED, &v, 4);
    return le32_to_cpu(v);
}

typedef struct { uint8_t res, id, rid, n; uint8_t data[64]; } CmtDspReply;

static void dsp_wr32(uint32_t a, uint32_t v)
{
    v = cpu_to_le32(v);
    address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED, &v, 4);
}

static void dsp_raise(uint32_t bits)
{
    dsp.pending |= bits;
    for (int n = 0; n < 6; n++) {
        if (bits & (1u << n)) {
            intc_raise(0x12 + n);
        }
    }
}

static void dsp_raise_later(uint32_t bits)
{
    dsp.raise_later |= bits;
    timer_mod(dsp.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);    /* 100 us */
}

static void dsp_log(const char *dir, const uint8_t *m, uint32_t n)
{
    if (!cmt.log) {
        return;
    }
    fprintf(cmt.log, "DSP %8.3f %s len=%u:", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, dir, n);
    for (uint32_t i = 0; i < n && i < 48; i++) {
        fprintf(cmt.log, " %02x", m[i]);
    }
    fputc('\n', cmt.log);
    fflush(cmt.log);
}

static void dsp_d2m_entry(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    uint32_t m = dsp_rd32(MDI_M2D);
    if (((dsp.d2m_wr - (m >> 16)) & 31) >= 16) {
        if (cmt.log) {
            fprintf(cmt.log, "DSP mailbox full: entry %08x dropped\n", w2);
        }
        return;
    }
    uint32_t slot = MDI_D2M + 4 + (dsp.d2m_wr & 15) * 16;
    dsp_wr32(slot, w0);
    dsp_wr32(slot + 4, w1);
    dsp_wr32(slot + 8, w2);
    dsp_wr32(slot + 12, w3);
    dsp.d2m_wr = (dsp.d2m_wr + 1) & 31;
    dsp_wr32(MDI_D2M, dsp.m2d_rd << 16 | dsp.d2m_wr);
    dsp_raise_later(0x02);                             /* intc 0x13: D2M has entries */
}

/* a PhoNet message (big-endian length at [4..5]) from the DSP to the MCU */
static void dsp_send(const uint8_t *b, uint32_t n)
{
    uint32_t sz = (n + 31) & ~31u;
    if (dsp.alloc + sz > MDI0_SIZE) {
        dsp.alloc = 0;                                 /* wrap: buffers are returned long before */
    }
    uint32_t addr = MDI0_DSP + dsp.alloc;
    dsp.alloc += sz;
    address_space_write(&address_space_memory, SDRAM + addr, MEMTXATTRS_UNSPECIFIED, b, n);
    dsp_log("-> MCU", b, n);
    dsp_d2m_entry(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24,
                  b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24, 0x06000000u, addr);
}

static void dsp_drain_m2d(void)
{
    uint32_t wr = dsp_rd32(MDI_M2D) & 0xffff;
    while (dsp.m2d_rd != wr) {
        uint32_t e = MDI_M2D + 4 + (dsp.m2d_rd & 15) * 16;
        uint32_t w0 = dsp_rd32(e), w1 = dsp_rd32(e + 4), w2 = dsp_rd32(e + 8), w3 = dsp_rd32(e + 12);
        uint32_t flags = w2 >> 24;
        if (flags == 0x18) {                           /* the MCU's config entry: the DSP is "up" */
            if (cmt.log) {
                fprintf(cmt.log, "DSP config %08x %08x %08x %08x\n", w0, w1, w2, w3);
            }
            static const uint8_t alive[] = { 0x12, 0x00, 0x7c, 0xda, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
            dsp_send(alive, sizeof alive);
        } else if (flags == 0x06) {                    /* a long PhoNet message for the DSP */
            uint32_t len = ((w1 & 0xff) << 8 | (w1 >> 8 & 0xff)) + 6;
            uint8_t msg[512];
            address_space_read(&address_space_memory, SDRAM + w3, MEMTXATTRS_UNSPECIFIED, msg, MIN(len, sizeof msg));
            dsp_log("<- MCU", msg, MIN(len, sizeof msg));
            dsp_d2m_entry(w3, 0, 0x09020000u, 0);      /* the buffer back (flags 8|bit0, sub 2) */
            for (guint i = 0; len >= 10 && dsp.replies && i < dsp.replies->len; i++) {
                CmtDspReply *rp = &g_array_index(dsp.replies, CmtDspReply, i);
                if (rp->res == msg[3] && rp->id == msg[9]) {
                    uint8_t r[80] = { 0x12, msg[2], 0x7c, msg[3], 0x00, 4 + rp->n, msg[7], msg[6], msg[8], rp->rid };
                    memcpy(r + 10, rp->data, rp->n);
                    dsp_send(r, 10 + rp->n);
                    break;
                }
            }
            if (len >= 10 && dsp.answer && msg[9] != 0xf0) {   /* never answer a common message (f0 01 = the
                                                                   MCU's "not identified" for a bad reply) */
                /* a generic answer [I: the DSP's real replies are not known]: same resource, message id + 1,
                   transaction echoed, objects swapped, one status byte 0 (OK) */
                uint8_t r[12] = { 0x12, msg[2], 0x7c, msg[3], 0x00, 0x06, msg[7], msg[6], msg[8], msg[9] + 1,
                                  0x00, 0x00 };
                dsp_send(r, sizeof r);
            }
        } else {
            if (cmt.log) {
                fprintf(cmt.log, "DSP %8.3f entry flags %#x: %08x %08x %08x %08x\n",
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, flags, w0, w1, w2, w3);
            }
            if (flags == 0x04 && (w0 >> 24) == 0xda) {
                /* an inline message for the DSP's resource 0xDA (its control/alive resource): answer with a
                   0xDA message from device 0x7c, which keeps the MDI watchdog (reset 9, FUN_f0121d2a) quiet [I] */
                static const uint8_t alive[] = { 0x12, 0x00, 0x7c, 0xda, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
                dsp_send(alive, sizeof alive);
            }
        }
        dsp.m2d_rd = (dsp.m2d_rd + 1) & 31;
        dsp_wr32(MDI_D2M, dsp.m2d_rd << 16 | dsp.d2m_wr);
    }
    dsp_raise_later(0x08);                             /* intc 0x15: M2D space */
}

static void dsp_timer(void *opaque)
{
    uint32_t b = dsp.raise_later;
    dsp.raise_later = 0;
    dsp_raise(b);
}

static void dsp_start(void)
{
    dsp.started = true;
    dsp.hs_step = dsp.m2d_rd = dsp.d2m_wr = dsp.alloc = 0;
    if (cmt.log) {
        fprintf(cmt.log, "DSP %8.3f started\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    }
    dsp_raise_later(0x20);                             /* intc 0x17 #1 */
}

static uint64_t mdi_read(void *opaque, hwaddr addr, unsigned size)
{
    return addr < sizeof dsp.regs ? dsp.regs[addr / 4] : 0;
}

static void mdi_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    if (addr < sizeof dsp.regs) {
        dsp.regs[addr / 4] = v;
    }
    if (addr == 0x08) {                                /* interrupt clear */
        dsp.pending &= ~v;
        for (int n = 0; n < 6; n++) {
            if (v & (1u << n)) {
                intc_lower(0x12 + n);
            }
        }
    } else if (addr == 0x04 && dsp.started) {          /* doorbell */
        if ((v & 0x10) && dsp.hs_step < 2) {
            dsp.hs_step++;
            dsp_raise_later(0x20);                     /* intc 0x17 #2, #3 */
        }
        if (v & 0x01) {
            dsp_drain_m2d();
        }
    }
}

static const MemoryRegionOps mdi_ops = {
    .read = mdi_read, .write = mdi_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dsp_install(MemoryRegion *sysmem)
{
    MemoryRegion *io = g_new0(MemoryRegion, 1);
    memory_region_init_io(io, NULL, &mdi_ops, NULL, "cmt.mdi", 0x80);
    memory_region_add_subregion(sysmem, MDI_BASE, io);
    dsp.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_timer, NULL);
    dsp.present = dsp_on = true;
}

/* ---- CBUS controller and the chips behind it (register files) ---- */
static uint16_t retu_adc[32];
static uint32_t cbus_io[0x40];

static uint64_t cbus_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == 0x08) {
        return 0;                       /* never busy: transfers complete at once */
    }
    return addr < sizeof cbus_io ? cbus_io[addr / 4] : 0;
}

/* map "retortc": Retu's RTC, seconds since its zero = virtual seconds + offset */
static struct {
    bool on;
    int64_t offset;                     /* seconds */
} retu_rtc;

static uint64_t retu_rtc_now(void)
{
    return (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / NANOSECONDS_PER_SECOND + retu_rtc.offset);
}

static uint16_t retu_rtc_read(unsigned reg)
{
    uint64_t t = retu_rtc_now();
    if (reg == 0x03) {
        return ((t / 86400) & 0xff) << 8 | (t % 60);
    }
    return ((t / 3600) % 24) << 8 | ((t / 60) % 60);      /* 0x04 */
}

static void retu_rtc_write(unsigned reg, uint16_t v)
{
    uint64_t t = retu_rtc_now(), want;
    if (reg == 0x03) {                  /* days and seconds; hours and minutes kept */
        want = (uint64_t)(v >> 8) * 86400 + (t % 86400) / 60 * 60 + MIN(v & 0xff, 59);
    } else {                            /* hours and minutes; days kept, seconds 0 */
        want = t / 86400 * 86400 + MIN(v >> 8, 23) * 3600 + MIN(v & 0xff, 59) * 60;
    }
    retu_rtc.offset += (int64_t)want - (int64_t)t;
}

/* map "retuirq": Retu's interrupt status/mask and its SIM timer */
static struct {
    bool on;
    int line;
    uint32_t hz;
    QEMUTimer *sim_timer;
} retu_irq;

static void retu_irq_update(void)
{
    if (!retu_irq.on) {
        return;
    }
    if (cbus_regs[1][1] & ~cbus_regs[1][2]) {
        intc_raise(retu_irq.line);
    } else {
        intc_lower(retu_irq.line);
    }
}

static void retu_sim_timer_expire(void *opaque)
{
    cbus_regs[1][1] |= 0x0400;          /* event 5: SIM timer expired (no card answered: no ATR) */
    if (cmt.log) {
        fprintf(cmt.log, "RETU %8.3f SIM timer expired: event 5\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    }
    retu_irq_update();
}

/* a Retu register write (CBUS device 1) after it is stored (reg 0x01, status, is write-1-to-clear: cbus_write):
   interrupt and SIM timer side effects */
static void retu_written(unsigned reg, uint16_t v)
{
    if (!retu_irq.on) {
        return;
    }
    if (reg == 0x1c) {           /* SIM control: power on (bit 3, level shifter) starts the timer */
        if (!v) {
            timer_del(retu_irq.sim_timer);
        } else if ((v & 0x08) && cbus_regs[1][0x1d] && retu_irq.hz) {
            timer_mod_ns(retu_irq.sim_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         muldiv64(cbus_regs[1][0x1d], NANOSECONDS_PER_SECOND, retu_irq.hz));
        }
    }
    retu_irq_update();
}

static void cbus_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    if (addr >= sizeof cbus_io) {
        return;
    }
    cbus_io[addr / 4] = v;
    unsigned dev = (v >> 6) & 7, reg = v & 0x1f;
    if (addr == 0x18) {                 /* read: the value appears in the data register */
        cbus_io[0x20 / 4] = cbus_regs[dev][reg];
        if (dev == 1 && (reg == 0x03 || reg == 0x04) && retu_rtc.on) {
            cbus_io[0x20 / 4] = retu_rtc_read(reg);
        }
        if (dev == 1 && reg == 0x08) {  /* Retu ADC: channel bits | that channel's result */
            unsigned ch = (cbus_regs[1][8] >> 10) & 0x1f;
            cbus_io[0x20 / 4] = (cbus_regs[1][8] & 0x7c00) | (retu_adc[ch] & 0x3ff);
        }
    } else if (addr == 0x1c) {          /* write the data register's value */
        if (dev == 1 && reg == 0x01 && retu_irq.on) {
            cbus_regs[1][1] &= ~cbus_io[0x20 / 4];      /* interrupt status: write 1 to clear */
        } else if (dev == 1 && (reg == 0x03 || reg == 0x04) && retu_rtc.on) {
            retu_rtc_write(reg, cbus_io[0x20 / 4]);
        } else {
            cbus_regs[dev][reg] = cbus_io[0x20 / 4];
        }
        if (dev == 1) {
            retu_written(reg, cbus_io[0x20 / 4]);
        }
        if (dev == 1 && reg == 0x17 && !(cbus_io[0x20 / 4] & 0xffff)) {
            /* Retu watchdog (reg 0x17: the period, kicked with 0x20 every few seconds) set to 0 = expire now: Retu
               cuts the phone's power. MTC does it at the end of MTC_POWER_OFF_REQ [V access log]. The machine
               stops; a linked phone sees its link close and powers off too (syborg-modem) */
            if (cmt.log) {
                fprintf(cmt.log, "Retu watchdog expired (reg 0x17 = 0): power off\n");
                fflush(cmt.log);
            }
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
    } else {
        return;
    }
    if (cmt.log) {
        fprintf(cmt.log, "CBUS %c dev=%u reg=0x%02x v=%04x pc=%08x\n", addr == 0x18 ? 'R' : 'W', dev, reg,
                cbus_io[0x20 / 4] & 0xffff, cmt_pc());
        fflush(cmt.log);
    }
}

static const MemoryRegionOps cbus_ops = {
    .read = cbus_read,
    .write = cbus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- boot ROM security service (high-level) ---- */
#define BOOTROM_IO 0xfffff000u
static uint32_t bootrom_regs[4];

static uint32_t bootrom_entry;

static uint32_t bootrom_call(void)
{
    uint8_t id[16] = { 0 };
    uint32_t r0 = bootrom_regs[0], r1 = bootrom_regs[1], r2 = bootrom_regs[2], r3 = bootrom_regs[3];
    uint32_t ret = 0;
    const char *what = "unknown entry: answered 0";
    unsigned fn = 0;
    if (bootrom_entry == 0xffff1209) {
        if (current_cpu) {
            cpu_memory_rw_debug(current_cpu, r0, id, sizeof id, false);
        }
        fn = id[14] | id[15] << 8;
        if (fn == 0x14) {   /* authenticate the BB5 image whose header is at r2 (NOLO FUN_080a1ce4): 1 = OK */
            ret = 1;
            what = "authenticate image: OK";
        } else {
            what = "unknown function: answered 0";
        }
    }
    if (cmt.log) {
        fprintf(cmt.log, "BOOTROM %08x fn=0x%02x id=", bootrom_entry, fn);
        for (int i = 0; i < 12; i++) {
            fprintf(cmt.log, "%02x", id[i]);
        }
        fprintf(cmt.log, " r0=%08x r1=%08x r2=%08x r3=%08x -> %u (%s)\n", r0, r1, r2, r3, ret, what);
        fflush(cmt.log);
    }
    return ret;
}

/* the secure-call gate: bootrom_regs[0] = r4 (service id, physical), [1] = r5 (args, physical), [2] = sp */
static uint32_t bootrom_pa_call(void)
{
    uint8_t id[16] = { 0 };
    uint32_t a[3] = { 0 }, b[4][4] = { { 0 } };
    if (current_cpu) {
        cpu_memory_rw_debug(current_cpu, bootrom_regs[0], id, sizeof id, false);
        cpu_memory_rw_debug(current_cpu, bootrom_regs[1], a, sizeof a, false);
        for (int i = 0; i < 3; i++) {          /* the words an argument points to, when it is an address */
            if (a[i] >= 0x1000) {
                cpu_memory_rw_debug(current_cpu, a[i], b[i], sizeof b[i], false);
            }
        }
    }
    unsigned fn = id[14] | id[15] << 8;
    uint32_t ret = 0;
    const char *what = "not modelled: answered 0";
    if (fn == 0x14) {   /* authenticate the BB5 image whose header is at a2 (MCUSW: DSP0, hal_mdi.c): 1 = OK */
        ret = 1;
        what = "authenticate image: OK";
    }
    if (cmt.log) {
        fprintf(cmt.log, "PACALL fn=0x%02x id=", fn);
        for (int i = 0; i < 12; i++) {
            fprintf(cmt.log, "%02x", id[i]);
        }
        fprintf(cmt.log, " a1=%08x a2=%08x a3=%08x", a[0], a[1], a[2]);
        for (int i = 0; i < 3; i++) {
            if (a[i] >= 0x1000) {
                fprintf(cmt.log, " [a%d: %08x %08x %08x %08x]", i + 1, b[i][0], b[i][1], b[i][2], b[i][3]);
            }
        }
        fprintf(cmt.log, " sp=%08x -> %u (%s)\n", bootrom_regs[2], ret, what);
        fflush(cmt.log);
    }
    return ret;
}

static uint64_t bootrom_read(void *opaque, hwaddr addr, unsigned size)
{
    return addr == 0x10 ? bootrom_call() : addr == 0x18 ? bootrom_pa_call() : addr < 0x10 ? bootrom_regs[addr / 4] : 0;
}

static void bootrom_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    if (addr < 0x10) {
        bootrom_regs[addr / 4] = v;
    } else if (addr == 0x14) {
        bootrom_entry = v;
    }
}

static const MemoryRegionOps bootrom_ops = {
    .read = bootrom_read,
    .write = bootrom_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint8_t *bootrom_mem;

static void bootrom_region(MemoryRegion *sysmem)
{
    if (!bootrom_mem) {
        MemoryRegion *rom = g_new0(MemoryRegion, 1);
        memory_region_init_rom(rom, NULL, "cmt.bootrom", 0xf000, &error_fatal);
        memory_region_add_subregion(sysmem, 0xffff0000u, rom);
        bootrom_mem = memory_region_get_ram_ptr(rom);
        memset(bootrom_mem, 0, 0xf000);
        MemoryRegion *io = g_new0(MemoryRegion, 1);
        memory_region_init_io(io, NULL, &bootrom_ops, NULL, "cmt.bootrom-io", 0x100);
        memory_region_add_subregion(sysmem, BOOTROM_IO, io);
    }
}

static void bootrom_pa_install(MemoryRegion *sysmem, uint32_t entry)
{
    /*
     * ARM: ldr r12, =IO; str r4, [r12]; str r5, [r12, #4]; str sp, [r12, #8]; ldr r0, =ENTRY;
     * str r0, [r12, #0x14]; ldr r0, [r12, #0x18] (the call); mov r4, #1; ldr r12, [sp], #4; bx r12; .word IO, ENTRY
     */
    const uint32_t stub[12] = { 0xe59fc020, 0xe58c4000, 0xe58c5004, 0xe58cd008, 0xe59f0014, 0xe58c0014,
                                0xe59c0018, 0xe3a04001, 0xe49dc004, 0xe12fff1c, BOOTROM_IO, entry };
    uint32_t off = entry - 0xffff0000u;
    if (entry < 0xffff0000u || off + sizeof stub > 0xf000 || (entry & 3)) {
        error_report("n95-cmt: bootrom-pa entry 0x%x: needs an ARM (4-byte aligned) address in "
                     "0xffff0000..0xfffeffd0", entry);
        exit(1);
    }
    bootrom_region(sysmem);
    memcpy(bootrom_mem + off, stub, sizeof stub);
}

static void bootrom_install(MemoryRegion *sysmem, uint32_t entry)
{
    /*
     * push {r4, r5}; ldr r4, =BOOTROM_IO; ldr r5, =ENTRY; str r5, [r4, #0x14]; str r0-r3, [r4, #0..12];
     * ldr r0, [r4, #0x10]; pop {r4, r5}; bx lr; nop; .word BOOTROM_IO, ENTRY   (32 bytes from ENTRY & ~1)
     */
    uint16_t stub[16] = { 0xb430, 0x4c05, 0x4d05, 0x6165, 0x6020, 0x6061, 0x60a2, 0x60e3, 0x6920, 0xbc30,
                          0x4770, 0x46c0, BOOTROM_IO & 0xffff, BOOTROM_IO >> 16, entry & 0xffff, entry >> 16 };
    uint32_t base = 0xffff0000u, off = (entry & ~1u) - base;
    if (entry < base || off + sizeof stub > 0xf000 || (off & 3) || !(entry & 1)) {
        error_report("n95-cmt: bootrom entry 0x%x: needs a Thumb (odd), 4-byte aligned address in "
                     "0xffff0000..0xfffeffe0", entry);
        exit(1);
    }
    bootrom_region(sysmem);
    memcpy(bootrom_mem + off, stub, sizeof stub);
}

static uint64_t parse_num(const char *s, const char *what, int line)
{
    char *end;
    uint64_t v = g_ascii_strtoull(s, &end, 0);
    if (!*s || *end) {
        error_report("n95-cmt: map line %d: bad %s '%s'", line, what, s);
        exit(1);
    }
    return v;
}

static void cmt_load_map(MemoryRegion *sysmem)
{
    g_autofree char *text = NULL;
    GError *err = NULL;
    if (!cmt_map) {
        return;
    }
    if (!g_file_get_contents(cmt_map, &text, NULL, &err)) {
        error_report("n95-cmt: map %s: %s", cmt_map, err->message);
        exit(1);
    }
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    MemoryRegion *last_ram = NULL;
    uint64_t last_base = 0;
    for (int n = 0; lines[n]; n++) {
        char *hash = strchr(lines[n], '#');
        if (hash) {
            *hash = 0;
        }
        g_auto(GStrv) f = g_strsplit_set(g_strstrip(lines[n]), " \t", -1);
        int c = 0;
        char *w[16] = { 0 };
        for (int i = 0; f[i] && c < 16; i++) {
            if (*f[i]) {
                w[c++] = f[i];
            }
        }
        if (!c) {
            continue;
        }
        if (!strcmp(w[0], "ram") && c >= 3) {
            uint64_t base = parse_num(w[1], "base", n + 1), size = parse_num(w[2], "size", n + 1);
            MemoryRegion *mr = g_new0(MemoryRegion, 1);
            memory_region_init_ram(mr, NULL, w[3] ? w[3] : g_strdup_printf("cmt.ram@%" PRIx64, base),
                                   size, &error_fatal);
            memory_region_add_subregion(sysmem, base, mr);
            last_ram = mr;
            last_base = base;
        } else if (!strcmp(w[0], "mirror") && c >= 2) {
            uint64_t window = parse_num(w[1], "window", n + 1);
            if (!last_ram) {
                error_report("n95-cmt: map line %d: mirror without a ram line before it", n + 1);
                exit(1);
            }
            uint64_t sz = memory_region_size(last_ram);
            for (uint64_t o = sz; o + sz <= window; o += sz) {
                MemoryRegion *al = g_new0(MemoryRegion, 1);
                memory_region_init_alias(al, NULL, "cmt.mirror", last_ram, 0, sz);
                memory_region_add_subregion(sysmem, last_base + o, al);
            }
        } else if (!strcmp(w[0], "rom") && c >= 3) {
            uint64_t base = parse_num(w[1], "base", n + 1);
            int64_t size = get_image_size(w[2], NULL);
            if (size <= 0) {
                error_report("n95-cmt: map line %d: cannot read %s", n + 1, w[2]);
                exit(1);
            }
            MemoryRegion *mr = g_new0(MemoryRegion, 1);
            memory_region_init_rom(mr, NULL, w[3] ? w[3] : g_strdup_printf("cmt.rom@%" PRIx64, base),
                                   ROUND_UP(size, 4096), &error_fatal);
            memory_region_add_subregion(sysmem, base, mr);
            load_image_targphys(w[2], base, size, &error_fatal);
        } else if (!strcmp(w[0], "val") && c >= 3) {
            CmtVal *v = g_new0(CmtVal, 1);
            v->addr = parse_num(w[1], "address", n + 1) & ~3u;
            v->value = parse_num(w[2], "value", n + 1);
            g_hash_table_insert(cmt.vals, GUINT_TO_POINTER(v->addr), v);
        } else if (!strcmp(w[0], "latch") && c >= 6) {
            CmtLatch l = {
                .addr = parse_num(w[1], "address", n + 1) & ~3u, .bit = parse_num(w[2], "bit", n + 1),
                .set = parse_num(w[3], "set address", n + 1), .clr = parse_num(w[4], "clear address", n + 1),
                .mask = parse_num(w[5], "mask", n + 1), .on = true };
            g_array_append_val(cmt.latches, l);
        } else if (!strcmp(w[0], "setclr") && c >= 3) {
            uint32_t sc[2] = { parse_num(w[1], "set address", n + 1) & ~3u,
                               parse_num(w[2], "clear address", n + 1) & ~3u };
            g_array_append_vals(cmt.setclr, sc, 2);
        } else if (!strcmp(w[0], "loop") && c >= 4) {
            CmtLoop l = { .addr = parse_num(w[1], "address", n + 1) & ~3u, .bit = parse_num(w[2], "bit", n + 1) };
            for (int k = 3; k < c; k++) {
                char kind = w[k][0];
                char *f1 = strchr(w[k], ':'), *f2 = f1 ? strchr(f1 + 1, ':') : NULL;
                if ((kind != 'm' && kind != 'r') || !f1 || !f2 || l.n >= ARRAY_SIZE(l.t)) {
                    error_report("n95-cmt: map line %d: bad loop term '%s' (m:ADDR:MASK[=V] or r:REG:MASK[=V])",
                                 n + 1, w[k]);
                    exit(1);
                }
                CmtTerm *t = &l.t[l.n++];
                char *eq = strchr(f2 + 1, '=');
                char *a = g_strndup(f1 + 1, f2 - f1 - 1);
                char *m = eq ? g_strndup(f2 + 1, eq - f2 - 1) : g_strdup(f2 + 1);
                t->retu = kind == 'r';
                t->a = parse_num(a, "loop address", n + 1);
                t->mask = parse_num(m, "loop mask", n + 1);
                t->value = eq ? parse_num(eq + 1, "loop value", n + 1) : t->mask;
                g_free(a);
                g_free(m);
            }
            g_array_append_val(cmt.loops, l);
        } else if (!strcmp(w[0], "counter") && c >= 3) {
            CmtVal *v = g_new0(CmtVal, 1);
            v->addr = parse_num(w[1], "address", n + 1) & ~3u;
            v->value = parse_num(w[2], "Hz", n + 1);
            g_hash_table_insert(cmt.counters, GUINT_TO_POINTER(v->addr), v);
        } else if (!strcmp(w[0], "orbits") && c >= 3) {
            CmtVal *v = g_new0(CmtVal, 1);
            v->addr = parse_num(w[1], "address", n + 1) & ~3u;
            v->value = parse_num(w[2], "mask", n + 1);
            g_hash_table_insert(cmt.orbits, GUINT_TO_POINTER(v->addr), v);
        } else if (!strcmp(w[0], "sham") && c >= 2) {
            MemoryRegion *io = g_new0(MemoryRegion, 1);
            memory_region_init_io(io, NULL, &sham_ops, NULL, "cmt.sham", 0x100);
            memory_region_add_subregion_overlap(sysmem, parse_num(w[1], "base", n + 1), io, 1);
        } else if (!strcmp(w[0], "intc") && c >= 2) {
            MemoryRegion *io = g_new0(MemoryRegion, 1);
            memory_region_init_io(io, NULL, &intc_ops, NULL, "cmt.intc", 0x100);
            memory_region_add_subregion(sysmem, parse_num(w[1], "base", n + 1), io);
            intc.cpu_irq = qdev_get_gpio_in(DEVICE(cmt.cpu), ARM_CPU_IRQ);
            intc.present = true;
        } else if (!strcmp(w[0], "oneshot") && c >= 4) {
            CmtOneshot *t = g_new0(CmtOneshot, 1);
            t->hz = parse_num(w[2], "Hz", n + 1);
            t->irq = parse_num(w[3], "irq", n + 1);
            t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, oneshot_expire, t);
            MemoryRegion *io = g_new0(MemoryRegion, 1);
            memory_region_init_io(io, NULL, &oneshot_ops, t, "cmt.ostimer", 0x80);
            memory_region_add_subregion(sysmem, parse_num(w[1], "base", n + 1), io);
        } else if (!strcmp(w[0], "dspreply") && c >= 4) {
            CmtDspReply rp = { .res = parse_num(w[1], "resource", n + 1), .id = parse_num(w[2], "id", n + 1),
                               .rid = parse_num(w[3], "reply id", n + 1) };
            for (int i = 4; i < c; i++) {
                for (const char *h = w[i]; h[0] && h[1] && rp.n < sizeof rp.data; h += 2) {
                    char b[3] = { h[0], h[1], 0 };
                    rp.data[rp.n++] = strtoul(b, NULL, 16);
                }
            }
            if (!dsp.replies) {
                dsp.replies = g_array_new(false, false, sizeof(CmtDspReply));
            }
            g_array_append_val(dsp.replies, rp);
        } else if (!strcmp(w[0], "ssisendat") && c >= 3) {
            CmtLater *l = g_new0(CmtLater, 1);
            uint64_t ms = parse_num(w[1], "ms", n + 1);
            for (int i = 2; i < c; i++) {
                for (const char *h = w[i]; h[0] && h[1] && l->n < sizeof l->m; h += 2) {
                    char b[3] = { h[0], h[1], 0 };
                    l->m[l->n++] = strtoul(b, NULL, 16);
                }
            }
            l->t = timer_new_ms(QEMU_CLOCK_VIRTUAL, later_fire, l);
            timer_mod(l->t, ms);
        } else if (!strcmp(w[0], "ssisend") && c >= 2) {
            uint8_t m[256];
            unsigned k = 0;
            for (int i = 1; i < c; i++) {
                for (const char *h = w[i]; h[0] && h[1] && k < sizeof m; h += 2) {
                    char b[3] = { h[0], h[1], 0 };
                    m[k++] = strtoul(b, NULL, 16);
                }
            }
            ssi_from_ape(m, k);
        } else if (!strcmp(w[0], "i2c") && c >= 3) {
            if (i2c_count == I2C_MAX) {
                error_report("n95-cmt: map line %d: at most %d i2c buses", n + 1, I2C_MAX);
                exit(1);
            }
            CmtI2c *b = g_new0(CmtI2c, 1);
            b->irq = parse_num(w[2], "irq", n + 1);
            b->dev = -1;
            MemoryRegion *io = g_new0(MemoryRegion, 1);
            memory_region_init_io(io, NULL, &i2c_ops, b, "cmt.i2c", 0x80);
            memory_region_add_subregion(sysmem, parse_num(w[1], "base", n + 1), io);
            i2c_bus[i2c_count++] = b;
        } else if (!strcmp(w[0], "i2cdev") && c >= 3) {
            unsigned bus = parse_num(w[1], "bus", n + 1);
            if (bus >= (unsigned)i2c_count) {
                error_report("n95-cmt: map line %d: no i2c bus %u (declare it first)", n + 1, bus);
                exit(1);
            }
            CmtI2c *b = i2c_bus[bus];
            for (int i = 0; i < 8; i++) {
                if (!b->devs[i].present) {
                    b->devs[i].present = true;
                    b->devs[i].addr = parse_num(w[2], "address", n + 1);
                    break;
                }
            }
        } else if (!strcmp(w[0], "pokev") && c >= 5) {
            CmtPoke *pk = g_new0(CmtPoke, 1);
            uint64_t ms = parse_num(w[1], "ms", n + 1);
            pk->vaddr = parse_num(w[2], "address", n + 1);
            pk->value = parse_num(w[3], "value", n + 1);
            pk->size = parse_num(w[4], "size", n + 1);
            if (pk->size != 1 && pk->size != 2 && pk->size != 4) {
                error_report("n95-cmt: map line %d: pokev size 1, 2 or 4", n + 1);
                exit(1);
            }
            pk->t = timer_new_ms(QEMU_CLOCK_VIRTUAL, poke_fire, pk);
            timer_mod(pk->t, ms);
        } else if (!strcmp(w[0], "dsp")) {
            dsp_install(sysmem);
            dsp.answer = c >= 2 && !strcmp(w[1], "answer");
        } else if (!strcmp(w[0], "ssi") && c >= 2) {
            ssi_install(sysmem, w[1]);
        } else if (!strcmp(w[0], "cbus") && c >= 2) {
            MemoryRegion *io = g_new0(MemoryRegion, 1);
            memory_region_init_io(io, NULL, &cbus_ops, NULL, "cmt.cbus", 0x100);
            memory_region_add_subregion(sysmem, parse_num(w[1], "base", n + 1), io);
        } else if (!strcmp(w[0], "retortc")) {
            retu_rtc.on = true;
        } else if (!strcmp(w[0], "retuirq") && c >= 3) {
            retu_irq.on = true;
            retu_irq.line = parse_num(w[1], "intc line", n + 1);
            retu_irq.hz = parse_num(w[2], "Hz", n + 1);
            retu_irq.sim_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, retu_sim_timer_expire, NULL);
        } else if (!strcmp(w[0], "cbusreg") && c >= 4) {
            unsigned dev = parse_num(w[1], "device", n + 1), reg = parse_num(w[2], "register", n + 1);
            if (dev > 7 || reg > 31) {
                error_report("n95-cmt: map line %d: cbus device 0..7, register 0..31", n + 1);
                exit(1);
            }
            cbus_regs[dev][reg] = parse_num(w[3], "value", n + 1);
        } else if (!strcmp(w[0], "retuadc") && c >= 3) {
            unsigned ch = parse_num(w[1], "channel", n + 1);
            if (ch > 31) {
                error_report("n95-cmt: map line %d: Retu ADC channel 0..31", n + 1);
                exit(1);
            }
            retu_adc[ch] = parse_num(w[2], "value", n + 1);
        } else if (!strcmp(w[0], "bootrom-pa") && c >= 2) {
            bootrom_pa_install(sysmem, parse_num(w[1], "entry", n + 1));
        } else if (!strcmp(w[0], "bootrom") && c >= 2) {
            for (int i = 1; i < c; i++) {
                bootrom_install(sysmem, parse_num(w[i], "entry", n + 1));
            }
        } else if (!strcmp(w[0], "pflash") && c >= 5) {
            uint64_t base = parse_num(w[1], "base", n + 1), size = parse_num(w[2], "size", n + 1);
            DriveInfo *di = drive_get(IF_PFLASH, 0, 0);
            if (!di) {
                error_report("n95-cmt: map line %d: pflash needs -drive if=pflash,format=raw,file=NOR", n + 1);
                exit(1);
            }
            DeviceState *d = qdev_new(TYPE_PFLASH_CFI01);
            qdev_prop_set_drive(d, "drive", blk_by_legacy_dinfo(di));
            qdev_prop_set_uint32(d, "num-blocks", size / (128 * KiB));
            qdev_prop_set_uint64(d, "sector-length", 128 * KiB);
            qdev_prop_set_uint8(d, "width", 2);
            qdev_prop_set_uint8(d, "device-width", 2);   /* word 0 = id0, word 1 = id1 (NOLO reads them so) */
            qdev_prop_set_uint16(d, "id0", parse_num(w[3], "id0", n + 1));
            qdev_prop_set_uint16(d, "id1", parse_num(w[4], "id1", n + 1));
            qdev_prop_set_string(d, "name", "cmt.nor");
            sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, base);
        } else if (!strcmp(w[0], "dev") && c >= 3) {
            DeviceState *d = qdev_new(w[1]);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, parse_num(w[2], "base", n + 1));
        } else if (!strcmp(w[0], "hwsem") && c >= 3) {
            uint64_t base = parse_num(w[1], "base", n + 1), end = base + parse_num(w[2], "size", n + 1);
            g_array_append_val(cmt.hwsem, base);
            g_array_append_val(cmt.hwsem, end);
        } else if (!strcmp(w[0], "stream") && c >= 3) {
            uint64_t base = parse_num(w[1], "base", n + 1), end = base + parse_num(w[2], "size", n + 1);
            g_array_append_val(cmt.stream, base);
            g_array_append_val(cmt.stream, end);
            if (!cmt.slog) {
                g_autofree char *sp = g_strdup_printf("%s.stream", cmt_log ? cmt_log : "n95-cmt");
                cmt.slog = fopen(sp, "w");
            }
        } else if (!strcmp(w[0], "quiet") && c >= 3) {
            uint64_t base = parse_num(w[1], "base", n + 1), end = base + parse_num(w[2], "size", n + 1);
            g_array_append_val(cmt.quiet, base);
            g_array_append_val(cmt.quiet, end);
        } else {
            error_report("n95-cmt: map line %d: unknown '%s'", n + 1, w[0]);
            exit(1);
        }
    }
}

static void cmt_reset(void *opaque)
{
    CPUState *cs = CPU(cmt.cpu);
    cpu_reset(cs);
    cpu_set_pc(cs, cmt_entry & ~1ull);
    cmt.cpu->env.thumb = cmt_entry & 1;
}

static void cmt_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj = object_new(machine->cpu_type);

    object_property_set_uint(cpuobj, "midr", cmt_midr, &error_fatal);
    /* MCUSW turns the MMU on at its physical address and relies on the prefetched "nop; bx lr" (patch 0011) */
    object_property_set_bool(cpuobj, "x-sctlr-prefetch", true, &error_fatal);
    /* TCM region registers and the ETM cp14 interface, read by MCUSW at start-up (patch 0011) */
    object_property_set_bool(cpuobj, "x-arm1136-tcm-etm", true, &error_fatal);
    if (object_property_find(cpuobj, "has_el3")) {
        object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    }
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    cmt.cpu = ARM_CPU(cpuobj);

    cmt.pages = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    cmt.seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    cmt.vals = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    cmt.latches = g_array_new(false, false, sizeof(CmtLatch));
    cmt.setclr = g_array_new(false, false, sizeof(uint32_t));
    cmt.loops = g_array_new(false, false, sizeof(CmtLoop));
    cmt.counters = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    cmt.orbits = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    cmt.quiet = g_array_new(false, false, sizeof(uint64_t));
    cmt.stream = g_array_new(false, false, sizeof(uint64_t));
    cmt.hwsem = g_array_new(false, false, sizeof(uint64_t));
    cmt.log = cmt_log ? fopen(cmt_log, "w") : stderr;
    if (!cmt.log) {
        error_report("n95-cmt: cannot open log %s", cmt_log);
        exit(1);
    }

    /* everything not declared: the learning memory, under every other region */
    MemoryRegion *learn = g_new0(MemoryRegion, 1);
    memory_region_init_io(learn, NULL, &cmt_learn_ops, NULL, "cmt.learn", 1ull << 32);
    memory_region_add_subregion_overlap(sysmem, 0, learn, -1);

    if (cmt_image) {            /* MCUSW alone at its link address (the first probe); NOLO runs use the map's rom */
        int64_t size = get_image_size(cmt_image, NULL);
        if (size <= 0) {
            error_report("n95-cmt: cannot read %s", cmt_image);
            exit(1);
        }
        MemoryRegion *rom = g_new0(MemoryRegion, 1);
        memory_region_init_rom(rom, NULL, "cmt.mcusw", ROUND_UP(size, 64 * KiB), &error_fatal);
        memory_region_add_subregion(sysmem, CMT_IMAGE_BASE, rom);
        load_image_targphys(cmt_image, CMT_IMAGE_BASE, size, &error_fatal);
    }

    cmt_load_map(sysmem);
    qemu_register_reset(cmt_reset, NULL);
}

#define CMT_STR_PROP(name)                                                     \
    static char *get_##name(Object *o, Error **e) { return g_strdup(cmt_##name ? cmt_##name : ""); } \
    static void set_##name(Object *o, const char *v, Error **e) { g_free(cmt_##name); cmt_##name = g_strdup(v); }
CMT_STR_PROP(image)
CMT_STR_PROP(map)
CMT_STR_PROP(log)

static char *get_entry(Object *o, Error **e) { return g_strdup_printf("0x%" PRIx64, cmt_entry); }
static void set_entry(Object *o, const char *v, Error **e) { cmt_entry = g_ascii_strtoull(v, NULL, 0); }
static char *get_midr(Object *o, Error **e) { return g_strdup_printf("0x%" PRIx64, cmt_midr); }
static void set_midr(Object *o, const char *v, Error **e) { cmt_midr = g_ascii_strtoull(v, NULL, 0); }

static void cmt_machine_init(MachineClass *mc)
{
    ObjectClass *oc = OBJECT_CLASS(mc);
    mc->desc = "N95 8GB modem (RAP3G ARM1136) probe: MCUSW at 0xf0100000 over a learning memory";
    mc->init = cmt_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1136-r2");
    object_class_property_add_str(oc, "image", get_image, set_image);
    object_class_property_add_str(oc, "map", get_map, set_map);
    object_class_property_add_str(oc, "log", get_log, set_log);
    object_class_property_add_str(oc, "entry", get_entry, set_entry);
    object_class_property_add_str(oc, "midr", get_midr, set_midr);
}

DEFINE_MACHINE_ARM("n95-cmt", cmt_machine_init)
