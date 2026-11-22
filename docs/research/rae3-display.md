# Nokia 9210 (RAE-3) display: what a board model needs

Static research (no runs). [V] = read in the ROM code, [I] = inferred. Evidence: headless Ghidra output in
`build/evidence/ghidra-rae3/` (`ekern.c(.lst)`, `boot.c.lst`, `ecust.c(.lst)`, `scdv.c`, `epbus.c`).

## Summary

- The LCD controller is NOT in the 0x05000000 peripheral block. It sits in the 4 KB block at **PA 0x04000000**
  (the block the board file calls "memory controller"), mapped by the bootstrap at **VA 0x59000000**
  [V boot 0x50000E28: map(VA 0x59000000, PA 0x04000000, 0x28, 0x1000)]. The same block also holds clock/power
  bits (0x00, 0x04) and chip-select timings (0x10..0x18).
- The frame buffer is a fixed region at the **start of RAM, PA 0x03000000** (= kernel VA 0x5A000000), sized by
  the bootstrap from the ROM header: width 0x280 (640) [V ROM+0x38], height 0xC8 (200) [V ROM+0x3C],
  bpp 0x10 (16) [V ROM+0x40]; size = 640*200*16/8 + 0x20 = 0x3E820, the super page starts at the next 4 KB
  page (0x0303F000) [V boot 0x50000E8C].
- Layout [V kernel 0x50001F18 .. 0x50002040]: the kernel writes
  `LCD+0x40 = 0x80`, `LCD+0x20 = 0x03000000` (DMA/frame start), `LCD+0x24 = 0x03000000 + 0x20 + size - 2`
  (= 0x0303E81E, address of the last halfword, inclusive). So the DMA region is 32 bytes of header
  (a 16 x 16-bit palette, OMAP1-LCD style) followed by 640x200 16-bit pixels at **PA 0x03000020** [I for the
  0x20 split; the +0x20/-2 arithmetic is V].
- Pixel format: `Wsini.ini` says `WINDOWMODE COLOR4096` [V] -> EColor4K: one 16-bit little-endian word per
  pixel, `0000 RRRR GGGG BBBB`; stride 640*2 = 1280 bytes; row 0 at the top, pixel 0 at the left [I, standard
  EPOC]. No palette lookup is needed for this mode [I] (the 32-byte header can be ignored; palette[0] would
  carry a bpp code on an OMAP1-type controller [I]).
- Interrupt: the kernel's name table has "FiqLcdMatch"/"IrqLcdMatch" (source 45) but no code reads the LCD
  status register (0x44 accessors 0x500032DC/0x500032EC are exported but nothing imports or calls them) [V
  import scan]. Nothing waits for an LCD interrupt or polls an LCD status bit [V as far as ECust and the kernel
  go]. The model need not raise it.
- Cover display: EPOC code never drives it [I]; it belongs to the phone (CMT) side. An optional map of
  VA 0x59010000 -> PA 0x02400000 happens only if ROM header +0x58 bit 30 is set; it is 0 in this ROM [V].
- The 0x0500A000 block written by EpBus.dll is the MMC/MultiMediaCard host (command numbers, response bytes
  read by >>24 shifts, 20-iteration busy waits), not the LCD [I].

## Registers (LCD/system block, PA 0x04000000, VA 0x59000000)

Accessors are kernel exports (EKern 0x50003074..0x50003368); the only importer is ECust.dll (variant), whose
init is `FUN_50093F20`, display-on `FUN_50094154`, display-off `FUN_5009420C` [V].

| Off | Access | Values written / read | Meaning |
|-----|--------|----------------------|---------|
| 0x00 | RMW (IRQs off) | bit0 set at display on, cleared at off/init; bits 0x30000000 = 0 then 0x30000000; bits 0xFF0000; bits 0x30 read | clock/power control; bit0 = LCD clock enable [I] |
| 0x04 | RMW | init: = 0x160405 (mask 0xFFFFFFFF); bits 0xC00 = 0 then 0xC00; bit 0x200 set then cleared; bits 5 read | clock/power control [I] |
| 0x10 / 0x14 / 0x18 | W | 0x10004050 / 0x4050 / 0x4050 | chip-select (EMIF) timings, not LCD [I] |
| 0x20 | W (R) | 0x03000000 | LCD DMA frame start (PA) [V write, I meaning] |
| 0x24 | W (R) | 0x0303E81E | LCD DMA frame end, last halfword (PA) [V write, I meaning] |
| 0x28..0x38 | W/R | not used at boot by kernel/ECust | (exported accessors only) |
| 0x40 | W/R | 0x80 (kernel boot, off), 0x81 (on); read & 0x382 by a kernel query 0x50001A40 | LCD control; bit0 = enable [I] |
| 0x44 | R / W 0 | never used | LCD status [I] |
| 0x50 | W | (640-1) \| (0x40-1)<<10 \| (2-1)<<16 \| (0x5E-1)<<24 = 0x5D01FE7F... computed by 0x50003300 from (0x280,0x40,2,0x5E) | Timing0: PPL-1, HSW-1, HFP-1, HBP-1 (OMAP1 LCD layout) [V arithmetic, I names] |
| 0x54 | W | (200-1) \| (4-1)<<10 \| 4<<16 \| 2<<24 from (200,4,4,2) | Timing1: LPP-1, VSW-1, VFP, VBP [V arithmetic, I names] |
| 0x58 | W direct | = 3 at init; on: (v & 0xFF) \| 0x700000; off: (v & 0xFF) \| 0x800000 | Timing2: pixel clock divider 3 + panel power-sequence bits [I] |

Exact 0x50 value: 0x27F | 0x3F<<10 | 1<<16 | 0x5D<<24 = 0x5D01FE7F. 0x54: 0xC7 | 3<<10 | 4<<16 | 2<<24 = 0x02040CC7.

Sequences [V ECust]:
- on (FUN_50094154): 0x40=0x80; reg0 |= 1; 0x58=(lo)|0x700000; spin 0x200; 0x40=0x81; plus reg 0x0F004/0x0F014 = 2 (0x0500F000 block, backlight/LCD power [I]).
- off (FUN_5009420C): 0x40=0x80; 0x58=(lo)|0x800000; 0x40=0x81; 0x40=0x80; 0x0F004/0x0F014 = 0; reg0 &= ~1.
No register is polled in either sequence (delays are CPU spin loops, kernel 0x50003508) [V].

## Minimal model proposal

```c
/* PA 0x04000000, 4 KB: system/LCD block. Plain register file (reads return the last write), plus a display. */
static uint32_t sys[0x400];
static uint64_t rae3_sys_read(void *o, hwaddr a, unsigned s)
{
    if (a == 0x44) return 0;                 /* LCD status: nothing pending, nobody reads it */
    return sys[a >> 2];
}
static void rae3_sys_write(void *o, hwaddr a, uint64_t v, unsigned s)
{
    sys[a >> 2] = v;
    if (a == 0x40 || a == 0x20 || a == 0x24) lcd_invalidate();
}
/* Display: 640x200, refresh from a timer or QEMU's gfx update callback */
static void lcd_update(void *o)
{
    uint32_t start = sys[0x20 >> 2] ? sys[0x20 >> 2] : 0x03000000;   /* palette header first */
    if (!(sys[0x40 >> 2] & 1)) { /* blank, or keep last frame */ return; }
    const uint16_t *src = ram_ptr(start + 0x20);                     /* PA -> host pointer */
    for (y = 0; y < 200; y++)
        for (x = 0; x < 640; x++) {
            uint16_t p = le16(src[y * 640 + x]);                     /* 0000RRRRGGGGBBBB */
            uint32_t r = (p >> 8) & 0xF, g = (p >> 4) & 0xF, b = p & 0xF;
            dst[y * 640 + x] = (r * 17) << 16 | (g * 17) << 8 | (b * 17);
        }
}
/* optional: raise source 45 (IrqLcdMatch) once per frame only if 0x44/0x40 ever show an enable; not needed now */
```

Width/height could be read from 0x50/0x54 (PPL-1, LPP-1) instead of hard-coding, but the ROM header values are
the same [V]. Use dirty-page tracking (memory_region_snapshot_and_clear_dirty) on the 0x3E820-byte region for
speed.

## Open / unverified

- Whether the header is exactly 32 bytes of palette and pixels begin at +0x20 [I]; if the picture is shifted
  by 16 pixels, drop the +0x20.
- Who writes the palette header and with what (kernel screen-chunk set-up near 0x5000FBA0) [not read].
- Meaning of the bits in 0x00/0x04/0x58 [I]; a model that keeps written values is enough, nothing polls them.
