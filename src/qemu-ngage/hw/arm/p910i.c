/*
 * Sony Ericsson P910i (UIQ 2.1, Symbian 7.0 / EKA1, Philips Nexperia PNX4000) bring-up board: -M p910i.
 * docs/devices/se-p910i/bring-up.md has the evidence for every value here; the SoC map is
 * docs/research/p910i-soc-map.md.
 *
 * No QEMU model or public datasheet of the PNX4000 exists: every device here is modelled from what
 * the phone's own bootstrap, kernel and drivers do with it.
 *   0x90000000  NOR flash: the core ROM image (XIP, runs from here with the MMU off), the ROLF image
 *               (drive A:) at +0x1D00000 [V: the flash files' headers]; code patches (H-p910-*) applied
 *               to it at start-up
 *   everything else unmodelled: a catch-all that logs ("p910i: R/W addr size value pc") and returns
 *   the last value written, so polls and probes show up in the log
 * Options: -M p910i,ppm=DIR[,isi=FILE]   DIR = eka1/p910i (core/ROM.bin, rolf.bin, c.img); FILE = the
 *          GSM side's OSE reply table (src/qemu-ngage/p910i-ose-replies.txt)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
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
#include "p910i.h"
#include "ngage_p910_doc_stub.h"

#define P910_FLASH_BASE  0x90000000
#define P910_FLASH_SIZE  (32 * MiB)
#define P910_ROLF_OFF    0x01D00000

static char *p910_dir;              /* -M p910i,ppm=DIR */
static char *p910_isi;              /* -M p910i,isi=FILE (the modem's reply table) */

const char *p910_images(void)
{
    return p910_dir;
}

const char *p910_isi_file(void)
{
    return p910_isi;
}

/* ---- catch-all for unmodelled addresses -------------------------------------------------------- */

typedef struct {
    MemoryRegion mr;
    hwaddr base;
    GHashTable *mem;
    GHashTable *seen;
} P910Unknown;

uint32_t p910_pc(void)
{
    return current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void p910_unknown_log(P910Unknown *u, hwaddr addr, bool wr, uint64_t val, unsigned size)
{
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(u->seen, k));

    if (n < 16) {
        g_hash_table_insert(u->seen, k, GUINT_TO_POINTER(n + 1));
        qemu_log_mask(LOG_UNIMP, "p910i: %s %08" HWADDR_PRIx " %u %08" PRIx64 " pc=%08x%s\n",
                      wr ? "W" : "R", u->base + addr, size, val, p910_pc(),
                      n + 1 == 16 ? " (further accesses not logged)" : "");
    }
}

/*
 * Status bits the bootstrap polls before any device here is modelled: read as set, always. Each line:
 * physical word address, bits, and the poll that needs them (docs/devices/se-p910i/bring-up.md).
 */
static const struct { uint32_t addr, bits; } p910_ready[] = {
    /* 0x58000700 bit 4 (bootstrap 0x900004C8): the LCD FIFO block, p910i_lcd.c */
    { 0, 0 },
};

static uint64_t p910_unknown_read(void *opaque, hwaddr addr, unsigned size)
{
    P910Unknown *u = opaque;
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem, GUINT_TO_POINTER((guint)(addr & ~3u))));
    uint64_t v;

    for (unsigned i = 0; i < ARRAY_SIZE(p910_ready); i++) {
        if (p910_ready[i].addr == u->base + (addr & ~3u)) {
            w |= p910_ready[i].bits;
        }
    }

    v = (w >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
    p910_unknown_log(u, addr, false, v, size);
    return v;
}

static void p910_unknown_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    P910Unknown *u = opaque;
    gpointer k = GUINT_TO_POINTER((guint)(addr & ~3u));
    uint32_t w = GPOINTER_TO_UINT(g_hash_table_lookup(u->mem, k));
    unsigned sh = (addr & 3) * 8;
    uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1) << sh;

    w = (w & ~mask) | (((uint32_t)val << sh) & mask);
    g_hash_table_insert(u->mem, k, GUINT_TO_POINTER(w));
    p910_unknown_log(u, addr, true, val, size);
}

static const MemoryRegionOps p910_unknown_ops = {
    .read = p910_unknown_read,
    .write = p910_unknown_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void p910_unknown(MemoryRegion *sys, hwaddr base, uint64_t size, const char *name, int prio)
{
    P910Unknown *u = g_new0(P910Unknown, 1);

    u->base = base;
    u->mem = g_hash_table_new(g_direct_hash, g_direct_equal);
    u->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&u->mr, NULL, &p910_unknown_ops, u, name, size);
    memory_region_add_subregion_overlap(sys, base, &u->mr, prio);
}

/* ---- NOR flash: core ROM + ROLF ----------------------------------------------------------------- */

static void p910_load(MemoryRegion *flash, const char *rel, hwaddr off)
{
    g_autofree char *path = g_build_filename(p910_dir, rel, NULL);
    g_autofree gchar *buf = NULL;
    gsize len;

    if (!g_file_get_contents(path, &buf, &len, NULL) || off + len > P910_FLASH_SIZE) {
        error_report("p910i: cannot load %s", path);
        exit(1);
    }
    memcpy((uint8_t *)memory_region_get_ram_ptr(flash) + off, buf, len);
}

/*
 * Replace code in the NOR image (VA = 0x50000000 + offset): the old bytes must match, else the patch
 * is skipped with a warning (another firmware). Returns whether it was applied.
 */
bool p910_patch(uint8_t *nor, uint32_t va, const void *old, size_t oldlen, const void *new, size_t len)
{
    uint8_t *p = nor + (va - 0x50000000);

    if (memcmp(p, old, oldlen)) {
        warn_report("p910i: %08x does not hold the expected code; not patched", va);
        return false;
    }
    memcpy(p, new, len);
    return true;
}

/*
 * H-p910-doc: udiskonchip.dll's TrueFFS dispatcher (0x500A27C4) -> doc_standin_p910.S, which takes
 * sectors from the board's block device (p910i_soc.c); the TrueFFS FAT filter (0x500A4868, called
 * directly by Write, result ignored) and the flExit-like 0x500A31D8 (destructor only) return 0.
 * docs/research/p910i-diskonchip.md [V].
 */
static void p910_doc_standin(uint8_t *nor)
{
    static const uint8_t prologue[] = { 0xf0, 0xb5, 0x57, 0x46, 0x4e, 0x46, 0x45, 0x46 };
    static const uint8_t disp_old[] = { 0xf0, 0xb5, 0x57, 0x46, 0x4e, 0x46, 0x45, 0x46,
                                        0xe0, 0xb4, 0x87, 0xb0, 0x80, 0x46, 0x05, 0x91 };
    static const uint8_t ret0[] = { 0x00, 0x20, 0x70, 0x47 };          /* movs r0,#0; bx lr */

    QEMU_BUILD_BUG_ON(sizeof(ngage_p910_doc_nos_stub) > 1930);
    if (getenv("P910_NO_DOC_STANDIN")) {            /* experiments: the phone's own TrueFFS */
        return;
    }
    if (p910_patch(nor, NGAGE_P910_DOC_NOS_STUB_VA, disp_old, sizeof(disp_old),
                   ngage_p910_doc_nos_stub, sizeof(ngage_p910_doc_nos_stub))) {
        p910_patch(nor, 0x500a4868, prologue, sizeof(prologue), ret0, sizeof(ret0));
        p910_patch(nor, 0x500a31d8, prologue, sizeof(prologue), ret0, sizeof(ret0));
        info_report("p910i: H-p910-doc: udiskonchip's TrueFFS -> block device stand-in");
    }
}

static void p910_cpu_reset(void *opaque)
{
    ARMCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    /* the boot ROM's hand-over [I]: straight to the image's reset branch in NOR, MMU off */
    cpu->env.regs[15] = P910_FLASH_BASE;
}

static void p910i_init(MachineState *machine)
{
    MemoryRegion *sys = get_system_memory();
    MemoryRegion *flash = g_new(MemoryRegion, 1);
    ARMCPU *cpu;

    if (!p910_dir) {
        error_report("p910i: give the extracted images with -M p910i,ppm=eka1/p910i");
        exit(1);
    }
    cpu = ARM_CPU(cpu_create(machine->cpu_type));

    p910_unknown(sys, 0, 0x100000000ULL, "p910i.unknown", -100);

    memory_region_init_rom(flash, NULL, "p910i.nor", P910_FLASH_SIZE, &error_fatal);
    memset(memory_region_get_ram_ptr(flash), 0xff, P910_FLASH_SIZE);
    p910_load(flash, "core/ROM.bin", 0);
    p910_load(flash, "rolf.bin", P910_ROLF_OFF);
    p910_doc_standin(memory_region_get_ram_ptr(flash));
    memory_region_add_subregion(sys, P910_FLASH_BASE, flash);

    /* SDRAM [I: the bootstrap table's 32 MB at 0xD0000000; the controller at 0x78000000] */
    memory_region_add_subregion(sys, P910_RAM_BASE, machine->ram);
    /* internal SRAM: the bootstrap copies its flash-ID routine to 0x68000020 and runs it [V] */
    {
        MemoryRegion *sram = g_new(MemoryRegion, 1);

        memory_region_init_ram(sram, NULL, "p910i.sram", P910_SRAM_SIZE, &error_fatal);
        memory_region_add_subregion(sys, P910_SRAM_BASE, sram);
    }
    p910_board_init(machine, sys, cpu);
    qemu_register_reset(p910_cpu_reset, cpu);
}

static char *p910_get_ppm(Object *obj, Error **errp)
{
    return g_strdup(p910_dir ? p910_dir : "");
}

static void p910_set_ppm(Object *obj, const char *value, Error **errp)
{
    g_free(p910_dir);
    p910_dir = *value ? g_strdup(value) : NULL;
}

static char *p910_get_isi(Object *obj, Error **errp)
{
    return g_strdup(p910_isi ? p910_isi : "");
}

static void p910_set_isi(Object *obj, const char *value, Error **errp)
{
    g_free(p910_isi);
    p910_isi = *value ? g_strdup(value) : NULL;
}

static char *p910_flash;             /* -M p910i,flash=FILE: the phone's C: (kept across boots) */

const char *p910_flash_file(void)
{
    return p910_flash;
}

static char *p910_get_flash(Object *obj, Error **errp)
{
    return g_strdup(p910_flash ? p910_flash : "");
}

static void p910_set_flash(Object *obj, const char *value, Error **errp)
{
    g_free(p910_flash);
    p910_flash = *value ? g_strdup(value) : NULL;
}

static void p910i_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "ppm", p910_get_ppm, p910_set_ppm);
    object_class_property_set_description(oc, "ppm", "eka1/p910i (core/ROM.bin, rolf.bin, c.img)");
    object_class_property_add_str(oc, "isi", p910_get_isi, p910_set_isi);
    object_class_property_add_str(oc, "flash", p910_get_flash, p910_set_flash);
    object_class_property_set_description(oc, "flash", "the phone's C: image, kept across boots (created from c.img)");
    mc->desc = "Sony Ericsson P910i (PNX4000, UIQ 2.1, Symbian 7.0 EKA1) bring-up board";
    mc->init = p910i_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->default_ram_size = P910_RAM_SIZE;
    mc->default_ram_id = "p910i.sdram";
}

static const TypeInfo p910i_type = {
    .name = MACHINE_TYPE_NAME("p910i"),
    .parent = TYPE_MACHINE,
    .class_init = p910i_class_init,
    .interfaces = arm_machine_interfaces,
};

static void p910i_register(void)
{
    type_register_static(&p910i_type);
}

type_init(p910i_register)
