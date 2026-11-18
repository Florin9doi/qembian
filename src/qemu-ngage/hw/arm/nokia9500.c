/*
 * Nokia 9500 Communicator (RA-2, S80 2nd edition, Symbian 7.0s / EKA1) bring-up board: -M nokia9500.
 * docs/devices/nokia-9500-ra2/bring-up.md has the evidence for every value here.
 *
 * The SoC is QEMU's own OMAP310/1510 model (hw/arm/omap1.c, as -M sx1): interrupt controllers,
 * timers, clocks, UARTs, LCD controller, MMC, GPIO. This file adds the phone around it:
 *   0x00000000  CS0: a two-word boot stub standing in for the DiskOnChip loader (H-ra2-loader)
 *   0x10000000  SDRAM, 64 MB (bootstrap bank table, ROM 0x7F0) [V]
 *   0x10600000  the core ROM image (the C record of RA2n6270.c01, unpacked) [V load address]
 *   0x11800000  the variant image (V record of .l18) [V load address]
 *   0xFFFB0800  UART2 replaced by the XBUS link to a modelled modem (ra2_uart, nokia9500_xbus.c)
 *   I2C 0x56    the keyboard controller: keys and the lid (nokia9500_kbd.c; machine properties
 *               cover-open, kbd-byte)
 *   Microwire CS0: the cover display (nokia9500_cover.c, its own graphic console; the inner panel
 *               is the OMAP LCD controller's console 0)
 *   CS1..CS3    catch-all: every unmodelled access logged ("nokia9500:"), reads return the last
 *               write (so polling loops and probes show up)
 * Options: -M nokia9500,ppm=DIR[,isi=FILE]  DIR = eka1/ra2/images (ra2_extract.py output), FILE = the
 *          modem's ISI reply table (ngage_isi.c format)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/arm/omap.h"
#include "hw/core/boards.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "target/arm/cpu.h"
#include "qemu/cutils.h"
#include "system/reset.h"
#include "system/memory.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "ngage_isi.h"
#include "nokia9500_xbus.h"
#include "nokia9500_cover.h"
#include "nokia9500_kbd.h"

#define RA2_SDRAM_SIZE   (64 * MiB)
#define RA2_CORE_LOAD    0x10600000
#define RA2_VARIANT_LOAD 0x11800000

static char *ra2_images;            /* -M nokia9500,ppm=DIR */
static char *ra2_isi;               /* -M nokia9500,isi=FILE */

/* ---- catch-all for unmodelled chip selects (as ngage.c's) ---------------------------------- */

typedef struct {
    MemoryRegion mr;
    hwaddr base;
    GHashTable *mem;
    GHashTable *seen;
} Ra2Unknown;

static uint32_t ra2_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void ra2_unknown_log(Ra2Unknown *u, hwaddr addr, bool wr, uint64_t val, unsigned size)
{
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(u->seen, k));

    if (n < 16) {
        g_hash_table_insert(u->seen, k, GUINT_TO_POINTER(n + 1));
        qemu_log_mask(LOG_UNIMP, "ngage: %s %08" HWADDR_PRIx " %u %08" PRIx64 " pc=%08x%s\n",
                      wr ? "W" : "R", u->base + addr, size, val, ra2_pc(),
                      n + 1 == 16 ? " (further accesses not logged)" : "");
    }
}

static uint64_t ra2_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    Ra2Unknown *u = opaque;
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem, GUINT_TO_POINTER((guint)(addr & ~3u))));
    uint64_t v;

    if (u->base + (addr & ~3u) == 0x1038) {
        /* DiskOnChip status: MedDoc's power callback (0x500611B4) waits for bit 0 [V static] */
        w |= 1;
    }
    v = (w >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
    ra2_unknown_log(u, addr, false, v, size);
    return v;
}

static void ra2_unknown_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ra2Unknown *u = opaque;
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem, k));
    unsigned sh = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;

    w = (w & ~mask) | (((uint32_t)val << sh) & mask);
    g_hash_table_insert(u->mem, k, GUINT_TO_POINTER(w));
    ra2_unknown_log(u, addr, true, val, size);
}

static const MemoryRegionOps ra2_unknown_ops = {
    .read = ra2_unknown_read,
    .write = ra2_unknown_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void ra2_unknown(MemoryRegion *sys, hwaddr base, uint64_t size, const char *name, int prio)
{
    Ra2Unknown *u = g_new0(Ra2Unknown, 1);

    u->base = base;
    u->mem = g_hash_table_new(g_direct_hash, g_direct_equal);
    u->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&u->mr, NULL, &ra2_unknown_ops, u, name, size);
    memory_region_add_subregion_overlap(sys, base, &u->mr, prio);
}

/* ---- block device for the TrueFFS stand-in (H-ra2-doc) ------------------------------------------ */
/*
 * The phone keeps its ROFS, ROFx and C: on a DiskOnChip behind M-Systems' TrueFFS (DiskOnChip.dll).
 * Until the DiskOnChip itself is modelled, a stand-in for DiskOnChip.dll's API asks this device for
 * sectors. Registers (32-bit, at RA2_BLK_BASE):
 *   +00 W command: 1 read, 2 write, 3 info (size of the partition into +18)
 *   +04 partition, +08 first sector (512 bytes), +0C sector count, +10 buffer (virtual, the caller's)
 *   +14 R result: 0 ok, -1 bad partition, -2 out of range, -3 buffer not mapped
 *   +18 R partition size in sectors (after info)
 * Partitions (MedDoc.pdd's TrueFFS numbering [V static]): 0 = C: (FAT16, 163808 sectors [V boot
 * sector]; EFile mounts it: a blank partition 0 gave KErrCorrupt and EStart's format query), 1 = a
 * 1 KB directory (count at +0x28, entries of 0x28 bytes from +0x34: id, byte offset, sectors) then
 * ROFS (id 5) and ROFx (id 6) back to back, 2 = only sized, blank [I]. Writes stay in memory.
 */
#define RA2_BLK_BASE 0xfff0f000      /* in the kernel's peripheral mapping (virtual 0x5800F000) */

typedef struct {
    const char *file;               /* under ppm=: "RA2n6270.c01/r02-" prefix of a .bin */
    const char *prefix;
    uint32_t sectors;               /* 0 = the image's size */
    uint8_t *data;
    uint32_t len;
} Ra2Part;

static Ra2Part ra2_parts[] = {
    { "RA2n6270.u18", "r01-", 163808 },         /* 0: C: (EFile reads its boot sector) [V] */
    { NULL, NULL, 0 },                          /* 1: directory + ROFS + ROFx (ra2_part1) */
    { NULL, NULL, 2048 },                       /* 2: asked for its size only; blank [I] */
};

typedef struct {
    MemoryRegion mr;
    uint32_t r[8];
    unsigned logged;           /* commands logged (-d unimp), at most 400 */
} Ra2Blk;

static bool ra2_read_bin(const char *file, const char *prefix, gchar **buf, gsize *len)
{
    g_autofree char *dir = g_build_filename(ra2_images, file, NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;
    bool ok = false;

    while (d && !ok && (name = g_dir_read_name(d))) {
        if (g_str_has_prefix(name, prefix) && g_str_has_suffix(name, ".bin")) {
            g_autofree char *path = g_build_filename(dir, name, NULL);
            ok = g_file_get_contents(path, buf, len, NULL);
        }
    }
    if (d) {
        g_dir_close(d);
    }
    return ok;
}

/* partition 1: the directory MedDoc reads (sectors 0-1), then ROFS (id 5) and ROFx (id 6) */
static void ra2_part1(Ra2Part *p)
{
    g_autofree gchar *rofs = NULL, *rofx = NULL;
    gsize lr, lx;
    uint8_t *dir;

    if (!ra2_read_bin("RA2n6270.c01", "r02-", &rofs, &lr) ||
        !ra2_read_bin("RA2n6270.l18", "r06-", &rofx, &lx)) {
        error_report("nokia9500: ROFS / ROFx images missing in %s", ra2_images);
        exit(1);
    }
    p->len = 0x400 + lr + lx;
    p->data = g_malloc0(p->len);
    dir = p->data;
    dir[0x28] = 2;
    stl_le_p(dir + 0x34, 5);
    stl_le_p(dir + 0x38, 0x400);
    stl_le_p(dir + 0x3c, lr / 512);
    stl_le_p(dir + 0x5c, 6);
    stl_le_p(dir + 0x60, 0x400 + lr);
    stl_le_p(dir + 0x64, lx / 512);
    memcpy(p->data + 0x400, rofs, lr);
    memcpy(p->data + 0x400 + lr, rofx, lx);
    info_report("nokia9500: block partition 1: ROFS %zu + ROFx %zu bytes", (size_t)lr, (size_t)lx);
}

static void ra2_part_load(Ra2Part *p)
{
    if (!p->file) {
        if (p->sectors) {
            p->len = p->sectors * 512;
            p->data = g_malloc0(p->len);
        } else {
            ra2_part1(p);
        }
        return;
    }
    g_autofree char *dir = g_build_filename(ra2_images, p->file, NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;

    while (d && (name = g_dir_read_name(d))) {
        if (g_str_has_prefix(name, p->prefix) && g_str_has_suffix(name, ".bin")) {
            g_autofree char *path = g_build_filename(dir, name, NULL);
            gchar *buf;
            gsize len;

            if (g_file_get_contents(path, &buf, &len, NULL)) {
                uint32_t total = MAX(len, (gsize)p->sectors * 512);

                p->data = g_malloc0(total);
                memcpy(p->data, buf, len);
                p->len = total;
                g_free(buf);
                info_report("nokia9500: block partition %s -> %u sectors", name, total / 512);
            }
        }
    }
    if (d) {
        g_dir_close(d);
    }
}

static uint64_t ra2_blk_read(void *opaque, hwaddr addr, unsigned size)
{
    Ra2Blk *b = opaque;

    return addr < sizeof(b->r) ? b->r[addr >> 2] : 0;
}

static void ra2_blk_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Ra2Blk *b = opaque;
    uint32_t part, sec, cnt, va;
    Ra2Part *p;

    if (addr >= sizeof(b->r)) {
        return;
    }
    b->r[addr >> 2] = val;
    if (addr != 0) {
        return;
    }
    part = b->r[1];
    sec = b->r[2];
    cnt = b->r[3];
    va = b->r[4];
    if (b->logged < 400) {
        b->logged++;
        qemu_log_mask(LOG_UNIMP, "nokia9500-blk: cmd %u part %u sector %u count %u buf %08x\n",
                      (unsigned)val, part, sec, cnt, va);
    }
    if (part >= ARRAY_SIZE(ra2_parts) || !ra2_parts[part].data) {
        b->r[5] = -1;
        return;
    }
    p = &ra2_parts[part];
    if (val == 3) {
        b->r[6] = p->len / 512;
        b->r[5] = 0;
        return;
    }
    if ((uint64_t)(sec + cnt) * 512 > p->len) {
        b->r[5] = -2;
        qemu_log_mask(LOG_UNIMP, "nokia9500-blk: part %u sector %u+%u out of range\n", part, sec, cnt);
        return;
    }
    if (!current_cpu || cpu_memory_rw_debug(current_cpu, va, p->data + (uint64_t)sec * 512,
                                            (uint64_t)cnt * 512, val == 1)) {
        b->r[5] = -3;
        return;
    }
    b->r[5] = 0;
}

static const MemoryRegionOps ra2_blk_ops = {
    .read = ra2_blk_read, .write = ra2_blk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---- images ---------------------------------------------------------------------------------- */

/* load the one file in DIR/SUB whose name starts with PREFIX and contains TAG at SDRAM address */
static size_t ra2_load(const char *sub, const char *prefix, const char *tag, hwaddr addr)
{
    g_autofree char *dir = g_build_filename(ra2_images, sub, NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;
    size_t n = 0;

    if (!d) {
        error_report("nokia9500: ppm=%s: no %s", ra2_images, sub);
        exit(1);
    }
    while ((name = g_dir_read_name(d))) {
        if (g_str_has_prefix(name, prefix) && strstr(name, tag) && g_str_has_suffix(name, ".img")) {
            g_autofree char *path = g_build_filename(dir, name, NULL);
            g_autofree char *buf = NULL;
            gsize len;

            if (!g_file_get_contents(path, &buf, &len, NULL)) {
                error_report("nokia9500: cannot read %s", path);
                exit(1);
            }
            address_space_write_rom(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, buf, len);
            info_report("nokia9500: %s -> %08" HWADDR_PRIx " (%zu bytes)", name, addr, (size_t)len);
            n = len;
            break;
        }
    }
    g_dir_close(d);
    if (!n) {
        error_report("nokia9500: no %s*%s*.img in %s", prefix, tag, dir);
        exit(1);
    }
    return n;
}

/*
 * H-ra2-loader: on the phone a loader reads the images from the DiskOnChip into SDRAM and starts
 * the core's bootstrap [I: load addresses in the flash file; MedDoc.pdd]. Until the DiskOnChip is
 * modelled, the board puts the images there itself and CS0 holds two words: ldr pc, [pc, #-4];
 * .word 0x10600000 (the core's reset vector).
 */
static const uint32_t ra2_boot_stub[] = { 0xe51ff004, RA2_CORE_LOAD };

#include "ngage_ra2_doc_stub.h"

/* ROM virtual address -> its physical address in the SDRAM copy of the core */
#define RA2_CORE_PHYS(va) (RA2_CORE_LOAD + ((va) - 0x50000000))

static void ra2_put32(uint32_t va, uint32_t w)
{
    uint8_t b[4];

    stl_le_p(b, w);
    address_space_write_rom(&address_space_memory, RA2_CORE_PHYS(va), MEMTXATTRS_UNSPECIFIED, b, 4);
}

static uint32_t ra2_branch(uint32_t from, uint32_t to)
{
    return 0xea000000 | (((to - (from + 8)) >> 2) & 0x00ffffff);
}

/*
 * H-ra2-doc: DiskOnChip.dll's exports (directory 0x5007F224, each a one-word thunk at 0x50065F20.. [V]) branch into the TrueFFS stand-in,
 * copied over the DLL's text after its entry point (0x500623D4, a single branch the kernel calls).
 */
static void ra2_doc_standin(void)
{
    static const uint32_t exports[][2] = {
        { 0x50065f20, NGAGE_RA2_DOC_NOS_SYM_doc_flcall },      /* ord 1 flCall */
        { 0x50065f28, NGAGE_RA2_DOC_NOS_SYM_doc_ord2 },        /* ord 2 (a branch to 0x50068154) */
        { 0x50065f24, NGAGE_RA2_DOC_NOS_SYM_doc_exit },        /* ord 3 flExit */
        { 0x50065f3c, NGAGE_RA2_DOC_NOS_SYM_doc_setenv },      /* ord 4 flSetEnvAll */
        { 0x50065f2c, NGAGE_RA2_DOC_NOS_SYM_doc_setbase },     /* ord 5 setDocBase */
    };

    address_space_write_rom(&address_space_memory, RA2_CORE_PHYS(NGAGE_RA2_DOC_NOS_STUB_VA),
                            MEMTXATTRS_UNSPECIFIED, ngage_ra2_doc_nos_stub,
                            sizeof(ngage_ra2_doc_nos_stub));
    for (unsigned i = 0; i < ARRAY_SIZE(exports); i++) {
        ra2_put32(exports[i][0], ra2_branch(exports[i][0], exports[i][1]));
    }
    info_report("nokia9500: H-ra2-doc: DiskOnChip.dll exports -> TrueFFS stand-in");
}

/*
 * H-ra2-dsp-boot: no DSP is emulated. Ehwaomap.pdd (TI DSP/BIOS Bridge) starts the DSP, writes 0xFFFF
 * to a shared-memory word and spins until the DSP's boot code clears it (0x507F60C8..0x507F60D4, no
 * timeout) [V: docs/research/ra2-dsp-bridge.md]; the loop's branch becomes a no-op, so the start
 * returns as if the DSP had booted.
 */
static void ra2_dsp_boot_patch(void)
{
    static const uint32_t va = 0x507f60d4, old = 0x1afffffb, nop = 0xe1a00000;
    uint8_t b[4];

    address_space_read(&address_space_memory, RA2_CORE_PHYS(va), MEMTXATTRS_UNSPECIFIED, b, 4);
    if (ldl_le_p(b) != old) {
        warn_report("nokia9500: H-ra2-dsp-boot: %08x holds %08x, not %08x; not patched", va,
                    ldl_le_p(b), old);
        return;
    }
    ra2_put32(va, nop);
}

#include "ngage_ra2_bal_stub.h"

/*
 * H-ra2-bal: Bal.dll's DSP node and stream calls succeed without a DSP. The stand-in goes over the
 * bodies of exports 2/13/3 (0x50925ECC..) and the export directory (0x50926BC0) points there; each
 * slot's old value is checked first [V: docs/research/ra2-dsp-bridge.md section 5].
 */
#define RA2_DMS_READY_B 0x50935e68
#define RA2_DMS_OPEN_RETRY 0x50935324

/* Dasfgwc: send-and-wait-for-the-DSP thunk, reply parser (first word checked) */
static const struct {
    uint32_t va, old;
} ra2_dasf_ok[] = {
    { 0x5092ad6c, 0xe59fc004 },
    { 0x5092a5d0, 0xe92d4030 },
};

static void ra2_bal_standin(void)
{
    static const struct {
        unsigned ord;
        uint32_t old, to;
    } slots[] = {
        { 2, 0x50925ecc, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 3, 0x50926120, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 4, 0x50926580, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 5, 0x509262ac, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 8, 0x509264ac, NGAGE_RA2_BAL_NOS_SYM_bal_attr },
        { 9, 0x50925ea4, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 10, 0x509265e0, NGAGE_RA2_BAL_NOS_SYM_bal_msg_cancel },
        { 11, 0x509265a0, NGAGE_RA2_BAL_NOS_SYM_bal_msg_get },
        { 13, 0x50925fe8, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 14, 0x50926560, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 15, 0x50926598, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 17, 0x5092646c, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 18, 0x50926578, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 19, 0x50926240, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 22, 0x50926308, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 23, 0x509263b8, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 24, 0x5092642c, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 25, 0x50926504, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 26, 0x509266c0, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
        { 27, 0x509266d0, NGAGE_RA2_BAL_NOS_SYM_bal_ok },
    };
    uint8_t b[4];

    for (unsigned i = 0; i < ARRAY_SIZE(slots); i++) {
        uint32_t slot = 0x50926bc0 + 4 * (slots[i].ord - 1);

        address_space_read(&address_space_memory, RA2_CORE_PHYS(slot), MEMTXATTRS_UNSPECIFIED, b, 4);
        if (ldl_le_p(b) != slots[i].old) {
            warn_report("nokia9500: H-ra2-bal: Bal export %u is %08x, not %08x; not patched",
                        slots[i].ord, ldl_le_p(b), slots[i].old);
            return;
        }
    }
    address_space_write_rom(&address_space_memory, RA2_CORE_PHYS(NGAGE_RA2_BAL_NOS_STUB_VA),
                            MEMTXATTRS_UNSPECIFIED, ngage_ra2_bal_nos_stub,
                            sizeof(ngage_ra2_bal_nos_stub));
    for (unsigned i = 0; i < ARRAY_SIZE(slots); i++) {
        ra2_put32(0x50926bc0 + 4 * (slots[i].ord - 1), slots[i].to);
    }
    /* DMS 0x50935E68 `b 0x5093731C` -> `b dms_ready` (drops a "ready" with no owner yet) */
    address_space_read(&address_space_memory, RA2_CORE_PHYS(RA2_DMS_READY_B), MEMTXATTRS_UNSPECIFIED, b, 4);
    if (ldl_le_p(b) != 0xea00052b) {
        warn_report("nokia9500: H-ra2-bal: DMS 0x%08x is %08x, not b 0x5093731c; not patched",
                    RA2_DMS_READY_B, ldl_le_p(b));
        return;
    }
    ra2_put32(RA2_DMS_READY_B, ra2_branch(RA2_DMS_READY_B, NGAGE_RA2_BAL_NOS_SYM_dms_ready));
    /*
     * DMS 0x50935268 opens the audio plug-in until it succeeds (`bne` at 0x50935324). Without a DSP
     * the plug-in's first command (sample rate, Dasfgwc 0x5092A7B4) waits for a DSP reply message
     * and fails with -39, the open with -13, for ever: DMS accepts the failure once (no audio).
     */
    address_space_read(&address_space_memory, RA2_CORE_PHYS(RA2_DMS_OPEN_RETRY), MEMTXATTRS_UNSPECIFIED, b, 4);
    if (ldl_le_p(b) != 0x1afffff4) {
        warn_report("nokia9500: H-ra2-bal: DMS 0x%08x is %08x, not the open retry; not patched",
                    RA2_DMS_OPEN_RETRY, ldl_le_p(b));
        return;
    }
    ra2_put32(RA2_DMS_OPEN_RETRY, 0xe1a00000);  /* nop */

    /*
     * Dasfgwc.dll (the audio plug-ins' DSP stream wrapper): every command (sample rate, channels,
     * volume, ...) goes through the thunk 0x5092AD6C (send to the node and wait for the DSP's reply
     * message) and the reply parser 0x5092A5D0. Without a DSP no reply comes (-39): both return 0,
     * the command is accepted. The audio plug-in then opens and DevSound users (Wserv's key click,
     * CuiWs' tone player) start.
     */
    for (unsigned i = 0; i < ARRAY_SIZE(ra2_dasf_ok); i++) {
        address_space_read(&address_space_memory, RA2_CORE_PHYS(ra2_dasf_ok[i].va),
                           MEMTXATTRS_UNSPECIFIED, b, 4);
        if (ldl_le_p(b) != ra2_dasf_ok[i].old) {
            warn_report("nokia9500: H-ra2-bal: Dasfgwc 0x%08x is %08x, not %08x; not patched",
                        ra2_dasf_ok[i].va, ldl_le_p(b), ra2_dasf_ok[i].old);
            return;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ra2_dasf_ok); i++) {
        ra2_put32(ra2_dasf_ok[i].va, 0xe3a00000);       /* mov r0, #0 */
        ra2_put32(ra2_dasf_ok[i].va + 4, 0xe12fff1e);   /* bx lr */
    }
}

static void ra2_reset(void *opaque)
{
    ra2_load("RA2n6270.c01", "r01-", "C", RA2_CORE_LOAD);
    ra2_load("RA2n6270.l18", "r09-", "V", RA2_VARIANT_LOAD);
    ra2_doc_standin();
    ra2_dsp_boot_patch();
    ra2_bal_standin();
}

/*
 * ---- UART2: the XBUS link to the modem -----------------------------------------------------------
 * The phone's own XbusDma.pdd drives OMAP UART2 with system DMA both ways (RX channel synced to
 * request 15, TX to 14), polls CTS (MSR bit 4) as "modem awake", and takes MPUIO 3 as the modem's
 * "I have data" edge [V: docs/research/ra2-xbus-modem.md]. QEMU's omap_uart is a bare 16550 without
 * the OMAP registers or DMA requests, so this device sits over it: 16550 core + the OMAP extras as
 * storage, DMA requests from the FIFO state, and nokia9500_xbus.c as the far end.
 * Flow control: the modem drops CTS when a byte arrives and raises it again 100 us later (about
 * a byte's time on the wire), with a modem-status interrupt; the PDD's UART interrupt handler (0x500B4898) takes "CTS high
 * again" as the end of a transmit and calls XbusProtocol's TxDone [V]. Its 70 ms fallback timer
 * never expires while the phone resends sync bytes every 5 ms. CTS is high otherwise (the modem is
 * awake; the GPIO 8 wake pulse needs no answer). The modem's answer (an ACK, a reply) goes out
 * 500 us after CTS is back: XbusProtocol looks at an ACK only once TxDone has put its transmitter in
 * the waiting-for-ACK state (0x500B6508) [V].
 */
#define RA2_UART2_BASE 0xfffb0800

typedef struct {
    MemoryRegion mr;
    qemu_irq irq, drq_tx, drq_rx, data_line;
    QEMUTimer *tick;
    Ra2Xbus x;
    uint8_t ier, lcr, mcr, fcr, scr, efr, dll, dlh, spr, tcr, tlr, mdr1, xon[4], reg4c;
    bool thr_int;                   /* THR-empty interrupt pending (cleared by reading IIR) */
    bool cts;                       /* the modem's CTS (MSR bit 4) */
    uint8_t msr_delta;              /* MSR bits 0-3 (DCTS ...), cleared by reading MSR */
    QEMUTimer *flow;                /* CTS back up, then the modem may answer */
    int64_t answer_at;              /* ns: the modem's bytes wait until then (and CTS high) */
    bool msr_pending;               /* CTS rose; the phone has not read MSR (its TxDone) yet */
    int64_t cts_rose;               /* ns */
    int acked_frame;                /* the modem DATA frame the ACK timer runs for, or -1 */
    int64_t ack_deadline;           /* ms */
    bool up;                        /* the phone set DMA mode: the link driver is running */
    unsigned logged;                /* bytes logged (-d unimp), at most 200000 */
    int64_t next_sync;
    int64_t out_since;              /* ms: modem bytes waiting since (stall diagnostic), or -1 */
    bool stall_logged;
    struct soc_dma_s *dma;          /* for the stall diagnostic (patch 0009) */
} Ra2Uart;

static Ra2Uart *ra2_uart;

static bool ra2_uart_dma(Ra2Uart *u)
{
    return (u->scr & 1) ? ((u->scr >> 1) & 3) != 0 : (u->fcr & 8) != 0;
}

static bool ra2_uart_answering(Ra2Uart *u)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /*
     * After the phone's last byte the answer waits until the phone has seen CTS come back (it reads
     * MSR in its UART interrupt and calls XbusProtocol's TxDone, 0x500B4898) and 500 us more:
     * XbusProtocol takes an ACK only after TxDone ([link+0x30] 4/5, 0x500B6508) [V]. A fixed 500 us
     * after the CTS edge was too early for a slowed guest (regression xbus_ack_before_txdone). The
     * PDD's own 70 ms TxDone timer is the fallback (ra2_xbus_may_answer).
     */
    return ra2_xbus_may_answer(u->cts, u->msr_pending, now, u->cts_rose, u->answer_at);
}

static unsigned ra2_uart_iir(Ra2Uart *u)
{
    if ((u->ier & 1) && ra2_xbus_ready(&u->x) && ra2_uart_answering(u) && !ra2_uart_dma(u)) {
        return 0x04;                /* RX data */
    }
    if ((u->ier & 2) && u->thr_int) {
        return 0x02;                /* THR empty */
    }
    if ((u->ier & 8) && u->msr_delta) {
        return 0x00;                /* modem status */
    }
    return 0x01;
}

static void ra2_uart_update(Ra2Uart *u)
{
    bool dma = ra2_uart_dma(u);

    qemu_set_irq(u->drq_tx, dma);
    qemu_set_irq(u->drq_rx, dma && ra2_xbus_ready(&u->x) && ra2_uart_answering(u));
    qemu_set_irq(u->irq, ra2_uart_iir(u) != 0x01);
}

static void ra2_uart_set_cts(Ra2Uart *u, bool on)
{
    if (u->cts != on) {
        u->cts = on;
        u->msr_delta |= 1;          /* DCTS */
    }
}

static void ra2_uart_flow(void *opaque)
{
    Ra2Uart *u = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!u->cts) {
        ra2_uart_set_cts(u, true);              /* the modem took the byte */
        u->msr_pending = true;
        u->cts_rose = now;
        u->answer_at = MAX(u->answer_at, now + 500 * SCALE_US);
        timer_mod(u->flow, u->answer_at);
    }
    ra2_uart_update(u);
}

static void ra2_uart_tick(void *opaque)
{
    Ra2Uart *u = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint32_t before = ra2_xbus_pending(&u->x);


    if (u->up) {
        if (!u->x.synced) {
            if (now >= u->next_sync && !ra2_xbus_pending(&u->x)) {
                ra2_xbus_poll(&u->x);           /* SYNC, every 200 ms until the phone answers */
                u->next_sync = now + 200;
            }
        } else {
            if (u->x.wait_ack >= 0 && u->x.wait_ack != u->acked_frame) {
                u->acked_frame = u->x.wait_ack;     /* a new modem DATA frame: start its ACK timer */
                u->ack_deadline = now + 100;
            } else if (u->x.wait_ack < 0) {
                u->acked_frame = -1;
            } else if (now >= u->ack_deadline && !ra2_xbus_pending(&u->x)) {
                ra2_xbus_ack_timeout(&u->x);        /* no ACK in 100 ms: send it again */
                u->ack_deadline = now + 100;
            }
            ra2_xbus_poll(&u->x);               /* the next ISI message, once the last is ACKed */
        }
    }
    if (!ra2_xbus_pending(&u->x)) {
        u->out_since = -1;
        u->stall_logged = false;
    } else if (u->out_since < 0) {
        u->out_since = now;
    } else if (now - u->out_since >= 50 && !u->stall_logged) {
        /* bytes for the phone not taken for 50 ms: say why (DMA off, CTS, receive not armed) */
        u->stall_logged = true;
        qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: %u bytes unread for %" PRId64 " ms at %" PRId64
                      " ms: dma %d cts %d msr_pending %d ready %d answering %d ier %02x scr %02x fcr %02x"
                      " wait_ack %02x want %u rxdma %x txdma %x\n",
                      ra2_xbus_pending(&u->x), now - u->out_since, now, ra2_uart_dma(u), u->cts,
                      u->msr_pending, ra2_xbus_ready(&u->x), ra2_uart_answering(u), u->ier, u->scr,
                      u->fcr, u->x.wait_ack & 0xff, u->x.want,
                      omap_dma_sync_state(u->dma, OMAP_DMA_UART2_RX),
                      omap_dma_sync_state(u->dma, OMAP_DMA_UART2_TX));
    }
    if (ra2_xbus_pending(&u->x) > before) {
        /* MPUIO 3: the modem has data (edge) */
        qemu_irq_raise(u->data_line);
        qemu_irq_lower(u->data_line);
    }
    ra2_uart_update(u);
    timer_mod(u->tick, now + 5);
}

static uint64_t ra2_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    Ra2Uart *u = opaque;
    uint8_t b;

    switch (addr >> 2) {
    case 0:
        if (u->lcr & 0x80) {
            return u->dll;
        }
        b = 0;
        if (ra2_uart_answering(u)) {
            bool end;

            if (ra2_xbus_pop(&u->x, &b, &end)) {
                u->out_since = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
                if (u->logged < 200000) {
                    u->logged++;
                    qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: -> phone %02x\n", b);
                }
                if (end) {
                    /* a gap between frames: one receive per frame on the phone's side */
                    u->answer_at = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * SCALE_MS;
                    timer_mod(u->flow, u->answer_at);
                }
            }
        }
        ra2_uart_update(u);
        return b;
    case 1:
        return (u->lcr & 0x80) ? u->dlh : u->ier;
    case 2:
        if (u->lcr == 0xbf) {
            return u->efr;
        } else {
            unsigned iir = ra2_uart_iir(u);

            if (iir == 0x02) {
                u->thr_int = false;
                ra2_uart_update(u);
            }
            return iir | ((u->fcr & 1) ? 0xc0 : 0);
        }
    case 3:
        return u->lcr;
    case 4:
        return u->lcr == 0xbf ? u->xon[0] : u->mcr;
    case 5:                                     /* LSR: THR and shifter always empty */
        return u->lcr == 0xbf ? u->xon[1]
                              : ra2_xbus_lsr(ra2_uart_dma(u), ra2_xbus_ready(&u->x) && ra2_uart_answering(u));
    case 6:                                     /* MSR: CTS, DSR, DCD; or TCR */
        if (u->lcr == 0xbf || (u->mcr & 0x40 && (u->efr & 0x10))) {
            return u->lcr == 0xbf ? u->xon[2] : u->tcr;
        }
        {
            uint8_t msr = 0xa0 | (u->cts ? 0x10 : 0) | u->msr_delta;    /* DCD, DSR, CTS */

            if (u->cts && u->msr_pending) {
                /* the phone sees CTS back: its TxDone runs now; answer 500 us later */
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

                u->msr_pending = false;
                u->answer_at = MAX(u->answer_at, now + 500 * SCALE_US);
                timer_mod(u->flow, u->answer_at);
            }
            u->msr_delta = 0;
            ra2_uart_update(u);
            return msr;
        }
    case 7:
        if (u->lcr == 0xbf || (u->mcr & 0x40 && (u->efr & 0x10))) {
            return u->lcr == 0xbf ? u->xon[3] : u->tlr;
        }
        return u->spr;
    case 0x8:
        return u->mdr1;
    case 0x10:
        return u->scr;
    case 0x11:                                  /* SSR: TX FIFO never full */
        return 0;
    case 0x13:
        return u->reg4c;
    case 0x16:                                  /* SYSS: reset done */
        return 1;
    default:
        return 0;
    }
}

static void ra2_uart_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    Ra2Uart *u = opaque;

    switch (addr >> 2) {
    case 0:
        if (u->lcr & 0x80) {
            u->dll = v;
        } else {
            if (u->logged < 200000) {
                u->logged++;
                qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: <- phone %02x\n", (unsigned)v & 0xff);
            }
            ra2_xbus_from_phone(&u->x, v);
            ra2_uart_set_cts(u, false);  /* busy for a byte's time */
            timer_mod(u->flow, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * SCALE_US);
            u->thr_int = true;
        }
        break;
    case 1:
        if (u->lcr & 0x80) {
            u->dlh = v;
        } else {
            u->ier = v;
            u->thr_int = v & 2;
        }
        break;
    case 2:
        if (u->lcr == 0xbf) {
            u->efr = v;
        } else {
            u->fcr = v;
        }
        break;
    case 3:
        u->lcr = v;
        break;
    case 4:
        if (u->lcr == 0xbf) {
            u->xon[0] = v;
        } else {
            u->mcr = v;
        }
        break;
    case 5:
        if (u->lcr == 0xbf) {
            u->xon[1] = v;
        }
        break;
    case 6:
        if (u->lcr == 0xbf) {
            u->xon[2] = v;
        } else if (u->mcr & 0x40) {
            u->tcr = v;
        }
        break;
    case 7:
        if (u->lcr == 0xbf) {
            u->xon[3] = v;
        } else if (u->mcr & 0x40) {
            u->tlr = v;
        } else {
            u->spr = v;
        }
        break;
    case 0x8:
        u->mdr1 = v;
        break;
    case 0x10:
        u->scr = v;
        break;
    case 0x13:
        u->reg4c = v;
        break;
    }
    if (ra2_uart_dma(u) && !u->up) {
        u->up = true;
        u->next_sync = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 20;
        qemu_log_mask(LOG_UNIMP, "nokia9500-xbus: UART2 in DMA mode, link starting\n");
    }
    ra2_uart_update(u);
}

static const MemoryRegionOps ra2_uart_ops = {
    .read = ra2_uart_read, .write = ra2_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void ra2_uart_reset(void *opaque)
{
    Ra2Uart *u = opaque;

    ra2_xbus_reset(&u->x);
    u->ier = u->lcr = u->mcr = u->fcr = u->scr = u->efr = u->mdr1 = 0;
    u->thr_int = u->up = false;
    u->cts = true;
    u->msr_delta = 0;
    u->answer_at = 0;
    u->out_since = -1;
    u->stall_logged = false;
    u->msr_pending = false;
    timer_del(u->flow);
    u->acked_frame = -1;
    timer_mod(u->tick, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);
}

static void ra2_uart_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu)
{
    Ra2Uart *u = g_new0(Ra2Uart, 1);

    u->irq = qdev_get_gpio_in(mpu->ih[1], OMAP_INT_UART2);
    u->drq_tx = mpu->drq[OMAP_DMA_UART2_TX];
    u->drq_rx = mpu->drq[OMAP_DMA_UART2_RX];
    u->dma = mpu->dma;
    u->data_line = omap_mpuio_in_get(mpu->mpuio)[3];
    u->tick = timer_new_ms(QEMU_CLOCK_VIRTUAL, ra2_uart_tick, u);
    u->flow = timer_new_ns(QEMU_CLOCK_VIRTUAL, ra2_uart_flow, u);
    memory_region_init_io(&u->mr, NULL, &ra2_uart_ops, u, "nokia9500.uart2-xbus", 0x400);
    memory_region_add_subregion_overlap(sys, RA2_UART2_BASE, &u->mr, 10);
    if (ra2_isi) {
        ngage_isi_load(ra2_isi);
    }
    qemu_register_reset(ra2_uart_reset, u);
    ra2_uart = u;
}

static void nokia9500_init(MachineState *machine)
{
    MemoryRegion *sys = get_system_memory();
    MemoryRegion *boot = g_new(MemoryRegion, 1);
    struct omap_mpu_state_s *mpu;

    if (!ra2_images) {
        error_report("nokia9500: give the extracted images with -M nokia9500,ppm=eka1/ra2/images");
        exit(1);
    }
    memory_region_add_subregion(sys, OMAP_EMIFF_BASE, machine->ram);
    mpu = omap310_mpu_init(machine->ram, machine->cpu_type);
    ra2_uart_init(sys, mpu);
    ra2_cover_init(mpu);                /* the cover panel on Microwire CS0: graphic console 1 */
    ra2_kbd_init(OBJECT(machine), mpu); /* keyboard controller and lid on I2C 0x56, MPUIO 2 */
    ra2_codec_init(mpu);                /* TLV320AIC23 on I2C 0x1A */

    /* CS0: the boot stub, then the catch-all for the rest of the chip select */
    memory_region_init_rom(boot, NULL, "nokia9500.boot", 0x1000, &error_fatal);
    memcpy(memory_region_get_ram_ptr(boot), ra2_boot_stub, sizeof(ra2_boot_stub));
    memory_region_add_subregion(sys, OMAP_CS0_BASE, boot);
    ra2_unknown(sys, OMAP_CS0_BASE + 0x1000, OMAP_CS0_SIZE - 0x1000, "nokia9500.cs0", -10);
    ra2_unknown(sys, OMAP_CS1_BASE, 0x04000000, "nokia9500.cs1", -10);
    ra2_unknown(sys, OMAP_CS2_BASE, 0x04000000, "nokia9500.cs2", -10);
    ra2_unknown(sys, OMAP_CS3_BASE, 0x04000000, "nokia9500.cs3", -10);

    {
        Ra2Blk *b = g_new0(Ra2Blk, 1);

        for (unsigned i = 0; i < ARRAY_SIZE(ra2_parts); i++) {
            ra2_part_load(&ra2_parts[i]);
        }
        memory_region_init_io(&b->mr, NULL, &ra2_blk_ops, b, "nokia9500.blk", 0x100);
        memory_region_add_subregion_overlap(sys, RA2_BLK_BASE, &b->mr, 10);
    }
    qemu_register_reset(ra2_reset, NULL);
}

static char *ra2_get_ppm(Object *obj, Error **errp)
{
    return g_strdup(ra2_images ? ra2_images : "");
}

static void ra2_set_ppm(Object *obj, const char *value, Error **errp)
{
    g_free(ra2_images);
    ra2_images = *value ? g_strdup(value) : NULL;
}

static char *ra2_get_isi(Object *obj, Error **errp)
{
    return g_strdup(ra2_isi ? ra2_isi : "");
}

static void ra2_set_isi(Object *obj, const char *value, Error **errp)
{
    g_free(ra2_isi);
    ra2_isi = *value ? g_strdup(value) : NULL;
}

/* accepted for the ngage tools' command line; not used yet */
static char *ra2_get_none(Object *obj, Error **errp)
{
    return g_strdup("");
}

static void ra2_set_none(Object *obj, const char *value, Error **errp)
{
}

static void nokia9500_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "ppm", ra2_get_ppm, ra2_set_ppm);
    object_class_property_set_description(oc, "ppm", "eka1/ra2/images (ra2_extract.py output)");
    object_class_property_add_str(oc, "isi", ra2_get_isi, ra2_set_isi);
    object_class_property_set_description(oc, "isi", "the modem's ISI reply table (ngage_isi.c format)");
    object_class_property_add_str(oc, "flash", ra2_get_none, ra2_set_none);
    mc->desc = "Nokia 9500 Communicator (RA-2, OMAP1510, Symbian 7.0s EKA1) bring-up board";
    mc->init = nokia9500_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("ti925t");
    mc->default_ram_size = RA2_SDRAM_SIZE;
    mc->default_ram_id = "nokia9500.sdram";
}

static const TypeInfo nokia9500_type = {
    .name = MACHINE_TYPE_NAME("nokia9500"),
    .parent = TYPE_MACHINE,
    .class_init = nokia9500_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokia9500_register(void)
{
    type_register_static(&nokia9500_type);
}

type_init(nokia9500_register)
