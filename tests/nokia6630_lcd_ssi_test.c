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
 * Nokia N90 (controller mode, 352x416; docs/devices/nokia-n90-rm42/bring-up.md):
 *  rm42_lcd_hires: the console is 352x416 and pixels past x 175 / y 207 land (the 6630's size clipped them).
 *  rm42_lcd_window_38_48: register 38 {x0, y0, x1, y1} (16-bit LE) + 48 write a rectangle; DCS 2A/2B/2C mean
 *    nothing there (the N90 writes its register 2A = 0x81 / 0x87).
 *  rm42_lcd_logo_stripes: with register 2A = 0x87 a pixel is two 16-bit units, 0x00RRGGBB low half first (as
 *    single RGB565 units the logo came out twice as wide, every other column black; as RGB666 the UI was noise).
 *  rm42_lcd_early_logo_doubled: with 2A = 0x81 (the start-up logo, CPU FIFO) each unit is one RGB565 pixel (paired,
 *    the logo came out twice, side by side, in the top half, in pink).
 *  rm42_lcd_reads_zero: register reads answer 0 (no DCS RDDID queue).
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

static void ctrl_reg(Rm1Lcd *l, uint8_t reg, const uint8_t *p, unsigned n)  /* genericlcd 0x50049290 */
{
    dcs(l, reg, p, n);
}

static void ctrl_window(Rm1Lcd *l, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    uint8_t w[8] = { x0, x0 >> 8, y0, y0 >> 8, x1, x1 >> 8, y1, y1 >> 8 };

    ctrl_reg(l, 0x38, w, 8);
    ctrl_reg(l, 0x48, NULL, 0);
}

static void lcd_ctrl_tests(void)
{
    static Rm1Lcd l;
    uint8_t v;
    unsigned y0, y1;

    l.w = 352;
    l.h = 416;
    l.ctrl = true;
    rm1_lcd_reset(&l);
    l.mem_read = mem_read;
    rm1_sossi_write(&l, 0x0c, 0x67);
    assert(l.xe == 351 && l.ye == 415);

    /* rm42_lcd_early_logo_doubled: register 2A = 0x81, a FIFO word = two RGB565 pixels, low half first */
    v = 0x81;
    ctrl_reg(&l, 0x2a, &v, 1);
    ctrl_window(&l, 300, 400, 301, 400);
    rm1_sossi_write(&l, 0x10, 0x001fffff);          /* white, then blue [V words like 0873EF7F] */
    assert(l.fb[400 * 352 + 300] == 0xffffff);      /* rm42_lcd_hires: past 176x208 */
    assert(l.fb[400 * 352 + 301] == 0x0000ff);
    assert(rm1_lcd_take_dirty(&l, &y0, &y1) && y0 == 400 && y1 == 401);

    /* rm42_lcd_window_38_48: the rectangle wraps inside the window; DCS commands do not move it */
    ctrl_window(&l, 10, 20, 11, 21);
    rm1_sossi_write(&l, 0x10, 0xffffffff);
    rm1_sossi_write(&l, 0x10, 0xffffffff);
    assert(l.fb[20 * 352 + 10] == 0xffffff && l.fb[21 * 352 + 11] == 0xffffff);
    assert(l.fb[20 * 352 + 12] == 0 && l.fb[22 * 352 + 10] == 0);

    /* rm42_lcd_logo_stripes: register 2A = 0x87, LCD DMA of 16-bit units: a pixel is a unit pair, 0x00RRGGBB */
    v = 0x87;
    ctrl_reg(&l, 0x2a, &v, 1);
    ctrl_window(&l, 0, 0, 351, 415);
    rm1_sossi_write(&l, 0x10, 0x00123456);
    assert(l.fb[0] == 0x123456);
    {
        static const uint32_t row[2] = { 0x00ffffff, 0x00000099 };   /* white, the logo's blue */
        uint32_t src = 0x10002000;

        memcpy(guest + 0x2000, row, sizeof(row));
        ctrl_window(&l, 0, 1, 1, 1);
        RM1_D(&l, 0xbc0) = 0x141;                   /* 16-bit elements [V CSDP] */
        RM1_D(&l, 0xbc2) = 0x0000;
        RM1_D(&l, 0xbc8) = src;
        RM1_D(&l, 0xbca) = src >> 16;
        RM1_D(&l, 0xbe0) = 4;                       /* 2 units a pixel [V 704 units a 352-pixel row] */
        RM1_D(&l, 0xbe4) = 1;
        RM1_D(&l, 0xbc2) |= 0x80;
        rm1_lcd_dma_run(&l);
    }
    assert(l.fb[352] == 0xffffff && l.fb[353] == 0x000099);

    /* rm42_lcd_reads_zero: reading a register (04 is RDDID on a DCS panel) gives 0 */
    ctrl_reg(&l, 0x04, NULL, 0);
    assert(rm1_sossi_read(&l, 0x10) == 0 && rm1_sossi_read(&l, 0x10) == 0);

    /* rm42_lcd_rotate_90: register 34 = 3 swaps window to 416x352 and rotates on render */
    {
        static uint32_t out[352 * 416];
        uint8_t rot3 = 3, rot0 = 0, sc1 = 1, sc0 = 0;

        ctrl_reg(&l, 0x34, &rot3, 1);
        assert(rm1_lcd_src_w(&l) == 416 && rm1_lcd_src_h(&l) == 352);
        ctrl_window(&l, 415, 0, 415, 0);
        ctrl_reg(&l, 0x48, NULL, 0);
        rm1_sossi_write(&l, 0x10, 0x00ab1234);
        assert(l.fb[0 * 416 + 415] == 0xab1234);
        ctrl_window(&l, 0, 351, 0, 351);
        ctrl_reg(&l, 0x48, NULL, 0);
        rm1_sossi_write(&l, 0x10, 0x00cd5678);
        assert(l.fb[351 * 416 + 0] == 0xcd5678);
        rm1_lcd_render(&l, out);
        assert(out[415 * 352 + 351] == 0xab1234);
        assert(out[0 * 352 + 0] == 0xcd5678);

        /* rm42_lcd_scale_2x: register 36 = 1 replicates each pixel 2x2 */
        ctrl_reg(&l, 0x34, &rot0, 1);
        ctrl_reg(&l, 0x36, &sc1, 1);
        ctrl_window(&l, 10, 20, 10, 20);
        ctrl_reg(&l, 0x48, NULL, 0);
        rm1_sossi_write(&l, 0x10, 0x0042beef);
        rm1_lcd_render(&l, out);
        assert(out[40 * 352 + 20] == 0x42beef && out[40 * 352 + 21] == 0x42beef);
        assert(out[41 * 352 + 20] == 0x42beef && out[41 * 352 + 21] == 0x42beef);
        ctrl_reg(&l, 0x36, &sc0, 1);
    }
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
    lcd_ctrl_tests();
    ssi_tests();
    printf("nokia6630_lcd_ssi_test: ok\n");
    return 0;
}
