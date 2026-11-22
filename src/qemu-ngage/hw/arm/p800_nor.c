/*
 * Sony Ericsson P800 (-M p800): the second NOR at phys 0xB0000000, an Intel StrataFlash-class chip (x16)
 * driven by the phone's own medint.pdd ("Media.Lfs", 0x500A9D10) for the LFFS drive.
 * docs/research/p800-storage.md has the evidence.
 *
 * Why not QEMU's pflash_cfi01 [V]: medint issues two command cycles in one 32-bit store
 * ((count << 16) | 0xE8 for a buffered program, 0x00D00060 / 0x00010060 for block unlock / lock) and
 * never polls for completion: it waits for IRQ 45 and then reads the status, writes 0x50 and 0xFF.
 * So: every 32-bit store is two 16-bit cycles, low half first; program, erase and suspend end with
 * IRQ 45 raised (a level line) until the next 0x50 / 0xFF.
 *
 * Commands: FF read array, 70 status, 50 clear status, 90 ID, 98 CFI, B8 xx STS configuration (ignored),
 * 20/D0 block erase (128 KB), E8/count/data.../D0 buffered program, 40|10/data word program,
 * 60/D0|01 unlock / lock (no locking modelled), B0 suspend, D0 resume. Status 0x80 = ready.
 * Programming ANDs (bits only go 1 -> 0), as on the chip.
 *
 * Contents: the LNG image (an LFFS volume, 96 blocks) at 0, the ROLF at +0xC00000 [V: the flash files'
 * headers], 0xFF elsewhere. flash=FILE keeps the whole 32 MB window (sparse) across boots, written
 * through on every program / erase.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "system/reset.h"
#include "hw/core/irq.h"
#include "p910i.h"
#include "p800_nor.h"

#define NOR_SIZE   (32 * MiB)
#define NOR_BLOCK  0x20000          /* CFI 0x2F/0x30 = 0x0200 * 256; matches the LFFS block headers [V] */
#define NOR_IRQ    45               /* medint acknowledges bit 13 of IC1 [V] */

enum { M_READ, M_STATUS, M_ID, M_CFI, M_ERASE, M_PROG, M_BUF_COUNT, M_BUF_DATA, M_LOCK, M_CONFIG };

typedef struct {
    MemoryRegion mr;
    uint8_t *mem;
    qemu_irq irq;
    QEMUTimer *done;
    int mode;
    uint16_t status;
    uint32_t buf_addr, buf_left;    /* buffered program: next address, halfwords still to come */
    int fd;
    unsigned logged;
} P800Nor;

/* CFI query table, by index (read at offset 2 * index on this x16 part) [V: the indices medint reads] */
static uint16_t p800_nor_cfi(unsigned idx)
{
    switch (idx) {
    case 0x10: return 'Q';
    case 0x11: return 'R';
    case 0x12: return 'Y';
    case 0x13: return 0x01;         /* Intel/Sharp command set */
    case 0x27: return 0x18;         /* 2^24 = 16 MB [I: LNG + ROLF end at 0xFFFA00] */
    case 0x28: return 0x01;         /* x16 */
    case 0x2a: return 0x05;         /* write buffer 2^5 = 32 bytes */
    case 0x2c: return 0x01;         /* one erase region */
    case 0x2d: return 0x7f;         /* 128 blocks */
    case 0x2e: return 0x00;
    case 0x2f: return 0x00;         /* block size 0x0200 * 256 = 128 KB */
    case 0x30: return 0x02;
    case 0x36: return 0x00;         /* bit 5 clear: medint skips its unlock-all loop [V] */
    }
    return 0;
}

static void p800_nor_save(P800Nor *s, uint32_t off, uint32_t len)
{
    if (s->fd >= 0 && pwrite(s->fd, s->mem + off, len, off) != (ssize_t)len) {
        warn_report("p800: NOR file write at %08x failed", off);
    }
}

/* an operation finished: ready, and IRQ 45 until the driver's 0x50 / 0xFF */
static void p800_nor_done(void *opaque)
{
    P800Nor *s = opaque;

    s->status |= 0x80;
    qemu_irq_raise(s->irq);
}

static void p800_nor_busy(P800Nor *s)
{
    s->status = 0x00;               /* busy until the timer */
    s->mode = M_STATUS;
    timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200 * SCALE_US);
}

static void p800_nor_program16(P800Nor *s, uint32_t off, uint16_t v)
{
    off &= NOR_SIZE - 2;
    s->mem[off] &= v;
    s->mem[off + 1] &= v >> 8;
}

static void p800_nor_set_mode(P800Nor *s, int mode)
{
    s->mode = mode;
    memory_region_rom_device_set_romd(&s->mr, mode == M_READ);
}

/* one 16-bit bus cycle */
static void p800_nor_cycle(P800Nor *s, uint32_t off, uint16_t v)
{
    uint8_t cmd = v & 0xff;

    if (s->logged < 300) {
        s->logged++;
        qemu_log_mask(LOG_UNIMP, "p800-nor: mode %d W %08x %04x pc=%08x\n", s->mode, 0xB0000000 + off, v,
                      p910_pc());
    }
    switch (s->mode) {
    case M_ERASE:
        if (cmd == 0xd0) {
            uint32_t b = off & ~(NOR_BLOCK - 1);

            memset(s->mem + b, 0xff, NOR_BLOCK);
            p800_nor_save(s, b, NOR_BLOCK);
            p800_nor_busy(s);
        } else {
            s->status |= 0x30;      /* command sequence error */
            p800_nor_set_mode(s, M_STATUS);
        }
        return;
    case M_PROG:
        p800_nor_program16(s, off, v);
        p800_nor_save(s, off & ~1u, 2);
        p800_nor_busy(s);
        return;
    case M_BUF_COUNT:
        s->buf_left = (v & 0xff) + 1;   /* N = halfwords - 1 */
        s->mode = M_BUF_DATA;
        return;
    case M_BUF_DATA:
        if (s->buf_left) {
            if (s->buf_addr == UINT32_MAX) {
                s->buf_addr = off & ~1u;
            }
            p800_nor_program16(s, off, v);
            s->buf_left--;
            return;
        }
        if (cmd == 0xd0) {
            uint32_t start = s->buf_addr == UINT32_MAX ? (off & ~1u) : s->buf_addr;

            p800_nor_save(s, start & ~31u, 64);
            p800_nor_busy(s);
        } else {
            s->status |= 0x30;
            p800_nor_set_mode(s, M_STATUS);
        }
        return;
    case M_LOCK:                    /* D0 unlock / 01 lock: no lock bits modelled */
    case M_CONFIG:                  /* B8 xx: STS configuration */
        s->status |= 0x80;
        p800_nor_set_mode(s, M_STATUS);
        return;
    }
    switch (cmd) {
    case 0xff:
        qemu_irq_lower(s->irq);
        p800_nor_set_mode(s, M_READ);
        break;
    case 0x50:
        qemu_irq_lower(s->irq);
        s->status &= 0x80;
        break;
    case 0x70:
        p800_nor_set_mode(s, M_STATUS);
        break;
    case 0x90:
        p800_nor_set_mode(s, M_ID);
        break;
    case 0x98:
        p800_nor_set_mode(s, M_CFI);
        break;
    case 0x20:
        p800_nor_set_mode(s, M_ERASE);
        break;
    case 0x40:
    case 0x10:
        p800_nor_set_mode(s, M_PROG);
        break;
    case 0xe8:
        s->buf_addr = UINT32_MAX;
        p800_nor_set_mode(s, M_BUF_COUNT);
        s->status |= 0x80;          /* the buffer is free (XSR) */
        break;
    case 0x60:
        p800_nor_set_mode(s, M_LOCK);
        break;
    case 0xb8:
        p800_nor_set_mode(s, M_CONFIG);
        break;
    case 0xb0:                      /* suspend: nothing runs long enough to suspend; report suspended */
        s->status = 0xc0;
        p800_nor_set_mode(s, M_STATUS);
        timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50 * SCALE_US);
        break;
    case 0xd0:                      /* resume */
        s->status = 0x80;
        p800_nor_set_mode(s, M_STATUS);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "p800-nor: unknown command %02x at %08x\n", cmd, 0xB0000000 + off);
        break;
    }
}

static uint64_t p800_nor_read(void *opaque, hwaddr addr, unsigned size)
{
    P800Nor *s = opaque;
    uint32_t v16;

    switch (s->mode) {
    case M_CFI:
        v16 = p800_nor_cfi((addr >> 1) & 0xff);
        break;
    case M_ID:
        v16 = (addr & 2) ? 0x0018 : 0x0089;     /* Intel, 28F128J3-like [I: medint never reads it] */
        break;
    case M_READ:
        return ldn_le_p(s->mem + addr, size);
    default:
        v16 = s->status;
        break;
    }
    return size == 4 ? (v16 | v16 << 16) : v16;
}

static void p800_nor_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    P800Nor *s = opaque;

    if (size == 4) {
        p800_nor_cycle(s, addr, val & 0xffff);
        p800_nor_cycle(s, addr + 2, val >> 16);
    } else {
        p800_nor_cycle(s, addr, val & 0xffff);
    }
}

static const MemoryRegionOps p800_nor_ops = {
    .read = p800_nor_read,
    .write = p800_nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void p800_nor_reset(void *opaque)
{
    P800Nor *s = opaque;

    timer_del(s->done);
    s->status = 0x80;
    qemu_irq_lower(s->irq);
    p800_nor_set_mode(s, M_READ);
}

static void p800_nor_load(P800Nor *s, const char *dir, const char *name, uint32_t off)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);
    g_autofree gchar *buf = NULL;
    gsize len;

    if (!g_file_get_contents(path, &buf, &len, NULL) || off + len > NOR_SIZE) {
        error_report("p800: cannot load %s", path);
        exit(1);
    }
    memcpy(s->mem + off, buf, len);
}

void p800_nor_init(MemoryRegion *sys, const char *dir, const char *flash)
{
    P800Nor *s = g_new0(P800Nor, 1);
    struct stat st;

    memory_region_init_rom_device(&s->mr, NULL, &p800_nor_ops, s, "p800.nor2", NOR_SIZE, &error_fatal);
    s->mem = memory_region_get_ram_ptr(&s->mr);
    s->irq = p910_irq(NOR_IRQ);
    s->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, p800_nor_done, s);
    s->fd = -1;
    if (flash) {
        s->fd = open(flash, O_RDWR | O_CREAT, 0644);
        if (s->fd < 0) {
            error_report("p800: flash=%s: %s", flash, strerror(errno));
            exit(1);
        }
        if (fstat(s->fd, &st) == 0 && st.st_size == NOR_SIZE &&
            pread(s->fd, s->mem, NOR_SIZE, 0) == NOR_SIZE) {
            info_report("p800: NOR from %s", flash);
            goto done;
        }
    }
    memset(s->mem, 0xff, NOR_SIZE);
    p800_nor_load(s, dir, "lng.bin", 0);
    p800_nor_load(s, dir, "rolf.bin", 0xC00000);
    if (s->fd >= 0) {
        if (ftruncate(s->fd, 0) || ftruncate(s->fd, NOR_SIZE)) {
            error_report("p800: flash=%s: cannot size it", flash);
            exit(1);
        }
        for (uint32_t o = 0; o < NOR_SIZE; o += 0x10000) {
            if (!buffer_is_zero(s->mem + o, 0x10000)) {
                p800_nor_save(s, o, 0x10000);
            }
        }
    }
done:
    memory_region_add_subregion(sys, 0xB0000000, &s->mr);
    qemu_register_reset(p800_nor_reset, s);
}
