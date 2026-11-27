/*
 * Nokia N-Gage (NEM-4) and N-Gage QD (RH-29), DCT4 "UPP_WD2", Symbian 6.1 / EKA1, bring-up board.
 *
 * Runs the phone's own ROM (bootstrap, ekern.exe, ecust.dll): see docs/eka1-ngage.md.
 * This is the discovery stage (roadmap P1): only what the ROM is known to need is modelled,
 * every other physical access goes to a logging catch-all that remembers what was written
 * (reads return the last value, else 0), so polling loops and probes show up in the log:
 *   qemu-system-arm -M ngageqd -bios ROM.bin -d unimp,guest_errors -D ngage.log
 *
 * The memory map and each modelled device answer a question in docs/eka1-ngage-questions.md:
 *   0x00000000  flash chip 0 (Intel CFI, x16), ROM.bin offsets 0..0xEFFFFF     Q1 Q4 Q12
 *   0x02000000  flash chip 1 (Intel CFI, x16), ROM.bin from offset 0xF00000    Q2 Q12
 *   0x0A000000  SDRAM, 16 MB                                                   Q3
 *   0x0C120100  interrupt controller, 4 banks x 32                              Q16
 *   0x0C120A00  timers 0-5 (0x20 apart), interrupt sources 0-5                 Q13 Q31
 *   0x0C120300  DMA engine (7 channels), 0x0C120600 MMC host (ngage_mmc.c, -M ngage,mmc=CARD.img)
 *   0x0C1F0000  NOS stand-in device (board-only, H-nos-standin)                 Q20 Q21
 *   0x0DE00000  kernel debug output (trace port), to serial 0                 Q18
 *   0x0C150420  CBus master to the external chips (device 1: power ASIC)       Q11
 *               device 5: the Bluetooth module (LPRF, ngage_lprf.c), its interrupt CTSI 2 bit 4
 *   0x0C121000  CCP camera receiver (-M nokia7650: ngage_cam.c test pattern, interrupt 86; the sensor's I2C on
 *               GPIO 0x0C15020C/0x0C150210 bits 7/8, frames by DMA from 0x0C150554)
 *   0x0C000000  UPP peripherals and internal SRAM (0x0C004000), virtual 0x58000000: catch-all
 *
 * Machines: -M ngage (N-Gage, NEM-4), -M ngageqd (N-Gage QD, RH-29) and -M nokia6600 (Nokia 6600, NHL-10, Symbian 7.0s,
 * docs/devices/nokia-6600-nhl10/bring-up.md). Both are the same DCT4 UPP SoC; what differs per
 * phone (flash chips, ROM split, NOS entry, kernel addresses the workarounds use) is an
 * NgageModel below. Comments name N-Gage addresses unless they say otherwise.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/block/flash.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qemu/datadir.h"
#include "system/address-spaces.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "ngage_mmc.h"
#include "ngage_keys.h"
#include "ngage_lprf.h"
#include "ngage_cam.h"
#include "ngage_sir.h"
#include "accel/tcg/cpu-loop.h"
#include "tcg/insn-start-words.h"
#include "exec/target_page.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "system/system.h"
#include "system/rtc.h"
#include "qemu/cutils.h"
#include "ui/console.h"
#include "exec/cpu-common.h"
#include "qemu/host-utils.h"

/* Answers behind each value: docs/eka1-ngage-questions.md */
#define NGAGE_FLASH_PARTITION 0x00200000  /* read-while-write partition, 16 Mbit (Q48) [I] */
#define NGAGE_FLASH0_BASE   0x00000000      /* boot flash, holds ROM.bin (Q1, Q4) */
#define NGAGE_FLASH1_BASE   0x02000000      /* second chip (Q2); chip n is at n << 25 (Q4) */
#define NGAGE_FLASH_SIZE    (32 * MiB)      /* [I] the chip stride (Q5) */
#define NGAGE_FLASH_SECTOR  (64 * KiB)      /* [V] medint.pdd steps blocks by 0x10000 (Q27);
                                                the top 8 KB parameter blocks are not modelled */
#define NGAGE_FLASH_MANUF   0x89            /* Intel; accepted by 0x599c and 0x97c (Q2, Q4) */
#define NGAGE_FLASH_DEVICE  0x8874          /* bootstrap type 8 (Q5) */
#define NGAGE_SDRAM_BASE    0x0a000000      /* Q3 */
#define NGAGE_MAX_CHIPS     4               /* chip n at n << 25 */

/*
 * What differs between the phones on this board. ROM.bin (virtual 0x50000000) is split over
 * chips by rom_map; ppm segments below the first range's end go into the ROM image (boot block),
 * the others into the chip their flash address names.
 */
typedef struct {
    uint32_t rom_off, len;          /* ROM.bin range ... */
    unsigned chip;                  /* ... at the start of this chip */
} NgageRomRange;

/* the camera a phone's driver expects (docs/devices/nokia-7650-nhl2/camera.md) */
typedef enum NgageCamKind {
    NGAGE_CAM_NONE,                 /* no camera modelled */
    NGAGE_CAM_NHL2,                 /* 7650, 6600: camerapdd / campdd, sensor 0x3C with 8-bit registers, YUV stills */
    NGAGE_CAM_NHL8,                 /* 3650/3660: the sensor at 0x04 with 16-bit registers, JPEG stills */
    NGAGE_CAM_RM25,                 /* 6260: the 6600's driver; the phone turns every frame 90 degrees for display
                                       (ngage_cam.h rot90) */
    NGAGE_CAM_CAMX,                 /* 7610/6670: camxpdd's frames (header, frame start, JPEG stills: ngage_cam.h
                                       camx) */
    NGAGE_CAM_RM51,                 /* 3230: the camera module at 0x30 behind a command port (ngage_cam.h cmdport) */
} NgageCamKind;

typedef struct {
    const char *name;
    unsigned nchips;                /* chips 0..n-1 answer an ID query (the bootstrap halts otherwise) */
    NgageRomRange rom_map[3];
    unsigned nrom_map;
    uint32_t nos_alias;             /* virtual address of physical flash 0 (the NOS's view, Q19) */
    const uint8_t *stub;            /* NOS stand-in stub, copied at stub_va (H-nos-standin) */
    size_t stub_len;
    uint32_t stub_va;
    const uint32_t (*nos_exports)[2];   /* {NOS address, stub symbol}: jumps into the stub */
    unsigned nnos_exports;
    uint32_t screen_chunk_ptr;      /* H-ngage-fb: kernel word holding the screen chunk (0 = off) */
    bool screen_rgb565;             /* screen chunk pixels: RGB565 (EColor64K), else 0RGB 4444 */
    uint32_t nos_rx_dfc, nos_rx_flag;   /* the stub's ISI delivery DFC and flag (0 = none) */
    uint32_t nos_item14;            /* product data item 14 (the product code) when not 0: the 3660's product_info */
    uint32_t trace_mask_or;         /* bits set in the ROM header's trace mask (+0xD8) */
    uint16_t flash_manuf, flash_device;     /* the ID every chip answers (CFI01: Intel commands) */
    uint16_t flash_device1;                 /* chips 1.. if they differ (0 = flash_device) */
    bool flash_amd;                 /* AMD command set (QEMU's CFI02, unlock 0x555/0x2AA x16): the 3230's
                                       bootstrap and medint.pdd accept only AMD/Samsung parts [V] */
    bool uem_rtc;                   /* the UEM's RTC kept like a phone's: calibration set, so rtc.dll
                                       finds a valid clock (H-nhl10-rtc) */
    uint32_t md5_entries[2];        /* ROM offsets of the bootstrap's digest entries (H-ngage-md5);
                                       0 = the N-Gage's 0x8F00 / 0x8F18 */
    NgageCamKind camera;            /* the camera (ngage_cam.c) and its driver family; NGAGE_CAM_NONE = not modelled */
    bool slide;                     /* the 7650's keypad slide (the camera lens is behind it) */
    bool irda;                      /* the SIR UART behind euart1.pdd ("SIR.PDD", ngage_sir.c) on the 2nd -serial */
} NgageModel;

static const NgageModel *ngm;

/* Guest PC of the instruction doing the current MMIO access (0 if unknown), for the logs. */
static uint32_t ngage_pc(void)
{
    uint64_t data[INSN_START_WORDS] = { 0 };

    if (current_cpu && current_cpu->mem_io_pc &&
        cpu_unwind_state_data(current_cpu, current_cpu->mem_io_pc, data)) {
        /*
         * ARM TBs are PC-relative (CF_PCREL): the unwind data holds the page offset, and
         * regs[15] still holds the TB's page. Log-only: a TB crossing a page can be one off.
         */
        uint32_t off = qemu_target_page_size() - 1;    /* 1 KB pages on ARMv4/v5 */
        return (ARM_CPU(current_cpu)->env.regs[15] & ~off) | ((uint32_t)data[0] & off);
    }
    return 0;
}

static uint32_t ngage_lr(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[14] : 0;
}

/*
 * Hot-path logging (Q56): the interrupt controller, timers, CBUS and NOS calls are logged for
 * their first NGAGE_LOG_HOT accesses per register (or slot/method), then not at all, unless
 * NGAGE_VERBOSE is set. Logging every access (pc lookup + a line each) took most of the host
 * time: 28 MB of log a minute.
 */
#define NGAGE_LOG_HOT 32

static bool ngage_log_hot(uint16_t *count)
{
    static int verbose = -1;

    if (verbose < 0) {
        verbose = getenv("NGAGE_VERBOSE") != NULL;
    }
    if (verbose) {
        return true;
    }
    if (*count >= NGAGE_LOG_HOT) {
        return false;
    }
    (*count)++;
    return true;
}

/* ---- logging catch-all ------------------------------------------------- */

typedef struct {
    MemoryRegion mr;
    GHashTable *mem;        /* word address -> last written value */
    GHashTable *seen;       /* word address -> number of accesses logged */
    uint64_t total;
} NgageUnknown;

#define NGAGE_LOG_PER_ADDR 16

static void ngage_unknown_log(NgageUnknown *u, hwaddr addr, bool wr,
                              uint64_t val, unsigned size)
{
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(u->seen, k));

    u->total++;
    if (n < NGAGE_LOG_PER_ADDR) {
        g_hash_table_insert(u->seen, k, GUINT_TO_POINTER(n + 1));
        qemu_log_mask(LOG_UNIMP, "ngage: %s %08" HWADDR_PRIx " %u %08" PRIx64 " pc=%08x lr=%08x%s\n",
                      wr ? "W" : "R", addr, size, val, ngage_pc(), ngage_lr(),
                      n + 1 == NGAGE_LOG_PER_ADDR ? " (further accesses not logged)" : "");
    }
}

/*
 * Registers whose read behaviour is answered (docs/eka1-ngage-questions.md) but that have no
 * device model yet: a read returns the last written word with 'set' bits forced on.
 */
static const struct {
    uint32_t addr;
    uint32_t set;
    const char *why;
} ngage_known[] = {
    { 0x0c120020, 0x0c000000, "Q9: clock status; bit 26 after 0xC, bit 27 after 3 to +4: PLL locked [I]" },
    { 0x0c149000, 0x00000001, "Q10: written 0x2210, bootstrap 0x37fc waits for bit 0: ready/ack [I]" },
    /*
     * Nokia 3230: BC02_UART1.pdd (the CSR BlueCore on a UPP UART) polls the 16-bit register at 0x0C150572
     * for bit 7 before each byte (0x5041385C, no timeout): BTTestServer spun there for ever and starved
     * the start-up (H-rm51-bt-uart; transmitter ready [I]; no BlueCore behind it)
     */
    { 0x0c150570, 0x00800000, "H-rm51-bt-uart: 0x0C150572 bit 7, BlueCore UART transmitter ready [I]" },
    /*
     * N-Gage (NEM-4): euart1.pdd (0x510E8370, the UPP UART at 0x0C150510) waits for 0x0C150514 bit 2 before it
     * reconfigures or stops the port (0x510E8820, 0x510E89CC, 0x510E8BE8): `while (*0x800004A8 && !(st & 4))`,
     * no timeout [V]. Bit 2 never came, so the thread that opened the port spun for ever and the phone froze.
     * Its interrupt handler reads bit 5 of the same register (receive error) but not bit 2 [V]: bit 2 =
     * transmitter empty [I] (H-nem4-uart-txempty). The QD's newer euart1.pdd polls the same bit but gives up
     * after 1000 tries (0x503F345C) [V], which is why the QD never hung there.
     */
    { 0x0c150514, 0x00000004, "H-nem4-uart-txempty: 0x0C150514 bit 2, UART transmitter empty [I]" },
};

/*
 * H-upp-fiq-status: 0x0C120050, three FIQ causes the dispatcher checks ahead of the controller banks. The kernel
 * writes 7 at start-up (ekern 0x503110ac, 6600) [V] and its FIQ dispatcher (0x503112b4) reads the word first: any of
 * bits 0-2 set -> it writes the value back (acknowledge), passes the kernel's own start-up object (0x8000001C, made
 * at 0x503110a4) to 0x503361d8 for bit 1 and returns WITHOUT dispatching bank 3 [V]. Nothing on the board raises these causes, so
 * the word reads 0 and writes do not stick. The catch-all used to keep the 7: the first FIQ that went through the
 * dispatcher (the 6600's IrDA UART, FiqCTSI_FIQ) re-entered for ever. What raises bits 0-2 is unknown [I].
 */
#define NGAGE_FIQ_CAUSES 0x0c120050

/*
 * H-ngage-startup (Q51; NGAGE_STARTCODE=N overrides, -1 = off): the start-up reason hand-over
 * the NOS (or the hardware) makes [I]. Default 1 = a normal power-on. ecust's start-up mode getter (0x503439ac) takes the "reset" path when 0x0C150000
 * bits 1-3 are set and maps a code from internal SRAM (0x0C00420E when 0x420C/0x4208 are 0)
 * through 0x50343880: 1 -> mode 0 = Starter's normal list (keys 0,100) [V]. Cold (bits 0) it
 * can only give 4 or 9 [V]. Applied once the NOS stand-in ran, so the bootstrap still sees a
 * cold start.
 */
static bool ngage_nos_entered;

static uint32_t ngage_startup_handover(hwaddr word, uint32_t w)
{
    static int code = -2;

    if (code == -2) {
        code = getenv("NGAGE_STARTCODE") ? (int)strtol(getenv("NGAGE_STARTCODE"), NULL, 0) : 1;
    }
    if (code < 0 || !ngage_nos_entered) {
        return w;
    }
    if (word == 0x0c150000) {
        w |= 0x0002;
    } else if (word == 0x0c00420c) {
        w = (w & ~0x00ff0000u) | (uint32_t)(code & 0xff) << 16;
    }
    return w;
}

static uint64_t ngage_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageUnknown *u = opaque;
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem,
                                  GUINT_TO_POINTER((guint)(addr & ~3u))));

    w = ngage_startup_handover(addr & ~3u, w);
    if ((addr & ~3u) == NGAGE_FIQ_CAUSES) {
        w = 0;                                      /* H-upp-fiq-status: nothing raises them */
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ngage_known); i++) {
        if (ngage_known[i].addr == (addr & ~3u)) {
            w |= ngage_known[i].set;
        }
    }
    uint64_t v = (w >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);

    ngage_unknown_log(u, addr, false, v, size);
    return v;
}

static void ngage_unknown_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageUnknown *u = opaque;
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem, k));
    unsigned sh = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;

    w = (w & ~mask) | (((uint32_t)val << sh) & mask);
    if ((addr & ~3u) == NGAGE_FIQ_CAUSES) {
        w = 0;                          /* H-upp-fiq-status: a status, not a store */
    }
    g_hash_table_insert(u->mem, k, GUINT_TO_POINTER(w));
    ngage_unknown_log(u, addr, true, val, size);
}

static const MemoryRegionOps ngage_unknown_ops = {
    .read = ngage_unknown_read,
    .write = ngage_unknown_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- CBus master to the external chips (Q11) ----------------------------- */
/*
 * Register interface [V] (bootstrap 0x35ac/0x3634, cbus.dll 0x5037d2fc via ekern's 16-bit
 * accessor 0x5031e708):
 *   +0x20  transfer format (0x0F: 16-bit data, 0x07: 8-bit; other high bits: Q11)
 *   +0x22  command: bits 0-4 register, bits 5-7 device, 0x100 = read, 0x200 = flag (Q11);
 *          reads back with bit 10 set = done (the drivers poll it)
 *   +0x24  data, high half (transfers longer than 16 bits)
 *   +0x26  data, low half; a write command commits on this store (written after +0x22)
 * The device/register split is the Nokia CBus format (3-bit device, 5-bit register; Linux
 * drivers/cbus for Retu/Tahvo). Model: a register file per (device, register), status registers
 * from ngage_cbus_status, every transaction logged ("ngage-cbus:").
 */
#define NGAGE_CBUS_BASE 0x0c150420

typedef struct {
    MemoryRegion mr;
    uint16_t fmt, cmd, hi, lo;
    bool pending;
    uint32_t regs[256];
    bool written[256];
} NgageCbus;

/* Status registers: a read returns this value whatever was written. */
static const struct {
    uint8_t dev, reg;
    uint16_t val;
    const char *why;
} ngage_cbus_status[] = {
    { 1, 0x15, 0x0010, "Q14/Q39: power-on reason; bootstrap 0x50002e6c stays on for bit 3 or 4; "
                       "the variant's start-up mode decoder (0x50343b64) gives mode 4 for bit 4, "
                       "else 9, and Starter then starts only its minimal list: bit 4 = power key "
                       "[I]" },
    { 4, 0x00, 0x0400, "Q15: ASIC id; cbus.dll 0x5037e064 requires (val & 0xff00) == 0x0400 "
                       "or faults 'CBus 25'" },
    { 1, 0x1a, 480, "Q35: ADC result (conversion started by reg 0x19). cbus.dll 0x5037e270 "
                    "needs 380..590 (else start-up mode 9: the variant resets); the bootstrap "
                    "powers off above 0x3DD and skips its delay for 5..93. 480 [I]" },
};

/*
 * CBus device 5 = the Bluetooth module (LPRF): btldd.ldd's logical registers 37-44 are device 5
 * registers 0, 1, 6, 8, 9, 10, 11, 16 (cbus.dll table 0x5037e544) [V]. The module (ngage_lprf.c)
 * raises CTSI status 2 bit 4 (interrupt source 25) a moment after each event, never from inside
 * the CBus access itself.
 */
#define NGAGE_CBUS_LPRF   5
#define NGAGE_CTSI2_LPRF  0x0010
#define NGAGE_LPRF_DELAY_NS 50000       /* 50 us [I] */

static NgageLprf ngage_lprf;
static QEMUTimer *ngage_lprf_timer;

static void ngage_lprf_kick(void *opaque)
{
    if (!timer_pending(ngage_lprf_timer)) {
        timer_mod(ngage_lprf_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NGAGE_LPRF_DELAY_NS);
    }
}

static void ngage_lprf_expire(void *opaque)
{
    if (ngage_lprf_deliver(&ngage_lprf)) {
        ngage_ctsi2_raise(NGAGE_CTSI2_LPRF);
    }
}

static void ngage_lprf_log(void *opaque, const char *msg)
{
    qemu_log_mask(LOG_UNIMP, "ngage-bt: %s\n", msg);
}

static void ngage_lprf_setup(void)
{
    ngage_lprf.kick = ngage_lprf_kick;
    ngage_lprf.log = ngage_lprf_log;
    ngage_lprf_init(&ngage_lprf);
    ngage_lprf_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ngage_lprf_expire, NULL);
}

/*
 * IrDA (models with irda): the UPP SIR UART (ngage_sir.c) on QEMU's 2nd serial port (-serial / -chardev,
 * serial_hd(1)); the 1st stays the kernel debug port. The phone's IrDA.prt frames SIR itself, so the
 * chardev carries the raw SIR byte stream: two phones joined by one link (udp, or tcp server + client)
 * beam to each other; a host tty at the phone's baud (set through CHR_IOCTL_SERIAL_SET_PARAMS) reaches
 * a serial IR dongle. Interrupt: CTSI FIQ = kernel source 26 ("FiqCTSI_FIQ", the bank-3 side of 26).
 * The end of a TX burst is signalled after its char time (10 bits a byte at the phone's baud).
 */
#define NGAGE_SIR_IRQ 26

static NgageSir ngage_sir;
static CharFrontend ngage_sir_chr;
static QEMUTimer *ngage_sir_timer;
static qemu_irq ngage_sir_line;
static MemoryRegion ngage_sir_mr[2];

static void ngage_sir_tx_byte(void *opaque, uint8_t b)
{
    /* never blocks the CPU: a peer that does not read loses the byte, as light nobody sees */
    qemu_chr_fe_write(&ngage_sir_chr, &b, 1);
}

static void ngage_sir_burst(void *opaque, unsigned n)
{
    int64_t ns = (int64_t)n * 10 * NANOSECONDS_PER_SECOND / MAX(ngage_sir.baud, 1200u);

    timer_mod(ngage_sir_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void ngage_sir_expire(void *opaque)
{
    ngage_sir_tx_done(&ngage_sir);
}

static void ngage_sir_set_baud(void *opaque, unsigned baud)
{
    QEMUSerialSetParams p = { .speed = baud, .parity = 'N', .data_bits = 8, .stop_bits = 1 };

    qemu_log_mask(LOG_UNIMP, "ngage-sir: %u baud\n", baud);
    qemu_chr_fe_ioctl(&ngage_sir_chr, CHR_IOCTL_SERIAL_SET_PARAMS, &p);   /* only ttys take it */
}

static void ngage_sir_set_irq(void *opaque, bool level)
{
    qemu_set_irq(ngage_sir_line, level);
}

static int ngage_sir_can_receive(void *opaque)
{
    return ngage_sir_rx_room(&ngage_sir);
}

static void ngage_sir_receive(void *opaque, const uint8_t *buf, int size)
{
    ngage_sir_rx(&ngage_sir, buf, size);
}

static uint64_t ngage_sir_mmio_read(void *opaque, hwaddr a, unsigned size)
{
    uint32_t addr = (uint32_t)(uintptr_t)opaque + a;
    uint16_t v = ngage_sir_read(&ngage_sir, addr);
    static uint16_t count[0x60];

    if (ngage_log_hot(&count[(addr >> 1) % 0x60]) && addr != SIR_RXDATA) {
        qemu_log_mask(LOG_UNIMP, "ngage-sir: R %08x -> %04x pc=%08x lr=%08x\n", addr, v, ngage_pc(), ngage_lr());
    }
    return v;
}

static void ngage_sir_mmio_write(void *opaque, hwaddr a, uint64_t val, unsigned size)
{
    uint32_t addr = (uint32_t)(uintptr_t)opaque + a;
    static uint16_t count[0x60];

    if (ngage_log_hot(&count[(addr >> 1) % 0x60]) && addr != SIR_TXDATA) {
        qemu_log_mask(LOG_UNIMP, "ngage-sir: W %08x = %04x pc=%08x lr=%08x\n", addr, (unsigned)val,
                      ngage_pc(), ngage_lr());
    }
    ngage_sir_write(&ngage_sir, addr, (uint16_t)val);
}

static const MemoryRegionOps ngage_sir_ops = {
    .read = ngage_sir_mmio_read, .write = ngage_sir_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },     /* the drivers use 16-bit accesses */
};

static void ngage_sir_setup(MemoryRegion *sys, qemu_irq line)
{
    ngage_sir_init(&ngage_sir);
    ngage_sir.tx_byte = ngage_sir_tx_byte;
    ngage_sir.tx_burst_done = ngage_sir_burst;
    ngage_sir.set_baud = ngage_sir_set_baud;
    ngage_sir.irq = ngage_sir_set_irq;
    ngage_sir_line = line;
    ngage_sir_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ngage_sir_expire, NULL);
    if (serial_hd(1)) {
        qemu_chr_fe_init(&ngage_sir_chr, serial_hd(1), &error_abort);
        qemu_chr_fe_set_handlers(&ngage_sir_chr, ngage_sir_can_receive, ngage_sir_receive, NULL, NULL,
                                 NULL, NULL, true);
    }
    /* the CTSI FIQ enables/status words (0x0C15000C..0F) and the UART (0x0C150510..63) */
    memory_region_init_io(&ngage_sir_mr[0], NULL, &ngage_sir_ops, (void *)(uintptr_t)SIR_CTSI_DISABLE,
                          "ngage.sir-ctsi", 4);
    memory_region_add_subregion(sys, SIR_CTSI_DISABLE, &ngage_sir_mr[0]);
    memory_region_init_io(&ngage_sir_mr[1], NULL, &ngage_sir_ops, (void *)(uintptr_t)SIR_BASE,
                          "ngage.sir", SIR_END - SIR_BASE);
    memory_region_add_subregion(sys, SIR_BASE, &ngage_sir_mr[1]);
    /* 0x0C1500BA: a one-word region of its own */
    {
        MemoryRegion *st = g_new0(MemoryRegion, 1);
        memory_region_init_io(st, NULL, &ngage_sir_ops, (void *)(uintptr_t)SIR_CTSI_STATUS, "ngage.sir-status", 2);
        memory_region_add_subregion(sys, SIR_CTSI_STATUS, st);
    }
}

/*
 * H-uem-rtc: the UEM's real-time clock (CBus dev 1), on models with uem_rtc [V rtc.dll 7650/6600, read statically:
 * docs/research/6600-rtc-clock.md, docs/research/ngage-uem-rtc.md]. The UEM counts time since its last reset:
 * reg 0x11 seconds, 0x12 minutes, 0x13 hours, 0x01 days (0xFF = not running). rtc.dll never writes them; it only
 * resets them (reg 2 bit 7) when it finds the clock invalid. The date the count starts from is not in the UEM: the
 * NOS keeps it in its permanent memory and hands it to rtc.dll at start-up (request 1, nos_standin.S), and rtc.dll
 * sets the kernel's time to that date with the UEM's time of day, plus the days. The board starts the count at
 * the host's midnight (in QEMU's -rtc base: the launchers pass base=localtime) and gives that midnight to the
 * stand-in as the stored date (NOS device +0x48/+0x4C), so every boot comes up at the host's time.
 */
static int64_t ngage_uem_zero;          /* wall-clock seconds (mktimegm domain) at which the count was 0 */
static bool ngage_uem_zero_set;

static int64_t ngage_uem_now(void)
{
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    return mktimegm(&tm);
}

static int64_t ngage_uem_elapsed(void)
{
    int64_t now = ngage_uem_now();

    if (!ngage_uem_zero_set) {
        ngage_uem_zero = now - now % 86400;     /* power applied at midnight */
        ngage_uem_zero_set = true;
    }
    return MAX(now - ngage_uem_zero, 0);
}

/* The UEM counter register r (dev 1), or -1 if r is not one. */
static int ngage_uem_counter(unsigned r)
{
    int64_t t;

    if (!ngm->uem_rtc) {
        return -1;
    }
    t = ngage_uem_elapsed();
    switch (r) {
    case 1 << 5 | 0x01: return MIN(t / 86400, 0xfe);     /* 0xFF would read as "not running" */
    case 1 << 5 | 0x11: return t % 60;
    case 1 << 5 | 0x12: return t / 60 % 60;
    case 1 << 5 | 0x13: return t / 3600 % 24;
    }
    return -1;
}

/*
 * The date the NOS hands over (a TTime, microseconds), 0 if this model has no UEM clock. Anchored on the kernel's own
 * TTime of 2000-01-01 00:00, its boot-time base (0x80006758 after 7650 ekern 0x5031f11c, TDateTime(2000, January, 0))
 * [V read in a run]: a plain count from 0001-01-01 came out 378 days early on the phone.
 */
#define NGAGE_TTIME_2000 0x00e03ab44a676000ULL
#define NGAGE_UNIX_2000  946684800LL

static uint64_t ngage_uem_base_ttime(void)
{
    if (!ngm->uem_rtc) {
        return 0;
    }
    ngage_uem_elapsed();
    return NGAGE_TTIME_2000 + (uint64_t)(ngage_uem_zero - NGAGE_UNIX_2000) * 1000000;
}

static uint64_t ngage_cbus_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageCbus *c = opaque;

    switch (addr) {
    case 0x0: return c->fmt;
    case 0x2: return c->cmd | 0x400;
    case 0x4: return c->hi;
    case 0x6: return c->lo;
    }
    return 0;
}

static void ngage_cbus_commit_write(NgageCbus *c)
{
    unsigned r = c->cmd & 0xff;

    static uint16_t count[256];

    c->regs[r] = (uint32_t)c->hi << 16 | c->lo;
    c->written[r] = true;
    if (r >> 5 == NGAGE_CBUS_LPRF) {
        ngage_lprf_write(&ngage_lprf, r & 0x1f, c->lo);
    }
    if (ngm->uem_rtc && r == (1 << 5 | 0x02) && (c->lo & 0x80)) {
        ngage_uem_zero = ngage_uem_now();           /* H-uem-rtc: reset pulse, the count restarts at 0 */
        ngage_uem_zero_set = true;
        qemu_log_mask(LOG_UNIMP, "ngage-cbus: UEM RTC reset (H-uem-rtc)\n");
    }
    if (ngage_log_hot(&count[r]))
    qemu_log_mask(LOG_UNIMP, "ngage-cbus: W dev %u reg %02x = %04x%s (fmt %02x cmd %03x) pc=%08x\n",
                  r >> 5, r & 0x1f, c->lo, c->hi ? " hi" : "", c->fmt, c->cmd, ngage_pc());
}

static void ngage_cbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageCbus *c = opaque;
    unsigned r;

    switch (addr) {
    case 0x0:
        c->fmt = val;
        return;
    case 0x4:
        c->hi = val;
        return;
    case 0x6:
        c->lo = val;
        if (c->pending) {
            ngage_cbus_commit_write(c);
            c->pending = false;
        }
        return;
    case 0x2:
        c->cmd = val & 0x3ff;
        r = val & 0xff;
        if (val & 0x100) {
            const char *src = c->written[r] ? "" : " (never written)";
            uint32_t v = c->regs[r];

            for (unsigned i = 0; i < ARRAY_SIZE(ngage_cbus_status); i++) {
                if ((ngage_cbus_status[i].dev << 5 | ngage_cbus_status[i].reg) == r) {
                    v = ngage_cbus_status[i].val;
                    src = " (declared status)";
                }
            }
            if (ngm->uem_rtc && r == (1 << 5 | 0x04) && !c->written[r]) {
                /*
                 * H-nhl10-rtc: UEM RTC calibration. rtc.dll takes the clock as valid only when
                 * this is non-zero (and days != 0xFF) [V docs/research/6600-rtc-clock.md]; a phone
                 * keeps it (the driver's default is 0xC0). Read as 0, every boot asked for the time
                 * and date, and a slow answer let the view server kill Phone.app (regression
                 * "locked_after_restart").
                 */
                v = 0xc0;
                src = " (UEM RTC calibration, H-nhl10-rtc)";
            }
            int uem = ngage_uem_counter(r);

            if (uem >= 0) {
                v = uem;
                src = " (UEM RTC counter, H-uem-rtc)";
            }
            if (r >> 5 == NGAGE_CBUS_LPRF) {
                v = ngage_lprf_read(&ngage_lprf, r & 0x1f);
                src = " (Bluetooth module)";
            }
            if (r == (1 << 5 | 0x15) && getenv("NGAGE_POWERON")) {   /* experiments (Q51) */
                v = strtoul(getenv("NGAGE_POWERON"), NULL, 0);
                src = " (NGAGE_POWERON)";
            }
            c->lo = v;
            c->hi = v >> 16;
            static uint16_t rcount[256];
            if (ngage_log_hot(&rcount[r]))
            qemu_log_mask(LOG_UNIMP, "ngage-cbus: R dev %u reg %02x -> %04x%s (fmt %02x cmd %03x) pc=%08x\n",
                          r >> 5, r & 0x1f, c->lo, src, c->fmt, c->cmd, ngage_pc());
            c->pending = false;
        } else {
            c->pending = true;
        }
        return;
    }
}

static const MemoryRegionOps ngage_cbus_ops = {
    .read = ngage_cbus_read,
    .write = ngage_cbus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

/* ---- camera (Nokia 7650): CCP receiver, frames, DMA source (ngage_cam.c) -------------------- */
/*
 * camerapdd.pdd drives the sensor and the UPP's CCP receiver itself (docs/devices/nokia-7650-nhl2/camera.md); the
 * board supplies a test pattern: while the receiver is on (0x0C121038 bit 0) a frame starts every NGAGE_CAM_FRAME_NS
 * (the frame-start event, if unmasked: the 7610's driver arms its buffer on it) and ends NGAGE_CAM_START_NS later.
 * If a DMA transfer from the CCP FIFO is armed then, the frame lands in its buffer and the channel completes
 * (remaining count -1), then the frame-end event raises IrqCCPStatus (86) unless masked.
 */
#define NGAGE_CAM_FRAME_NS  (50 * 1000 * 1000)     /* 20 frames/s; camerapdd's watchdog (70 ms ticks)
                                                      restarts the receiver after two ticks without a frame [V] */
#define NGAGE_CAM_START_NS  (30 * 1000 * 1000)     /* frame start to frame end: room for the arming DFC [I] */

static NgageCam ngage_cam;
static QEMUTimer *ngage_cam_timer;
static qemu_irq ngage_cam_irq_line;
static int ngage_cam_dma_ch = -1;
static bool ngage_cam_in_frame;                 /* between the frame-start and frame-end events */
static uint32_t ngage_cam_dma_dst, ngage_cam_dma_bytes;
static bool ngage_slide_closed;                 /* machine property slide-closed (7650) */

static void ngage_cam_update(void)
{
    qemu_set_irq(ngage_cam_irq_line, ngage_cam_irq(&ngage_cam));
    if (!ngage_cam_streaming(&ngage_cam)) {
        timer_del(ngage_cam_timer);
        ngage_cam_in_frame = false;
    } else if (!timer_pending(ngage_cam_timer)) {
        timer_mod(ngage_cam_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (ngage_cam_in_frame ? NGAGE_CAM_START_NS : NGAGE_CAM_FRAME_NS - NGAGE_CAM_START_NS));
    }
}

static void ngage_cam_frame(void *opaque)
{
    static uint16_t nlog;

    if (!ngage_cam_in_frame) {
        ngage_cam_in_frame = true;
        ngage_cam_frame_start(&ngage_cam);
        ngage_cam_update();
        return;
    }
    ngage_cam_in_frame = false;
    if (ngage_cam_dma_ch >= 0) {
        g_autofree uint8_t *buf = g_malloc(ngage_cam_dma_bytes);
        int ch = ngage_cam_dma_ch;

        uint32_t sent = ngage_cam_fill(&ngage_cam, buf, ngage_cam_dma_bytes);
        address_space_write(&address_space_memory, ngage_cam_dma_dst, MEMTXATTRS_UNSPECIFIED, buf, sent);
        ngage_cam_dma_ch = -1;
        if (sent < ngage_cam_dma_bytes) {
            ngage_dma_finish_bytes(ch, sent);       /* a 7610 JPEG still: shorter than the buffer */
        } else {
            ngage_dma_finish(ch);
        }
        static uint32_t last_bytes;
        bool changed = ngage_cam_dma_bytes != last_bytes;     /* a new mode (viewfinder / still): always logged */
        last_bytes = ngage_cam_dma_bytes;
        if (ngage_log_hot(&nlog) || changed) {
            qemu_log_mask(LOG_UNIMP, "ngage-cam: frame %u, %u bytes %s to %08x (DMA ch %d)\n", ngage_cam.frames,
                          ngage_cam_dma_bytes, ngage_cam_rgb444(&ngage_cam) ? "RGB444" :
                          ngage_cam.reg16 && ngage_cam.regs[CAM_REG_JPEG] == 6 ? "JPEG" : "YUV422",
                          ngage_cam_dma_dst, ch);
        }
    }
    ngage_cam_frame_end(&ngage_cam);
    ngage_cam_update();
}

static bool ngage_cam_dma_start(void *opaque, int ch, uint32_t src, uint32_t dst, uint32_t bytes)
{
    if ((src & 0x00ffffff) != (CAM_CCP_FIFO & 0x00ffffff) || (src >> 24 != 0x10 && src >> 24 != 0x0c) ||
        !bytes || bytes > CAM_BUF_SIZE ||
        !address_space_access_valid(&address_space_memory, dst, bytes, true, MEMTXATTRS_UNSPECIFIED)) {
        return false;
    }
    ngage_cam_dma_ch = ch;
    ngage_cam_dma_dst = dst;
    ngage_cam_dma_bytes = bytes;
    return true;
}

static void ngage_cam_dma_stop(void *opaque, int ch)
{
    if (ch == ngage_cam_dma_ch) {
        ngage_cam_dma_ch = -1;
    }
}

static uint64_t ngage_ccp_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t v = ngage_cam_ccp_read(&ngage_cam, addr & ~3u);

    ngage_cam_update();
    return (v >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
}

static void ngage_ccp_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    static uint16_t wcount[16];
    unsigned off = addr & ~3u, sh = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;
    uint32_t w = off == CAM_CCP_STATUS ? 0 : ngage_cam.ccp[off / 4];

    w = (w & ~mask) | (((uint32_t)val << sh) & mask);
    if (ngage_log_hot(&wcount[(off / 4) & 15])) {
        qemu_log_mask(LOG_UNIMP, "ngage-cam: W ccp +%02x = %08x pc=%08x\n", off, w, ngage_pc());
    }
    ngage_cam_ccp_write(&ngage_cam, off, w);
    ngage_cam_update();
}

static const MemoryRegionOps ngage_ccp_ops = {
    .read = ngage_ccp_read, .write = ngage_ccp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static uint16_t ngage_cam_lines(void *opaque)
{
    return ngage_cam_gpio_lines(&ngage_cam);
}

static void ngage_cam_drive(void *opaque, uint16_t drive)
{
    ngage_cam_gpio_drive(&ngage_cam, drive);
}

static void ngage_cam_log(void *opaque, const char *msg)
{
    static uint16_t nlog;

    if (ngage_log_hot(&nlog) || !strncmp(msg, "module", 6) || !strncmp(msg, "camx", 4)) {
        qemu_log_mask(LOG_UNIMP, "ngage-cam: %s\n", msg);
    }
}

static void ngage_cam_setup(MemoryRegion *sys, qemu_irq irq)
{
    static MemoryRegion mr;
    static const NgageDmaPeer peer = { ngage_cam_dma_start, ngage_cam_dma_stop, NULL };

    ngage_cam.log = ngage_cam_log;
    ngage_cam_init(&ngage_cam);
    switch (ngm->camera) {
    case NGAGE_CAM_NHL8:
        ngage_cam_set_sensor(&ngage_cam, CAM_I2C_ADDR_NHL8, true);
        break;
    case NGAGE_CAM_RM25:
        ngage_cam.rot90 = true;
        break;
    case NGAGE_CAM_CAMX:
        ngage_cam.camx = true;
        break;
    case NGAGE_CAM_RM51:
        ngage_cam_set_sensor(&ngage_cam, CAM_I2C_ADDR_RM51, false);
        ngage_cam.cmdport = true;
        break;
    default:
        break;
    }
    ngage_cam_irq_line = irq;
    ngage_cam_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ngage_cam_frame, NULL);
    memory_region_init_io(&mr, NULL, &ngage_ccp_ops, NULL, "ngage.ccp", CAM_CCP_SIZE);
    memory_region_add_subregion(sys, CAM_CCP_BASE, &mr);
    ngage_dma_set_peer(&peer);
    ngage_keys_gpio_peer(ngage_cam_lines, ngage_cam_drive, NULL);
    if (ngm->slide) {
        ngage_keys_slide(true, ngage_slide_closed);
    }
}

/* ---- interrupt controller (Q16) ---------------------------------------- */
/*
 * 0x0C120100, four banks of 32 sources, 0x40 apart [V: ekern 0x5031e874 / 0x5031e8cc /
 * 0x5031e798 / 0x5031e868, dispatcher 0x5031c87c]:
 *   +08  write 1<<n: enable source n (the bootstrap and kernel enable the timer as bit 0)
 *   +0C  write 1<<n: disable source n [I]. Not an acknowledge: the bootstrap's timer handler
 *        writes it and leaves the timer status set; the kernel clears that status and
 *        re-enables source 0. Any other reading kills the kernel on the leftover status.
 *   +10  pending bits (ekern's "is pending" helper reads it)
 *   +14  number of the source to service; 0 = none. [I] 1-based: source 0 (the timer) is used.
 * Banks 0-2 drive IRQ (the dispatcher reads +14 of each); bank 3 drives FIQ [I] (the other
 * dispatcher reads 0x0C1201D4). Sources are level-sensitive [V for source 0: the kernel's
 * timer handler 0x5031c5b0 clears the timer's own status and never writes +0C].
 * Unknown offsets store and log.
 */
#define NGAGE_INTC_BASE 0x0c120100

typedef struct {
    MemoryRegion mr;
    qemu_irq irq, fiq;
    uint32_t enable[4], pending[4], level[4];
    uint32_t other[4][16];
} NgageIntc;

static void ngage_intc_update(NgageIntc *c)
{
    bool irq = false;

    for (int b = 0; b < 3; b++) {
        irq |= (c->pending[b] & c->enable[b]) != 0;
    }
    qemu_set_irq(c->irq, irq);
    qemu_set_irq(c->fiq, (c->pending[3] & c->enable[3]) != 0);
}

static void ngage_intc_set(void *opaque, int n, int level)
{
    NgageIntc *c = opaque;
    uint32_t bit = 1u << (n & 31);
    int b = n >> 5;

    c->level[b] = level ? c->level[b] | bit : c->level[b] & ~bit;
    c->pending[b] = level ? c->pending[b] | bit : c->pending[b] & ~bit;
    if (b == 0) {
        /*
         * Bank 3 is the FIQ side of sources 0-31 (Q46) [I: medint.pdd runs timer 3 and
         * enables bank 3 bit 3; the kernel enables bank 3 bit 27]: the same source
         * pending bits, its own enable mask.
         */
        c->level[3] = c->level[0];
        c->pending[3] = c->pending[0];
    }
    ngage_intc_update(c);
}

static uint64_t ngage_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageIntc *c = opaque;
    int b = addr >> 6, r = addr & 0x3f;
    uint32_t v;

    switch (r) {
    case 0x08: v = c->enable[b]; break;
    case 0x10: v = c->pending[b]; break;
    case 0x14: {
        uint32_t act = c->pending[b] & c->enable[b];
        v = act ? ctz32(act) + 1 : 0;
        return v;                       /* polled by the dispatchers: not logged */
    }
    default: v = c->other[b][r >> 2]; break;
    }
    static uint16_t rcount[4][16];
    if (ngage_log_hot(&rcount[b & 3][(r >> 2) & 15]))
    qemu_log_mask(LOG_UNIMP, "ngage-intc: R bank %d +%02x -> %08x pc=%08x lr=%08x\n", b, r, v,
                  ngage_pc(), ngage_lr());
    return v;
}

static void ngage_intc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageIntc *c = opaque;
    int b = addr >> 6, r = addr & 0x3f;
    static uint16_t wcount[4][16];

    if (ngage_log_hot(&wcount[b & 3][(r >> 2) & 15]))
    qemu_log_mask(LOG_UNIMP, "ngage-intc: W bank %d +%02x = %08x pc=%08x lr=%08x\n", b, r,
                  (unsigned)val, ngage_pc(), ngage_lr());
    switch (r) {
    case 0x08: c->enable[b] |= val; break;
    case 0x0c: c->enable[b] &= ~(uint32_t)val; break;
    default: c->other[b][r >> 2] = val; break;
    }
    ngage_intc_update(c);
}

static const MemoryRegionOps ngage_intc_ops = {
    .read = ngage_intc_read, .write = ngage_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- timers at 0x0C120A00 + 0x20 * i, interrupt sources 0-5 (Q13, Q17, Q31) */
/*
 * Registers [V: kernel helpers 0x5031ea58 (+04 mode, written and read back), eac4 (+18),
 * eae8 (+10), eafc (+14), eb14 (+08), eadc (read +10); timer 0 = the tick (0x5031c718),
 * timer 1 = the one-shot timer queue (object 0x80000558, methods 0x5031ca64-0x5031cb80)]:
 *   +04 mode: 0 stopped (the remaining count is kept), 1 one-shot down-counter, 2 periodic (reload from
 *       +18); a running mode written while stopped resumes from +10
 *   +08 interrupt status: set when the count expires; handlers write it (tick: 0, timer 1
 *       and the bootstrap: 1), which acknowledges; the bootstrap then polls it for != 0
 *   +10 count: writing (re)starts the countdown from the value; reading returns the live
 *       remaining count (timer 1's AddDelta reads it and writes count + delta)
 *   +18 reload (periodic)
 * Interrupt source i (name table IrqTimer0-5, Q30) is raised on expiry, lowered by a write to
 * +08 or +10 or by mode 0.
 * Clocks [V for timer 0: its period 0x843E2 is the kernel's 15625 us tick (0x3D09, export
 * 466) -> 34.67 MHz; I for the others: timer 1's ms-to-ticks factor is 2311 = 34.67 MHz/15].
 * Timer 3 runs at 34.67 MHz too [I, Q54]: medint.pdd programs flash one halfword per timer-3 FIQ
 * period of 0x1FE ticks, i.e. 14.7 us (a typical Intel word-program time); at /15 every flash
 * write took 15 times longer and Phone's profile commit outlasted ViewSrv's timeout.
 */
#define NGAGE_TIMER_BASE   0x0c120a00
#define NGAGE_TIMERS       6
#define NGAGE_TIMER0_HZ    34666667        /* 13 MHz * 8 / 3 */
#define NGAGE_TIMERN_HZ    (NGAGE_TIMER0_HZ / 15)

typedef struct NgageTimers NgageTimers;
typedef struct {
    NgageTimers *owner;
    int index;
    QEMUTimer *t;
    qemu_irq irq;
    uint32_t regs[8];
    int64_t start_ns;           /* when the current countdown started */
    uint32_t start_count;
} NgageTimer;

struct NgageTimers {
    MemoryRegion mr;
    NgageTimer t[NGAGE_TIMERS];
};

static uint64_t ngage_timer_hz(NgageTimer *s)
{
    return (s->index == 0 || s->index == 3) ? NGAGE_TIMER0_HZ : NGAGE_TIMERN_HZ;
}

static void ngage_timer_arm(NgageTimer *s, uint32_t ticks)
{
    s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->start_count = ticks;
    timer_mod(s->t, s->start_ns + muldiv64(ticks, NANOSECONDS_PER_SECOND, ngage_timer_hz(s)));
}

static uint32_t ngage_timer_count(NgageTimer *s)
{
    uint64_t gone;

    if (!s->regs[0x04 >> 2] || !timer_pending(s->t)) {
        return s->regs[0x10 >> 2];
    }
    gone = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns, ngage_timer_hz(s),
                    NANOSECONDS_PER_SECOND);
    return gone >= s->start_count ? 0 : s->start_count - gone;
}

/*
 * H-ngage-flash-fiq: timer 3 in periodic mode with a period under NGAGE_TIMER_FAST_NS is medint.pdd's flash-program
 * clock (0x186-0x1FE ticks = 11-15 us, one halfword per FIQ, Q54). QEMU delivered each period's host timer ~200 us
 * late on average (up to ~9 ms) [V, measured 2026-11-26], so the phone programmed ~3500 halfwords/s instead of
 * ~80 000. Instead of a host timer per period, a period counts as elapsed at once: at the start of a run (+10) and
 * whenever the FIQ handler acknowledges (+08), so the FIQ comes again as soon as the handler returns, until it stops
 * the timer (+04 = 0) at the end of its buffer. QEMU's flash completes every program at once, so the handler always
 * finds it ready. NEM-4 factory-fresh boot to SysAp: 252 s -> ~200 s. Slow writes are the suspected cause [I] of
 * the NEM-4's ViewSrv 11 restarts (Phone.app in EStor.dll, Starter renamed "CRASH"); NGAGE_NO_FAST_FIQ=1 turns it off.
 */
#define NGAGE_TIMER_FAST_NS 100000

static bool ngage_timer_fast(NgageTimer *s)
{
    return s->index == 3 && s->regs[0x04 >> 2] == 2 && s->regs[0x18 >> 2] &&
           muldiv64(s->regs[0x18 >> 2], NANOSECONDS_PER_SECOND, ngage_timer_hz(s)) < NGAGE_TIMER_FAST_NS &&
           !getenv("NGAGE_NO_FAST_FIQ");
}

static void ngage_timer_expire(void *opaque)
{
    NgageTimer *s = opaque;

    s->regs[0x08 >> 2] |= 1;
    s->regs[0x10 >> 2] = 0;
    qemu_set_irq(s->irq, 1);
    if (s->regs[0x04 >> 2] == 2 && s->regs[0x18 >> 2]) {
        if (ngage_timer_fast(s)) {
            return;             /* the next period starts at the acknowledgement (+08) */
        }
        ngage_timer_arm(s, s->regs[0x18 >> 2]);
    }
}

static uint64_t ngage_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageTimers *ts = opaque;
    NgageTimer *s = &ts->t[(addr >> 5) % NGAGE_TIMERS];
    unsigned r = addr & 0x1f;

    if (r == 0x10) {
        return ngage_timer_count(s);
    }
    return s->regs[r >> 2];
}

static void ngage_timer_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageTimers *ts = opaque;
    NgageTimer *s = &ts->t[(addr >> 5) % NGAGE_TIMERS];
    unsigned r = addr & 0x1f;

    static uint16_t tcount[NGAGE_TIMERS][8];

    if (r != 0x08 && !(s->index == 1 && r == 0x10) &&   /* per-tick traffic: not logged */
        ngage_log_hot(&tcount[s->index][(r >> 2) & 7])) {
        qemu_log_mask(LOG_UNIMP, "ngage-timer%d: W +%02x = %08x pc=%08x lr=%08x\n", s->index,
                      r, (unsigned)val, ngage_pc(), ngage_lr());
    }
    uint32_t live = ngage_timer_count(s);     /* the remaining count before this write */
    s->regs[r >> 2] = val;
    switch (r) {
    case 0x04:
        /* mode 0 stops the countdown and keeps the remaining count; a running mode written while stopped
           resumes from the count register. The kernel's one-shot queue (timer 1) toggles 0/1 around its
           updates: when only +10 restarted the countdown, a stop-start without a new count left timer 1
           dead, and with it every one-shot kernel timer (the keypad's scan stalled mid-scan: no key worked
           a minute after boot, docs/eka1-ngage.md Next 1) */
        if (!val) {
            if (timer_pending(s->t)) {
                s->regs[0x10 >> 2] = live;
            }
            timer_del(s->t);
            qemu_set_irq(s->irq, 0);
        } else if (!timer_pending(s->t) && s->regs[0x10 >> 2]) {
            ngage_timer_arm(s, s->regs[0x10 >> 2]);
        }
        break;
    case 0x08:
        qemu_set_irq(s->irq, 0);
        if (ngage_timer_fast(s) && !timer_pending(s->t)) {
            s->regs[0x08 >> 2] |= 1;            /* H-ngage-flash-fiq: the next period has passed */
            qemu_set_irq(s->irq, 1);
        }
        break;
    case 0x10:                  /* (re)start the countdown; written after the mode [V] */
        qemu_set_irq(s->irq, 0);
        /* medint.pdd starts timer 3 by writing the count without a mode (Q46): one-shot */
        if (val && ngage_timer_fast(s)) {
            timer_del(s->t);                    /* H-ngage-flash-fiq: the first period too */
            s->regs[0x08 >> 2] |= 1;
            s->regs[0x10 >> 2] = 0;
            qemu_set_irq(s->irq, 1);
        } else if (val) {
            ngage_timer_arm(s, val);
        } else if (s->regs[0x04 >> 2] == 1) {
            /*
             * H-ngage-timer-zero: a one-shot loaded with 0 is due now [I: a down-counter at its terminal count].
             * The kernel's one-shot queue (timer 1) loads the head's delta unclamped when the queue was empty
             * (ekern 0x503414d4, 6260); ecomm.ldd's turnaround timer has a delta of 0, and stopping here left
             * it pending for ever: the IrDA write it gates never started (6260 infrared, 2026-11-26).
             */
            ngage_timer_arm(s, 0);
        } else {
            timer_del(s->t);
        }
        break;
    }
}

static const MemoryRegionOps ngage_timer_ops = {
    .read = ngage_timer_read, .write = ngage_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- LCD interface at 0x0C121200 (Q37) ----------------------------------- */
/*
 * [V] lcd.ldd accessors and the bootstrap (pc 0x6658/0x66e0): +0x00 control word,
 * +0x04 control (2, 1) / status (bit 2 = ready, polled before each data word), +0x08 data
 * (panel commands such as 0x11 sleep-out and 0x04 read-ID; reads return panel answers).
 * Discovery model: always ready; every control and data access logged ("ngage-lcd:") up to
 * a limit, so the panel protocol can be decoded (P4).
 */
#define NGAGE_LCDIF_BASE 0x0c121200

/*
 * Control word +0x00 [V: ecust/lcd.ldd SendCommand 0x5034580c, ReadID 0x50344f54]:
 *   0x09280000           command phase: the next data word is a DCS command byte
 *   0x09240000 | (n - 1) parameter/data phase, n bytes
 *   0x07840000 | n       read n bytes (the first is a dummy cycle)
 *   0x49200000, 0x4f300000: idle / configuration [I]
 * The panel is a DCS controller [I: commands 0x11, 0x04, 0x09, 0x36, 0x3A, 0x2x seen in the
 * drivers]. ecust identifies it by ID1: 0x29 -> type 16, 0x83 -> type 4, else unknown
 * (0x50345470) [V]; this board answers 0x83 [I].
 */
#define NGAGE_LCD_ID1 0x83

#define NGAGE_LCD_W 176             /* ROM header screen size, 12 bpp (Q37) */
#define NGAGE_LCD_H 208

typedef struct {
    MemoryRegion mr;
    QemuConsole *con;
    uint16_t fb[NGAGE_LCD_H][NGAGE_LCD_W];      /* RGB444 */
    bool dirty;
    unsigned x0, x1, y0, y1, x, y;              /* window (0x2A/0x2B) and cursor */
    uint8_t pend[3];                            /* byte-mode pixel data in progress */
    unsigned npend;
    uint32_t ctl, ctl2, data;
    uint64_t nlog, ndata;
    uint8_t cmd;                /* last DCS command */
    unsigned nparam;            /* parameter bytes received since the command */
    uint8_t params[8];
    uint8_t rd[8];              /* read response */
    unsigned rdlen, rdpos;
    uint64_t pixbytes;          /* bytes after RAMWR (0x2C) */
    uint64_t panel_frames;      /* whole frames of pixels from the OS side (not the bootstrap) */
    bool os_ramwr;              /* the current RAMWR was issued by lcd.ldd */
    bool fb_logged;
    bool from_chunk;            /* fb holds the screen chunk (H-ngage-fb), not panel pixels */
} NgageLcdIf;

#define NGAGE_LCD_LOG_MAX 4000

static void ngage_lcd_cmd_done(NgageLcdIf *l)
{
    if (l->cmd == 0x2c) {
        qemu_log_mask(LOG_UNIMP, "ngage-lcd: RAMWR: %" PRIu64 " pixel bytes\n", l->pixbytes);
    } else if (l->nparam && l->nlog++ < NGAGE_LCD_LOG_MAX) {
        qemu_log_mask(LOG_UNIMP, "ngage-lcd: cmd %02x params %02x %02x %02x %02x (%u)\n", l->cmd,
                      l->params[0], l->params[1], l->params[2], l->params[3], l->nparam);
    }
}

/* one 12-bit pixel at the cursor, then advance within the window */
static void ngage_lcd_pixel(NgageLcdIf *l, uint16_t rgb444)
{
    if (l->x < NGAGE_LCD_W && l->y < NGAGE_LCD_H) {
        l->fb[l->y][l->x] = rgb444;
        l->dirty = true;
    }
    if (++l->x > l->x1) {
        l->x = l->x0;
        if (++l->y > l->y1) {
            l->y = l->y0;
        }
    }
}

/* DCS 12 bpp (COLMOD 3): 3 bytes = 2 pixels, R1G1 B1R2 G2B2 */
static void ngage_lcd_pixels3(NgageLcdIf *l, uint8_t b0, uint8_t b1, uint8_t b2)
{
    ngage_lcd_pixel(l, (b0 << 4) | (b1 >> 4));
    ngage_lcd_pixel(l, ((b1 & 0xf) << 8) | b2);
}

static void ngage_lcd_params_done(NgageLcdIf *l)
{
    if (l->cmd == 0x2a && l->nparam >= 2) {
        l->x0 = MIN(l->params[0], NGAGE_LCD_W - 1);
        l->x1 = MIN(l->params[1], NGAGE_LCD_W - 1);
    } else if (l->cmd == 0x2b && l->nparam >= 2) {
        l->y0 = MIN(l->params[0], NGAGE_LCD_H - 1);
        l->y1 = MIN(l->params[1], NGAGE_LCD_H - 1);
    }
}

static void ngage_lcd_command(NgageLcdIf *l, uint8_t c)
{
    ngage_lcd_params_done(l);
    ngage_lcd_cmd_done(l);
    l->cmd = c;
    l->nparam = 0;
    l->pixbytes = 0;
    memset(l->params, 0, sizeof(l->params));
    l->rdlen = l->rdpos = 0;
    switch (c) {
    case 0x04:                  /* read display ID: dummy, ID1, ID2, ID3 */
        l->rd[0] = 0; l->rd[1] = NGAGE_LCD_ID1; l->rd[2] = 0; l->rd[3] = 0; l->rdlen = 4;
        break;
    case 0x09:                  /* read display status: dummy + 4 bytes */
        memset(l->rd, 0, 5); l->rdlen = 5;
        break;
    case 0x2c:                  /* memory write: from the window's start */
        l->os_ramwr = ngage_lr() >= 0x50000000;      /* lcd.ldd, not the bootstrap */
        l->x = l->x0;
        l->y = l->y0;
        l->npend = 0;
        break;
    }
    if (l->nlog++ < NGAGE_LCD_LOG_MAX) {
        qemu_log_mask(LOG_UNIMP, "ngage-lcd: cmd %02x lr=%08x\n", c, ngage_lr());
    }
}

static uint64_t ngage_lcdif_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageLcdIf *l = opaque;

    switch (addr) {
    case 0: return l->ctl;
    case 4: return l->ctl2 | 4;                 /* bit 2: ready */
    case 8:
        return l->rdpos < l->rdlen ? l->rd[l->rdpos++] : 0;
    }
    return 0;
}

static void ngage_lcdif_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageLcdIf *l = opaque;

    switch (addr) {
    case 0: l->ctl = val; return;
    case 4: l->ctl2 = val; return;
    case 8:
        l->ndata++;
        if (l->ctl & 0x80000) {                 /* command phase */
            ngage_lcd_command(l, val & 0xff);
        } else if (l->cmd == 0x2c) {
            /* control bit 31: 24-bit words, 2 pixels each [V: 54912 bytes in 18304 words] */
            if (l->ctl & 0x80000000) {
                ngage_lcd_pixels3(l, val >> 16, val >> 8, val);
            } else {
                l->pend[l->npend++] = val;
                if (l->npend == 3) {
                    ngage_lcd_pixels3(l, l->pend[0], l->pend[1], l->pend[2]);
                    l->npend = 0;
                }
            }
            l->pixbytes++;
            if (l->os_ramwr && l->pixbytes == NGAGE_LCD_W * NGAGE_LCD_H / 2) {
                /* lcd.ldd delivered a whole frame itself: H-ngage-fb no longer needed */
                l->panel_frames++;
            }
        } else if (l->nparam < sizeof(l->params)) {
            l->params[l->nparam++] = val;
        }
        return;
    }
}

/*
 * H-ngage-fb (Q38): the window server draws into the screen chunk, whose address
 * Plat::ScreenInfo (ekern export 336) computes as [[0x800066EC] + 24]; lcd.ldd copies it to
 * the panel (by DMA, not modelled yet). Until lcd.ldd's own transfers reach the panel model,
 * the display shows that RAM directly, read through the MMU: a 32-byte header, then
 * EColor4K pixels, 16 bits each (0x0RGB) [V: the buffer reads 0x0FFF white after the header;
 * chunk object +20 physical base 0x0A000000, +24 virtual base, +28 size 0x12000]. Off while the pointer is not set yet (the bootstrap's panel picture shows).
 */
#define NGAGE_SCREEN_HDR       32          /* [V] header (first halfword 0x2000), then pixels */

static bool ngage_lcd_from_screen_chunk(NgageLcdIf *l)
{
    uint32_t chunk = 0, base = 0;
    static uint16_t line[NGAGE_LCD_W];
    CPUState *cs = first_cpu;

    if (!cs || !ngm->screen_chunk_ptr ||
        cpu_memory_rw_debug(cs, ngm->screen_chunk_ptr, &chunk, 4, false) || !chunk ||
        cpu_memory_rw_debug(cs, chunk + 24, &base, 4, false) || !base) {
        return false;
    }
    for (int y = 0; y < NGAGE_LCD_H; y++) {
        if (cpu_memory_rw_debug(cs, base + NGAGE_SCREEN_HDR + y * NGAGE_LCD_W * 2, line,
                                sizeof(line), false)) {
            return false;
        }
        for (int x = 0; x < NGAGE_LCD_W; x++) {
            l->fb[y][x] = ngm->screen_rgb565 ? le16_to_cpu(line[x]) : le16_to_cpu(line[x]) & 0xfff;
        }
    }
    if (!l->fb_logged) {
        qemu_log_mask(LOG_UNIMP, "ngage-lcd: H-ngage-fb: showing the screen chunk at %08x "
                      "(chunk object %08x)\n", base, chunk);
        l->fb_logged = true;
    }
    return true;
}

static bool ngage_lcd_gfx_update(void *opaque)
{
    NgageLcdIf *l = opaque;
    DisplaySurface *surf = qemu_console_surface(l->con);
    uint8_t *dst;
    int stride;

    if (!l->panel_frames && ngage_lcd_from_screen_chunk(l)) {
        l->dirty = true;
        l->from_chunk = true;
    }
    if (!l->dirty) {
        return true;
    }
    if (surface_width(surf) != NGAGE_LCD_W || surface_height(surf) != NGAGE_LCD_H) {
        qemu_console_resize(l->con, NGAGE_LCD_W, NGAGE_LCD_H);
        surf = qemu_console_surface(l->con);
    }
    dst = surface_data(surf);
    stride = surface_stride(surf);
    for (int y = 0; y < NGAGE_LCD_H; y++) {
        uint32_t *row = (uint32_t *)(dst + y * stride);
        for (int x = 0; x < NGAGE_LCD_W; x++) {
            uint16_t p = l->fb[y][x];

            if (ngm->screen_rgb565 && l->from_chunk) {
                uint32_t r = p >> 11, g = (p >> 5) & 0x3f, b = p & 0x1f;
                row[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            } else {
                uint32_t r = (p >> 8) & 0xf, g = (p >> 4) & 0xf, b = p & 0xf;
                row[x] = (r * 0x11) << 16 | (g * 0x11) << 8 | (b * 0x11);
            }
        }
    }
    l->dirty = false;
    qemu_console_update_full(l->con);
    return true;
}

static void ngage_lcd_invalidate(void *opaque)
{
    ((NgageLcdIf *)opaque)->dirty = true;
}

static const GraphicHwOps ngage_lcd_gfx_ops = {
    .gfx_update = ngage_lcd_gfx_update,
    .invalidate = ngage_lcd_invalidate,
};

static const MemoryRegionOps ngage_lcdif_ops = {
    .read = ngage_lcdif_read, .write = ngage_lcdif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- kernel debug output (Q18) ------------------------------------------ */
/*
 * [V] ekern 0x5031e4a8 (enabled by debug-mask bit 30): the cursor (superpage 0x400003FC)
 * starts at virtual 0x59000000 = physical 0x0DE00000; a message starts with 0xC0 written at
 * the base, each character is stored at the next byte address, '\n' writes 0xE0 to virtual
 * 0x591D0000 (physical 0x0DFD0000) and resets the cursor; '\r' is dropped.
 * Model: collect the characters, emit a line on the 0xE0 store, to serial 0 and the log.
 */
#define NGAGE_DBG_BASE  0x0de00000
#define NGAGE_DBG_SIZE  0x00200000
#define NGAGE_DBG_END   0x001d0000          /* offset of the end-of-message store */

typedef struct {
    MemoryRegion mr;
    CharFrontend chr;
    GString *line;
} NgageDbg;

static void ngage_dbg_flush(NgageDbg *d)
{
    qemu_log_mask(LOG_UNIMP, "ngage-dbg: %s\n", d->line->str);
    g_string_append(d->line, "\r\n");
    qemu_chr_fe_write_all(&d->chr, (const uint8_t *)d->line->str, d->line->len);
    g_string_truncate(d->line, 0);
}

static uint64_t ngage_dbg_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void ngage_dbg_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageDbg *d = opaque;

    if (addr == NGAGE_DBG_END) {
        ngage_dbg_flush(d);
    } else if (addr == 0 && val == 0xc0 && d->line->len == 0) {
        /* message header; the first character then lands on the same address */
    } else if (addr < NGAGE_DBG_END) {
        g_string_append_c(d->line, (char)val);
    }
}

static const MemoryRegionOps ngage_dbg_ops = {
    .read = ngage_dbg_read, .write = ngage_dbg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

/* ---- NOS stand-in (H-nos-standin, Q20, Q21) ------------------------------ */
/*
 * The cellular software (NOS) does not run: its code is scrambled in the flash file. The
 * guest stub src/qemu-ngage/nos-standin/nos_standin.S is copied over the NOS entry point
 * (virtual 0x5149E4C4 = flash 0x9E4C4), so nosstarter.ext calls it. It registers trap objects
 * in the variant's interface slots; each method call lands here:
 *   +00 W  call: slot << 8 | method (0xFF00 = stub entered, 0xFE00|slot = registered, r0 = ok)
 *   +04..+10 r0..r3, +14..+20 the caller's first four stack arguments
 *   +28 R  result (r0)
 *   +30 W  ISI delivery (Q50, ngage_isi.c): 1 = fetch the next one into +34 channel, +38 the
 *          client's receive descriptor, +3C request number, +40 length (+34 = 0: none left),
 *          the frame at +100; 2 = done, +44 = DThread::Write's result
 * Every call is logged ("ngage-nos:"); answers come from ngage_nos_answers, default 0.
 * The ISI stand-in (the phone protocol to our fake modem) joins this device later.
 */
#define NGAGE_NOS_BASE  0x0c1f0000          /* board-only address, virtual 0x581F0000 */

#include "ngage_nos_stub.h"
#include "ngage_nhl10_nos_stub.h"
#include "ngage_nem4_nos_stub.h"
#include "ngage_rm51_nos_stub.h"
#include "ngage_rh51_nos_stub.h"
#include "ngage_rm25_nos_stub.h"
#include "ngage_nhl2_nos_stub.h"
#include "ngage_nhl8_nos_stub.h"
#include "ngage_nhl8x_nos_stub.h"
static bool ngage_flash_restored;    /* Q55: saved flash applied, not a factory-fresh phone */
#include "ngage_isi.h"
#include "ngage_keys.h"

typedef struct {
    MemoryRegion mr;
    uint32_t a[9];
    uint32_t result;
    uint32_t dfc;               /* +2C: TDfc the stub queues after the call (Q34) */
    bool idle_logged;
    bool rx_dfc_busy;           /* the stub's ISI delivery DFC is queued or running (Q50) */
    uint32_t rx[5];             /* +34..+44: the delivery being fetched */
    uint32_t rx_flag;           /* last value written to the stub's RX_FLAG */
} NgageNos;


/*
 * Hand the stub its delivery DFC when a reply waits for a posted receive (Q50). While one
 * waits but can't be handed out yet, RX_FLAG makes the stub's idle hook take the device path
 * (it answers the idle hook from RAM otherwise, Q56).
 */
static void ngage_nos_rx_kick(NgageNos *n)
{
    bool ready = ngage_isi_ready();
    uint32_t flag;

    if (!ngm->nos_rx_dfc) {
        return;
    }
    if (ready && !n->rx_dfc_busy && !n->dfc) {
        n->dfc = ngm->nos_rx_dfc;
        n->rx_dfc_busy = true;
    }
    flag = cpu_to_le32(ready && !n->dfc);
    if (flag != n->rx_flag && current_cpu &&
        !cpu_memory_rw_debug(current_cpu, ngm->nos_rx_flag, &flag, 4, true)) {
        n->rx_flag = flag;
    }
}

static const struct {
    uint8_t slot, method;
    uint32_t r0;
    const char *why;
} ngage_nos_answers[] = {
    { 0, 0, 0, NULL },
};

/*
 * Slot 3 (product data, Q21) method 18 (vtbl +0x48) = Read(item r1, r2, out r3, sp[0],
 * length sp[1], sp[2]) [V: product_info.dll 0x50419e94: item 14 into a stack word, item 15
 * straight into its own field this+8, which must then be <= 3]. The NOS writes the item
 * there; the stand-in writes these values (little-endian, 'length' bytes), 0 for any other
 * item (logged).
 */
static const struct {
    uint32_t item, value;
    const char *why;
    bool pending;               /* asynchronous read left pending: a notification (Q34) */
} ngage_nos_items[] = {
    { 5, 0, "Q34: ecust's asynchronous read at start-up (DFC at variant+0xB4). Its completion "
            "resets the phone unless the start-up mode is 1 or 2 (alarm/charger-type starts), "
            "so it is an event the NOS signals later: left pending [I]", true },
    { 14, 0, "Q23: product_info accepts value/100 in {0, 1004, 4400} [V]; 0 [I]" },
    { 15, 0, "Q23: product_info's index (must be <= 3) [V]; 0 [I]" },
    { 12, 105, "Q34/Q44: read asynchronously by helper.ldd request 1 (EStart), 2 bytes, "
               "EStart's request result [V]. EStart 0x503e9bce: if its C: mount fails and the "
               "value is 105 it formats C: (internal flash) instead of falling back to a RAM C: "
               "that Starter rejects (Q42) [V]. 105 = 'format the user area' [I]" },
};

static void ngage_nos_write_out(uint32_t va, uint32_t value, uint32_t len)
{
    uint8_t buf[16] = { 0 };

    len = MIN(len, sizeof(buf));
    stl_le_p(buf, value);
    if (current_cpu && va) {
        cpu_memory_rw_debug(current_cpu, va, buf, len, true);
    }
}

/*
 * Read an EKA1 8-bit descriptor at virtual 'va' (the current process's view) into buf: the top
 * 4 bits of the first word are the type [V: 6.1/7.0s e32des8.h layout], the rest the length.
 *   0 TBufC: data at +4; 1 TPtrC: pointer at +4; 2 TPtr: pointer at +8; 3 TBuf: data at +8;
 *   4 TPtr over an HBufC (EBufCPtr): +8 points to the HBufC, its data at +4.
 * Returns the length read, -1 if unreadable.
 */
static int ngage_read_des8(uint32_t va, uint8_t *buf, int max)
{
    uint32_t w[3], p;
    int len;

    if (!current_cpu || cpu_memory_rw_debug(current_cpu, va, w, sizeof(w), false)) {
        return -1;
    }
    len = MIN((int)(le32_to_cpu(w[0]) & 0x0fffffff), max);
    switch (le32_to_cpu(w[0]) >> 28) {
    case 0: p = va + 4; break;
    case 1: p = le32_to_cpu(w[1]); break;
    case 2: p = le32_to_cpu(w[2]); break;
    case 3: p = va + 8; break;
    case 4: p = le32_to_cpu(w[2]) + 4; break;
    default: return -1;
    }
    return cpu_memory_rw_debug(current_cpu, p, buf, len, false) ? -1 : len;
}

static uint64_t ngage_nos_read(void *opaque, hwaddr addr, unsigned size)
{
    NgageNos *n = opaque;

    if (addr == 0x2c) {
        uint32_t d = n->dfc;
        n->dfc = 0;
        return d;
    }
    if (addr >= 0x34 && addr <= 0x44) {
        return n->rx[(addr - 0x34) >> 2];
    }
    if (addr >= 0x100) {
        return ngage_isi_fetched_word(addr - 0x100);
    }
    if (addr == 0x48 || addr == 0x4c) {
        /* H-uem-rtc: the date the NOS keeps for the RTC (TTime lo/hi; 0 = none: the stand-in passes rtc.dll's own) */
        uint64_t t = ngage_uem_base_ttime();
        return addr == 0x48 ? (uint32_t)t : (uint32_t)(t >> 32);
    }
    return addr == 0x28 ? n->result : (addr < 0x24 ? n->a[addr >> 2] : 0);
}

static void ngage_nos_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    NgageNos *n = opaque;
    unsigned slot, method;

    if (addr == 0x30) {
        if (val == 1) {
            memset(n->rx, 0, sizeof(n->rx));
            if (!ngage_isi_fetch(&n->rx[0], &n->rx[1], &n->rx[2], &n->rx[3])) {
                n->rx_dfc_busy = false;         /* the DFC ends; the next reply kicks it again */
            }
        } else if (val == 2) {
            ngage_isi_done(n->rx[4]);
        }
        return;
    }
    if (addr == 0x44) {
        n->rx[4] = val;
        return;
    }
    if (addr) {
        if (addr < 0x24) {
            n->a[addr >> 2] = val;
        }
        return;
    }
    if (val == 0xff00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: stand-in entered (instead of the NOS)\n");
        ngage_nos_entered = true;
        return;
    }
    if (val == 0xfc00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: NOSDRIVER channel object %08x constructed, unit %u\n",
                      n->a[1], n->a[2]);
        ngage_isi_ctor(n->a[1], n->a[2]);
        return;
    }
    if (val == 0xf900) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: NOS object %08x constructed (enosdriver, export 4)\n",
                      n->a[1]);
        return;
    }
    if (val == 0xfa00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: H-nhl10-hwchunk: hardware chunk %08x reserved\n",
                      n->a[1]);
        return;
    }
    if (val == 0xfb00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: RTC hand-over to rtc.dll (request 1) -> %u (Q49)\n",
                      n->a[1]);
        return;
    }
    if (val == 0xfd00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: variant NOS status set to %u\n", n->a[1]);
        return;
    }
    if ((val & 0xff00) == 0xfe00) {
        qemu_log_mask(LOG_UNIMP, "ngage-nos: interface %u registered: %s\n",
                      (unsigned)(val & 0xff), n->a[1] ? "ok" : "REFUSED (slot taken)");
        return;
    }
    slot = (val >> 8) & 0xff;
    method = val & 0xff;
    n->result = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(ngage_nos_answers); i++) {
        if (ngage_nos_answers[i].why && ngage_nos_answers[i].slot == slot &&
            ngage_nos_answers[i].method == method) {
            n->result = ngage_nos_answers[i].r0;
        }
    }
    if (slot == 3 && method == 18 && n->a[2] == 13 && !n->a[4]) {
        /* the null thread's per-pass idle hook (Q25): log once */
        if (!n->idle_logged) {
            qemu_log_mask(LOG_UNIMP, "ngage-nos: idle hook (slot 3 item 13) called; further calls "
                          "not logged\n");
            n->idle_logged = true;
        }
        n->result = 0;
        ngage_nos_rx_kick(n);
        return;
    }
    if (slot == 3 && method == 18) {
        uint32_t item = n->a[2], value = 0;
        const char *src = " (unknown item: 0)";
        bool pending = false;

        for (unsigned i = 0; i < ARRAY_SIZE(ngage_nos_items); i++) {
            if (ngage_nos_items[i].item == item) {
                value = ngage_nos_items[i].value;
                pending = ngage_nos_items[i].pending;
                src = "";
            }
        }
        if (item == 14 && ngm->nos_item14) {
            value = ngm->nos_item14;
            src = " (model product code)";
        }
        if (item == 12 && ngage_flash_restored) {
            /* Q55: 105 makes Starter run the deep factory restore, which formats the user area
             * again; a phone whose flash was kept reports a normal start [I] */
            value = 100;
            src = " (saved flash: normal start)";
        }
        if (item == 12 && getenv("NGAGE_ITEM12")) {    /* experiments (Q39) */
            value = strtoul(getenv("NGAGE_ITEM12"), NULL, 0);
            src = " (NGAGE_ITEM12)";
        }
        /* r2 != 0: an asynchronous read; r2 is the caller's TDfc, queued when done (Q34) */
        if (n->a[3] && pending) {
            qemu_log_mask(LOG_UNIMP, "ngage-nos: product data item %u: asynchronous, left pending "
                          "(notification)\n", item);
        } else {
            ngage_nos_write_out(n->a[4], value, n->a[6]);
            n->dfc = n->a[3];
            qemu_log_mask(LOG_UNIMP, "ngage-nos: product data item %u -> %u (%u bytes)%s%s\n",
                          item, value, n->a[6], src, n->dfc ? ", asynchronous: DFC queued" : "");
        }
    }
    static uint16_t ncount[128][64];
    if (slot == 65 || slot == 66 || slot == 70 || slot == 71 || ngage_log_hot(&ncount[slot & 127][method & 63]))
    qemu_log_mask(LOG_UNIMP, "ngage-nos: slot %u method %u (vtbl +0x%x) this=%08x "
                  "r1=%08x r2=%08x r3=%08x sp[0..3]=%08x %08x %08x %08x -> %08x lr=%08x\n",
                  slot, method, method * 4, n->a[1], n->a[2], n->a[3], n->a[4],
                  n->a[5], n->a[6], n->a[7], n->a[8], n->result, ngage_lr());
    if (slot == 70 && method == 1) {
        /* Nokia 6600: NOS export 1 = send (object, const TDesC8&, length) [I] */
        uint8_t f[512];
        int len = ngage_read_des8(n->a[2], f, sizeof(f));
        GString *hx = g_string_new(NULL);

        for (int i = 0; i < len; i++) {
            g_string_append_printf(hx, " %02x", f[i]);
        }
        qemu_log_mask(LOG_UNIMP, "ngage-isa: send (r2=%u) %d bytes:%s\n", n->a[3], len, hx->str);
        g_string_free(hx, true);
        if (len > 0) {
            ngage_isi_isa_send(f, len);
        }
    }
    if (slot == 65 && method >= 3 && method <= 5) {
        ngage_isi_call(n->a[1], method, n->a[2], n->a[3], n->a[4]);     /* Q50 */
    }
    if (slot == 66 && method == 0) {
        ngage_isi_open(n->a[1], n->a[2]);
    }
    ngage_nos_rx_kick(n);
}

static const MemoryRegionOps ngage_nos_ops = {
    .read = ngage_nos_read, .write = ngage_nos_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- machine ------------------------------------------------------------ */

static struct arm_boot_info ngage_binfo;

/*
 * One x16 Intel flash device (CFI01). Both chips answer an Intel ID query (Q2, Q4):
 * device-width must be set, otherwise CFI01's legacy ID mode packs both ID bytes into one
 * word. Without a drive the contents are erased (0xFF); 'image' fills them from the start.
 */
static DeviceState *ngage_flash(hwaddr base, const char *name, const void *image, size_t len,
                                unsigned chip)
{
    DeviceState *fl;
    uint8_t *p;

    if (ngm->flash_amd) {
        fl = qdev_new(TYPE_PFLASH_CFI02);
        qdev_prop_set_uint32(fl, "num-blocks", NGAGE_FLASH_SIZE / NGAGE_FLASH_SECTOR);
        qdev_prop_set_uint32(fl, "sector-length", NGAGE_FLASH_SECTOR);
        qdev_prop_set_uint8(fl, "width", 2);
        qdev_prop_set_uint8(fl, "mappings", 1);
        qdev_prop_set_uint16(fl, "id0", ngm->flash_manuf);
        qdev_prop_set_uint16(fl, "id1", chip && ngm->flash_device1 ? ngm->flash_device1 : ngm->flash_device);
        qdev_prop_set_uint16(fl, "id2", 0);      /* extended IDs (+0x1C/+0x1E) read as 0 [V: Samsung rows] */
        qdev_prop_set_uint16(fl, "id3", 0);
        qdev_prop_set_uint16(fl, "unlock-addr0", 0x555);
        qdev_prop_set_uint16(fl, "unlock-addr1", 0x2aa);
        qdev_prop_set_string(fl, "name", name);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(fl), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(fl), 0, base);
        p = memory_region_get_ram_ptr(pflash_cfi02_get_memory(PFLASH_CFI02(fl)));
        memset(p, 0xff, NGAGE_FLASH_SIZE);
        if (image) {
            memcpy(p, image, len);
        }
        return fl;
    }
    fl = qdev_new(TYPE_PFLASH_CFI01);

    qdev_prop_set_uint32(fl, "num-blocks", NGAGE_FLASH_SIZE / NGAGE_FLASH_SECTOR);
    qdev_prop_set_uint64(fl, "sector-length", NGAGE_FLASH_SECTOR);
    /*
     * Read-while-write (Q48): medint.pdd programs C: in chip 1 while the kernel runs code and
     * reads vtables from the ROM part of the same chip. Intel 0x8874 (StrataFlash Wireless
     * L18, 256 Mbit) has 16 Mbit partitions [I]; without them every read of chip 1 returned
     * the status word (0x0080) during a program and nosdriver.ldd's vtable became 0x00800080.
     */
    qdev_prop_set_uint32(fl, "partition-size", NGAGE_FLASH_PARTITION);
    qdev_prop_set_uint8(fl, "width", 2);
    qdev_prop_set_uint8(fl, "device-width", 2);
    qdev_prop_set_uint16(fl, "id0", ngm->flash_manuf);
    qdev_prop_set_uint16(fl, "id1", chip && ngm->flash_device1 ? ngm->flash_device1 : ngm->flash_device);
    qdev_prop_set_string(fl, "name", name);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(fl), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(fl), 0, base);
    p = memory_region_get_ram_ptr(pflash_cfi01_get_memory(PFLASH_CFI01(fl)));
    memset(p, 0xff, NGAGE_FLASH_SIZE);
    if (image) {
        memcpy(p, image, len);
    }
    return fl;
}

/*
 * Declared code patches (docs/eka1-ngage-questions.md names each), applied to the board's
 * flash copy only. Each checks the original word first and refuses to patch anything else.
 */
static const struct {
    uint32_t va;
    uint32_t orig, patched;
    const char *why;
} ngage_patches[] = {
    { 0, 0, 0, NULL },
};

static void ngage_apply_patches(uint8_t *img, size_t len)
{
    for (unsigned i = 0; i < ARRAY_SIZE(ngage_patches) && ngage_patches[i].va; i++) {
        size_t o = ngage_patches[i].va - 0x50000000;
        uint32_t w;

        if (o + 4 > len) {
            continue;
        }
        w = ldl_le_p(img + o);
        if (w != ngage_patches[i].orig) {
            warn_report("ngage: patch at %08x not applied: found %08x, expected %08x (%s)",
                        ngage_patches[i].va, w, ngage_patches[i].orig, ngage_patches[i].why);
            continue;
        }
        stl_le_p(img + o, ngage_patches[i].patched);
        info_report("ngage: %s", ngage_patches[i].why);
    }
}

/*
 * Declared workaround H-ngage-md5 (Q12, docs/eka1-ngage-questions.md). The bootstrap
 * (0x50003238) MD5s ROM ranges listed at ROM offset 0x8f00/0x8f18 ({virtual start, end,
 * 16-byte digest}) and halts on a mismatch. The digest stored in the RH-29 V 04.10 flash file
 * is not the MD5 of the file's bytes (checked with the ROM's own MD5 code on this board), so
 * the board writes the digest of the image it actually serves into its flash copy. ROM.bin
 * on disk is not changed.
 */
static void ngage_fix_md5_table(uint8_t *img, size_t len)
{
    static const size_t ngage_entries[] = { 0x8f00, 0x8f18 };

    for (unsigned i = 0; i < ARRAY_SIZE(ngage_entries); i++) {
        size_t e = ngm->md5_entries[i] ? ngm->md5_entries[i] : ngage_entries[i];
        uint32_t start, end;
        uint8_t dg[16];
        gsize dlen = sizeof(dg);
        GChecksum *ck;

        if (e + 24 > len) {
            continue;
        }
        start = ldl_le_p(img + e);
        end = ldl_le_p(img + e + 4);
        if (start == 0xffffffff || start < 0x50000000 || end < start ||
            end - 0x50000000 >= len) {
            continue;
        }
        ck = g_checksum_new(G_CHECKSUM_MD5);
        g_checksum_update(ck, img + (start - 0x50000000), end - start + 1);
        g_checksum_get_digest(ck, dg, &dlen);
        g_checksum_free(ck);
        if (memcmp(dg, img + e + 8, 16)) {
            warn_report("ngage: H-ngage-md5: ROM MD5 entry 0x%zx (%08x-%08x) does not match the "
                        "image; writing the image's digest into the board's flash copy (Q12)",
                        e, start, end);
            memcpy(img + e + 8, dg, 16);
        }
    }
}

/*
 * The PPM flash file (RH2941.026: second Z: tree, language data, and a rewritten boot block
 * with the PPM's MD5 entry at 0x8F00, Q28) as extracted segments, loaded over the ROM like a
 * second flashing: -M ngage,ppm=DIR, files segment_NNN_typeTT_<flash address>.bin.
 * Phase 1 (img != NULL): segments below the ROM split go into the virtual ROM image (boot
 * block: before the patches and the MD5 workaround). Phase 2: the others into the chips.
 */
static char *ngage_ppm_dir;
static char *ngage_isi_file;         /* -M ngage,isi=FILE: the ISI reply table (Q50) */
static char *ngage_flash_file;       /* -M ngage,flash=FILE: the phone's own flash (Q55) */
static char *ngage_mmc_file;         /* -M ngage,mmc=FILE: the memory card image (ngage_mmc.c) */

/*
 * Persistent flash (Q55): the phone's writes to its flash (C:, the internal user disk, and any
 * other block the OS programs) survive a restart. At exit every 64 KB block that differs from
 * the freshly composed image is saved to FILE ("NGFL1" + {chip, offset, 64 KB} records); at
 * start the image is composed as usual and the records are applied. So a changed ROM or stand-in
 * stays in effect for the blocks the phone never wrote, and without FILE nothing persists (each
 * boot formats C: and runs the first-boot factory settings again: ~75 s).
 */
static uint8_t *ngage_flash_chip[NGAGE_MAX_CHIPS], *ngage_flash_pristine[NGAGE_MAX_CHIPS];
static Notifier ngage_flash_exit;

static void ngage_flash_save(Notifier *n, void *data)
{
    g_autofree char *tmp = g_strdup_printf("%s.tmp", ngage_flash_file);
    FILE *f = fopen(tmp, "wb");
    unsigned blocks = 0;

    if (!f) {
        error_report("ngage: flash=%s: cannot write: %s", ngage_flash_file, strerror(errno));
        return;
    }
    fwrite("NGFL1", 1, 6, f);
    for (uint32_t c = 0; c < ngm->nchips; c++) {
        for (uint32_t o = 0; o < NGAGE_FLASH_SIZE; o += NGAGE_FLASH_SECTOR) {
            if (memcmp(ngage_flash_chip[c] + o, ngage_flash_pristine[c] + o, NGAGE_FLASH_SECTOR)) {
                uint32_t hdr[2] = { cpu_to_le32(c), cpu_to_le32(o) };
                fwrite(hdr, 1, sizeof(hdr), f);
                fwrite(ngage_flash_chip[c] + o, 1, NGAGE_FLASH_SECTOR, f);
                blocks++;
            }
        }
    }
    if (fclose(f) == 0 && rename(tmp, ngage_flash_file) == 0) {
        info_report("ngage: flash: %u changed blocks saved to %s", blocks, ngage_flash_file);
    }
}

static void ngage_flash_persist(void)
{
    g_autofree char *buf = NULL;
    gsize n = 0, pos = 6;
    unsigned blocks = 0;

    for (unsigned c = 0; c < ngm->nchips; c++) {
        ngage_flash_pristine[c] = g_memdup2(ngage_flash_chip[c], NGAGE_FLASH_SIZE);
    }
    if (g_file_get_contents(ngage_flash_file, &buf, &n, NULL)) {
        if (n < 6 || memcmp(buf, "NGFL1", 6)) {
            error_report("ngage: flash=%s: not a saved flash file", ngage_flash_file);
            exit(1);
        }
        while (pos + 8 + NGAGE_FLASH_SECTOR <= n) {
            uint32_t c = ldl_le_p(buf + pos), o = ldl_le_p(buf + pos + 4);

            if (c < ngm->nchips && o < NGAGE_FLASH_SIZE && !(o % NGAGE_FLASH_SECTOR)) {
                memcpy(ngage_flash_chip[c] + o, buf + pos + 8, NGAGE_FLASH_SECTOR);
                blocks++;
            }
            pos += 8 + NGAGE_FLASH_SECTOR;
        }
        info_report("ngage: flash: %u saved blocks applied from %s", blocks, ngage_flash_file);
        ngage_flash_restored = blocks > 0;
    } else {
        info_report("ngage: flash: %s is new: factory-fresh phone", ngage_flash_file);
    }
    ngage_flash_exit.notify = ngage_flash_save;
    qemu_add_exit_notifier(&ngage_flash_exit);
}

static void ngage_load_segments(uint8_t *img, size_t len)
{
    uint32_t boot_end = ngm->rom_map[0].len;    /* the boot block: in chip 0's ROM range */

    GDir *dir;
    const char *name;

    if (!ngage_ppm_dir) {
        return;
    }
    dir = g_dir_open(ngage_ppm_dir, 0, NULL);
    if (!dir) {
        error_report("ngage: ppm=%s: not a directory", ngage_ppm_dir);
        exit(1);
    }
    while ((name = g_dir_read_name(dir))) {
        const char *u = strrchr(name, '_'), *end = NULL;
        uint64_t fa;
        gchar *path, *buf;
        gsize n;

        if (!g_str_has_prefix(name, "segment_") || !g_str_has_suffix(name, ".bin") || !u ||
            qemu_strtou64(u + 1, &end, 16, &fa) || strcmp(end, ".bin")) {
            continue;
        }
        path = g_build_filename(ngage_ppm_dir, name, NULL);
        if (!g_file_get_contents(path, &buf, &n, NULL)) {
            error_report("ngage: cannot read %s", path);
            exit(1);
        }
        if (img && fa + n <= boot_end && fa + n <= len) {
            memcpy(img + fa, buf, n);
            info_report("ngage: ppm: %s -> flash %08" PRIx64 " (%zu bytes, boot block)", name, fa, n);
        } else if (!img && !(fa < boot_end && fa + n <= boot_end)) {
            uint64_t c = fa / NGAGE_FLASH_SIZE, o = fa % NGAGE_FLASH_SIZE;

            if (c < ngm->nchips && o + n <= NGAGE_FLASH_SIZE) {
                memcpy(ngage_flash_chip[c] + o, buf, n);
                info_report("ngage: ppm: %s -> flash %08" PRIx64 " (%zu bytes)", name, fa, n);
            } else {
                warn_report("ngage: ppm: %s at flash %08" PRIx64 " is outside the chips", name, fa);
            }
        }
        g_free(buf);
        g_free(path);
    }
    g_dir_close(dir);
}

static uint8_t *ngage_flash_ptr(DeviceState *fl)
{
    if (ngm->flash_amd) {
        return memory_region_get_ram_ptr(pflash_cfi02_get_memory(PFLASH_CFI02(fl)));
    }
    return memory_region_get_ram_ptr(pflash_cfi01_get_memory(PFLASH_CFI01(fl)));
}

/* N-Gage QD (RH-29 V 04.10): ROM split at 0xF00000 (Q12), NOS alias 0x51400000 (Q19) */
static const uint32_t ngage_rh29_nos_exports[][2] = {
    /* the two NOS functions nosdriver.ldd imports (Q33): jumps into the stub */
    { 0x514a260c, NGAGE_NOS_SYM_nos_channel_ctor },
    { 0x514a1c90, NGAGE_NOS_SYM_nos_channel_open },
};

static const NgageModel ngage_rh29 = {
    .name = "ngageqd",
    .nchips = 2,
    .rom_map = { { 0, 0x00f00000, 0 }, { 0x00f00000, 0x01100000, 1 } },
    .nrom_map = 2,
    .nos_alias = 0x51400000,
    .stub = ngage_nos_stub, .stub_len = sizeof(ngage_nos_stub), .stub_va = NGAGE_NOS_STUB_VA,
    .nos_exports = ngage_rh29_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_rh29_nos_exports),
    .screen_chunk_ptr = 0x800066ec,     /* Q38 */
    .nos_rx_dfc = 0x8000c578,           /* nos_standin.S RX_DFC / RX_FLAG (Q50, Q56) */
    .nos_rx_flag = 0x8000c570,
    .flash_manuf = NGAGE_FLASH_MANUF, .flash_device = NGAGE_FLASH_DEVICE,
    .uem_rtc = true,
};

/*
 * N-Gage (NEM-4 V 4.03, the first N-Gage; docs/devices/nokia-ngage-nem4/bring-up.md): the QD's kernel generation
 * (506 ekern exports, the same ordinals and kernel-data layout: 0x800066EC, 0x80006740, 0x80000398 referenced as
 * often as in the QD's ROM; bootstrap digests at ROM 0x8F00/0x8F18) [V static], with the 3650's flash layout: ROM.bin
 * 0..7 MB in chip 0, 0x700000.. in chip 1 (flash 0x02000000), 0xB00000.. in chip 2 (flash 0x04000000) [V: the MCU
 * file's blocks]. NOS entry 0x5149D284 (nosstarter literal), NOS bss 0x8000C680; nosdriver's two NOS imports
 * (slots 0x511B4028 / 0x511B4030) [V]. Trace mask bit 30 already set in this ROM (0xC020A003).
 */
static const uint32_t ngage_nem4_nos_exports[][2] = {
    { 0x514a13cc, NGAGE_NEM4_NOS_SYM_nos_channel_ctor },
    { 0x514a0a50, NGAGE_NEM4_NOS_SYM_nos_channel_open },
};

static const NgageModel ngage_nem4 = {
    .name = "ngage",
    .nchips = 3,
    .rom_map = { { 0, 0x00700000, 0 }, { 0x00700000, 0x00400000, 1 }, { 0x00b00000, 0x006c0000, 2 } },
    .nrom_map = 3,
    .nos_alias = 0x51400000,
    .stub = ngage_nem4_nos_stub, .stub_len = sizeof(ngage_nem4_nos_stub), .stub_va = NGAGE_NEM4_NOS_STUB_VA,
    .nos_exports = ngage_nem4_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_nem4_nos_exports),
    .screen_chunk_ptr = 0x800066ec,
    .nos_rx_dfc = 0x8000c680 + 0x200,   /* nos_standin.S RX_DFC / RX_FLAG with this ROM's RAM */
    .nos_rx_flag = 0x8000c680 + 0x1f8,
    .flash_manuf = NGAGE_FLASH_MANUF, .flash_device = NGAGE_FLASH_DEVICE,
    .uem_rtc = true,
};

/*
 * Nokia 6600 (NHL-10 V 05.53), docs/devices/nokia-6600-nhl10/bring-up.md: four chips at n << 25
 * (the bootstrap's chip loop 0x61F4 halts unless all four answer) [V]; ROM.bin 0..8 MB in chip 0,
 * 0x800000..0xF00000 in chip 2, the rest in chip 3; the PPM (2 MB) and C: in chip 1 (bank table
 * ROM 0x64C) [V]. NOS entry 0x52882024 = flash 0x82024: alias 0x52800000 [I]. Trace mask bit 30
 * (kernel debug port) is off in this ROM: H-nhl10-trace sets it.
 */
/*
 * Nokia 7650 (NHL-2NA V 5.06, Symbian 6.1), docs/devices/nokia-7650-nhl2/bring-up.md: the N-Gage's
 * generation (phonet/nosdriver, the same kernel-data layout: screen chunk word 0x800066EC [V]).
 * ROM.bin (0xF00000) fills chip 0 up to 0xF00000, the PPM sits at flash 0x00F00000 [V flash file];
 * NOS entry 0x5148C1C4 (alias 0x51400000 as N-Gage) [V nosstarter]; the stub is nos_standin.S
 * built with NHL2 (ported kernel addresses).
 */
static const uint32_t ngage_nhl2_nos_exports[][2] = {
    /* the two NOS functions nosdriver.ldd imports (slots 0x50EF92F0 / 0x50EF92E8) [V] */
    { 0x514903cc, NGAGE_NHL2_NOS_SYM_nos_channel_ctor },
    { 0x5148fa88, NGAGE_NHL2_NOS_SYM_nos_channel_open },
};

static const NgageModel ngage_nhl2 = {
    .name = "nokia7650",
    .nchips = 2,                        /* [I] as N-Gage; chip 1 erased */
    .rom_map = { { 0, 0x00f00000, 0 } },
    .nrom_map = 1,
    .nos_alias = 0x51400000,
    .stub = ngage_nhl2_nos_stub, .stub_len = sizeof(ngage_nhl2_nos_stub),
    .stub_va = NGAGE_NHL2_NOS_STUB_VA,
    .nos_exports = ngage_nhl2_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_nhl2_nos_exports),
    .screen_chunk_ptr = 0x800066ec,
    .nos_rx_dfc = 0x8000b6e4 + 0x200,   /* nos_standin.S RX_DFC / RX_FLAG with NHL2 */
    .nos_rx_flag = 0x8000b6e4 + 0x1f8,
    /*
     * The bootstrap's Intel probe (0x4F64) accepts 0x8872 (type 8) or 0x8876 (type 7); it loads
     * 0x8874 but never compares it [V]. Any other answer falls through to the AMD-style probes,
     * whose unlock cycles leave CFI01 out of read-array mode: the CPU then ran the flash's
     * status words from 0x507C on (undefined instruction at 0x200010). The EMIF set-up 0x51A8
     * then takes chip 0 type 7 or 12 only (else it spins at 0x51D4) [V]: 0x8876 (type 7).
     * medint.pdd places C: by chip 1's type: 4/8/9/13 -> 0x02000000, 4 MB (the bootstrap's bank
     * table maps exactly that), else 0x02600000, 2 MB, which faulted [V 0x5036CBE8]: chip 1 is
     * 0x8872 (type 8). Two different parts, as on the phone [I].
     */
    .flash_manuf = NGAGE_FLASH_MANUF, .flash_device = 0x8876, .flash_device1 = 0x8872,
    /* bootstrap 0x2A8C checks {start, end, MD5} entries at ROM 0x7FD0 (PPM) / 0x7FE8 (MCU,
     * 0x50009000-0x50EF9FFF, stored digest wrong as on the N-Gage) and halts at 0x2A84 [V] */
    .md5_entries = { 0x7fd0, 0x7fe8 },
    .uem_rtc = true,                    /* rtc.dll and cbus.dll are the 6600's code (relocated) [V] */
    .camera = NGAGE_CAM_NHL2,
    .irda = true,
    .slide = true,
};

/*
 * Nokia 3650 (NHL-8 V 04.17) and 3660 (NHL-8X V 05.08): the N-Gage's generation of kernel, not the 7650's (506 ekern
 * exports, the N-Gage's ordinals 469/394/493/7/479/64, bootstrap digests at ROM 0x8F00/0x8F18, three flash banks), so the
 * model is the RH-29's with the values read from these ROMs (statically; none of it has been run). NOS alias 0x51400000
 * as the N-Gage. Stub: nos_standin.S with the ROM's own ekern addresses (build.sh).
 * [U] nchips 3 (the ROM spans chips 0..2 of the bank table; chip count and flash IDs are the N-Gage's defaults),
 * the fatal-fault address in ngage_run.py, the 3660's RGB565 screen (3650: 12-bit as the N-Gage).
 */
static const uint32_t ngage_nhl8_nos_exports[][2] = {
    { 0x514a1440, NGAGE_NHL8_NOS_SYM_nos_channel_ctor },
    { 0x514a0ac4, NGAGE_NHL8_NOS_SYM_nos_channel_open },
};

static const NgageModel ngage_nhl8 = {
    .name = "nokia3650",
    .nchips = 3,
    .rom_map = { { 0, 0x00700000, 0 }, { 0x00700000, 0x00400000, 1 }, { 0x00b00000, 0x006e0000, 2 } },
    .nrom_map = 3,
    .nos_alias = 0x51400000,
    .stub = ngage_nhl8_nos_stub, .stub_len = sizeof(ngage_nhl8_nos_stub), .stub_va = NGAGE_NHL8_NOS_STUB_VA,
    .nos_exports = ngage_nhl8_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_nhl8_nos_exports),
    .screen_chunk_ptr = 0x800066ec,
    .nos_rx_dfc = 0x8000be38,           /* nos_standin.S RX_DFC / RX_FLAG with this ROM's RAM base */
    .nos_rx_flag = 0x8000be30,
    .flash_manuf = NGAGE_FLASH_MANUF, .flash_device = NGAGE_FLASH_DEVICE,
    .uem_rtc = true,
    .camera = NGAGE_CAM_NHL8,           /* camerapdd: the 7650's receiver, its own sensor (docs/.../camera.md) */
    .irda = true,
};

static const uint32_t ngage_nhl8x_nos_exports[][2] = {
    { 0x514a147c, NGAGE_NHL8X_NOS_SYM_nos_channel_ctor },
    { 0x514a0b00, NGAGE_NHL8X_NOS_SYM_nos_channel_open },
};

static const NgageModel ngage_nhl8x = {
    .name = "nokia3660",
    .nchips = 3,
    .rom_map = { { 0, 0x00700000, 0 }, { 0x00700000, 0x00400000, 1 }, { 0x00b00000, 0x006d0000, 2 } },
    .nrom_map = 3,
    .nos_alias = 0x51400000,
    .stub = ngage_nhl8x_nos_stub, .stub_len = sizeof(ngage_nhl8x_nos_stub), .stub_va = NGAGE_NHL8X_NOS_STUB_VA,
    .nos_exports = ngage_nhl8x_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_nhl8x_nos_exports),
    .screen_chunk_ptr = 0x800066ec,
    .screen_rgb565 = true,              /* [U] Wikipedia: 16-bit display */
    .nos_rx_dfc = 0x8000bfa0,
    .nos_rx_flag = 0x8000bf98,
    /*
     * The 3660's product_info (ROM 0x50414330) reads item 14 and accepts the codes 0x2186404, 0x2186468, 0x21864cc,
     * 0x2186530 and 0x2186594 (all lead to the same state 3) or value/100 in {0, 1004, 4400}; with 0 it reads item 15
     * and, for an index of 0 or 1, ends in the "product_" panic (reason 0) at 0x50414ac8 [V static]. Which of the
     * five is this phone's is not known: the lowest, as any of them takes the same path [U].
     */
    .nos_item14 = 0x2186404,
    .flash_manuf = NGAGE_FLASH_MANUF, .flash_device = NGAGE_FLASH_DEVICE,
    .uem_rtc = true,
    .camera = NGAGE_CAM_NHL8,           /* as the 3650 (sensor variant 1: register 0xC007 != 0x36) */
    .irda = true,
};

/* the NOS exports enosdriver.ldd imports (hidden module 0x504128B0, nos_standin_nhl10.S) */
static const uint32_t ngage_nhl10_nos_exports[][2] = {
    { 0x528887e8, NGAGE_NHL10_NOS_SYM_nos_export1 },
    { 0x528887b8, NGAGE_NHL10_NOS_SYM_nos_export2 },
    { 0x5288877c, NGAGE_NHL10_NOS_SYM_nos_export4 },
};

static const NgageModel ngage_nhl10 = {
    .name = "nokia6600",
    .uem_rtc = true,
    .camera = NGAGE_CAM_NHL2,           /* campdd.pdd: the 7650's hardware, sensor at 0x3C (docs/.../camera.md) */
    .irda = true,                       /* euart1.pdd is SIR.PDD, COMM::1 of irda.esk (docs/research/6600-irda.md) */
    .nchips = 4,
    .rom_map = { { 0, 0x00800000, 0 }, { 0x00800000, 0x00700000, 2 }, { 0x00f00000, 0x00800000, 3 } },
    .nrom_map = 3,
    .nos_alias = 0x52800000,
    .stub = ngage_nhl10_nos_stub, .stub_len = sizeof(ngage_nhl10_nos_stub),
    .stub_va = NGAGE_NHL10_NOS_STUB_VA,
    /* the screen chunk object (export 65 stores it there, Plat screen info reads +24) [V];
     * 32-byte header, then RGB565 pixels (white = 0xFFFF) [V] */
    .screen_chunk_ptr = 0x80001780, .screen_rgb565 = true,
    .nos_exports = ngage_nhl10_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_nhl10_nos_exports),
    .trace_mask_or = 0x40000000,
    .nos_rx_dfc = 0x80006f44,           /* nos_standin_nhl10.S RX_DFC / RX_FLAG */
    .nos_rx_flag = 0x80006f3c,
    /*
     * medint.pdd (switch 0x5036c32c) supports flash types 0, 2, 3, 9, 10, 15 (AMD) and 11, 12
     * (ST 0x88C0 / 0x88BE), not Intel (type 4: Fault FLASHDD 5) [V]. Its ST driver (0x5036b018..)
     * uses the Intel command set (0x70, 0x20/0xD0, 0x40, 0x50, 0xFF, 0x60/0xD0, 0x90) [V], which
     * CFI01 implements. The bootstrap's ID table (ROM 0x74E0) gives type 11 for ST 0x20/0x88C0,
     * read with the Intel-style query [V]. Which ST part the phone really has: [I].
     */
    .flash_manuf = 0x20, .flash_device = 0x88c0,
};

/*
 * Nokia 3230 (RM-51 V 05.07, S60 2nd Edition FP1, Symbian 7.0s), docs/devices/nokia-3230-rm51/bring-up.md:
 * the 6600's generation (same primary modules, 478 ekern exports in the same order). Three chips at
 * n << 25 (the bootstrap's loop checks 0..2) [V]: ROM 0..16 MB in chip 0, the rest in chip 1 (PPM at
 * flash 0x02B00000) [V: bank table ROM 0x64C]. The NOS stand-in is the 6600's with this phone's
 * addresses (build.sh); the hidden NOS module (header 0x50455BF0) exports 1, 2, 4 [V].
 */
static const uint32_t ngage_rm51_nos_exports[][2] = {
    { 0x5289aa88, NGAGE_RM51_NOS_SYM_nos_export1 },
    { 0x5289aa58, NGAGE_RM51_NOS_SYM_nos_export2 },
    { 0x5289aa1c, NGAGE_RM51_NOS_SYM_nos_export4 },
};

static const NgageModel ngage_rm51 = {
    .name = "nokia3230",
    .uem_rtc = true,
    .camera = NGAGE_CAM_RM51,           /* campdd: a camera module behind a command port (I2C 0x30) */
    .nchips = 3,
    .rom_map = { { 0, 0x01000000, 0 }, { 0x01000000, 0x00b00000, 1 } },
    .nrom_map = 2,
    .nos_alias = 0x52800000,            /* nosstarter: entry 0x528943A4, data 0x80002230 [V] */
    .stub = ngage_rm51_nos_stub, .stub_len = sizeof(ngage_rm51_nos_stub),
    .stub_va = NGAGE_RM51_NOS_STUB_VA,
    .screen_chunk_ptr = 0x80001780,     /* as the 6600 (4 users agree) */
    .screen_rgb565 = true,
    .nos_exports = ngage_rm51_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_rm51_nos_exports),
    .trace_mask_or = 0x40000000,
    .nos_rx_dfc = 0x8000755c + 0x200,   /* the stub's RX_DFC / RX_FLAG (RAM = the NOS bss) */
    .nos_rx_flag = 0x8000755c + 0x1f8,
    /*
     * The bootstrap accepts chip types 2, 8, 10, 13-17 only (table ROM 0x7A60; ID table 0x7910) and medint's
     * type switch (0x503959BC) has no ST/Intel types: Samsung 0xEC/0x22FA = type 14 [V tables; I which
     * part the phone has], AMD command set (the bootstrap's fallback ID read 0x5FC0 unlocks at 0x555/0x2AA)
     */
    .flash_manuf = 0xec, .flash_device = 0x22fa, .flash_amd = true,
    .irda = true,
};

/*
 * Nokia 7610 (RH-51) and 6670 (RH-67), V 4.20 (S60 2nd Edition FP1, Symbian 7.0s): one core image
 * (RH51_52_67_68PE706420.C1), a phone's own PPM (.26); docs/devices/nokia-7610-rh51/bring-up.md.
 * ekern is the 3230's byte for byte, 0x22000 lower (kernel-data layout identical) [V]; the bootstrap's chip
 * ID and combination tables are the 3230's [V]. Bank table: ROM 0..16 MB in chip 0, 9 MB in chip 1 (PPM at
 * flash 0x02A00000), an 8 MB device window at 0x04000000 (C:) [V: bootstrap table ROM 0x650].
 * NOS: nosstarter entry 0x528929E4, data 0x80002230 (0x5054 bytes from 0x52B0357C), bss 0x80007284 + 0xC55D0;
 * the hidden NOS module (header 0x5042D620) exports 1, 2, 4 [V].
 */
static const uint32_t ngage_rh51_nos_exports[][2] = {
    { 0x52898f10, NGAGE_RH51_NOS_SYM_nos_export1 },
    { 0x52898ee0, NGAGE_RH51_NOS_SYM_nos_export2 },
    { 0x52898ea4, NGAGE_RH51_NOS_SYM_nos_export4 },
};

static const NgageModel ngage_rh51 = {
    .name = "nokia7610",
    .uem_rtc = true,
    .camera = NGAGE_CAM_CAMX,           /* camxpdd: sensor 0x3C, 44-byte frame header, JPEG stills [V] */
    .nchips = 3,
    .rom_map = { { 0, 0x01000000, 0 }, { 0x01000000, 0x00900000, 1 } },
    .nrom_map = 2,
    .nos_alias = 0x52800000,
    .stub = ngage_rh51_nos_stub, .stub_len = sizeof(ngage_rh51_nos_stub),
    .stub_va = NGAGE_RH51_NOS_STUB_VA,
    .screen_chunk_ptr = 0x80001780,     /* the 3230's kernel data [V same ekern] */
    .screen_rgb565 = true,
    .nos_exports = ngage_rh51_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_rh51_nos_exports),
    .trace_mask_or = 0x40000000,
    .nos_rx_dfc = 0x80007284 + 0x200,   /* the stub's RX_DFC / RX_FLAG (RAM = the NOS bss) */
    .nos_rx_flag = 0x80007284 + 0x1f8,
    .flash_manuf = 0xec, .flash_device = 0x22fa, .flash_amd = true,  /* as the 3230 (same tables) [V tables] */
};

static NgageModel ngage_rh67;           /* the 6670: ngage_rh51 under its own name (same core image) */

/*
 * Nokia 6260 (RM-25), V 5.0536 (S60 2nd Edition FP1, Symbian 7.0s): core image RM25PE505360.C1, PPM .26;
 * docs/devices/nokia-6260-rm25/bring-up.md. ekern is the 3230's byte for byte, 0x4000 higher (kernel-data
 * layout identical) [V]. Bank table as the 7610's: ROM 0..16 MB in chip 0, 13 MB in chip 1 (PPM at flash
 * 0x02D00000), an 8 MB device window at 0x04000000 (C:) [V: bootstrap table ROM 0x650].
 * NOS: nosstarter entry 0x52894194, data 0x80002230 (0x5314 bytes from 0x52B286A4), bss 0x80007544 + 0xC6568;
 * the hidden NOS module (header 0x504573F0) exports 1, 2, 4 [V].
 */
static const uint32_t ngage_rm25_nos_exports[][2] = {
    { 0x5289a6c0, NGAGE_RM25_NOS_SYM_nos_export1 },
    { 0x5289a690, NGAGE_RM25_NOS_SYM_nos_export2 },
    { 0x5289a654, NGAGE_RM25_NOS_SYM_nos_export4 },
};

static const NgageModel ngage_rm25 = {
    .name = "nokia6260",
    .uem_rtc = true,
    .camera = NGAGE_CAM_RM25,           /* campdd.pdd: the 6600's driver (128 sensor registers), same hardware;
                                           viewfinder 176x144 and stills in YUV, shown turned 90 degrees [V run] */
    .nchips = 3,
    .rom_map = { { 0, 0x01000000, 0 }, { 0x01000000, 0x00d00000, 1 } },
    .nrom_map = 2,
    .nos_alias = 0x52800000,
    .stub = ngage_rm25_nos_stub, .stub_len = sizeof(ngage_rm25_nos_stub),
    .stub_va = NGAGE_RM25_NOS_STUB_VA,
    .screen_chunk_ptr = 0x80001780,     /* the 3230's kernel data [V same ekern] */
    .screen_rgb565 = true,
    .nos_exports = ngage_rm25_nos_exports, .nnos_exports = ARRAY_SIZE(ngage_rm25_nos_exports),
    .trace_mask_or = 0x40000000,
    .nos_rx_dfc = 0x80007544 + 0x200,   /* the stub's RX_DFC / RX_FLAG (RAM = the NOS bss) */
    .nos_rx_flag = 0x80007544 + 0x1f8,
    .flash_manuf = 0xec, .flash_device = 0x22fa, .flash_amd = true,  /* as the 3230 [I] */
    .irda = true,
};

static void ngage_init(MachineState *machine)
{
    MemoryRegion *sys = get_system_memory();
    NgageUnknown *u = g_new0(NgageUnknown, 1);
    ARMCPU *cpu;
    char *fn;
    gsize len;
    gchar *data;
    GError *err = NULL;

    cpu = ARM_CPU(cpu_create(machine->cpu_type));

    u->mem = g_hash_table_new(g_direct_hash, g_direct_equal);
    u->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&u->mr, NULL, &ngage_unknown_ops, u, "ngage.unknown", 4 * GiB);
    memory_region_add_subregion_overlap(sys, 0, &u->mr, -1000);

    if (!machine->firmware) {
        error_report("%s: give the phone's core ROM image with -bios ROM.bin", ngm->name);
        exit(1);
    }
    fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!fn || !g_file_get_contents(fn, &data, &len, &err)) {
        error_report("ngage: cannot read %s: %s", machine->firmware, err ? err->message : "not found");
        exit(1);
    }
    {
        const NgageRomRange *last = &ngm->rom_map[ngm->nrom_map - 1];
        if (len > last->rom_off + last->len) {
            error_report("%s: %s is %zu bytes, more than the ROM ranges hold (0x%x)", ngm->name, fn,
                         (size_t)len, last->rom_off + last->len);
            exit(1);
        }
    }
    ngage_load_segments((uint8_t *)data, len);     /* PPM boot block (Q28) */
    ngage_apply_patches((uint8_t *)data, len);
    /* H-nos-standin: the stub over the NOS entry (virtual nos_alias + flash offset, Q19) */
    memcpy(data + (ngm->stub_va - ngm->nos_alias), ngm->stub, ngm->stub_len);
    info_report("%s: H-nos-standin: NOS entry %08x replaced by the stand-in stub (%zu bytes)",
                ngm->name, ngm->stub_va, ngm->stub_len);
    for (unsigned i = 0; i < ngm->nnos_exports; i++) {
        uint8_t *p = (uint8_t *)data + (ngm->nos_exports[i][0] - ngm->nos_alias);
        stl_le_p(p, 0xe51ff004);                /* ldr pc, [pc, #-4] */
        stl_le_p(p + 4, ngm->nos_exports[i][1]);
    }
    if (ngm->trace_mask_or && len >= 0xdc) {
        /* H-nhl10-trace: the kernel copies the header's trace mask to the superpage (Q18) */
        uint32_t m = ldl_le_p(data + 0xd8);
        stl_le_p(data + 0xd8, m | ngm->trace_mask_or);
        info_report("%s: trace mask %08x -> %08x (kernel debug port on)", ngm->name, m,
                    m | ngm->trace_mask_or);
    }
    ngage_fix_md5_table((uint8_t *)data, len);

    /*
     * ROM.bin is the image as the MMU sees it (virtual 0x50000000); on the phone it is split over
     * the chips (Q12, the bootstrap's bank table): ngm->rom_map.
     */
    for (unsigned c = 0; c < ngm->nchips; c++) {
        g_autofree char *nm = g_strdup_printf("ngage.flash%u", c);
        const void *img = NULL;
        size_t n = 0;

        for (unsigned r = 0; r < ngm->nrom_map; r++) {
            if (ngm->rom_map[r].chip == c && len > ngm->rom_map[r].rom_off) {
                img = data + ngm->rom_map[r].rom_off;
                n = MIN(len - ngm->rom_map[r].rom_off, ngm->rom_map[r].len);
            }
        }
        ngage_flash_chip[c] = ngage_flash_ptr(ngage_flash((hwaddr)c << 25, nm, img, n, c));
    }
    ngage_load_segments(NULL, 0);                                         /* Q28 */
    if (ngage_flash_file) {
        ngage_flash_persist();                                            /* Q55 */
    }
    g_free(data);
    g_free(fn);

    memory_region_add_subregion(sys, NGAGE_SDRAM_BASE, machine->ram);   /* Q3 */

    {
        NgageCbus *c = g_new0(NgageCbus, 1);
        memory_region_init_io(&c->mr, NULL, &ngage_cbus_ops, c, "ngage.cbus", 0x8);
        memory_region_add_subregion(sys, NGAGE_CBUS_BASE, &c->mr);       /* Q11 */
    }

    {
        NgageIntc *ic = g_new0(NgageIntc, 1);
        NgageTimers *t = g_new0(NgageTimers, 1);
        qemu_irq *in;

        ic->irq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ);
        ic->fiq = qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ);
        memory_region_init_io(&ic->mr, NULL, &ngage_intc_ops, ic, "ngage.intc", 0x100);
        memory_region_add_subregion(sys, NGAGE_INTC_BASE, &ic->mr);       /* Q16 */
        in = qemu_allocate_irqs(ngage_intc_set, ic, 128);

        for (int i = 0; i < NGAGE_TIMERS; i++) {
            t->t[i].owner = t;
            t->t[i].index = i;
            t->t[i].t = timer_new_ns(QEMU_CLOCK_VIRTUAL, ngage_timer_expire, &t->t[i]);
            t->t[i].irq = in[i];                                         /* IrqTimer<i> */
        }
        ngage_keys_init(sys, in[25], ngm == &ngage_rm51 ? NGAGE_KPD_RM51
            : ngm == &ngage_nem4 ? NGAGE_KPD_NEM4
            : ngm == &ngage_rm25 ? NGAGE_KPD_RM25
            : (ngm == &ngage_rh51 || ngm == &ngage_rh67) ? NGAGE_KPD_RH51
            : (ngm == &ngage_nhl10 || ngm == &ngage_nhl2 || ngm == &ngage_nhl8) ? NGAGE_KPD_NHL10
            : (ngm == &ngage_nhl8x) ? NGAGE_KPD_NHL8X
            : NGAGE_KPD_NGAGE);  /* 7650: the 6600's ekeyb (94% same code, same key lists) [V] */                       /* keypad, IrqCTSI_IRQ (Q53) */
        ngage_lprf_setup();                                 /* Bluetooth, CBus dev 5, CTSI 2 */
        ngage_mmc_init(sys, in, ngage_mmc_file);            /* MMC 21-24, DMA channels 6.. [I] */
        if (ngm->camera != NGAGE_CAM_NONE) {
            ngage_cam_setup(sys, in[CAM_IRQ]);              /* camera (IrqCCPStatus) */
        }
        if (ngm->irda) {
            ngage_sir_setup(sys, in[NGAGE_SIR_IRQ]);        /* IrDA SIR UART (FiqCTSI_FIQ) */
        }
        memory_region_init_io(&t->mr, NULL, &ngage_timer_ops, t, "ngage.timers",
                              0x20 * NGAGE_TIMERS);
        memory_region_add_subregion(sys, NGAGE_TIMER_BASE, &t->mr);       /* Q13 Q17 Q31 */
    }

    {
        NgageDbg *d = g_new0(NgageDbg, 1);
        d->line = g_string_new(NULL);
        if (serial_hd(0)) {
            qemu_chr_fe_init(&d->chr, serial_hd(0), &error_abort);
        }
        memory_region_init_io(&d->mr, NULL, &ngage_dbg_ops, d, "ngage.dbg", NGAGE_DBG_SIZE);
        memory_region_add_subregion(sys, NGAGE_DBG_BASE, &d->mr);          /* Q18 */
    }

    {
        NgageNos *n = g_new0(NgageNos, 1);
        memory_region_init_io(&n->mr, NULL, &ngage_nos_ops, n, "ngage.nos", 0x400);
        ngage_isi_load(ngage_isi_file);                                    /* Q50 */
        memory_region_add_subregion(sys, NGAGE_NOS_BASE, &n->mr);          /* H-nos-standin */
    }

    {
        NgageLcdIf *l = g_new0(NgageLcdIf, 1);
        l->x1 = NGAGE_LCD_W - 1;
        l->y1 = NGAGE_LCD_H - 1;
        l->con = qemu_graphic_console_create(NULL, 0, &ngage_lcd_gfx_ops, l);
        qemu_console_resize(l->con, NGAGE_LCD_W, NGAGE_LCD_H);
        memory_region_init_io(&l->mr, NULL, &ngage_lcdif_ops, l, "ngage.lcdif", 0x10);
        memory_region_add_subregion(sys, NGAGE_LCDIF_BASE, &l->mr);         /* Q37 */
    }

    ngage_binfo.ram_size = machine->ram_size;
    ngage_binfo.loader_start = NGAGE_SDRAM_BASE;
    ngage_binfo.firmware_loaded = true;
    arm_load_kernel(cpu, machine, &ngage_binfo);
}

static char *ngage_get_ppm(Object *obj, Error **errp)
{
    return g_strdup(ngage_ppm_dir ? ngage_ppm_dir : "");
}

static void ngage_set_ppm(Object *obj, const char *value, Error **errp)
{
    g_free(ngage_ppm_dir);
    ngage_ppm_dir = *value ? g_strdup(value) : NULL;
}

static char *ngage_get_isi(Object *obj, Error **errp)
{
    return g_strdup(ngage_isi_file ? ngage_isi_file : "");
}

static void ngage_set_isi(Object *obj, const char *value, Error **errp)
{
    g_free(ngage_isi_file);
    ngage_isi_file = *value ? g_strdup(value) : NULL;
}

static char *ngage_get_flash(Object *obj, Error **errp)
{
    return g_strdup(ngage_flash_file ? ngage_flash_file : "");
}

static void ngage_set_flash(Object *obj, const char *value, Error **errp)
{
    g_free(ngage_flash_file);
    ngage_flash_file = *value ? g_strdup(value) : NULL;
}

static char *ngage_get_mmc(Object *obj, Error **errp)
{
    return g_strdup(ngage_mmc_file ? ngage_mmc_file : "");
}

static void ngage_set_mmc(Object *obj, const char *value, Error **errp)
{
    g_free(ngage_mmc_file);
    ngage_mmc_file = *value ? g_strdup(value) : NULL;
}

static void ngage_init_rh29(MachineState *machine)
{
    ngm = &ngage_rh29;
    ngage_init(machine);
}

static void ngage_init_nem4(MachineState *machine)
{
    ngm = &ngage_nem4;
    ngage_init(machine);
}

static void ngage_init_nhl2(MachineState *machine)
{
    ngm = &ngage_nhl2;
    ngage_init(machine);
}

static void ngage_init_nhl8(MachineState *machine)
{
    ngm = &ngage_nhl8;
    ngage_init(machine);
}

static void ngage_init_nhl8x(MachineState *machine)
{
    ngm = &ngage_nhl8x;
    ngage_init(machine);
}

static void ngage_init_nhl10(MachineState *machine)
{
    ngm = &ngage_nhl10;
    ngage_init(machine);
}

static void ngage_init_rm51(MachineState *machine)
{
    ngm = &ngage_rm51;
    ngage_init(machine);
}

static void ngage_init_rh51(MachineState *machine)
{
    ngm = &ngage_rh51;
    ngage_init(machine);
}

static uint8_t ngage_swivel = 1;                /* machine property swivel (6260), before the machine runs */
static bool ngage_swivel_live;

static void ngage_init_rm25(MachineState *machine)
{
    ngm = &ngage_rm25;
    ngage_init(machine);
    ngage_keys_swivel(ngage_swivel);
    ngage_swivel_live = true;
}

static void ngage_init_rh67(MachineState *machine)
{
    ngage_rh67 = ngage_rh51;
    ngage_rh67.name = "nokia6670";
    ngm = &ngage_rh67;
    ngage_init(machine);
}

static void ngage_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "mmc", ngage_get_mmc, ngage_set_mmc);
    object_class_property_set_description(oc, "mmc",
        "memory card image (MBR + FAT16, eka1/tools/ngage_card.py), the phone's E:");

    object_class_property_add_str(oc, "flash", ngage_get_flash, ngage_set_flash);
    object_class_property_set_description(oc, "flash",
        "the phone's own flash: changed blocks saved here at exit and applied at start");

    object_class_property_add_str(oc, "isi", ngage_get_isi, ngage_set_isi);
    object_class_property_set_description(oc, "isi",
        "ISI reply table for the NOSDRIVER channels (reply RES REQ RESP BYTES...)");

    object_class_property_add_str(oc, "ppm", ngage_get_ppm, ngage_set_ppm);
    object_class_property_set_description(oc, "ppm",
        "directory of extracted PPM flash segments (segment_*_<flash address>.bin)");

    mc->desc = "Nokia N-Gage (NEM-4, DCT4 UPP, Symbian 6.1 EKA1) bring-up board";
    mc->init = ngage_init_nem4;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("ti925t");
    mc->default_ram_size = 16 * MiB;           /* Q3 */
    mc->default_ram_id = "ngage.sdram";
}

static const TypeInfo ngage_machine_type = {
    .name = MACHINE_TYPE_NAME("ngage"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

/* N-Gage QD: the same board and options, its own NgageModel */
static void ngage_rh29_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    mc->desc = "Nokia N-Gage QD (RH-29, DCT4 UPP, Symbian 6.1 EKA1) bring-up board";
    mc->init = ngage_init_rh29;
}

static const TypeInfo ngage_rh29_machine_type = {
    .name = MACHINE_TYPE_NAME("ngageqd"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_rh29_class_init,
    .interfaces = arm_machine_interfaces,
};

/* Nokia 6600 */
static void ngage_nhl10_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    mc->desc = "Nokia 6600 (NHL-10, DCT4 UPP, Symbian 7.0s EKA1) bring-up board";
    mc->init = ngage_init_nhl10;
}

static const TypeInfo ngage_nhl10_machine_type = {
    .name = MACHINE_TYPE_NAME("nokia6600"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_nhl10_class_init,
    .interfaces = arm_machine_interfaces,
};

/* Nokia 7650: its keypad slide (the camera lens is behind it), open unless slide-closed=on */
static bool ngage_get_slide(Object *obj, Error **errp)
{
    return ngage_cam_timer ? ngage_keys_slide_closed() : ngage_slide_closed;
}

static void ngage_set_slide(Object *obj, bool value, Error **errp)
{
    ngage_slide_closed = value;
    if (ngage_cam_timer) {                          /* the machine runs: move it now */
        ngage_keys_slide(true, value);
    }
}

static void ngage_nhl2_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    object_class_property_add_bool(oc, "slide-closed", ngage_get_slide, ngage_set_slide);
    object_class_property_set_description(oc, "slide-closed",
        "the keypad slide is closed (covers the camera; F7 toggles it)");
    mc->desc = "Nokia 7650 (NHL-2NA, DCT4 UPP, Symbian 6.1 EKA1) bring-up board";
    mc->init = ngage_init_nhl2;
}

static const TypeInfo ngage_nhl2_machine_type = {
    .name = MACHINE_TYPE_NAME("nokia7650"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_nhl2_class_init,
    .interfaces = arm_machine_interfaces,
};

/* Nokia 3650 */
static void ngage_nhl8_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    mc->desc = "Nokia 3650 (NHL-8, DCT4 UPP, Symbian 6.1 EKA1) bring-up board";
    mc->init = ngage_init_nhl8;
}

static const TypeInfo ngage_nhl8_machine_type = {
    .name = MACHINE_TYPE_NAME("nokia3650"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_nhl8_class_init,
    .interfaces = arm_machine_interfaces,
};

/* Nokia 3660 */
static void ngage_nhl8x_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    mc->desc = "Nokia 3660 (NHL-8X, DCT4 UPP, Symbian 6.1 EKA1) bring-up board";
    mc->init = ngage_init_nhl8x;
}

static const TypeInfo ngage_nhl8x_machine_type = {
    .name = MACHINE_TYPE_NAME("nokia3660"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_nhl8x_class_init,
    .interfaces = arm_machine_interfaces,
};

/* Nokia 3230 */
static void ngage_rm51_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    mc->desc = "Nokia 3230 (RM-51, DCT4 UPP, S60 2nd FP1, Symbian 7.0s EKA1) bring-up board";
    mc->init = ngage_init_rm51;
}

static const TypeInfo ngage_rm51_machine_type = {
    .name = MACHINE_TYPE_NAME("nokia3230"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_rm51_class_init,
    .interfaces = arm_machine_interfaces,
};

/* Nokia 7610 and 6670 (one core image) */
static void ngage_rh51_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    mc->desc = "Nokia 7610 (RH-51, DCT4 UPP, S60 2nd FP1, Symbian 7.0s EKA1) bring-up board";
    mc->init = ngage_init_rh51;
}

static const TypeInfo ngage_rh51_machine_type = {
    .name = MACHINE_TYPE_NAME("nokia7610"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_rh51_class_init,
    .interfaces = arm_machine_interfaces,
};

static void ngage_rh67_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    mc->desc = "Nokia 6670 (RH-67, DCT4 UPP, S60 2nd FP1, Symbian 7.0s EKA1; the 7610's core) bring-up board";
    mc->init = ngage_init_rh67;
}

static const TypeInfo ngage_rh67_machine_type = {
    .name = MACHINE_TYPE_NAME("nokia6670"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_rh67_class_init,
    .interfaces = arm_machine_interfaces,
};

/* Nokia 6260: its swivel/fold position, ekeyb's 0-5 (H-rm25-switches; 1 = all sensor lines high) */
static void ngage_get_swivel(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t p = ngage_swivel_live ? ngage_keys_swivel_pos() : ngage_swivel;

    visit_type_uint8(v, name, &p, errp);
}

static void ngage_set_swivel(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t p;

    if (!visit_type_uint8(v, name, &p, errp)) {
        return;
    }
    if (p >= NGAGE_SWIVEL_POSITIONS) {
        error_setg(errp, "swivel: position 0-%d", NGAGE_SWIVEL_POSITIONS - 1);
        return;
    }
    ngage_swivel = p;
    if (ngage_swivel_live) {
        ngage_keys_swivel(p);
    }
}

static void ngage_rm25_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ngage_machine_class_init(oc, data);
    object_class_property_add(oc, "swivel", "uint8", ngage_get_swivel, ngage_set_swivel, NULL, NULL);
    object_class_property_set_description(oc, "swivel",
        "the swivel/fold position as ekeyb decodes it, 0-5 (scan codes 0xB6, B7, B8, BB, BA, B9; 1 at power-on)");
    mc->desc = "Nokia 6260 (RM-25, DCT4 UPP, S60 2nd FP1, Symbian 7.0s EKA1) bring-up board";
    mc->init = ngage_init_rm25;
}

static const TypeInfo ngage_rm25_machine_type = {
    .name = MACHINE_TYPE_NAME("nokia6260"),
    .parent = TYPE_MACHINE,
    .class_init = ngage_rm25_class_init,
    .interfaces = arm_machine_interfaces,
};

static void ngage_machine_init(void)
{
    type_register_static(&ngage_rm25_machine_type);
    type_register_static(&ngage_rh51_machine_type);
    type_register_static(&ngage_rh67_machine_type);
    type_register_static(&ngage_machine_type);
    type_register_static(&ngage_rh29_machine_type);
    type_register_static(&ngage_nhl2_machine_type);
    type_register_static(&ngage_nhl8_machine_type);
    type_register_static(&ngage_nhl8x_machine_type);
    type_register_static(&ngage_nhl10_machine_type);
    type_register_static(&ngage_rm51_machine_type);
}

type_init(ngage_machine_init)
