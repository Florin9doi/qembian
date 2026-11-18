# Nokia 9500 (RA-2): inner 640x200 display, for mocking

Static reading of `eka1/ra2/core/ROM.bin` (VA = 0x50000000 + offset; kernel VA 0x58000000 + x =
phys 0xFFF00000 + x). [V] = read in the code at the given address, [I] = inferred.
Companion: `ra2-cover-display.md` (cover panel, uWire).

## 1. Hardware: OMAP LCDC + OMAP1510 LCD DMA, panel commands on uWire channel 3

- `VideoDriver.dll` (0x500552C0) has no LCDC literal because it goes through **EKern accessors** that
  build the address as `0x58000000 + 0xEC000 + off` [V]:
  - LCDC write32 `0x5000FAE0` (stub 0x50058F04), LCDC modify `0x5000FB4C` (stub 0x50058EF8),
    LCDC read `0x5000FB58` (stub 0x50059000).
  - DMA write16 `0x5000FCEC` / modify16 `0x5000FD04`: `0x58000000 + 0xED800 + off`, used with off
    0x300..0x308 = phys 0xFFFEDB00.. (OMAP1510 LCD DMA) and 0x400 (0xFFFEDC00, DMA GCR) [V].
  - Generic modify16 `0x5000EAD8` (set bits) / `0x5000EAFC` (clear bits) on ARM_IDLECT2
    0xFFFECE08 [V]; GPIO 0xFFFCE000 (+4/+8 via 0x5000F8C8/0x5000F8E0) [V].
  - So it is the **on-chip OMAP LCDC**, QEMU's `omap_lcdc` + `omap_dma` 3.1 LCD channel should fit [I].
- Interrupt bound as "Lv1_LCDCtrl" (OMAP level-1 LCD controller IRQ) [V string 0x5005B273, I use].
- Panel controller commands go over **uWire channel 3** via the EuWire controller
  (EKern ord-1 getter 0x5000FF70, stub 0x50058FB8): init does `Reserve(3)` (vtbl+28) and
  `SetConfig(3, cfg@0x5005B304)` (vtbl+36) at 0x50056678..0x500566C8 [V]. Transfers: 0x5005868C,
  channel 3, vtbl+8 (r1!=0) or vtbl+12 (r1==0) with a completion callback, retried while
  obj+108 != 0, max 11 tries [V]. The "CS3 TX 0x0004" seen at boot is this channel [I].
- Panel reset: GPIO13 (mask 0x2000) low, ~10 short delays (EKern 0x50013C68), high (0x500572F4..0x50057360) [V].

## 2. Who switches it on, and why nothing happened

- Load: `VideoDriver.ldd` (0x5005B500) is a shell whose ord 1 jumps to VideoDriver.dll ord 2
  (0x50058C0C, CreateLogicalDevice) [V]. It is loaded on demand by **Hal.dll** (0x50043D58: open
  channel "VideoDriver", on failure `LoadLogicalDevice("Z:\SYSTEM\LIBS\VideoDriver.LDD")`, retry) and
  by **LightServer.exe** (same strings at 0x50098060..) [V for strings and Hal code; I for LightServer code].
- DLL entry 0x50058C48 -> init 0x50056560 with the driver object **0x80003284** [V]:
  allocates the frame buffer, sets screen info in EKern (0x50011A00..0x50011A38), reserves uWire 3,
  then 0x50057044(-1, 0): GPIO13 reset + uWire 3 read of 25 words with callback 0x50058784, which
  parses the panel ID into obj+212 (DoControl returns -18 KErrNotReady while it is 0) [V for code,
  I for "panel ID"]. **No LCDC/LCD-DMA access in this path** [V].
- LCDC is programmed only in the power-on state machine, started by **LDD async request 0**
  (DoRequest dispatcher 0x5005889C: req 0 -> PowerOn 0x50057180, 1 -> 0x50058338 (off),
  2 -> 0x50056B00, 3 -> 0x50057044) [V]. DoControl 0..24 is at 0x500588F8 [V].
  - PowerOn 0x50057180: if obj+168 != 0 (already on) completes at once; if obj+0 != -1 (busy)
    completes with -14 KErrInUse; else stores (r1, r2), state obj+224 = 0, runs the step 0x500571E4 [V].
  - Step (also NTimer callback at obj+180, re-armed 5000 / 0x3D40 ticks): state 0 -> GPIO13 reset;
    1 -> **0x50057368 LCD on (LCDC + DMA)**; 2 -> 0x50057FC0 (panel command list over uWire,
    0x500574B4); 3 -> wait; 4 -> 0x500580B4 + uWire 0x500585D8; 5 -> 0x500581B8 (done) [V for the
    jump table 0x50057204, I for what states 2..5 send].
- Hal.dll: `HAL::Set(attr 36, v)` -> v=1: request 0 (on), v=0: request 1 (off), then records
  attr 40 (0x50044038..0x500440B0) [V]. attr 36 = `HALData::EDisplayState` [I]. attr 37 -> control 5,
  attr 81 -> control 12, attr 84 -> control 10 [V].
- **Conclusion: nobody issued HAL::Set(EDisplayState, 1) / request 0** (the LCDC would be written in
  state 1, before any further uWire traffic) [I, consistent with the LCDC reset value]. The caller is
  a user process at boot: LightServer (owns display/backlight, "ScrSvrStatus") or the starter/SysAp
  equivalent [I]. Look at why it did not get that far (not started, blocked on a modem/P&S value).

## 3. Frame buffer and LCD programming (0x50056598.., 0x50057368) [V]

- Phys = `[[0x40000000 + 24] + 8] + 0x500000` (super page -> RAM bank base + 5 MB), mapped as a
  hardware chunk of **0x3E820** bytes = 32-byte palette + 640x200x2. Linear address at obj+140.
- Screen info at obj+112..: 640, 200, twips 6259 x 1956, bpp 16 (obj+136), palette offset 32
  (obj+144), stride 1280 (obj+148). Palette[0] = **0x4000** (bits 12-14 = 4 -> 16 bpp in omap_lcdc).
- LCD on (0x50057368): ARM_IDLECT2 clear 0x0C then set 0x04; LCD DMA 0x302/0x304 = phys lo/hi
  (top), 0x306/0x308 = phys + 640*200*2 + 30 (bottom); ARM_IDLECT2 |= 0x08; LCDC Timing0 (+4) =
  0x3F2F7E7F, Timing1 (+8) = 0x010104C7, Timing2 (+0x0C) = 0x0270000A; LCD DMA ctrl 0x300 = 0x40 if
  phys == 0x20000000 (IMIF) else 0x04 (then an interrupt object obj+176 is enabled); LCDC Control (+0)
  = 0x80 (TFT); EKern 0x5002A6A0(obj+104, 0x100); finally Control |= 1 (LCD_EN).

## 4. Cheapest way to a picture

1. Check from a session: `0x80003284+0` (-1 = idle), `+168` (on flag), `+224` (state), `+212` (panel
   ID), `+140` (FB linear), `+108` (uWire error/retry flag). Breakpoints: Hal 0x50044038 (Set attr 36),
   VideoDriver 0x50057180 (PowerOn), 0x500571E4 (step), 0x50057368 (LCD on), 0x50058784 (ID callback).
2. If 0x50057180 is never hit: find the process that should call HAL::Set(36,1) (LightServer first);
   unblock it. As a test hack, force the request from a running Hal client or set PC in a
   kernel-thread context to 0x50057180 with r0=0x80003284, r1=-1 (any value), r2=0 (no TRequestStatus)
   [I: r2=0 path skips completion, see 0x5005718C].
3. The uWire channel-3 transfers (ID read, panel command list) must complete their callbacks
   (EuWire may use DMA/DRQ, see the cover doc); otherwise state 2+ stalls but the LCDC is already on.
4. QEMU side: omap_lcdc + 1510 LCD DMA (0xFFFEDB00) should draw 640x200 RGB565 from the frame buffer;
   the uWire panel on CS3 only needs to answer the ID read (25 words, contents [I]) and swallow commands.
