/*
 * Host test of src/qemu-ngage/hw/arm/nokia9210_dataflash.h: the Nokia 9210's C: DataFlash as Medata.pdd and
 * the bootstrap drive it (docs/devices/nokia-9210-rae3/bring-up.md).
 *   cc -O2 -Wall -o build/nokia9210_dataflash_test tests/nokia9210_dataflash_test.c && build/nokia9210_dataflash_test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/qemu-ngage/hw/arm/nokia9210_dataflash.h"

static Rae3DataFlash d;

static void cmd(const uint8_t *b, unsigned n)
{
    rae3_df_select(&d, true);
    for (unsigned i = 0; i < n; i++) {
        rae3_df_tx(&d, b[i]);
    }
}

static void addr_cmd(uint8_t op, uint32_t addr, unsigned dummies)
{
    uint8_t b[8] = { op, addr >> 16, addr >> 8, addr };

    cmd(b, 4 + dummies);
}

/* Regression rae3_bootstrap_status: the bootstrap sends 0x57 and reads ONE byte: it must be the status, with
 * (status & 0x38) == 0x30, or it sets an error bit and blinks its LED for ever [V 0x50000CE0]. */
static void test_bootstrap_status(void)
{
    static const uint8_t s = 0x57;

    cmd(&s, 1);
    assert((rae3_df_rx(&d) & 0x38) == 0x30);
    rae3_df_select(&d, false);
}

/* Medata.pdd's status read: 0x57 then three reads, the third is the status (bit 7 ready) */
static void test_driver_status(void)
{
    static const uint8_t s = 0x57;
    uint8_t v[3];

    cmd(&s, 1);
    for (int i = 0; i < 3; i++) {
        v[i] = rae3_df_rx(&d);
    }
    assert(v[2] & 0x80);
    rae3_df_select(&d, false);
}

/* Page read: 0x52 + address + 4 don't-care bytes, the driver flushes 2 bytes, then the page data follows
 * (by DMA from +0x08) from the byte address on. */
static void test_page_read(void)
{
    uint32_t page = 1234;

    for (unsigned i = 0; i < RAE3_DF_PAGE; i++) {
        d.mem[page * RAE3_DF_PAGE + i] = (uint8_t)(i * 7);
    }
    addr_cmd(0x52, page << 10 | 0x200, 4);
    rae3_df_rx(&d);
    rae3_df_rx(&d);                         /* the two command bytes left in the FIFO */
    for (unsigned i = 0x200; i < 0x210; i++) {
        assert(rae3_df_rx(&d) == (uint8_t)(i * 7));
    }
    assert(rae3_df_rx(&d) == 0);            /* wraps to byte 0 of the same page */
    rae3_df_select(&d, false);
}

/* Buffer write (0x84) + buffer to page with erase (0x83) on release; compare (0x60) clears status bit 6 */
static void test_write_program_compare(void)
{
    uint32_t page = 0x1fb6;
    static const uint8_t st = 0x57;

    addr_cmd(0x84, 0x200, 0);
    for (unsigned i = 0; i < 16; i++) {
        rae3_df_tx(&d, 0xa0 + i);
    }
    rae3_df_select(&d, false);
    addr_cmd(0x83, page << 10, 0);
    rae3_df_select(&d, false);              /* programs now */
    assert(d.mem[page * RAE3_DF_PAGE + 0x200] == 0xa0);
    assert(d.mem[page * RAE3_DF_PAGE + 0x20f] == 0xaf);
    assert(d.dirty);
    addr_cmd(0x60, page << 10, 0);
    rae3_df_select(&d, false);
    cmd(&st, 1);
    assert(!(rae3_df_rx(&d) & 0x40));       /* same */
    rae3_df_select(&d, false);
}

/* A command is not executed without its address, and nothing happens while not selected */
static void test_short_and_unselected(void)
{
    uint32_t page = 7;

    memset(d.mem + page * RAE3_DF_PAGE, 0x11, RAE3_DF_PAGE);
    rae3_df_tx(&d, 0x81);                   /* not selected */
    rae3_df_select(&d, true);
    rae3_df_tx(&d, 0x81);                   /* page erase without its address */
    rae3_df_select(&d, false);
    assert(d.mem[page * RAE3_DF_PAGE] == 0x11);
    addr_cmd(0x81, page << 10, 0);
    rae3_df_select(&d, false);
    assert(d.mem[page * RAE3_DF_PAGE] == 0xff);
}

/* Block erase 0x50: 8 pages, address block << 13 */
static void test_block_erase(void)
{
    memset(d.mem + 16 * RAE3_DF_PAGE, 0, 9 * RAE3_DF_PAGE);
    addr_cmd(0x50, 2u << 13, 0);
    rae3_df_select(&d, false);
    assert(d.mem[16 * RAE3_DF_PAGE] == 0xff && d.mem[24 * RAE3_DF_PAGE - 1] == 0xff);
    assert(d.mem[24 * RAE3_DF_PAGE] == 0);  /* the next block untouched */
}

int main(void)
{
    d.mem = malloc(RAE3_DF_SIZE);
    memset(d.mem, 0xff, RAE3_DF_SIZE);
    rae3_df_reset(&d);
    test_bootstrap_status();
    test_driver_status();
    test_page_read();
    test_write_program_compare();
    test_short_and_unselected();
    test_block_erase();
    printf("nokia9210_dataflash_test: ok\n");
    return 0;
}
