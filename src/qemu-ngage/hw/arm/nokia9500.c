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
#include "qapi/visitor.h"
#include "hw/arm/omap.h"
#include "hw/core/boards.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "target/arm/cpu.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "system/reset.h"
#include "system/memory.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "ngage_isi.h"
#include "nokia9500_xbus.h"
#include "nokia9500_cover.h"
#include "nokia9500_kbd.h"
#include "nokia7710_input.h"
#include "nokia6630_soc.h"

#define RA2_SDRAM_SIZE   (64 * MiB)
#define RA2_CORE_LOAD    0x10600000     /* both phones' core ROM image (9500 C record, 7710 r06) [V] */

static char *ra2_images;            /* -M nokia9500,ppm=DIR */
static char *ra2_isi;               /* -M nokia9500,isi=FILE */
static char *ra2_flash;             /* -M nokia9500,flash=FILE: C: kept across boots */
static int ra2_flash_fd = -1;

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

static Ra2Part ra2_parts_9300[] = {
    { "RAE6n6270.u18", "r01-", 163808 },        /* 0: C: (EFile reads its boot sector) [V] */
    { NULL, NULL, 0 },                          /* 1: directory + ROFS + ROFx (ra2_part1) */
    { NULL, NULL, 2048 },                       /* 2: asked for its size only; blank [I] */
};

static Ra2Part ra2_parts_9300i[] = {
    { "RA8n6270.u18", "r01-", 163808 },         /* 0: C: (EFile reads its boot sector) [V] */
    { NULL, NULL, 0 },                          /* 1: directory + ROFS + ROFx (ra2_part1) */
    { NULL, NULL, 2048 },                       /* 2: asked for its size only; blank [I] */
};

static Ra2Part ra2_parts_9500[] = {
    { "RA2n6270.u18", "r01-", 163808 },         /* 0: C: (EFile reads its boot sector) [V] */
    { NULL, NULL, 0 },                          /* 1: directory + ROFS + ROFx (ra2_part1) */
    { NULL, NULL, 2048 },                       /* 2: asked for its size only; blank [I] */
};

/* Nokia 7710: the same TrueFFS / MedDoc scheme [I until the first run]; the factory C: is c1u r05 */
static Ra2Part ra2_parts_7710[] = {
    { "RM12mPR4.10.0.c1u", "r05-", 174048 },    /* 0: C: (FAT16, 174048 sectors in its boot sector; the record holds the first 240) [V] */
    { NULL, NULL, 0 },                          /* 1: directory + ROFS (no ROFx on this phone) */
    { NULL, NULL, 2048 },                       /* 2: blank [I] */
};

/*
 * One phone on this board. The 9500 and the 7710 share the OMAP1510, the XBUS modem link, the
 * DiskOnChip/TrueFFS scheme and the DSP bridge; they differ in images, addresses in the ROM and the
 * input / display hardware around the SoC (docs/devices/nokia-7710-rm12/bring-up.md).
 */
typedef struct {
    const char *machine;            /* "nokia9500" */
    const char *core_file, *core_prefix, *core_tag;
    const char *var_file, *var_prefix, *var_tag;
    hwaddr var_load;                /* the variant / extension ROM image in SDRAM */
    const char *rofs_file, *rofs_prefix, *rofx_file, *rofx_prefix;  /* partition 1 (rofx may be NULL) */
    Ra2Part *parts;
    unsigned nparts;
    const uint32_t (*doc_exports)[2];   /* DiskOnChip.dll export thunk -> stand-in symbol */
    unsigned ndoc_exports;
    const uint8_t *doc_stub;
    size_t doc_stub_len;
    uint32_t doc_stub_va;
    /*
     * The DSP bridge (Ehwaomap.pdd, BridgeApi), Bal.dll + Dasfgwc.dll and DeviceManagementServer are the
     * same binaries on both phones, relocated: the 9500's patch addresses + these deltas [V: word compare]
     */
    uint32_t ehwa_delta, bal_delta, dms_delta;
    const uint8_t *bal_stub;        /* bal_standin_ra2.S assembled at 0x50925ECC + bal_delta */
    size_t bal_stub_len;
    void (*patches)(void);          /* phone-specific ROM patches at reset */
    void (*devices)(MachineState *machine, struct omap_mpu_state_s *mpu);
} Ra2Phone;

static const Ra2Phone *ra2_phone;

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
    gsize lr, lr0, lx;
    uint8_t *dir;

    lx = 0;
    if (!ra2_read_bin(ra2_phone->rofs_file, ra2_phone->rofs_prefix, &rofs, &lr) ||
        (ra2_phone->rofx_file &&
         !ra2_read_bin(ra2_phone->rofx_file, ra2_phone->rofx_prefix, &rofx, &lx))) {
        error_report("%s: ROFS / ROFx images missing in %s", ra2_phone->machine, ra2_images);
        exit(1);
    }
    lr0 = lr;
    lr = ROUND_UP(lr, 512);                 /* the 7710's ROFS record is not a sector multiple */
    p->len = 0x400 + lr + lx;
    p->data = g_malloc0(p->len);
    dir = p->data;
    dir[0x28] = rofx ? 2 : 1;
    stl_le_p(dir + 0x34, 5);
    stl_le_p(dir + 0x38, 0x400);
    stl_le_p(dir + 0x3c, lr / 512);
    memcpy(p->data + 0x400, rofs, lr0);
    if (rofx) {
        stl_le_p(dir + 0x5c, 6);
        stl_le_p(dir + 0x60, 0x400 + lr);
        stl_le_p(dir + 0x64, lx / 512);
        memcpy(p->data + 0x400 + lr, rofx, lx);
    }
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
    if (part >= ra2_phone->nparts || !ra2_phone->parts[part].data) {
        b->r[5] = -1;
        return;
    }
    p = &ra2_phone->parts[part];
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
    if (val == 2 && part == 0 && ra2_flash_fd >= 0 &&
        pwrite(ra2_flash_fd, p->data + (uint64_t)sec * 512, (size_t)cnt * 512, (off_t)sec * 512) !=
        (ssize_t)cnt * 512) {
        warn_report("%s: flash file: write of sectors %u+%u failed", ra2_phone->machine, sec, cnt);
    }
    b->r[5] = 0;
}

/*
 * flash=FILE: C: (partition 0) kept across boots. The file is the partition's image (sparse); a file of
 * another size (or none) starts from the factory image, written there first. Each write is written
 * through, so the file is consistent whenever QEMU is stopped.
 */
static void ra2_flash_open(Ra2Part *p)
{
    struct stat st;

    if (!ra2_flash) {
        return;
    }
    ra2_flash_fd = open(ra2_flash, O_RDWR | O_CREAT, 0644);
    if (ra2_flash_fd < 0) {
        error_report("%s: flash=%s: %s", ra2_phone->machine, ra2_flash, strerror(errno));
        exit(1);
    }
    if (fstat(ra2_flash_fd, &st) == 0 && st.st_size == p->len &&
        pread(ra2_flash_fd, p->data, p->len, 0) == (ssize_t)p->len) {
        info_report("%s: C: from %s", ra2_phone->machine, ra2_flash);
        return;
    }
    /* new phone: the factory C:, written sparsely (zero 64 KB blocks skipped) */
    if (ftruncate(ra2_flash_fd, 0) || ftruncate(ra2_flash_fd, p->len)) {
        error_report("%s: flash=%s: cannot size it", ra2_phone->machine, ra2_flash);
        exit(1);
    }
    for (uint32_t o = 0; o < p->len; o += 0x10000) {
        uint32_t n = MIN(0x10000u, p->len - o);

        if (!buffer_is_zero(p->data + o, n) && pwrite(ra2_flash_fd, p->data + o, n, o) != n) {
            error_report("%s: flash=%s: write failed", ra2_phone->machine, ra2_flash);
            exit(1);
        }
    }
    info_report("%s: C: factory image -> %s", ra2_phone->machine, ra2_flash);
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
#include "ngage_rae6_doc_stub.h"
#include "ngage_ra8_doc_stub.h"

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

static const uint32_t ra2_doc_exports_9300[][2] = {
    { 0x50064ec0, NGAGE_RAE6_DOC_NOS_SYM_doc_flcall },     /* ord 1 flCall */
    { 0x50064ec8, NGAGE_RAE6_DOC_NOS_SYM_doc_ord2 },       /* ord 2 */
    { 0x50064ec4, NGAGE_RAE6_DOC_NOS_SYM_doc_exit },       /* ord 3 flExit */
    { 0x50064edc, NGAGE_RAE6_DOC_NOS_SYM_doc_setenv },     /* ord 4 flSetEnvAll */
    { 0x50064ecc, NGAGE_RAE6_DOC_NOS_SYM_doc_setbase },    /* ord 5 setDocBase */
};

static const uint32_t ra2_doc_exports_9300i[][2] = {
    { 0x50067a60, NGAGE_RA8_DOC_NOS_SYM_doc_flcall },      /* ord 1 flCall */
    { 0x50067a68, NGAGE_RA8_DOC_NOS_SYM_doc_ord2 },        /* ord 2 */
    { 0x50067a64, NGAGE_RA8_DOC_NOS_SYM_doc_exit },        /* ord 3 flExit */
    { 0x50067a7c, NGAGE_RA8_DOC_NOS_SYM_doc_setenv },      /* ord 4 flSetEnvAll */
    { 0x50067a6c, NGAGE_RA8_DOC_NOS_SYM_doc_setbase },     /* ord 5 setDocBase */
};

/*
 * H-ra2-doc: DiskOnChip.dll's exports (directory 0x5007F224, each a one-word thunk at 0x50065F20.. [V]) branch into the TrueFFS stand-in,
 * copied over the DLL's text after its entry point (0x500623D4, a single branch the kernel calls).
 */
static const uint32_t ra2_doc_exports_9500[][2] = {
    { 0x50065f20, NGAGE_RA2_DOC_NOS_SYM_doc_flcall },      /* ord 1 flCall */
    { 0x50065f28, NGAGE_RA2_DOC_NOS_SYM_doc_ord2 },        /* ord 2 (a branch to 0x50068154) */
    { 0x50065f24, NGAGE_RA2_DOC_NOS_SYM_doc_exit },        /* ord 3 flExit */
    { 0x50065f3c, NGAGE_RA2_DOC_NOS_SYM_doc_setenv },      /* ord 4 flSetEnvAll */
    { 0x50065f2c, NGAGE_RA2_DOC_NOS_SYM_doc_setbase },     /* ord 5 setDocBase */
};

/*
 * H-rm12-doc: the 7710's DiskOnChip.dll (0x500AC160) is another TrueFFS build with the same export
 * thunks (same words, export directory 0x500C5504) [V]; the 9500's stand-in, assembled at entry + 4.
 */
#include "ngage_rm12_doc_stub.h"
static const uint32_t ra2_doc_exports_7710[][2] = {
    { 0x500afd38, NGAGE_RM12_DOC_NOS_SYM_doc_flcall },     /* ord 1 flCall */
    { 0x500afd40, NGAGE_RM12_DOC_NOS_SYM_doc_ord2 },       /* ord 2 */
    { 0x500afd3c, NGAGE_RM12_DOC_NOS_SYM_doc_exit },       /* ord 3 flExit */
    { 0x500afd54, NGAGE_RM12_DOC_NOS_SYM_doc_setenv },     /* ord 4 flSetEnvAll */
    { 0x500afd44, NGAGE_RM12_DOC_NOS_SYM_doc_setbase },    /* ord 5 setDocBase */
};

static void ra2_doc_standin(void)
{
    const Ra2Phone *ph = ra2_phone;

    address_space_write_rom(&address_space_memory, RA2_CORE_PHYS(ph->doc_stub_va),
                            MEMTXATTRS_UNSPECIFIED, ph->doc_stub, ph->doc_stub_len);
    for (unsigned i = 0; i < ph->ndoc_exports; i++) {
        ra2_put32(ph->doc_exports[i][0], ra2_branch(ph->doc_exports[i][0], ph->doc_exports[i][1]));
    }
    info_report("%s: H-ra2-doc: DiskOnChip.dll exports -> TrueFFS stand-in", ph->machine);
}

/*
 * H-ra2-dsp-boot: no DSP is emulated. Ehwaomap.pdd (TI DSP/BIOS Bridge) starts the DSP, writes 0xFFFF
 * to a shared-memory word and spins until the DSP's boot code clears it (0x507F60C8..0x507F60D4, no
 * timeout) [V: docs/research/ra2-dsp-bridge.md]; the loop's branch becomes a no-op, so the start
 * returns as if the DSP had booted.
 */
static void ra2_dsp_boot_patch(void)
{
    const uint32_t va = 0x507f60d4 + ra2_phone->ehwa_delta, old = 0x1afffffb, nop = 0xe1a00000;
    uint8_t b[4];

    address_space_read(&address_space_memory, RA2_CORE_PHYS(va), MEMTXATTRS_UNSPECIFIED, b, 4);
    if (ldl_le_p(b) != old) {
        warn_report("%s: H-ra2-dsp-boot: %08x holds %08x, not %08x; not patched", ra2_phone->machine, va,
                    ldl_le_p(b), old);
        return;
    }
    ra2_put32(va, nop);
}

#include "ngage_ra2_bal_stub.h"
#include "ngage_rm12_bal_stub.h"
#include "ngage_rae6_bal_stub.h"
#include "ngage_ra8_bal_stub.h"

/*
 * H-ra2-bal: Bal.dll's DSP node and stream calls succeed without a DSP. The stand-in goes over the
 * bodies of exports 2/13/3 (0x50925ECC..) and the export directory (0x50926BC0) points there; each
 * slot's old value is checked first [V: docs/research/ra2-dsp-bridge.md section 5].
 */
#define RA2_DMS_READY_B (0x50935e68 + ra2_phone->dms_delta)
#define RA2_DMS_OPEN_RETRY (0x50935324 + ra2_phone->dms_delta)

/* Dasfgwc: send-and-wait-for-the-DSP thunk, reply parser (first word checked) */
static const struct {
    uint32_t va, old;
} ra2_dasf_ok[] = {
    { 0x5092ad6c, 0xe59fc004 },
    { 0x5092a5d0, 0xe92d4030 },
};

static void ra2_bal_standin(void)
{
    const uint32_t bd = ra2_phone->bal_delta;   /* Bal and Dasfgwc moved together [V] */
    const uint32_t dir = 0x50926bc0 + bd;
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
        uint32_t slot = dir + 4 * (slots[i].ord - 1);

        address_space_read(&address_space_memory, RA2_CORE_PHYS(slot), MEMTXATTRS_UNSPECIFIED, b, 4);
        if (ldl_le_p(b) != slots[i].old + bd) {
            warn_report("%s: H-ra2-bal: Bal export %u is %08x, not %08x; not patched",
                        ra2_phone->machine, slots[i].ord, ldl_le_p(b), slots[i].old + bd);
            return;
        }
    }
    address_space_write_rom(&address_space_memory, RA2_CORE_PHYS(NGAGE_RA2_BAL_NOS_STUB_VA + bd),
                            MEMTXATTRS_UNSPECIFIED, ra2_phone->bal_stub, ra2_phone->bal_stub_len);
    for (unsigned i = 0; i < ARRAY_SIZE(slots); i++) {
        ra2_put32(dir + 4 * (slots[i].ord - 1), slots[i].to + bd);
    }
    /* DMS 0x50935E68 `b 0x5093731C` -> `b dms_ready` (drops a "ready" with no owner yet) */
    address_space_read(&address_space_memory, RA2_CORE_PHYS(RA2_DMS_READY_B), MEMTXATTRS_UNSPECIFIED, b, 4);
    if (ldl_le_p(b) != 0xea00052b) {
        warn_report("%s: H-ra2-bal: DMS 0x%08x is %08x, not b 0x5093731c; not patched",
                    ra2_phone->machine, RA2_DMS_READY_B, ldl_le_p(b));
        return;
    }
    ra2_put32(RA2_DMS_READY_B, ra2_branch(RA2_DMS_READY_B, NGAGE_RA2_BAL_NOS_SYM_dms_ready + bd));
    /*
     * DMS 0x50935268 opens the audio plug-in until it succeeds (`bne` at 0x50935324). Without a DSP
     * the plug-in's first command (sample rate, Dasfgwc 0x5092A7B4) waits for a DSP reply message
     * and fails with -39, the open with -13, for ever: DMS accepts the failure once (no audio).
     */
    address_space_read(&address_space_memory, RA2_CORE_PHYS(RA2_DMS_OPEN_RETRY), MEMTXATTRS_UNSPECIFIED, b, 4);
    if (ldl_le_p(b) != 0x1afffff4) {
        warn_report("%s: H-ra2-bal: DMS 0x%08x is %08x, not the open retry; not patched",
                    ra2_phone->machine, RA2_DMS_OPEN_RETRY, ldl_le_p(b));
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
        address_space_read(&address_space_memory, RA2_CORE_PHYS(ra2_dasf_ok[i].va + bd),
                           MEMTXATTRS_UNSPECIFIED, b, 4);
        if (ldl_le_p(b) != ra2_dasf_ok[i].old) {
            warn_report("%s: H-ra2-bal: Dasfgwc 0x%08x is %08x, not %08x; not patched",
                        ra2_phone->machine, ra2_dasf_ok[i].va + bd, ldl_le_p(b), ra2_dasf_ok[i].old);
            return;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ra2_dasf_ok); i++) {
        ra2_put32(ra2_dasf_ok[i].va + bd, 0xe3a00000);       /* mov r0, #0 */
        ra2_put32(ra2_dasf_ok[i].va + bd + 4, 0xe12fff1e);   /* bx lr */
    }
}

static void ra2_patches_dsp(void)
{
    ra2_dsp_boot_patch();
    ra2_bal_standin();
}

static void ra2_reset(void *opaque)
{
    const Ra2Phone *ph = ra2_phone;

    ra2_load(ph->core_file, ph->core_prefix, ph->core_tag, RA2_CORE_LOAD);
    ra2_load(ph->var_file, ph->var_prefix, ph->var_tag, ph->var_load);
    ra2_doc_standin();
    if (ph->patches) {
        ph->patches();
    }
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

static void ra2_devices_9500(MachineState *machine, struct omap_mpu_state_s *mpu)
{
    ra2_cover_init(mpu);                /* the cover panel on Microwire CS0: graphic console 1 */
    ra2_kbd_init(OBJECT(machine), mpu); /* keyboard controller and lid on I2C 0x56, MPUIO 2 */
    ra2_codec_init(mpu);                /* TLV320AIC23 on I2C 0x1A */
}

static void ra2_devices_7710(MachineState *machine, struct omap_mpu_state_s *mpu)
{
    ra2_codec_init(mpu);                /* Aic23.ldd is in the ROM too [V module] */
    rm12_input_init(OBJECT(machine), mpu);  /* touch on Microwire CS 0 + MPUIO 4, keys on the MPUIO matrix */
}

static const Ra2Phone ra2_phone_9300 = {
    .machine = "nokia9300",
    .core_file = "RAE6n6270.c01", .core_prefix = "r01-", .core_tag = "C",
    .var_file = "RAE6n6270.l18", .var_prefix = "r09-", .var_tag = "V", .var_load = 0x11700000,
    .rofs_file = "RAE6n6270.c01", .rofs_prefix = "r02-", .rofx_file = "RAE6n6270.l18", .rofx_prefix = "r06-",
    .parts = ra2_parts_9300, .nparts = ARRAY_SIZE(ra2_parts_9300),
    .doc_exports = ra2_doc_exports_9300, .ndoc_exports = ARRAY_SIZE(ra2_doc_exports_9300),
    .doc_stub = ngage_rae6_doc_nos_stub, .doc_stub_len = sizeof(ngage_rae6_doc_nos_stub),
    .doc_stub_va = NGAGE_RAE6_DOC_NOS_STUB_VA,
    .ehwa_delta = 0x507ead64 - 0x507f60d4,      /* Ehwaomap.pdd */
    .bal_delta = 0x5091ab2c - 0x50925ecc,       /* Bal.dll (Dasfgwc.dll the same) */
    .dms_delta = 0x5092aac8 - 0x50935e68,       /* DeviceManagementServer.exe */
    .bal_stub = ngage_rae6_bal_nos_stub, .bal_stub_len = sizeof(ngage_rae6_bal_nos_stub),
    .patches = ra2_patches_dsp,
    .devices = ra2_devices_9500,
};

static const Ra2Phone ra2_phone_9300i = {
    .machine = "nokia9300i",
    .core_file = "RA8n6270.c01", .core_prefix = "r01-", .core_tag = "C",
    .var_file = "RA8n6270.l18", .var_prefix = "r09-", .var_tag = "V", .var_load = 0x11800000,
    .rofs_file = "RA8n6270.c01", .rofs_prefix = "r02-", .rofx_file = "RA8n6270.l18", .rofx_prefix = "r06-",
    .parts = ra2_parts_9300i, .nparts = ARRAY_SIZE(ra2_parts_9300i),
    .doc_exports = ra2_doc_exports_9300i, .ndoc_exports = ARRAY_SIZE(ra2_doc_exports_9300i),
    .doc_stub = ngage_ra8_doc_nos_stub, .doc_stub_len = sizeof(ngage_ra8_doc_nos_stub),
    .doc_stub_va = NGAGE_RA8_DOC_NOS_STUB_VA,
    .ehwa_delta = 0x507f2a94 - 0x507f60d4,      /* Ehwaomap.pdd */
    .bal_delta = 0x509228cc - 0x50925ecc,       /* Bal.dll (Dasfgwc.dll the same) */
    .dms_delta = 0x50932868 - 0x50935e68,       /* DeviceManagementServer.exe */
    .bal_stub = ngage_ra8_bal_nos_stub, .bal_stub_len = sizeof(ngage_ra8_bal_nos_stub),
    .patches = ra2_patches_dsp,
    .devices = ra2_devices_9500,
};

static const Ra2Phone ra2_phone_9500 = {
    .machine = "nokia9500",
    .core_file = "RA2n6270.c01", .core_prefix = "r01-", .core_tag = "C",
    .var_file = "RA2n6270.l18", .var_prefix = "r09-", .var_tag = "V", .var_load = 0x11800000,
    .rofs_file = "RA2n6270.c01", .rofs_prefix = "r02-", .rofx_file = "RA2n6270.l18", .rofx_prefix = "r06-",
    .parts = ra2_parts_9500, .nparts = ARRAY_SIZE(ra2_parts_9500),
    .doc_exports = ra2_doc_exports_9500, .ndoc_exports = ARRAY_SIZE(ra2_doc_exports_9500),
    .doc_stub = ngage_ra2_doc_nos_stub, .doc_stub_len = sizeof(ngage_ra2_doc_nos_stub),
    .doc_stub_va = NGAGE_RA2_DOC_NOS_STUB_VA,
    .bal_stub = ngage_ra2_bal_nos_stub, .bal_stub_len = sizeof(ngage_ra2_bal_nos_stub),
    .patches = ra2_patches_dsp,
    .devices = ra2_devices_9500,
};

/* Nokia 7710 (RM-12): docs/devices/nokia-7710-rm12/bring-up.md */
static const Ra2Phone ra2_phone_7710 = {
    .machine = "nokia7710",
    .core_file = "RM12mPR4.10.0.c1u", .core_prefix = "r06-", .core_tag = "RM_12",
    /* the language variant: an extension ROM (base 0x51B00000) loaded at 0x12100000 [V record] */
    .var_file = "RM12mPR4.10.0.V10", .var_prefix = "r02-", .var_tag = "ID10", .var_load = 0x12100000,
    .rofs_file = "RM12mPR4.10.0.c1u", .rofs_prefix = "r08-",
    .parts = ra2_parts_7710, .nparts = ARRAY_SIZE(ra2_parts_7710),
    .doc_exports = ra2_doc_exports_7710, .ndoc_exports = ARRAY_SIZE(ra2_doc_exports_7710),
    .doc_stub = ngage_rm12_doc_nos_stub, .doc_stub_len = sizeof(ngage_rm12_doc_nos_stub),
    .doc_stub_va = NGAGE_RM12_DOC_NOS_STUB_VA,
    .ehwa_delta = 0x5016dd60 - 0x507e2740,      /* Ehwaomap.pdd */
    .bal_delta = 0x50115540 - 0x50925c30,       /* Bal.dll (Dasfgwc.dll the same) */
    .dms_delta = 0x50366b70 - 0x50935090,       /* DeviceManagementServer.exe */
    .bal_stub = ngage_rm12_bal_nos_stub, .bal_stub_len = sizeof(ngage_rm12_bal_nos_stub),
    .patches = ra2_patches_dsp,
    .devices = ra2_devices_7710,
};

static void ra2_board_init(MachineState *machine, const Ra2Phone *ph)
{
    MemoryRegion *sys = get_system_memory();
    MemoryRegion *boot = g_new(MemoryRegion, 1);
    struct omap_mpu_state_s *mpu;

    ra2_phone = ph;
    if (!ra2_images) {
        error_report("%s: give the extracted images with -M %s,ppm=DIR (ra2_extract.py output)",
                     ph->machine, ph->machine);
        exit(1);
    }
    memory_region_add_subregion(sys, OMAP_EMIFF_BASE, machine->ram);
    mpu = omap310_mpu_init(machine->ram, machine->cpu_type);
    ra2_uart_init(sys, mpu);
    ph->devices(machine, mpu);

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

        for (unsigned i = 0; i < ra2_phone->nparts; i++) {
            ra2_part_load(&ra2_phone->parts[i]);
        }
        ra2_flash_open(&ra2_phone->parts[0]);
        memory_region_init_io(&b->mr, NULL, &ra2_blk_ops, b, "nokia9500.blk", 0x100);
        memory_region_add_subregion_overlap(sys, RA2_BLK_BASE, &b->mr, 10);
    }
    qemu_register_reset(ra2_reset, NULL);
}

/*
 * Nokia 6630 (RM-1, S60 2nd FP2, Symbian 8.0a, BB5 + TI OMAP1610): docs/devices/nokia-6630-rm1/bring-up.md.
 * Bring-up board on QEMU's OMAP310/1510 model (the 1610's differences come as they show up). H-rm1-loader:
 * on the phone NOLO / X-LOADER / SOS_LOADER and SOS_XZIP inflate the core from NAND; the board places the
 * inflated ROM (eka1/tools/xzip_core.py output) in SDRAM at 0x10100000, where the bootstrap is linked to run
 * (its jump tables hold 0x10102ED8.. [V]; the memory table's 0x10100000/32 MB entry [V]), and CS0 holds the
 * 9500's two-word stub `ldr pc, [pc, #-4]; .word 0x10100000`.
 */
#define RM1_CORE_LOAD 0x10100000

/*
 * The phones on this board: the Nokia 6630 and the Nokia N70 (RM-84, S60 2nd FP3, Symbian 8.1a: the same SoC
 * blocks at the same addresses [V bootstrap literals], docs/devices/nokia-n70-rm84/bring-up.md) with its relatives
 * (the N72 RM-180, the N90 RM-42). Per phone: the machine name and its core patches (each phone's own addresses,
 * old words checked).
 */
typedef struct Rm1Phone {
    const char *name;                       /* machine name, for messages */
    const char *images;                     /* the ppm= hint */
    void (*patch)(uint8_t *core, gsize len);
    bool omap1710;                          /* report the OMAP1710's ID registers (rm1_omap1710_ids) */
    unsigned lcd_w, lcd_h;                  /* the main display (0: the 6630's 176x208) */
    bool lcd_ctrl;                          /* behind a display controller (nokia6630_lcd.h "Controller mode") */
    int camera;                             /* campdd.pdd's camera (nokia6630_cam.c): RM1_CAM_NONE / _6630 / _N90 */
} Rm1Phone;
static const Rm1Phone *rm1_phone;

/*
 * H-rm84-omap1710: the Nokia N70's SoC is an OMAP1710 (custom.ldd names 1610/1611/1710). Its kernel types the
 * chip from PRODUCT_ID_MSB (0xFFFE2004) bits 0..15: 0xB5F7 = 1710 (type 2 at 0x80000004), else (>> 1) 0xB58C =
 * type 1, else 0 (0x5000CBEC) [V Ghidra], and keeps the word at 0xFFFED400 at 0x80000008. SecPdd's physical
 * monitor (PA 0x100FF000 + ..., VA 0x500A9928) reads 0xFFFED400 and calls the secure ROM (0x3C00) only for
 * 0x03330000 / 0x03330100 / 0x03330200 (r8 = 0x490D, 0x490D, 0x4921); anything else ends in `b .` (the
 * phone froze there with QEMU's OMAP310 0x03310315) [V run + Ghidra]. Reported: 0x0000B5F7 and 0x03330000
 * (which of the three the N70 has [I]); 0xFFFE2000 / 0xFFFED404 as QEMU's / a 1710 JTAG ID [I].
 */
static uint64_t rm1_id_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t v;

    switch ((uintptr_t)opaque + (addr & ~3)) {
    case 0xfffe2000: v = 0x00aaaafc; break;     /* PRODUCT_ID_LSB, as QEMU's */
    case 0xfffe2004: v = 0x0000b5f7; break;     /* PRODUCT_ID_MSB: OMAP1710 */
    case 0xfffed400: v = 0x03330000; break;
    default:         v = 0x0b5f702f; break;     /* 0xFFFED404 JTAG_ID_MSB [I] */
    }
    return v >> ((addr & 3) * 8);
}

static void rm1_id_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps rm1_id_ops = {
    .read = rm1_id_read,
    .write = rm1_id_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void rm1_omap1710_ids(MemoryRegion *sys)
{
    static const uint32_t at[] = { 0xfffe2000, 0xfffed400 };

    for (unsigned i = 0; i < ARRAY_SIZE(at); i++) {
        MemoryRegion *mr = g_new(MemoryRegion, 1);

        memory_region_init_io(mr, NULL, &rm1_id_ops, (void *)(uintptr_t)at[i], "omap1710.id", 8);
        memory_region_add_subregion_overlap(sys, at[i], mr, 20);
    }
}

/* a word of the core image (VA 0x50000000 + offset) changed before it goes to SDRAM; the old word checked */
static bool rm1_patch(uint8_t *core, gsize len, const char *hack, uint32_t va, uint32_t old, uint32_t new)
{
    uint32_t off = va - 0x50000000;

    if (off + 4 > len || ldl_le_p(core + off) != old) {
        warn_report("%s: %s: %08x holds %08x, not %08x; not patched", rm1_phone->name, hack, va,
                    off + 4 > len ? 0 : ldl_le_p(core + off), old);
        return false;
    }
    stl_le_p(core + off, new);
    return true;
}
static void rm1_patches_6630(uint8_t *core, gsize len)
{
    /*
     * H-rm1-dsp-boot (as H-ra2-dsp-boot): no DSP is emulated; EHWAOMAP.pdd spins until the DSP's boot code
     * clears a shared word (0x50242d78..0x50242d84, no timeout) [V]: the loop's branch becomes a no-op.
     */
    rm1_patch(core, len, "H-rm1-dsp-boot", 0x50242d84, 0x1afffffb, 0xe1a00000);
    /*
     * H-rm1-dsp-timeout: EHWAOMAP.pdd's DSP request timer (0x502387e0) completes the client's pending
     * request (types 1-6) with KErrTimedOut when the DSP does not answer; with no DSP every one does, and
     * DeviceManagementServer's start-up returned -33 (EikAppUiServer then too) [V run + Ghidra]. They
     * complete with KErrNone: `mvn r2, #0x20` -> `mov r2, #0` at 0x50238888.
     */
    rm1_patch(core, len, "H-rm1-dsp-timeout", 0x50238888, 0xe3e02020, 0xe3a02000);
    /*
     * H-rm1-bal-run: every DSP plug-in DeviceManagementServer opens (TestServerGwc, EapGwc, ...) creates its
     * node and then calls Bal.dll ord 22 (0x50285bd8, DoSvControl 0x80000007 to EHWA.ldd); without a DSP the
     * node never leaves state 0, EHWA answers KErrNotReady (0x5024adf8), the plug-in's open fails, DMS's
     * ConstructL leaves with -18 before its rendezvous and EikAppUiServer (its client via
     * DeviceManagementServerProxy) exits with the same code [V catches + Ghidra;
     * docs/research/rm1-dms-not-ready.md]. Ord 22 returns 0 (`mov r0, #0; bx lr`); no audio.
     */
    rm1_patch(core, len, "H-rm1-bal-run", 0x50285bd8, 0xe92d40f0, 0xe3a00000);
    rm1_patch(core, len, "H-rm1-bal-run", 0x50285bdc, 0xe24dd008, 0xe12fff1e);
    /*
     * Bal.dll ord 19 (0x50285b10: DoSvControl 0x80000014 to the node, then a synchronous wait for the DSP's
     * answer) blocked DMS's main thread for ever under EAPControlModule -> EapGwc -> Gwc 0x50287e3c, so DMS
     * served nobody and EikAppUiServer hung in CAknKeySoundSystem::NewL [V stacks + Ghidra]. It returns 0
     * (the 9500's stand-in did the same for its ord 19).
     */
    rm1_patch(core, len, "H-rm1-bal-run", 0x50285b10, 0xe92d4030, 0xe3a00000);
    rm1_patch(core, len, "H-rm1-bal-run", 0x50285b14, 0xe24dd00c, 0xe12fff1e);
    /*
     * H-rm1-eapgwc: EapGwc.dll's command call (0x502287a0, under DMS's EAPControlModule) sends the command
     * through Gwc and then loops on User::WaitForRequest for the DSP's reply; none comes, DMS's main thread
     * blocked for ever and EikAppUiServer with it (CAknKeySoundSystem::NewL) [V stacks + Ghidra]. The
     * command is accepted once sent (as the 9500's Dasfgwc): `bne` after the send at 0x502287b4 -> `b` to
     * the return with r0 = the send's 0.
     */
    rm1_patch(core, len, "H-rm1-eapgwc", 0x502287b4, 0x2600d12c, 0x2600e02c);
    /* the same loop in MuMdrcGwc.dll (0x50229700): the only other one in the ROM [V scan] */
    rm1_patch(core, len, "H-rm1-eapgwc", 0x50229708, 0xd1282800, 0xe0282800);
    /*
     * H-rm1-secenvinit: with no secure environment (H-rm1-secrom) SecEnvInit's main work (0x5030652c, in
     * E32Main's TRAP) ends in a sub-session close on a handle never opened: KERN-EXEC panic. Its thread is
     * system-critical once EStart marks it, so depending on timing the panic became Kern::Fault("PLAT", 91)
     * (DThread exit 0x5001dc08: a panicked system thread) [V catches + Ghidra]. It returns at once
     * (`movs r0, #0; bx lr`): SecEnvInit exits normally, no secure storage (Ss.dat) observers.
     */
    rm1_patch(core, len, "H-rm1-secenvinit", 0x5030652c, 0x4657b5f0, 0x47702000);
    /*
     * H-rm1-drmhelper: DRMHelper.dll's start-server routine (0x50abd41c) starts DRMHELPERSERVER.EXE and waits
     * on "DRMHelperServerSemaphore" with no timeout; with no secure environment the server panics
     * ("CDRMHelperServe" 1) before it signals, so the Menu (its MenuDRMPlugin) waited for ever: an empty grid
     * [V Ghidra + run]. The routine returns KErrNotFound at once (`movs r0, #1; negs r0, r0; bx lr`): DRM
     * clients get an error instead of a hang; no DRM.
     */
    rm1_patch(core, len, "H-rm1-drmhelper", 0x50abd41c, 0x4c19b510, 0x42402001);
    rm1_patch(core, len, "H-rm1-drmhelper", 0x50abd420, 0x1c0444a5, 0x46c04770);
}

static void rm1_board_init(MachineState *machine, const Rm1Phone *ph)
{
    MemoryRegion *sys = get_system_memory();
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    struct omap_mpu_state_s *mpu;
    g_autofree char *path = NULL;
    g_autofree gchar *buf = NULL;
    gsize len;

    rm1_phone = ph;

    if (!ra2_images) {
        error_report("%s: give the images with -M %s,ppm=%s (ROM.bin, nand.img)[,flash=FILE]", ph->name, ph->name,
                     ph->images);
        exit(1);
    }
    path = g_build_filename(ra2_images, "ROM.bin", NULL);
    if (!g_file_get_contents(path, &buf, &len, NULL) || len > OMAP_CS0_SIZE) {
        error_report("%s: cannot read %s", ph->name, path);
        exit(1);
    }
    memory_region_add_subregion(sys, OMAP_EMIFF_BASE, machine->ram);
    mpu = omap1610_mpu_init(machine->ram, machine->cpu_type);
    rm1_gptimers_init(sys, mpu);
    rm1_display_init(sys, mpu, ph->lcd_w, ph->lcd_h, ph->lcd_ctrl);
    rm1_gpio_init(sys, mpu);
    if (ph->camera) {
        rm1_cam_init(sys, mpu, ph->camera);
    }
    rm1_wdt_init(sys);
    rm1_ssi_init(sys, mpu, ra2_isi);
    rm1_keys_init(mpu, ph->omap1710);
    if (ph->omap1710) {
        rm1_omap1710_ids(sys);
        rm1_spi_init(sys, mpu);
    }
    {
        g_autofree char *nand = g_build_filename(ra2_images, "nand.img", NULL);

        rm1_nand_init(sys, mpu, nand, ra2_flash);
    }
    ph->patch((uint8_t *)buf, len);
    memcpy((uint8_t *)memory_region_get_ram_ptr(machine->ram) + (RM1_CORE_LOAD - OMAP_EMIFF_BASE), buf, len);
    memory_region_init_rom(rom, NULL, "nokia6630.boot", 0x4000, &error_fatal);
    stl_le_p(memory_region_get_ram_ptr(rom), 0xe51ff004);
    stl_le_p((uint8_t *)memory_region_get_ram_ptr(rom) + 4, RM1_CORE_LOAD);
    /*
     * H-rm1-secrom: the OMAP1610's secure ROM entry at 0x3C00, which SecPdd.pdd calls for every secure
     * service (MMU off, physical sp, return address at [sp]) (docs/research/rm1-secenv.md): answer
     * "not done" (r0 = 0; SecPdd reports KErrGeneral, SecEnvInit exits cleanly), return kind 1 (done),
     * pop the return address and go back to it.
     */
    {
        static const uint32_t secrom[] = {
            0xe3a00000,     /* mov r0, #0          result: not OK */
            0xe3a04001,     /* mov r4, #1          return kind: call complete */
            0xe49d1004,     /* ldr r1, [sp], #4    the return address (Thumb bit set) */
            0xe12fff11,     /* bx  r1 */
        };
        for (unsigned i = 0; i < ARRAY_SIZE(secrom); i++) {
            stl_le_p((uint8_t *)memory_region_get_ram_ptr(rom) + 0x3c00 + 4 * i, secrom[i]);
        }
    }
    memory_region_add_subregion(sys, OMAP_CS0_BASE, rom);
    ra2_unknown(sys, OMAP_CS0_BASE + 0x4000, OMAP_CS0_SIZE - 0x4000, "nokia6630.cs0", -10);
    ra2_unknown(sys, OMAP_CS1_BASE, 0x04000000, "nokia6630.cs1", -10);
    ra2_unknown(sys, OMAP_CS2_BASE, 0x04000000, "nokia6630.cs2", -10);
    ra2_unknown(sys, OMAP_CS3_BASE, 0x04000000, "nokia6630.cs3", -10);
    /* OMAP1610 blocks QEMU's 1510 model lacks: logged by the catch-all until modelled (the level-2
     * interrupt handler's 4 banks at 0xFFFE0000..0x3FF: omap1610_mpu_init) */
    ra2_unknown(sys, 0xfffe0000, 0x1000, "nokia6630.ih2", -10);
    ra2_unknown(sys, 0xfffbe000, 0x2000, "nokia6630.fffbe", -10);    /* around GPIO 1, 2 */
    ra2_unknown(sys, 0xfffbb000, 0x1000, "nokia6630.fffbb", -10);    /* around GPIO 3, 4 */
    ra2_unknown(sys, 0x08000000, 0x1000, "nokia6630.08000000", -10);
}

/*
 * The Nokia N70's core patches: the 6630's (H-rm1-*, reasons there and in docs/hacks.md) at the N70's addresses,
 * found by code match (same instructions around them, branch and literal offsets masked) with the old words
 * identical [V eka1/rm84/core/ROM.bin]; each was then needed by a stall of its own on the N70
 * (docs/devices/nokia-n70-rm84/bring-up.md).
 */
static void rm1_patches_n70(uint8_t *core, gsize len)
{
    /* H-rm84-dsp-boot (H-rm1-dsp-boot): EHWAOMAP.pdd's DSP boot wait 0x504fe188..0x504fe194 [V spin] */
    rm1_patch(core, len, "H-rm84-dsp-boot", 0x504fe194, 0x1afffffb, 0xe1a00000);
    /* H-rm84-dsp-timeout (H-rm1-dsp-timeout): the DSP request timer's KErrTimedOut -> KErrNone */
    rm1_patch(core, len, "H-rm84-dsp-timeout", 0x504f3c60, 0xe3e02020, 0xe3a02000);
    /* H-rm84-bal-run (H-rm1-bal-run): Bal.dll ord 22 and ord 19 return 0 */
    rm1_patch(core, len, "H-rm84-bal-run", 0x50493400, 0xe92d40f0, 0xe3a00000);
    rm1_patch(core, len, "H-rm84-bal-run", 0x50493404, 0xe24dd008, 0xe12fff1e);
    rm1_patch(core, len, "H-rm84-bal-run", 0x50493338, 0xe92d4030, 0xe3a00000);
    rm1_patch(core, len, "H-rm84-bal-run", 0x5049333c, 0xe24dd00c, 0xe12fff1e);
    /* H-rm84-eapgwc (H-rm1-eapgwc): EapGwc's and MuMdrcGwc's wait for the DSP's reply */
    rm1_patch(core, len, "H-rm84-eapgwc", 0x504977e8, 0x2600d12c, 0x2600e02c);
    rm1_patch(core, len, "H-rm84-eapgwc", 0x5048a548, 0xd1282800, 0xe0282800);
    /* H-rm84-secenvinit (H-rm1-secenvinit): SecEnvInit's main work (module +0x10c, as on the 6630) returns 0 */
    rm1_patch(core, len, "H-rm84-secenvinit", 0x5047c4ac, 0x4657b5f0, 0x47702000);
    /* H-rm84-drmhelper (H-rm1-drmhelper): DRMHelper's start-server routine returns KErrNotFound */
    rm1_patch(core, len, "H-rm84-drmhelper", 0x508214e0, 0x4c19b510, 0x42402001);
    rm1_patch(core, len, "H-rm84-drmhelper", 0x508214e4, 0x1c0444a5, 0x46c04770);
}

static void rm1_patches_6680(uint8_t *core, gsize len)
{
    /* Nokia 6680 (RM-36 V 5.04.40): the 6630's patches at RM-36's addresses (+0x7BD0 for DSP/Bal/EapGwc) */
    rm1_patch(core, len, "H-rm36-dsp-boot", 0x5024a954, 0x1afffffb, 0xe1a00000);
    rm1_patch(core, len, "H-rm36-dsp-timeout", 0x50240458, 0xe3e02020, 0xe3a02000);
    rm1_patch(core, len, "H-rm36-bal-run", 0x5028d7a8, 0xe92d40f0, 0xe3a00000);
    rm1_patch(core, len, "H-rm36-bal-run", 0x5028d7ac, 0xe24dd008, 0xe12fff1e);
    rm1_patch(core, len, "H-rm36-bal-run", 0x5028d6e0, 0xe92d4030, 0xe3a00000);
    rm1_patch(core, len, "H-rm36-bal-run", 0x5028d6e4, 0xe24dd00c, 0xe12fff1e);
    rm1_patch(core, len, "H-rm36-eapgwc", 0x50230384, 0x2600d12c, 0x2600e02c);
    rm1_patch(core, len, "H-rm36-eapgwc", 0x502312d8, 0xd1282800, 0xe0282800);
    rm1_patch(core, len, "H-rm36-secenvinit", 0x502f2cdc, 0x4657b5f0, 0x47702000);
    rm1_patch(core, len, "H-rm36-drmhelper", 0x50ac14bc, 0x4c19b510, 0x42402001);
    rm1_patch(core, len, "H-rm36-drmhelper", 0x50ac14c0, 0x1c0444a5, 0x46c04770);
}

/*
 * Nokia N90 (RM-42 V 5.0607.7, S60 2nd FP3, Symbian 8.1a, OMAP1710): the N70's patches (H-rm84-*) at RM-42's
 * addresses, each found by a unique 16/16 masked code match with the old word identical [V eka1/rm42/core/ROM.bin]
 * (docs/devices/nokia-n90-rm42/bring-up.md).
 */
static void rm1_patches_n90(uint8_t *core, gsize len)
{
    rm1_patch(core, len, "H-rm42-dsp-boot", 0x505f41c4, 0x1afffffb, 0xe1a00000);
    rm1_patch(core, len, "H-rm42-dsp-timeout", 0x505e9cb8, 0xe3e02020, 0xe3a02000);
    rm1_patch(core, len, "H-rm42-bal-run", 0x505296f0, 0xe92d40f0, 0xe3a00000);
    rm1_patch(core, len, "H-rm42-bal-run", 0x505296f4, 0xe24dd008, 0xe12fff1e);
    rm1_patch(core, len, "H-rm42-bal-run", 0x50529628, 0xe92d4030, 0xe3a00000);
    rm1_patch(core, len, "H-rm42-bal-run", 0x5052962c, 0xe24dd00c, 0xe12fff1e);
    rm1_patch(core, len, "H-rm42-eapgwc", 0x5052ef48, 0x2600d12c, 0x2600e02c);
    rm1_patch(core, len, "H-rm42-eapgwc", 0x50520858, 0xd1282800, 0xe0282800);
    rm1_patch(core, len, "H-rm42-secenvinit", 0x5050d06c, 0x4657b5f0, 0x47702000);
    rm1_patch(core, len, "H-rm42-drmhelper", 0x50ac0e4c, 0x4c19b510, 0x42402001);
    rm1_patch(core, len, "H-rm42-drmhelper", 0x50ac0e50, 0x1c0444a5, 0x46c04770);
}

/*
 * Nokia N72 (RM-180 V 5.0819.4, S60 2nd FP3, Symbian 8.1a, OMAP1710): the N70 relinked (the same kernel code, the
 * same drivers modulo relocated addresses, 176x208, the N70's keypad). The N70's patches (H-rm84-*) at RM-180's
 * addresses, each a unique exact match of the 32 words around the N70's site, at the same module offset, old word
 * identical [V eka1/rm180/core/ROM.bin] (docs/devices/nokia-n72-rm180/bring-up.md).
 */
static void rm1_patches_n72(uint8_t *core, gsize len)
{
    rm1_patch(core, len, "H-rm180-dsp-boot", 0x504f6864, 0x1afffffb, 0xe1a00000);
    rm1_patch(core, len, "H-rm180-dsp-timeout", 0x504ec330, 0xe3e02020, 0xe3a02000);
    rm1_patch(core, len, "H-rm180-bal-run", 0x5048df60, 0xe92d40f0, 0xe3a00000);
    rm1_patch(core, len, "H-rm180-bal-run", 0x5048df64, 0xe24dd008, 0xe12fff1e);
    rm1_patch(core, len, "H-rm180-bal-run", 0x5048de98, 0xe92d4030, 0xe3a00000);
    rm1_patch(core, len, "H-rm180-bal-run", 0x5048de9c, 0xe24dd00c, 0xe12fff1e);
    rm1_patch(core, len, "H-rm180-eapgwc", 0x50492348, 0x2600d12c, 0x2600e02c);
    rm1_patch(core, len, "H-rm180-eapgwc", 0x504850a8, 0xd1282800, 0xe0282800);
    rm1_patch(core, len, "H-rm180-secenvinit", 0x5047700c, 0x4657b5f0, 0x47702000);
    rm1_patch(core, len, "H-rm180-drmhelper", 0x50819bb0, 0x4c19b510, 0x42402001);
    rm1_patch(core, len, "H-rm180-drmhelper", 0x50819bb4, 0x1c0444a5, 0x46c04770);
}

static const Rm1Phone rm1_phone_6630 = { "nokia6630", "eka1/rm1/core", rm1_patches_6630, false, 0, 0, false, RM1_CAM_6630 };
static const Rm1Phone rm1_phone_6680 = { "nokia6680", "eka1/rm36/core", rm1_patches_6680, false, 0, 0, false, RM1_CAM_6630 };
static const Rm1Phone rm1_phone_n70 = { "nokian70", "eka1/rm84/core", rm1_patches_n70, true };
static const Rm1Phone rm1_phone_n72 = { "nokian72", "eka1/rm180/core", rm1_patches_n72, true };
/* H-rm42-display: the N90's 352x416 main display behind a controller on SoSSI (genericlcd_lcd.ldd) */
static const Rm1Phone rm1_phone_n90 = { "nokian90", "eka1/rm42/core", rm1_patches_n90, true, 352, 416, true, RM1_CAM_N90 };

static void nokia6630_init(MachineState *machine)
{
    rm1_board_init(machine, &rm1_phone_6630);
}

static void nokia6680_init(MachineState *machine)
{
    rm1_board_init(machine, &rm1_phone_6680);
}

static void nokian70_init(MachineState *machine)
{
    rm1_board_init(machine, &rm1_phone_n70);
}

static void nokian72_init(MachineState *machine)
{
    rm1_board_init(machine, &rm1_phone_n72);
}

static bool n90_fold_closed;
static bool n90_twisted;
static uint8_t n90_cam_angle = 180;
static bool n90_switches_live;

/* Nokia N90 (RM-42): four switch lines ekeyp.dll reads (docs/devices/nokia-n90-rm42/displays.md).
 * Resting open phone: GPIO 42 high, GPIO 61 / 54 / 43 low (camera-angle 180). */
static void n90_apply_switches(void)
{
    rm1_gpio_set_input(3, 13, n90_fold_closed);        /* GPIO 61 = Gpio4 bit 13, high = closed */
    rm1_gpio_set_input(2, 10, !n90_twisted);           /* GPIO 42 = Gpio3 bit 10, high = normal */
    rm1_gpio_set_input(3, 6, n90_cam_angle == 0);      /* GPIO 54 = Gpio4 bit 6, active high */
    rm1_gpio_set_input(2, 11, n90_cam_angle != 180);   /* GPIO 43 = Gpio3 bit 11, active low */
}

static void nokian90_init(MachineState *machine)
{
    rm1_board_init(machine, &rm1_phone_n90);
    n90_cover_init();
    n90_apply_switches();
    n90_switches_live = true;
}

static void nokia9300_init(MachineState *machine)
{
    ra2_board_init(machine, &ra2_phone_9300);
}

static void nokia9300i_init(MachineState *machine)
{
    ra2_board_init(machine, &ra2_phone_9300i);
}

static void nokia9500_init(MachineState *machine)
{
    ra2_board_init(machine, &ra2_phone_9500);
}

static void nokia7710_init(MachineState *machine)
{
    ra2_board_init(machine, &ra2_phone_7710);
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

static char *ra2_get_flash(Object *obj, Error **errp)
{
    return g_strdup(ra2_flash ? ra2_flash : "");
}

static void ra2_set_flash(Object *obj, const char *value, Error **errp)
{
    g_free(ra2_flash);
    ra2_flash = *value ? g_strdup(value) : NULL;
}

static void ra2_class_props(ObjectClass *oc)
{
    object_class_property_add_str(oc, "ppm", ra2_get_ppm, ra2_set_ppm);
    object_class_property_set_description(oc, "ppm", "eka1/ra2/images (ra2_extract.py output)");
    object_class_property_add_str(oc, "isi", ra2_get_isi, ra2_set_isi);
    object_class_property_set_description(oc, "isi", "the modem's ISI reply table (ngage_isi.c format)");
    object_class_property_add_str(oc, "flash", ra2_get_flash, ra2_set_flash);
    object_class_property_set_description(oc, "flash", "C: (partition 0) kept in this file across boots");
}

static void nokia9300_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
    mc->desc = "Nokia 9300 (RAE-6, OMAP1510, Symbian 7.0s EKA1) bring-up board";
    mc->init = nokia9300_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("ti925t");
    mc->default_ram_size = RA2_SDRAM_SIZE;
    mc->default_ram_id = "nokia9300.sdram";
}

static const TypeInfo nokia9300_type = {
    .name = MACHINE_TYPE_NAME("nokia9300"),
    .parent = TYPE_MACHINE,
    .class_init = nokia9300_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokia9300i_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
    mc->desc = "Nokia 9300i (RA-8, OMAP1510, Symbian 7.0s EKA1) bring-up board";
    mc->init = nokia9300i_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("ti925t");
    mc->default_ram_size = RA2_SDRAM_SIZE;
    mc->default_ram_id = "nokia9300i.sdram";
}

static const TypeInfo nokia9300i_type = {
    .name = MACHINE_TYPE_NAME("nokia9300i"),
    .parent = TYPE_MACHINE,
    .class_init = nokia9300i_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokia9500_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
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

static void nokia7710_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
    mc->desc = "Nokia 7710 (RM-12, OMAP1510, Series 90, Symbian 7.0s EKA1) bring-up board";
    mc->init = nokia7710_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("ti925t");
    mc->default_ram_size = RA2_SDRAM_SIZE;          /* bootstrap table: 64 MB at 0x10000000 [V] */
    mc->default_ram_id = "nokia7710.sdram";
}

static const TypeInfo nokia7710_type = {
    .name = MACHINE_TYPE_NAME("nokia7710"),
    .parent = TYPE_MACHINE,
    .class_init = nokia7710_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokia6630_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
    mc->desc = "Nokia 6630 (RM-1, BB5 OMAP1610, S60 2nd FP2, Symbian 8.0a EKA1) bring-up board";
    mc->init = nokia6630_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");     /* the OMAP1610's MPU core */
    mc->default_ram_size = 64 * MiB;    /* bootstrap table: RAM at 0x10100000 + 32 MB [V]; 64 MB covers it */
    mc->default_ram_id = "nokia6630.sdram";
}

static const TypeInfo nokia6630_type = {
    .name = MACHINE_TYPE_NAME("nokia6630"),
    .parent = TYPE_MACHINE,
    .class_init = nokia6630_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokia6680_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
    mc->desc = "Nokia 6680 (RM-36, BB5 OMAP1610, S60 2nd FP2, Symbian 8.0a EKA1) bring-up board";
    mc->init = nokia6680_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");     /* as the 6630 */
    mc->default_ram_size = 64 * MiB;                        /* as the 6630 */
    mc->default_ram_id = "nokia6680.sdram";
}

static const TypeInfo nokia6680_type = {
    .name = MACHINE_TYPE_NAME("nokia6680"),
    .parent = TYPE_MACHINE,
    .class_init = nokia6680_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokian70_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
    mc->desc = "Nokia N70 (RM-84, BB5 OMAP1610, S60 2nd FP3, Symbian 8.1a EKA1) bring-up board";
    mc->init = nokian70_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    /* bootstrap memory table: RAM at 0x10100000 + 64 MB (ROM +0xD20) [V]; with 64 MB from 0x10000000 the
     * bootstrap's stack (top of its table) fell off the RAM and it restarted for ever (stall 2) */
    mc->default_ram_size = 128 * MiB;
    mc->default_ram_id = "nokian70.sdram";
}

static const TypeInfo nokian70_type = {
    .name = MACHINE_TYPE_NAME("nokian70"),
    .parent = TYPE_MACHINE,
    .class_init = nokian70_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokian72_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
    mc->desc = "Nokia N72 (RM-180, BB5 OMAP1710, S60 2nd FP3, Symbian 8.1a EKA1) bring-up board";
    mc->init = nokian72_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    /* the bootstrap's memory table (ROM +0xD0C): 64 MB at 0x10100000, as the N70's [V] */
    mc->default_ram_size = 128 * MiB;
    mc->default_ram_id = "nokian72.sdram";
}

static const TypeInfo nokian72_type = {
    .name = MACHINE_TYPE_NAME("nokian72"),
    .parent = TYPE_MACHINE,
    .class_init = nokian72_class_init,
    .interfaces = arm_machine_interfaces,
};

static bool n90_get_fold(Object *obj, Error **errp)
{
    return n90_fold_closed;
}

/* object_class_property_add_bool's plain setter: a visitor-style one here took `value` for a Visitor * and crashed */
static void n90_set_fold(Object *obj, bool value, Error **errp)
{
    n90_fold_closed = value;
    if (n90_switches_live) {
        n90_apply_switches();
    }
}

static bool n90_get_twisted(Object *obj, Error **errp)
{
    return n90_twisted;
}

/* object_class_property_add_bool's plain setter: a visitor-style one here took `value` for a Visitor * and crashed */
static void n90_set_twisted(Object *obj, bool value, Error **errp)
{
    n90_twisted = value;
    if (n90_switches_live) {
        n90_apply_switches();
    }
}

static void n90_get_cam_angle(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    visit_type_uint8(v, name, &n90_cam_angle, errp);
}

static void n90_set_cam_angle(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint8_t val;

    if (!visit_type_uint8(v, name, &val, errp)) {
        return;
    }
    if (val != 0 && val != 90 && val != 180) {
        error_setg(errp, "camera-angle: 0, 90 or 180");
        return;
    }
    n90_cam_angle = val;
    if (n90_switches_live) {
        n90_apply_switches();
    }
}

static void nokian90_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    ra2_class_props(oc);
    object_class_property_add_bool(oc, "fold-closed", n90_get_fold, n90_set_fold);
    object_class_property_set_description(oc, "fold-closed", "clamshell fold closed (GPIO 61 high)");
    object_class_property_add_bool(oc, "twisted", n90_get_twisted, n90_set_twisted);
    object_class_property_set_description(oc, "twisted", "display twisted to landscape (GPIO 42 low)");
    object_class_property_add(oc, "camera-angle", "uint8", n90_get_cam_angle, n90_set_cam_angle, NULL, NULL);
    object_class_property_set_description(oc, "camera-angle", "camera hinge angle: 0, 90 or 180 (default 180)");
    mc->desc = "Nokia N90 (RM-42, BB5 OMAP1710, S60 2nd FP3, Symbian 8.1a EKA1) bring-up board";
    mc->init = nokian90_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    /* the bootstrap's memory table (ROM +0xD04): 64 MB at 0x10100000, as the N70's [V] */
    mc->default_ram_size = 128 * MiB;
    mc->default_ram_id = "nokian90.sdram";
}

static const TypeInfo nokian90_type = {
    .name = MACHINE_TYPE_NAME("nokian90"),
    .parent = TYPE_MACHINE,
    .class_init = nokian90_class_init,
    .interfaces = arm_machine_interfaces,
};

static void nokia9500_register(void)
{
    type_register_static(&nokia9300_type);
    type_register_static(&nokia9300i_type);
    type_register_static(&nokia9500_type);
    type_register_static(&nokia6630_type);
    type_register_static(&nokia6680_type);
    type_register_static(&nokian70_type);
    type_register_static(&nokian72_type);
    type_register_static(&nokian90_type);
    type_register_static(&nokia7710_type);
}

type_init(nokia9500_register)
