# Nokia 6630 (RM-1): lcd.ldd, SoSSI and the LCD DMA (what a QEMU model must do)

Static reading of `lcd.ldd` (ROM 0x5003d0b0, 31968 bytes, ARM code) in `eka1/rm1/core/ROM.bin`. No emulator run.
[V] = read in the code (address given), [I] = inferred / best guess.
Peripheral VA = phys - 0xFFF00000 + 0x58000000.

## 1. Summary

- The panel is a MIPI-DCS-style smart panel with its own frame memory, 176x208, behind the OMAP1610 **SoSSI**
  (0xFFFBAC00). Commands and their parameters go through the "B" channel (0x1C count/type, 0x20 FIFO);
  pixels go through the "A" channel (0x04 count/type, 0x10 FIFO), either written by the CPU or pushed by the
  OMAP1610 **LCD DMA channel** (DMA block 0xFFFED800 + 0xBC0 = 0xFFFEE3C0) with "external LCD" set [V].
- The OMAP LCD controller (0xFFFEC000) is never touched [V: no literal in the whole ROM].
- To get past the boot spin: 0x08 must read with **bit 2 (FIFO ready) and bit 3 (idle/transfer done)** set [V].
  The existing stub in `src/qemu-ngage/hw/arm/nokia6630_soc.c` sets only bit 2: `ec00` then retries 20 times
  and re-initialises (0x5003ec00..0x5003ec4c) [V].
- Panel reads (ID 0x04, status 0x09) come back byte by byte from 0x10; the driver checks them in its ESD
  check and self-test [V] (values in section 5).

## 2. Register access helpers in lcd.ldd [V]

All SoSSI accesses are 32-bit through stubs: write `0x50043944..0x500439c4` (kernel write32, ekern 0x5000c574),
read `0x50043a4c..0x50043a94` (read32, 0x5000c56c), modify `0x500439d4..0x50043a34` (ekern 0x5000c57c,
args `(addr, clear, set)` [I from use: reset pulse = set bit1 then clear bit1/set bit0]).
LCD DMA accesses are 16-bit through ekern helpers taking an **offset from the DMA base 0xFFFED800**:
`0x5004459c` = write16(off, v), `0x50044488` = modify16(off, clear, set), `0x500445a8` = read16(off) [V offsets
0xBC0..0xBF4 match the OMAP1610 LCD channel layout].

## 3. SoSSI registers as lcd.ldd uses them

| off | name (Linux) | lcd.ldd use | model |
|---|---|---|---|
| 0x00 | ID | stubs exist, never called [V] | read 0 |
| 0x04 | INIT1 (A) | init `0x833C0000` (0x5003eb84); before RAMWR `0x823C0000 \| (bytes-1)` (0x5003efb0); for panel reads `0x0CCC0000` (saved, set, restored around reads: 0x5003eed0, 0x5003f1a8); bit 30 cleared when the DMA path is armed (0x50043394), set again at 0x5003f39c [V]. Bits 0-17 = cycle count - 1, bit 18 = data (1) / command (0) [I, Linux sossi.c names; consistent with 0x1C] | store |
| 0x08 | INIT2 | init `0x141` (0x5003eb80); reset pulse: set bit1, then clear bit1 + set bit0 (0x5003ec60); bit5 pulsed set/clear before loading 0x14 (0x5003ec90); bit6 set by TE-on (0x5003f0a0/f0d4), cleared by TE-off (0x5003f108) [V]. **Polled: bit2** (0x5003ebd8, no timeout, before every FIFO word and before reads) and **bit3** (0x5003ec50, 20 tries then recovery, before every command) [V] | store; reads OR 0x0C |
| 0x0C | INIT3 | `0x67` for display modes 7 (EColor64K) and 10 (EColor4K), `0x47` for mode 11 (EColor16MU) (0x5003ecd0/ed18/ed60) [V]. Low 5 bits = bus width-1 (8 bit), bits 5.. = picks per word-1 (4 or 3) [I, Linux] | store; tells the pixel format |
| 0x10 | FIFO (A) | CPU pixel writes: 32-bit words after waiting for 0x08 bit2 (0x5003eeb8); **reads**: one panel byte per read, low 8 bits (0x5003eee4, 0x5003f1d0..f1f0) [V] | write: pixel stream; read: next byte of the read queue |
| 0x14 | REOTABLE | 5 words after the bit5 pulse: table 0x50044878 (mode 7), 0x5004488c (mode 10), 0x500448a0 (mode 11) [V] | store; first word identifies the mode (below) |
| 0x18 | TEARING | init `0x1C000147`; TE-on `0x0C000147` / `0x1C000147`; TE self-test `0x0C000009`, saved and restored [V] | store |
| 0x1C | INIT1B | before a command byte `0`; before parameters `(n-1) \| 0x40000` (0x5003f324) [V] | B-channel count + bit18 = data |
| 0x20 | FIFOB | command byte, then parameters packed LSB first, 4 per 32-bit write (CASET word `end<<24 \| start<<8`, 0x5003ef3c) [V]; 0x2D LUT = 128 bytes as 32 words, each after a bit2 wait (0x5003f35c) [V] | B-channel stream |

Display-mode tables [V]: mode 7 -> REO word0 `0x9AC5A928`, INIT3 0x67; mode 10 -> REO word0 `0x92859CC5`,
INIT3 0x67; mode 11 -> REO word0 `0xAB49CA30`, INIT3 0x47. Mode numbers are Symbian TDisplayMode [I].

## 4. Panel command protocol (DCS) [V commands; meanings from DCS I]

`send_cmd(cmd, n)` = 0x5003f324: wait 0x08 bit3; W 0x1C = 0; W 0x20 = cmd; if n: W 0x1C = (n-1) | 0x40000; then
the caller writes the n parameter bytes to 0x20 (one word, LSB first, or a word loop for n > 4).

| cmd | params | where |
|---|---|---|
| 0x01 SWRESET | - | power-on 0x500425b4 |
| 0x11 SLPOUT / 0x10 SLPIN | - | 0x5003efa4 / 0x5003ef98 |
| 0x13 NORON / 0x12 PTLON | - | 0x5003ef8c / 0x5003f318 |
| 0x29 DISPON / 0x28 DISPOFF | - | 0x5003ef68 / 0x5003ef74 |
| 0x2A CASET | 4: 0, xs, 0, xe | 0x5003ef3c (full: 0..175) |
| 0x2B RASET | 4: 0, ys, 0, ye | 0x5003ef00 |
| 0x2C RAMWR | - then INIT1 = 0x823C0000\|(bytes-1) | 0x5003efb0 |
| 0x2D RGBSET (LUT) | 128 bytes from 0x500448c4 | 0x5003f474 |
| 0x2E RAMRD | - (only W 0x04 = 0x0CCC0000) | 0x5003f02c |
| 0x30 PTLAR | 4 | 0x5003f2ec |
| 0x34/0x35 TEOFF/TEON | 0x35: 1 byte 0 | 0x5003f108 / 0x5003f0a0 |
| 0x36 MADCTL | 1: 0x00 (init) or 0xC0 (before the ESD read) | 0x5003f050 / 0x5003f06c |
| 0x38/0x39 IDMOFF/IDMON | - | 0x5003f094 / 0x5003f088 |
| 0x3A COLMOD | 1: 3 (12 bpp), 5 (16 bpp, modes 7 and 10), 6 (18 bpp, mode 11) | 0x5003f228/f244/f260 |
| 0x26 GAMSET | 1: 4 | 0x5003f3a8 |
| 0x24 (vendor) | 1: 5 / 15 / 30 | 0x5003efd8.. |
| 0x04 RDDID / 0x09 RDDST | reads, section 5 | 0x5003f1a8 / 0x5003f128 |
| 0x23, 0x02 | - | 0x5003f210 / 0x5003f21c |

Power-on (0x50042588) [V]: GPIO 4 bit 12 (0xFFFBBC00, panel reset via ekern GPIO helpers) high/low with delays,
SWRESET, wait, SLPOUT, wait, DISPON, LUT, IDMOFF+NORON, CASET 0..175, COLMOD 6, GAMSET 4, MADCTL 0, then RDDST.
Window bytes per RAMWR [V]: modes 7/10 `w*h*2` (0x5003edd0, 0x5003eda8 = 352*rows), mode 11 `w*h*3`
(0x5003ee14), 12-bpp `rows*264` (0x5003ed84, unused path [I]).

## 5. Read-back values the driver checks [V]

Reads: after the command, W 0x04 = 0x0CCC0000, then each read of 0x10 returns one byte; the **first byte is a
dummy** (discarded).
- RDDID 0x04: bytes dummy, ID1, ID2, ID3 -> `ID1<<16|ID2<<8|ID3`. ESD check (0x5003f980) and self-test (0x5003fd20)
  accept `ID1` in {0x29, 0x45, 0x83}; anything else counts as an error. Use **ID1 = 0x45** [I which panel].
- RDDST 0x09: bytes dummy, b1..b4 -> `S = b1<<24|b2<<16|b3<<8|b4`. ESD check (0x5003f968..f9dc) needs
  `(S & 0xA0000000) == 0xA0000000`, `S & 0x00020400 != 0`, `S & 0x1580E000 == 0`; self-test needs b3 bit1 = 0.
  Use **S = 0xA0020400** (bytes A0 02 04 00). Wrong values only trigger a periodic re-init + CPU redraw
  (0x5003f9fc..fa74), not a hang.

## 6. Pixel transfer

### CPU path [V]
RASET/CASET + RAMWR, then 32-bit writes to 0x10 (each after bit2): modes 7/10 two 16-bit pixels per word
(low half first: 0x500427e0 packs bytes 0,1 | 2,3), mode 11 one 32-bit pixel per word (0x5004284c, 176 per row).
Used at power-on (white fill, 0x50042788) and after an ESD recovery.

### LCD DMA path (normal updates) [V register writes, I bit meanings]
Open (0x50043e0c): CCR(0xBC2) clear 0x300; CTRL(0xBC4) = 0x106; CCR = 0x3440; SRC_EI_B1(0xBD8) = 1;
SRC_FI_B1_U(0xBF4) = 0; interrupt bound by name **"Lv1_DMAChLCD"** (ISR 0x50043f04: CTRL clear 0x38, CCR clear
0x80, completion callback). Also "Lv2_SoSSIMatch" (TE) is named in the module [V string 0x50044a6c area].
Arm (0x50043364): DMA CTRL |= 0x100 (external LCD), MOD_CONF_CTRL_1 (0xFFFE1110) |= 0x10000, SoSSI 0x04 clear
bit30. Disarm (0x500433bc) reverses it.
Rect update (0x50043f90: x, w, y, h, buffer): panel window set first via vtable +20 = 0x5003f52c (CASET/RASET/RAMWR);
then CTRL clear 0x38; CSDP(0xBC0) = 0x141 (16-bit elements) or 0x142 (32-bit, mode 11);
TOP_B1 (0xBC8 low, 0xBCA high) = phys(buf + (y*176 + x)*bpp); SRC_FI_B1_L(0xBDA) = (176-w)*bpp + 1;
CCR = 0x3440; SRC_EN_B1(0xBE0) = w (elements per frame = pixels per row); SRC_FN_B1(0xBE4) = h; CCR |= 0x80.
Frame buffer: a 176-pixel-stride buffer owned by lcd.ldd, 2 bytes/pixel (modes 7/10) or 4 (mode 11), passed
as `buf` (physical address via ekern 0x5000bc88-class helper 0x50044398) [V stride, I owner].
Full-screen variant (0x50042954): CCR = 0, CTRL = 0x100, CCR = 0x1440, CSDP = 0x102 (32-bit), TOP_B1 = phys,
SRC_EN_B1 = 0x4780 (18304 words = 176*208*2 bytes), SRC_FN_B1 = 1, CCR |= 0x80; completion **polled**:
CCR bit7 must clear (0x50042a2c, ~12000 tries) [V].
Element walk (OMAP double index, CCR bits 12-13 = 3): element size + (EI-1) inside a row, element size + (FI-1)
at the row end -> the (176-w)*bpp gap [I, matches the arithmetic].
Completion: clear CCR bit7, set CTRL bit3 (block 1 done) and raise the LCD-DMA interrupt when CTRL bit1 is set
[I: CTRL 0x106 sets bit1; ISR clears 0x38 with a plain write of 0, so treat bits 3-5 as plain stored bits].
Interrupt number: ekern resolves "Lv1_DMAChLCD"; look the name up in ekern's interrupt table [I; Linux
omap1 calls it INT_DMA_LCD, level-1].

### Pixel formats on the host [I]
- INIT3 = 0x47 (mode 11): each pushed 32-bit unit = 0x00RRGGBB, one pixel.
- INIT3 = 0x67 and REO word0 0x9AC5A928 (mode 7): 16-bit units RGB565.
- INIT3 = 0x67 and REO word0 0x92859CC5 (mode 10): 16-bit units 0x0RGB (4444).
The REOTABLE bit shuffling onto the 8-bit bus and the 18-bpp panel is not modelled: decode the CPU/DMA unit
directly, the panel frame memory is the host surface.

## 7. Proposed QEMU code (replaces the register-file stub in nokia6630_soc.c)

```c
/* SoSSI + panel (DCS, 176x208) + OMAP1610 LCD DMA channel (0xFFFEE3C0) for lcd.ldd.
 * docs/research/rm1-lcd-sossi.md. */
#define PW 176
#define PH 208

typedef struct {
    MemoryRegion mr, dma_mr;
    uint32_t reg[9];            /* 0x00..0x20 */
    /* B channel (commands) */
    uint32_t b_left;            /* parameter bytes still expected */
    uint8_t cmd, prm[8]; unsigned nprm;
    /* panel */
    unsigned xs, xe, ys, ye, cx, cy;
    bool writing;               /* after RAMWR */
    uint8_t rq[8]; unsigned rq_n, rq_i;  /* read queue (0x10 reads) */
    uint32_t reo0;              /* first REOTABLE word since the bit5 pulse */
    bool reo_first;
    uint32_t fb[PW * PH];       /* panel frame memory, XRGB8888 */
    QemuConsole *con;
    /* LCD DMA, 16-bit regs, offset 0xBC0..0xBFF of the DMA block */
    uint16_t d[32];
    qemu_irq dma_irq;
} RM1Lcd;

static void panel_px(RM1Lcd *s, uint32_t rgb)
{
    if (!s->writing) return;
    if (s->cx < PW && s->cy < PH) s->fb[s->cy * PW + s->cx] = rgb;
    if (++s->cx > s->xe) { s->cx = s->xs; if (++s->cy > s->ye) s->cy = s->ys; }
}

static uint32_t rgb565(uint16_t v)  { return ((v >> 11 & 31) * 255 / 31) << 16 | ((v >> 5 & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31; }
static uint32_t rgb444(uint16_t v)  { return (v >> 8 & 15) * 0x110000 | (v >> 4 & 15) * 0x1100 | (v & 15) * 0x11; }

static void panel_unit16(RM1Lcd *s, uint16_t v)
{
    panel_px(s, s->reo0 == 0x92859cc5 ? rgb444(v) : rgb565(v));
}

/* one CPU FIFO word or 32-bit DMA element */
static void panel_word(RM1Lcd *s, uint32_t w)
{
    if ((s->reg[3] & 0xff) == 0x47) panel_px(s, w & 0xffffff);
    else { panel_unit16(s, w); panel_unit16(s, w >> 16); }
}

static void panel_cmd_done(RM1Lcd *s)   /* command + all parameters received */
{
    switch (s->cmd) {
    case 0x2a: s->xs = s->prm[1]; s->xe = MIN(s->prm[3], PW - 1); break;
    case 0x2b: s->ys = s->prm[1]; s->ye = MIN(s->prm[3], PH - 1); break;
    }
}

static void panel_cmd(RM1Lcd *s, uint8_t c)
{
    s->cmd = c; s->nprm = 0; s->writing = false;
    switch (c) {
    case 0x2c: s->writing = true; s->cx = s->xs; s->cy = s->ys; break;
    case 0x04: { static const uint8_t r[] = {0, 0x45, 0x00, 0x00};       memcpy(s->rq, r, 4); s->rq_n = 4; s->rq_i = 0; break; }
    case 0x09: { static const uint8_t r[] = {0, 0xa0, 0x02, 0x04, 0x00}; memcpy(s->rq, r, 5); s->rq_n = 5; s->rq_i = 0; break; }
    case 0x29: case 0x13: dpy_gfx_update_full(s->con); break;
    }
}

static void fifob_write(RM1Lcd *s, uint32_t v)
{
    uint32_t ctl = s->reg[7];                     /* 0x1C */
    if (!(ctl & 0x40000)) { panel_cmd(s, v & 0xff); s->b_left = 0; return; }
    if (!s->b_left) s->b_left = (ctl & 0x3ffff) + 1;
    for (int i = 0; i < 4 && s->b_left; i++, s->b_left--) {
        if (s->nprm < sizeof(s->prm)) s->prm[s->nprm] = v >> (8 * i);
        s->nprm++;
    }
    if (!s->b_left) panel_cmd_done(s);
}

static uint64_t sossi_read(void *opaque, hwaddr a, unsigned size)
{
    RM1Lcd *s = opaque;
    switch (a & ~3) {
    case 0x08: return s->reg[2] | 0x0c;           /* bit2 FIFO ready, bit3 idle */
    case 0x10: return s->rq_i < s->rq_n ? s->rq[s->rq_i++] : 0;
    default:   return a < 0x24 ? s->reg[a >> 2] : 0;
    }
}

static void sossi_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RM1Lcd *s = opaque;
    if (a >= 0x24) return;
    switch (a & ~3) {
    case 0x08: if (!(s->reg[2] & 0x20) && (v & 0x20)) s->reo_first = true; break;
    case 0x10: panel_word(s, v); return;
    case 0x14: if (s->reo_first) { s->reo0 = v; s->reo_first = false; } break;
    case 0x20: fifob_write(s, v); return;
    }
    s->reg[a >> 2] = v;
}

/* LCD DMA: registers at DMA base + 0xBC0 (phys 0xFFFEE3C0), 16-bit */
#define D(o) s->d[((o) - 0xbc0) >> 1]
static void lcd_dma_run(RM1Lcd *s)
{
    hwaddr src = D(0xbc8) | (uint32_t)D(0xbca) << 16;
    unsigned esz = 1u << (D(0xbc0) & 3);          /* CSDP data type: 1 = 16 bit, 2 = 32 bit */
    unsigned en = D(0xbe0), fn = D(0xbe4);
    bool dbl = (D(0xbc2) >> 12 & 3) == 3;
    int32_t ei = (int16_t)D(0xbd8), fi = (int32_t)(D(0xbda) | (uint32_t)D(0xbf4) << 16);
    for (unsigned f = 0; f < fn; f++) {
        for (unsigned e = 0; e < en; e++) {
            uint32_t v = 0;
            cpu_physical_memory_read(src, &v, esz);
            if (esz == 4) panel_word(s, le32_to_cpu(v)); else panel_unit16(s, le16_to_cpu(v));
            src += esz + (dbl && e + 1 < en ? ei - 1 : 0);
        }
        if (dbl) src += fi - 1;
    }
    D(0xbc2) &= ~0x80;                            /* channel disabled = done (polled) */
    D(0xbc4) |= 0x08;                             /* block 1 done */
    if (D(0xbc4) & 0x02) qemu_irq_pulse(s->dma_irq);
    dpy_gfx_update(s->con, 0, 0, PW, PH);
}

static uint64_t lcddma_read(void *opaque, hwaddr a, unsigned size) { RM1Lcd *s = opaque; return s->d[(a >> 1) & 31]; }
static void lcddma_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RM1Lcd *s = opaque;
    s->d[(a >> 1) & 31] = v;
    if (a == 0x02 && (v & 0x80)) lcd_dma_run(s);  /* CCR enable; run synchronously, or from a short timer */
}
/* map: sossi 0xFFFBAC00 size 0x40, lcddma 0xFFFEE3C0 size 0x40 (overlap over the 1510 DMA model),
 * console 176x208 32bpp backed by s->fb (qemu_create_displaysurface_from). */
```

Notes for the model:
- Running the DMA synchronously when CCR bit7 is written is enough for the polling path; for the IRQ path a
  ~1 ms timer before completion is safer (the ISR expects the interrupt after `0x500443c8` returns) [I].
- The 1510 DMA model in QEMU (omap_dma 3.1) puts the LCD channel at +0x300, not +0xBC0; overlay a small region at
  0xFFFEE3C0 (higher priority) rather than switching the whole DMA model [I].
- TE ("Lv2_SoSSIMatch") is used only by the self-test and TE mode ([r5,#192]); pulse it at ~60 Hz while INIT2 bit6
  is set if a TE-mode path is hit [I].
- MOD_CONF_CTRL_1 (0xFFFE1110), ARM_IDLECT2 (0xFFFECE08), ULPD 0xFFFE087C and GPIO4 (0xFFFBBC00) bit 12 are
  only written at power-on / mode change: RAM-like registers suffice [V use, I meaning].
