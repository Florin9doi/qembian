/*
 * The Nokia 9210's C: drive: an Atmel AT45DB321-class DataFlash (8192 pages x 528 bytes) on the SoC's serial
 * interface at 0x0500B000, as Medata.pdd ("Media.SFlash") and the bootstrap drive it
 * (docs/devices/nokia-9210-rae3/bring-up.md). No QEMU types: tests/nokia9210_dataflash_test.c.
 *
 * Controller [V Medata.pdd + the kernel's accessors 0x50002E04..0x50002EA0]: +0x0C chip select (0 = selected,
 * 1 = released), +0x04 transmit a byte, +0x10 transmit busy (1 = busy), +0x14 a received byte is there,
 * +0x08 the received byte. Each transmitted byte clocks a byte back into a 2-byte receive FIFO; reading +0x08
 * with the FIFO empty clocks the next output byte (the driver reads page data by DMA from +0x08 after
 * flushing the two bytes the command left) [I: the FIFO depth from that flush].
 * DataFlash [V command bytes in Medata.pdd, Atmel's public command set]: 0x52 page read (3 address bytes:
 * page << 10 | byte, then 4 don't-care bytes), 0x53 / 0x55 page to buffer 1 / 2, 0x60 / 0x61 compare page with
 * buffer 1 / 2 (status bit 6 = differ), 0x84 / 0x87 buffer write (byte address), 0x83 / 0x86 buffer to page
 * with erase, 0x88 / 0x89 without erase, 0x81 page erase, 0x50 block erase (8 pages, address block << 13),
 * 0x57 / 0xD7 status (bit 7 ready, bit 6 compare, bits 5-2 density 1101: the bootstrap wants
 * (status & 0x38) == 0x30 [V 0x50000CE0]). The status byte comes back for the command byte itself, so the
 * bootstrap's first read is the status [V: it reads once after 0x57]. Program and erase run when the chip is
 * released (as on the chip) and complete at once.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA9210_DATAFLASH_H
#define NOKIA9210_DATAFLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RAE3_DF_PAGE   528u
#define RAE3_DF_PAGES  8192u
#define RAE3_DF_SIZE   (RAE3_DF_PAGE * RAE3_DF_PAGES)
#define RAE3_DF_STATUS 0xb4u            /* ready, density 1101 */

typedef struct Rae3DataFlash {
    uint8_t *mem;                       /* RAE3_DF_SIZE bytes */
    uint8_t buf[2][RAE3_DF_PAGE];
    bool selected;
    uint8_t cmd;
    unsigned n;                         /* bytes received since select */
    uint32_t addr;
    uint8_t rx[2];
    unsigned rxn;
    uint8_t status;
    bool dirty;                         /* mem changed since the last save */
    uint32_t dirty_lo, dirty_hi;        /* byte range changed */
} Rae3DataFlash;

static inline uint32_t rae3_df_page(uint32_t addr)
{
    return (addr >> 10) & (RAE3_DF_PAGES - 1);
}

static inline void rae3_df_touch(Rae3DataFlash *d, uint32_t off, uint32_t len)
{
    if (!d->dirty || off < d->dirty_lo) {
        d->dirty_lo = off;
    }
    if (!d->dirty || off + len > d->dirty_hi) {
        d->dirty_hi = off + len;
    }
    d->dirty = true;
}

static inline void rae3_df_reset(Rae3DataFlash *d)
{
    d->selected = false;
    d->n = 0;
    d->rxn = 0;
    d->status = RAE3_DF_STATUS;
}

static inline void rae3_df_push(Rae3DataFlash *d, uint8_t b)
{
    if (d->rxn == 2) {
        d->rx[0] = d->rx[1];
        d->rxn = 1;
    }
    d->rx[d->rxn++] = b;
}

/* the chip's output for the next clock (the data phase of a read) */
static inline uint8_t rae3_df_out(Rae3DataFlash *d)
{
    switch (d->cmd) {
    case 0x52: {
        uint32_t page = rae3_df_page(d->addr), byte = (d->addr & 0x3ff) % RAE3_DF_PAGE;
        uint8_t b = d->mem[page * RAE3_DF_PAGE + byte];

        d->addr = (d->addr & ~0x3ffu) | ((byte + 1) % RAE3_DF_PAGE);
        return b;
    }
    case 0x57: case 0xd7:
        return d->status;
    }
    return 0xff;
}

/* a byte transmitted while selected; returns the byte clocked back */
static inline uint8_t rae3_df_xfer(Rae3DataFlash *d, uint8_t v)
{
    unsigned n = d->n++;

    if (!d->selected) {
        return 0xff;
    }
    if (n == 0) {
        d->cmd = v;
        d->addr = 0;
        return (v == 0x57 || v == 0xd7) ? d->status : 0xff;
    }
    switch (d->cmd) {
    case 0x57: case 0xd7:
        return d->status;
    case 0x52:
        if (n <= 3) {
            d->addr = d->addr << 8 | v;
            return 0xff;
        }
        if (n <= 7) {
            return 0xff;                    /* don't care bytes */
        }
        return rae3_df_out(d);
    case 0x84: case 0x87:                   /* buffer write */
        if (n <= 3) {
            d->addr = d->addr << 8 | v;
            return 0xff;
        }
        d->buf[d->cmd == 0x87][(d->addr & 0x3ff) % RAE3_DF_PAGE] = v;
        d->addr = (d->addr & ~0x3ffu) | (((d->addr & 0x3ff) + 1) % RAE3_DF_PAGE);
        return 0xff;
    default:                                /* commands with an address only */
        if (n <= 3) {
            d->addr = d->addr << 8 | v;
        }
        return 0xff;
    }
}

/* released: commands that act on the array run now */
static inline void rae3_df_release(Rae3DataFlash *d)
{
    uint32_t page = rae3_df_page(d->addr), off = page * RAE3_DF_PAGE;

    if (!d->selected) {
        return;
    }
    d->selected = false;
    if (d->n < 4) {
        return;
    }
    switch (d->cmd) {
    case 0x53: case 0x55:
        memcpy(d->buf[d->cmd == 0x55], d->mem + off, RAE3_DF_PAGE);
        break;
    case 0x60: case 0x61:
        d->status = memcmp(d->buf[d->cmd == 0x61], d->mem + off, RAE3_DF_PAGE) ?
                    (d->status | 0x40) : (d->status & ~0x40);
        break;
    case 0x83: case 0x86:
        memcpy(d->mem + off, d->buf[d->cmd == 0x86], RAE3_DF_PAGE);
        rae3_df_touch(d, off, RAE3_DF_PAGE);
        break;
    case 0x88: case 0x89:
        for (unsigned i = 0; i < RAE3_DF_PAGE; i++) {
            d->mem[off + i] &= d->buf[d->cmd == 0x89][i];
        }
        rae3_df_touch(d, off, RAE3_DF_PAGE);
        break;
    case 0x81:
        memset(d->mem + off, 0xff, RAE3_DF_PAGE);
        rae3_df_touch(d, off, RAE3_DF_PAGE);
        break;
    case 0x50: {
        uint32_t first = ((d->addr >> 13) & (RAE3_DF_PAGES / 8 - 1)) * 8;

        memset(d->mem + first * RAE3_DF_PAGE, 0xff, 8 * RAE3_DF_PAGE);
        rae3_df_touch(d, first * RAE3_DF_PAGE, 8 * RAE3_DF_PAGE);
        break;
    }
    }
}

static inline void rae3_df_select(Rae3DataFlash *d, bool sel)
{
    if (sel) {
        d->selected = true;
        d->n = 0;
        d->rxn = 0;
    } else {
        rae3_df_release(d);
    }
}

/* controller: transmit (+0x04) */
static inline void rae3_df_tx(Rae3DataFlash *d, uint8_t v)
{
    rae3_df_push(d, rae3_df_xfer(d, v));
}

/* controller: receive (+0x08) */
static inline uint8_t rae3_df_rx(Rae3DataFlash *d)
{
    uint8_t b;

    if (d->rxn) {
        b = d->rx[0];
        d->rx[0] = d->rx[1];
        d->rxn--;
        return b;
    }
    if (!d->selected) {
        return 0xff;
    }
    d->n++;
    return rae3_df_out(d);
}

#endif
