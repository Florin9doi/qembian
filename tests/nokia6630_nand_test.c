/*
 * Host test of the Nokia 6630's NAND model (src/qemu-ngage/hw/arm/nokia6630_nand.h), driven with the
 * register sequences mednand.pdd uses (docs/research/rm1-nand.md).
 *   cc -O2 -Wall -o build/nokia6630_nand_test tests/nokia6630_nand_test.c && build/nokia6630_nand_test
 *
 * Regression rm1_nand_controller (stall 6): mednand polled 0xFFFBCC68 for the controller's reset with
 * nothing mapped there; with the controller it must find the chip (ID EC 76), read the ROFS pages the
 * image tool wrote (their ECC must check) and program/erase C:.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/qemu-ngage/hw/arm/nokia6630_nand.h"

static uint32_t flushed_first, flushed_pages;

static void flush(Rm1Nand *n, uint32_t first, uint32_t pages)
{
    flushed_first = first;
    flushed_pages = pages;
}

static Rm1Nand *chip(void)
{
    static Rm1Nand n;

    memset(&n, 0, sizeof(n));
    n.store = malloc(RM1_NAND_SIZE);
    memset(n.store, 0xff, RM1_NAND_SIZE);
    n.flush = flush;
    return &n;
}

static void controller_reset(Rm1Nand *n)          /* 0x500636c4 */
{
    rm1_nand_write(n, 0x64, 2, 1);
    assert(rm1_nand_read(n, 0x68, 1) & 1);
    rm1_nand_write(n, 0x24, 0xff, 1);
    assert(rm1_nand_read(n, 0x1c, 1) & 1);
}

static void read_id(Rm1Nand *n)                   /* 0x50063770 */
{
    rm1_nand_write(n, 0x08, 0, 4);
    rm1_nand_write(n, 0x20, 0x90, 1);
    assert(rm1_nand_read(n, 0x04, 1) == 0xec);
    assert(rm1_nand_read(n, 0x04, 1) == 0x76);   /* 4096 blocks x 32 pages x 528 in mednand's table */
    rm1_nand_read(n, 0x04, 2);
}

/* program with ECC through the posting FIFO (0x50064274), the main ECC into spare 6..8 */
static void program(Rm1Nand *n, uint32_t page, const uint8_t *data)
{
    uint8_t ecc[3], spare[16];
    uint32_t r;

    rm1_nand_write(n, 0x50, 1, 1);
    rm1_nand_write(n, 0x08, page << 9, 4);
    rm1_nand_write(n, 0x24, 0x00, 1);
    rm1_nand_write(n, 0x20, 0x80, 1);
    for (int i = 0; i < 512; i += 16) {
        assert(rm1_nand_read(n, 0x18, 1) & 8);
        for (int j = 0; j < 16; j += 4) {
            rm1_nand_write(n, 0x54, data[i + j] | data[i + j + 1] << 8 | data[i + j + 2] << 16 |
                           (uint32_t)data[i + j + 3] << 24, 4);
        }
    }
    r = rm1_nand_read(n, 0x2c, 4);
    rm1_ecc_pack(r, ecc);
    memset(spare, 0xff, sizeof(spare));
    memcpy(spare + 6, ecc, 3);
    for (int i = 0; i < 16; i++) {
        rm1_nand_write(n, 0x04, spare[i], 1);
    }
    rm1_nand_write(n, 0x24, 0x10, 1);
    rm1_nand_write(n, 0x24, 0x70, 1);
    assert((rm1_nand_read(n, 0x04, 1) & 1) == 0);   /* pass */
}

/* read with ECC through the prefetch FIFO (0x50063f44): data, and whether the ECC checks */
static bool read_page(Rm1Nand *n, uint32_t page, uint8_t *data)
{
    uint8_t stored[16], ecc[3];
    uint32_t r;

    rm1_nand_write(n, 0x50, 1, 1);
    rm1_nand_write(n, 0x08, page << 9, 4);
    rm1_nand_write(n, 0x20, 0x00, 1);
    for (int i = 0; i < 512; i += 4) {
        uint32_t w = rm1_nand_read(n, 0x54, 4);

        data[i] = w; data[i + 1] = w >> 8; data[i + 2] = w >> 16; data[i + 3] = w >> 24;
    }
    r = rm1_nand_read(n, 0x2c, 4);
    for (int i = 0; i < 16; i++) {
        stored[i] = rm1_nand_read(n, 0x04, 1);
    }
    rm1_ecc_pack(r, ecc);
    return memcmp(ecc, stored + 6, 3) == 0;
}

static void erase(Rm1Nand *n, uint32_t block)       /* 0x5006454c */
{
    uint32_t row = block << 5;

    rm1_nand_write(n, 0x08, (row & 0xff) | ((row >> 8) << 9), 4);
    rm1_nand_write(n, 0x20, 0x60, 4);
    rm1_nand_write(n, 0x24, 0xd0, 4);
    rm1_nand_write(n, 0x24, 0x70, 1);
    assert((rm1_nand_read(n, 0x04, 1) & 1) == 0);
}

int main(void)
{
    Rm1Nand *n = chip();
    uint8_t data[512], back[512];

    controller_reset(n);
    read_id(n);

    /* the ECC function and packing agree with eka1/tools/rm1_nand.py (eka1/test_rm1_nand.py) */
    for (int i = 0; i < 512; i++) {
        data[i] = "Nokia 6630 NAND"[i % 15];
    }
    assert(rm1_ecc(data, 512) == 0x3350335);
    {
        uint8_t p[3];

        rm1_ecc_pack(0x3350335, p);
        assert(p[0] == 0x35 && p[1] == 0x35 && p[2] == 0x33);
    }

    /* one flipped bit changes 12 of the 24 ECC bits (the driver's single-bit case) */
    {
        uint32_t a = rm1_ecc(data, 512);

        data[100] ^= 4;
        assert(__builtin_popcount(a ^ rm1_ecc(data, 512)) == 12);
        data[100] ^= 4;
    }

    /* program, read back, ECC checks; written through */
    program(n, 0x8123, data);
    assert(flushed_first == 0x8123 && flushed_pages == 1);
    memset(back, 0, sizeof(back));
    assert(read_page(n, 0x8123, back));
    assert(memcmp(back, data, 512) == 0);

    /* the bad-block check (spare byte 5 of pages 0, 1 of a block, area 0x50) reads 0xFF */
    rm1_nand_write(n, 0x08, (0x8120 << 9) | 5, 4);
    rm1_nand_write(n, 0x20, 0x50, 1);
    assert(rm1_nand_read(n, 0x04, 1) == 0xff);

    /* erase the block: blank again, the whole block flushed */
    erase(n, 0x8123 >> 5);
    assert(flushed_first == 0x8120 && flushed_pages == 32);
    read_page(n, 0x8123, back);
    for (int i = 0; i < 512; i++) {
        assert(back[i] == 0xff);
    }

    /* programming only clears bits */
    memset(data, 0x0f, sizeof(data));
    program(n, 7, data);
    memset(data, 0xf3, sizeof(data));
    program(n, 7, data);
    read_page(n, 7, back);
    assert(back[0] == 0x03);

    printf("nokia6630_nand_test: ok\n");
    return 0;
}
