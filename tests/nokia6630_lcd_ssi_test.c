/*
 * Host tests of the Nokia 6630's display core (src/qemu-ngage/hw/arm/nokia6630_lcd.h) and modem-link core
 * (nokia6630_ssi.h), driven with the sequences lcd.ldd and xbus.ldd use
 * (docs/research/rm1-lcd-sossi.md, docs/research/rm1-ssi-xbus.md).
 *   cc -O2 -Wall -o build/nokia6630_lcd_ssi_test tests/nokia6630_lcd_ssi_test.c && build/nokia6630_lcd_ssi_test
 *
 * Regressions:
 *  rm1_lcd_sossi_wait (stall 5): SoSSI 0x08 must read bits 2 and 3 set (FIFO ready, idle), or lcd.ldd spins.
 *  rm1_lcd_panel: window (CASET/RASET) + RAMWR + the LCD DMA's double-indexed rows land in the frame.
 *  rm1_ssi_hello_eaten: the modem's hello must come on the next tick, not inside the phone's Ctrl
 *    interrupt (ssi.pdd's start-up drain read ate it and the link never came up).
 *  rm1_ssi_ctrl_pending: the modem answers hello with hello (clears the phone's "Ctrl pending" flag) and
 *    only then link up (0x10000000), else every non-hello word is ignored.
 */
#include <assert.h>
#include <stdio.h>
#include "../src/qemu-ngage/hw/arm/nokia6630_lcd.h"
#include "../src/qemu-ngage/hw/arm/nokia6630_ssi.h"

static uint8_t guest[0x40000];

static void mem_read(Rm1Lcd *l, uint64_t addr, void *buf, unsigned len)
{
    memcpy(buf, guest + (addr - 0x10000000), len);
}

static void dcs(Rm1Lcd *l, uint8_t cmd, const uint8_t *p, unsigned n)     /* lcd.ldd 0x5003f324 */
{
    assert(rm1_sossi_read(l, 0x08) & 8);           /* idle */
    rm1_sossi_write(l, 0x1c, 0);
    rm1_sossi_write(l, 0x20, cmd);
    if (n) {
        rm1_sossi_write(l, 0x1c, (n - 1) | 0x40000);
        for (unsigned i = 0; i < n; i += 4) {
            uint32_t w = 0;

            for (unsigned j = 0; j < 4 && i + j < n; j++) {
                w |= (uint32_t)p[i + j] << (8 * j);
            }
            rm1_sossi_write(l, 0x20, w);
        }
    }
}

static void window(Rm1Lcd *l, unsigned x0, unsigned x1, unsigned y0, unsigned y1)
{
    uint8_t c[4] = { 0, x0, 0, x1 }, r[4] = { 0, y0, 0, y1 };

    dcs(l, 0x2a, c, 4);
    dcs(l, 0x2b, r, 4);
    dcs(l, 0x2c, NULL, 0);
}

static void lcd_tests(void)
{
    static Rm1Lcd l;
    unsigned y0, y1;

    rm1_lcd_reset(&l);
    l.mem_read = mem_read;
    assert((rm1_sossi_read(&l, 0x08) & 0x0c) == 0x0c);

    /* mode 7: bus format 0x67, reorder table word 0 = RGB565 */
    rm1_sossi_write(&l, 0x0c, 0x67);
    rm1_sossi_write(&l, 0x08, 0x20);
    rm1_sossi_write(&l, 0x14, 0x9ac5a928);
    rm1_sossi_write(&l, 0x08, 0x00);

    /* CPU path: two RGB565 pixels a word, low half first */
    window(&l, 10, 11, 20, 20);
    rm1_sossi_write(&l, 0x10, 0x07e0f800);          /* red, green */
    assert(l.fb[20 * RM1_LCD_W + 10] == 0xff0000);
    assert(l.fb[20 * RM1_LCD_W + 11] == 0x00ff00);
    assert(rm1_lcd_take_dirty(&l, &y0, &y1) && y0 == 20 && y1 == 21);
    assert(!rm1_lcd_take_dirty(&l, &y0, &y1));

    /* the panel reads: ID (dummy, 0x45 ..) and status (dummy, A0 02 04 00) */
    dcs(&l, 0x04, NULL, 0);
    rm1_sossi_read(&l, 0x10);
    assert(rm1_sossi_read(&l, 0x10) == 0x45);
    dcs(&l, 0x09, NULL, 0);
    rm1_sossi_read(&l, 0x10);
    assert(rm1_sossi_read(&l, 0x10) == 0xa0);
    assert(rm1_sossi_read(&l, 0x10) == 0x02);

    /* LCD DMA rectangle (lcd.ldd 0x50043f90): 3x2 pixels at (5,7) from a 176-stride RGB565 buffer */
    for (unsigned i = 0; i < 176 * 208; i++) {
        uint16_t v = 0x001f;                        /* blue */

        guest[2 * i] = v;
        guest[2 * i + 1] = v >> 8;
    }
    guest[2 * (8 * 176 + 7)] = 0x00;                /* (7,8): black */
    guest[2 * (8 * 176 + 7) + 1] = 0x00;
    window(&l, 5, 7, 7, 8);
    {
        uint32_t src = 0x10000000 + (7 * 176 + 5) * 2;

        RM1_D(&l, 0xbc0) = 0x141;                   /* 16-bit elements */
        RM1_D(&l, 0xbc2) = 0x3440;                  /* double-indexed source */
        RM1_D(&l, 0xbc8) = src;
        RM1_D(&l, 0xbca) = src >> 16;
        RM1_D(&l, 0xbd8) = 1;                       /* element index */
        RM1_D(&l, 0xbda) = (176 - 3) * 2 + 1;       /* frame index */
        RM1_D(&l, 0xbf4) = 0;
        RM1_D(&l, 0xbe0) = 3;
        RM1_D(&l, 0xbe4) = 2;
        RM1_D(&l, 0xbc2) |= 0x80;
        rm1_lcd_dma_run(&l);
    }
    assert(!(RM1_D(&l, 0xbc2) & 0x80) && (RM1_D(&l, 0xbc4) & 8));
    assert(l.fb[7 * RM1_LCD_W + 5] == 0x0000ff && l.fb[8 * RM1_LCD_W + 6] == 0x0000ff);
    assert(l.fb[8 * RM1_LCD_W + 7] == 0x000000);
    assert(l.fb[7 * RM1_LCD_W + 8] == 0);           /* outside the window: untouched */

    /* mode 11 (bus format 0x47): one 0x00RRGGBB pixel a word */
    rm1_sossi_write(&l, 0x0c, 0x47);
    window(&l, 0, 0, 0, 0);
    rm1_sossi_write(&l, 0x10, 0x00123456);
    assert(l.fb[0] == 0x123456);
}

static void ssi_tests(void)
{
    static Rm1SsiLink s;
    uint32_t w;
    uint8_t msg[10] = { 0, 0, 0, 0x15, 0, 4, 0x7f, 0, 0x50, 0x65 };

    memset(&s, 0, sizeof(s));
    /* the phone's hello (from its Ctrl interrupt): nothing until the tick */
    rm1_ssi_from_phone(&s, RM1_SSI_HELLO);
    assert(!rm1_ssi_pop(&s, &w));
    rm1_ssi_tick(&s);
    assert(rm1_ssi_pop(&s, &w) && w == RM1_SSI_HELLO);
    assert(rm1_ssi_pop(&s, &w) && w == RM1_SSI_UP);
    assert(s.up);

    /* header from the phone: checksum, ACK, length in words */
    assert(rm1_ssi_header(3) == 0x73000370);            /* the phone's first header [V run] */
    rm1_ssi_from_phone(&s, 0x73000370);
    assert(rm1_ssi_pop(&s, &w) && w == RM1_SSI_ACK && s.want_words == 3);
    rm1_ssi_from_phone(&s, 0x73000371);                  /* bad checksum */
    assert(rm1_ssi_pop(&s, &w) && w == RM1_SSI_NAK);

    /* the modem's message: payload first, then the header; busy until the phone's ACK and DMA */
    assert(rm1_ssi_modem_send(&s, msg, sizeof(msg)));
    assert(s.out_len == 12 && rm1_ssi_pop(&s, &w) && w == rm1_ssi_header(3));
    assert(!rm1_ssi_modem_send(&s, msg, sizeof(msg)));
    rm1_ssi_from_phone(&s, RM1_SSI_NAK);                 /* refused: the same header again */
    assert(rm1_ssi_pop(&s, &w) && w == rm1_ssi_header(3));
    rm1_ssi_from_phone(&s, RM1_SSI_ACK);
    assert(!s.out_wait_ack);

    /* the ISI length inside a padded payload */
    assert(rm1_ssi_isi_len(msg, 12) == 10);
}

int main(void)
{
    lcd_tests();
    ssi_tests();
    printf("nokia6630_lcd_ssi_test: ok\n");
    return 0;
}
