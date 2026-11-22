/*
 * The Nokia 6630's NAND: the OMAP1610 NAND controller (0xFFFBCC00) with a Samsung small-page chip
 * (maker 0xEC, device 0x76: 4096 blocks x 32 pages x (512 + 16) bytes), as mednand.pdd drives it.
 * docs/research/rm1-nand.md has the evidence. No QEMU types, so tests/nokia6630_nand_test.c runs it
 * on the host; nokia6630_soc.c maps it.
 *
 * Registers (offsets): 04 data (8/16/32, little-endian byte stream), 08 address (page << 9 | column;
 * erase: (row & 0xff) | (row >> 8) << 9), 10 control (CE#, FIFO modes: plain), 18 FIFO status (bits 2
 * and 3 always set), 1C ready (bit 0), 20 command + address, 24 command only, 2C ECC of the bytes moved
 * since the last 50 bit-0 write, 54 FIFO data (= 04), 58/5C/8C/90 plain, 64 control (bit 1 soft reset,
 * done at once), 68 reset done (bit 0).
 * Commands: 00/01/50 read pointer (area A, B, spare), 80 program setup, 10 program, 60/D0 erase,
 * 70 status (0xC0: ready, pass), 90 READ ID (EC 76 C0 FF), FF reset.
 * ECC: for every set bit at bit address a (byte index * 8 + bit), k = 0..11 toggles bit k of the low
 * half when (a >> k) & 1, else bit k of the high half; result = low | high << 16. The driver packs it as
 * {r & 0xff, (r >> 16) & 0xff, ((r >> 8) & 0x0f) | ((r >> 20) & 0xf0)} into spare 6..8 [V packing].
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA6630_NAND_H
#define NOKIA6630_NAND_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RM1_NAND_BLOCKS 4096
#define RM1_NAND_PPB    32
#define RM1_NAND_PAGE   512
#define RM1_NAND_OOB    16
#define RM1_NAND_RAW    (RM1_NAND_PAGE + RM1_NAND_OOB)
#define RM1_NAND_PAGES  (RM1_NAND_BLOCKS * RM1_NAND_PPB)
#define RM1_NAND_SIZE   ((uint64_t)RM1_NAND_PAGES * RM1_NAND_RAW)     /* 69206016 */

enum { RM1_NAND_IDLE, RM1_NAND_READ, RM1_NAND_PROG, RM1_NAND_ID, RM1_NAND_STATUS };

typedef struct Rm1Nand {
    uint8_t *store;                 /* RM1_NAND_SIZE bytes, page after page (main + spare) */
    void (*flush)(struct Rm1Nand *n, uint32_t first_page, uint32_t pages);   /* after program/erase */
    void *opaque;
    uint32_t addr, ctrl, fifo_cfg, cfg5c, cfg8c, cfg90, ctl64;
    uint32_t row, col;
    int mode;
    uint8_t ptr_area;
    uint8_t pbuf[RM1_NAND_RAW];
    int id_pos;
    uint16_t ecc_lo, ecc_hi;
    uint32_t ecc_idx;
} Rm1Nand;

static inline void rm1_ecc_reset(uint16_t *lo, uint16_t *hi, uint32_t *idx)
{
    *lo = *hi = 0;
    *idx = 0;
}

static inline void rm1_ecc_byte(uint16_t *lo, uint16_t *hi, uint32_t *idx, uint8_t b)
{
    for (int bit = 0; bit < 8; bit++) {
        uint32_t a = *idx * 8 + bit;

        if (!((b >> bit) & 1)) {
            continue;
        }
        for (int k = 0; k < 12; k++) {
            if ((a >> k) & 1) {
                *lo ^= 1u << k;
            } else {
                *hi ^= 1u << k;
            }
        }
    }
    (*idx)++;
}

/* ECC register value of a buffer, and the driver's 3-byte packing of it */
static inline uint32_t rm1_ecc(const uint8_t *p, uint32_t n)
{
    uint16_t lo, hi;
    uint32_t idx;

    rm1_ecc_reset(&lo, &hi, &idx);
    for (uint32_t i = 0; i < n; i++) {
        rm1_ecc_byte(&lo, &hi, &idx, p[i]);
    }
    return lo | (uint32_t)hi << 16;
}

static inline void rm1_ecc_pack(uint32_t r, uint8_t out[3])
{
    out[0] = r & 0xff;
    out[1] = (r >> 16) & 0xff;
    out[2] = ((r >> 8) & 0x0f) | ((r >> 20) & 0xf0);
}

static inline uint8_t *rm1_nand_page(Rm1Nand *n, uint32_t row)
{
    return n->store + (uint64_t)(row % RM1_NAND_PAGES) * RM1_NAND_RAW;
}

static inline uint8_t rm1_nand_data_in(Rm1Nand *n)
{
    uint8_t v = 0xff;
    static const uint8_t id[4] = { 0xec, 0x76, 0xc0, 0xff };

    switch (n->mode) {
    case RM1_NAND_ID:
        return n->id_pos < 4 ? id[n->id_pos++] : 0xff;
    case RM1_NAND_STATUS:
        return 0xc0;                            /* ready, not write-protected, pass */
    case RM1_NAND_READ:
        v = rm1_nand_page(n, n->row)[n->col];
        if (++n->col == RM1_NAND_RAW) {         /* sequential read into the next page */
            n->col = 0;
            n->row++;
        }
        break;
    }
    rm1_ecc_byte(&n->ecc_lo, &n->ecc_hi, &n->ecc_idx, v);
    return v;
}

static inline void rm1_nand_data_out(Rm1Nand *n, uint8_t v)
{
    if (n->mode == RM1_NAND_PROG && n->col < RM1_NAND_RAW) {
        n->pbuf[n->col++] &= v;
    }
    rm1_ecc_byte(&n->ecc_lo, &n->ecc_hi, &n->ecc_idx, v);
}

static inline uint32_t rm1_nand_area_col(uint8_t area, uint32_t c)
{
    return area == 0x50 ? RM1_NAND_PAGE + (c & 15) : area == 0x01 ? 256 + c : c;
}

static inline void rm1_nand_command(Rm1Nand *n, uint8_t cmd, bool with_addr)
{
    uint32_t a = n->addr;

    switch (cmd) {
    case 0x00: case 0x01: case 0x50:
        n->ptr_area = cmd;
        if (with_addr) {                        /* read */
            n->row = a >> 9;
            n->col = rm1_nand_area_col(cmd, a & 0xff);
            n->mode = RM1_NAND_READ;
        }
        break;
    case 0x80:                                  /* program setup; pointer from a cc24 00/01/50 */
        n->row = a >> 9;
        n->col = rm1_nand_area_col(n->ptr_area, a & 0xff);
        memset(n->pbuf, 0xff, sizeof(n->pbuf));
        n->mode = RM1_NAND_PROG;
        n->ptr_area = 0x00;
        break;
    case 0x10:                                  /* program: NAND only clears bits */
        if (n->mode == RM1_NAND_PROG) {
            uint8_t *p = rm1_nand_page(n, n->row);

            for (int i = 0; i < RM1_NAND_RAW; i++) {
                p[i] &= n->pbuf[i];
            }
            if (n->flush) {
                n->flush(n, n->row % RM1_NAND_PAGES, 1);
            }
        }
        n->mode = RM1_NAND_IDLE;
        break;
    case 0x60:                                  /* erase setup: row in the erase format */
        n->row = (a & 0xff) | ((a >> 9) << 8);
        break;
    case 0xd0: {                                /* erase */
        uint32_t first = (n->row % RM1_NAND_PAGES) & ~(uint32_t)(RM1_NAND_PPB - 1);

        memset(rm1_nand_page(n, first), 0xff, RM1_NAND_PPB * RM1_NAND_RAW);
        if (n->flush) {
            n->flush(n, first, RM1_NAND_PPB);
        }
        n->mode = RM1_NAND_IDLE;
        break;
    }
    case 0x70:
        n->mode = RM1_NAND_STATUS;
        break;
    case 0x90:
        n->mode = RM1_NAND_ID;
        n->id_pos = 0;
        break;
    case 0xff:
        n->mode = RM1_NAND_IDLE;
        n->ptr_area = 0;
        break;
    }
}

static inline uint32_t rm1_nand_read(Rm1Nand *n, uint32_t off, unsigned size)
{
    uint32_t v = 0;

    switch (off) {
    case 0x04: case 0x54:
        for (unsigned i = 0; i < size; i++) {
            v |= (uint32_t)rm1_nand_data_in(n) << (8 * i);
        }
        return v;
    case 0x08: return n->addr;
    case 0x10: return n->ctrl;
    case 0x18: return 0x0c;                     /* FIFO: read data there, write room */
    case 0x1c: return 1;                        /* R/B#: ready */
    case 0x2c: return n->ecc_lo | (uint32_t)n->ecc_hi << 16;
    case 0x58: return n->fifo_cfg;
    case 0x5c: return n->cfg5c;
    case 0x64: return n->ctl64;
    case 0x68: return 1;                        /* reset done */
    case 0x8c: return n->cfg8c;
    case 0x90: return n->cfg90;
    }
    return 0;
}

static inline void rm1_nand_write(Rm1Nand *n, uint32_t off, uint32_t v, unsigned size)
{
    switch (off) {
    case 0x04: case 0x54:
        for (unsigned i = 0; i < size; i++) {
            rm1_nand_data_out(n, v >> (8 * i));
        }
        break;
    case 0x08: n->addr = v; break;
    case 0x10: n->ctrl = v; break;
    case 0x18: break;                           /* FIFO acknowledgements */
    case 0x20: rm1_nand_command(n, v & 0xff, true); break;
    case 0x24: rm1_nand_command(n, v & 0xff, false); break;
    case 0x50:
        if (v & 1) {
            rm1_ecc_reset(&n->ecc_lo, &n->ecc_hi, &n->ecc_idx);
        }
        break;
    case 0x58: n->fifo_cfg = v; break;
    case 0x5c: n->cfg5c = v; break;
    case 0x64: n->ctl64 = v & ~2u; break;       /* soft reset completes at once */
    case 0x8c: n->cfg8c = v; break;
    case 0x90: n->cfg90 = v; break;
    }
}

#endif
