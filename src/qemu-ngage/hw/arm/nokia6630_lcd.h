/*
 * The Nokia 6630's display: the OMAP1610 SoSSI (0xFFFBAC00) with a DCS smart panel of 176x208 behind it,
 * and the 1610 LCD DMA channel (DMA block + 0xBC0 = 0xFFFEE3C0) that feeds it, as lcd.ldd drives them.
 * docs/research/rm1-lcd-sossi.md has the evidence. No QEMU types (tests/nokia6630_lcd_test.c).
 *
 * SoSSI: 04 A-channel config (pixels), 08 control/status (reads | 0x0C: FIFO ready, idle), 0C bus format
 * (0x67: 16-bit units, 0x47: 0x00RRGGBB units), 10 A FIFO (pixel words; reads: the panel's read queue,
 * one byte a read, the first a dummy), 14 reorder table (first word after the 08 bit-5 pulse tells RGB565
 * 0x9AC5A928 from 4444 0x92859CC5), 18 tearing, 1C B-channel count (0: next 20 write is a command; else
 * bit 18 + (n - 1) parameter bytes follow), 20 B FIFO (command byte, parameters LSB first, 4 a word).
 * Panel: 2A CASET / 2B RASET (0, start, 0, end), 2C RAMWR, 04 RDDID (dummy, 45 00 00), 09 RDDST
 * (dummy, A0 02 04 00) [V checks in lcd.ldd; I which panel]; other commands change nothing shown.
 * LCD DMA (16-bit registers from 0xBC0): C0 CSDP (bits 0-1 element size: 1 = 16, 2 = 32 bits), C2 CCR
 * (bit 7 enable/busy, bits 12-13 = 3 double-indexed source), C4 CTRL (bit 1 interrupt enable, bit 3 block
 * done, bit 8 to the SoSSI), C8/CA source address, D8 element index, DA/F4 frame index, E0 elements a
 * frame, E4 frames. A run sends every element to the A channel, clears CCR bit 7, sets CTRL bit 3.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA6630_LCD_H
#define NOKIA6630_LCD_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define RM1_LCD_W 176
#define RM1_LCD_H 208

typedef struct Rm1Lcd {
    uint32_t reg[9];                    /* SoSSI 0x00..0x20 */
    uint32_t b_left;                    /* parameter bytes still expected */
    uint8_t cmd, prm[8];
    unsigned nprm;
    unsigned xs, xe, ys, ye, cx, cy;
    bool writing;                       /* after RAMWR */
    uint8_t rq[8];
    unsigned rq_n, rq_i;
    uint32_t reo0;
    bool reo_first;
    uint32_t fb[RM1_LCD_W * RM1_LCD_H]; /* the panel's frame memory, 0x00RRGGBB */
    unsigned dirty_y0, dirty_y1;        /* rows changed since the last rm1_lcd_take_dirty (y1 exclusive) */
    uint16_t d[32];                     /* LCD DMA 0xBC0..0xBFF */
    /* the board: read guest memory for the DMA */
    void (*mem_read)(struct Rm1Lcd *l, uint64_t addr, void *buf, unsigned len);
    void *opaque;
} Rm1Lcd;

static inline void rm1_lcd_reset(Rm1Lcd *l)
{
    memset(l->reg, 0, sizeof(l->reg));
    l->b_left = l->nprm = 0;
    l->xs = l->ys = l->cx = l->cy = 0;
    l->xe = RM1_LCD_W - 1;
    l->ye = RM1_LCD_H - 1;
    l->writing = l->reo_first = false;
    l->rq_n = l->rq_i = 0;
    l->reo0 = 0;
    memset(l->d, 0, sizeof(l->d));
    l->dirty_y0 = RM1_LCD_H;
    l->dirty_y1 = 0;
}

static inline void rm1_lcd_px(Rm1Lcd *l, uint32_t rgb)
{
    if (!l->writing) {
        return;
    }
    if (l->cx < RM1_LCD_W && l->cy < RM1_LCD_H) {
        l->fb[l->cy * RM1_LCD_W + l->cx] = rgb;
        if (l->cy < l->dirty_y0) {
            l->dirty_y0 = l->cy;
        }
        if (l->cy + 1 > l->dirty_y1) {
            l->dirty_y1 = l->cy + 1;
        }
    }
    if (++l->cx > l->xe) {
        l->cx = l->xs;
        if (++l->cy > l->ye) {
            l->cy = l->ys;
        }
    }
}

static inline uint32_t rm1_rgb565(uint16_t v)
{
    return ((v >> 11 & 31) * 255 / 31) << 16 | ((v >> 5 & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
}

static inline uint32_t rm1_rgb444(uint16_t v)
{
    return (v >> 8 & 15) * 0x110000 | (v >> 4 & 15) * 0x1100 | (v & 15) * 0x11;
}

static inline void rm1_lcd_unit16(Rm1Lcd *l, uint16_t v)
{
    rm1_lcd_px(l, l->reo0 == 0x92859cc5 ? rm1_rgb444(v) : rm1_rgb565(v));
}

/* one CPU FIFO word or 32-bit DMA element */
static inline void rm1_lcd_word(Rm1Lcd *l, uint32_t w)
{
    if ((l->reg[3] & 0xff) == 0x47) {
        rm1_lcd_px(l, w & 0xffffff);
    } else {
        rm1_lcd_unit16(l, w);
        rm1_lcd_unit16(l, w >> 16);
    }
}

static inline void rm1_lcd_cmd_done(Rm1Lcd *l)
{
    switch (l->cmd) {
    case 0x2a:
        l->xs = l->prm[1] < RM1_LCD_W ? l->prm[1] : RM1_LCD_W - 1;
        l->xe = l->prm[3] < RM1_LCD_W ? l->prm[3] : RM1_LCD_W - 1;
        break;
    case 0x2b:
        l->ys = l->prm[1] < RM1_LCD_H ? l->prm[1] : RM1_LCD_H - 1;
        l->ye = l->prm[3] < RM1_LCD_H ? l->prm[3] : RM1_LCD_H - 1;
        break;
    }
}

static inline void rm1_lcd_cmd(Rm1Lcd *l, uint8_t c)
{
    static const uint8_t id[] = { 0, 0x45, 0x00, 0x00 };
    static const uint8_t st[] = { 0, 0xa0, 0x02, 0x04, 0x00 };

    l->cmd = c;
    l->nprm = 0;
    l->writing = false;
    switch (c) {
    case 0x2c:
        l->writing = true;
        l->cx = l->xs;
        l->cy = l->ys;
        break;
    case 0x04:
        memcpy(l->rq, id, sizeof(id));
        l->rq_n = sizeof(id);
        l->rq_i = 0;
        break;
    case 0x09:
        memcpy(l->rq, st, sizeof(st));
        l->rq_n = sizeof(st);
        l->rq_i = 0;
        break;
    }
}

static inline void rm1_lcd_fifob(Rm1Lcd *l, uint32_t v)
{
    uint32_t ctl = l->reg[7];           /* 0x1C */

    if (!(ctl & 0x40000)) {
        rm1_lcd_cmd(l, v & 0xff);
        l->b_left = 0;
        return;
    }
    if (!l->b_left) {
        l->b_left = (ctl & 0x3ffff) + 1;
    }
    for (int i = 0; i < 4 && l->b_left; i++, l->b_left--) {
        if (l->nprm < sizeof(l->prm)) {
            l->prm[l->nprm] = v >> (8 * i);
        }
        l->nprm++;
    }
    if (!l->b_left) {
        rm1_lcd_cmd_done(l);
    }
}

static inline uint32_t rm1_sossi_read(Rm1Lcd *l, uint32_t a)
{
    switch (a & ~3u) {
    case 0x08:
        return l->reg[2] | 0x0c;
    case 0x10:
        return l->rq_i < l->rq_n ? l->rq[l->rq_i++] : 0;
    }
    return a < 0x24 ? l->reg[a >> 2] : 0;
}

static inline void rm1_sossi_write(Rm1Lcd *l, uint32_t a, uint32_t v)
{
    if (a >= 0x24) {
        return;
    }
    switch (a & ~3u) {
    case 0x08:
        if (!(l->reg[2] & 0x20) && (v & 0x20)) {
            l->reo_first = true;
        }
        break;
    case 0x10:
        rm1_lcd_word(l, v);
        return;
    case 0x14:
        if (l->reo_first) {
            l->reo0 = v;
            l->reo_first = false;
        }
        break;
    case 0x20:
        rm1_lcd_fifob(l, v);
        return;
    }
    l->reg[a >> 2] = v;
}

#define RM1_D(l, o) ((l)->d[((o) - 0xbc0) >> 1])

/* a whole LCD DMA transfer (lcd.ldd's rectangle or full-screen form); the caller raises the interrupt */
static inline void rm1_lcd_dma_run(Rm1Lcd *l)
{
    uint64_t src = RM1_D(l, 0xbc8) | (uint32_t)RM1_D(l, 0xbca) << 16;
    unsigned esz = (RM1_D(l, 0xbc0) & 3) == 2 ? 4 : (RM1_D(l, 0xbc0) & 3) == 1 ? 2 : 1;
    unsigned en = RM1_D(l, 0xbe0), fn = RM1_D(l, 0xbe4);
    bool dbl = ((RM1_D(l, 0xbc2) >> 12) & 3) == 3;
    int32_t ei = (int16_t)RM1_D(l, 0xbd8);
    int32_t fi = (int32_t)(RM1_D(l, 0xbda) | (uint32_t)RM1_D(l, 0xbf4) << 16);

    for (unsigned f = 0; f < fn; f++) {
        for (unsigned e = 0; e < en; e++) {
            uint8_t b[4] = { 0 };

            l->mem_read(l, src, b, esz);
            if (esz == 4) {
                rm1_lcd_word(l, b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24);
            } else if (esz == 2) {
                rm1_lcd_unit16(l, b[0] | b[1] << 8);
            }
            src += esz;
            if (dbl) {
                src += e + 1 < en ? ei - 1 : fi - 1;
            }
        }
    }
    RM1_D(l, 0xbc2) &= ~0x80;           /* channel off = done (the full-screen form polls it) */
    RM1_D(l, 0xbc4) |= 0x08;            /* block done */
}

/* rows changed since the last call: [*y0, *y1); false when none */
static inline bool rm1_lcd_take_dirty(Rm1Lcd *l, unsigned *y0, unsigned *y1)
{
    if (l->dirty_y0 >= l->dirty_y1) {
        return false;
    }
    *y0 = l->dirty_y0;
    *y1 = l->dirty_y1;
    l->dirty_y0 = RM1_LCD_H;
    l->dirty_y1 = 0;
    return true;
}

#endif
