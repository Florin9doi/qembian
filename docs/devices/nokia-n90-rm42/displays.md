# Nokia N90 (RM-42): the cover display, the fold/twist switches and display rotation

Analysis of 2026-11-27 (firmware V 5.0607.7). Marks: **[V]** verified (firmware code read with Ghidra, or seen in a
run), **[I]** inferred. Runs used a clone of the owner's NAND (`build/ngage/phones/n90-agent2.flash`) on a private
QEMU build with a test-only machine property that drives the four switch lines (`build/evidence/rm42-displays/experiment.patch`,
not in `src/`). Evidence (git-ignored): `build/evidence/rm42-displays/` (screens, the cover replay `cover.py`),
`build/evidence/ghidra-rm42/{ekeyp,sysap,cuiws,cuilcdpdd,genericlcd_lcd}.c`.

## Summary

| Hardware | What the phone does with it |
|---|---|
| fold sensor, GPIO 61 (Gpio4_Bit13), high = closed | fold closed: SysAp sets shared data `Flip` = 1, CuiWs draws the **cover UI** on the cover panel; main panel left off [V run] |
| twist sensor, GPIO 42 (Gpio3_Bit10), **high = normal (not twisted)** | low = display twisted: the window server goes to screen mode 2 (416x352, rotation 90), the LCD driver writes controller register 34 = 3, the cover gets MADCTL 0xB0, the camera application comes up (imaging mode) [V run] |
| camera-angle sensors, GPIO 54 (Gpio4_Bit6, active high) and GPIO 43 (Gpio3_Bit11, active **low**) | SysAp shared data `CameraAngle0Open` / `CameraAngle180Open` and a position 2/3/4; CuiWs redraws the cover; no visible main-display change [V run] |
| cover panel, SPI device 1 | 132x132 DCS controller, frames 130x130 RGB565 at (1,1)-(130,130), sent but not shown by the board [V] |

**Consequence for the board today:** every GPIO input reads 0, which means *display twisted*. A boot can come up
in landscape with the camera app's "Hardware failure: Restart C[amera]" note, drawn garbled because the display
model ignores rotation (run 2 did, from the first frame; run 1 only after a fold close/open [V], why the two differ
[I]: SysAp state kept on the NAND). The resting state of a normal, open phone is **GPIO 42 high**, the others low.

## 1. The switches (ekeyp.dll "Switch" driver) [V Ghidra]

ekeyp.dll (0x50095A10) builds four detectors (0x5009781C), each `(GPIO module, GPIO number, active level, code when
active, code when inactive, debounce ms)`, binds a GPIO interrupt ("DetectorInt") per line and reports the
initial state at start-up. A change sends a raw **key down + key up** (TRawEvent 3 / 4) of one scan code
(0x50097718):

| # | GPIO | active level | code if active | code if inactive | debounce |
|---|---|---|---|---|---|
| 1 | 61 = Gpio4 bit 13 | 1 | 0xAF EStdKeyDeviceB **FoldClosed** | 0xAE DeviceA **FoldOpen** | 500 ms |
| 2 | 42 = Gpio3 bit 10 | 1 | 0xAD Device9 **FoldTwistClosed** | 0xAC Device8 **FoldTwistOpen** | 1000 ms |
| 3 | 54 = Gpio4 bit 6 | 1 | 0xEC Device23 **CamAng0Closed** | 0xEB Device22 **CamAng0Open** | 1000 ms |
| 4 | 43 = Gpio3 bit 11 | **0** | 0xEE Device25 **CamAng180Closed** | 0xED Device24 **CamAng180Open** | 1000 ms |

Names: SysAp's hidden power-key-menu test items (shown when `C:\Nokia\pmpm\` exists) simulate exactly these
keys: "Key: FoldOpen" = key code 0xF84C, FoldClosed 0xF84D, FoldTwistOpen 0xF84A / scan 0xAC, FoldTwistClosed 0xF84B,
CamAng 0 Open 0xF88B / 0xEB, CamAng 0 Closed 0xF88C / 0xEC, CamAng 180 Open 0xF88D / 0xED, CamAng 180 Closed
0xF88E / 0xEE (0x50EA7A10..0x50EA7A7C). ekdata.dll maps the scan codes to these key codes (table at 0x5009873C).
The same states can be read with custom.ldd ("Custom" channel) control 6, argument = switch 1..4; SysAp does this
at start-up (0x50EA3D90..) and posts the matching keys.

Which mechanical position is which: GPIO 42 high gave the normal portrait home screen and low gave landscape +
camera [V run], so "FoldTwistClosed" = display in its normal position. What the two camera-angle sensors detect
(the camera/hinge turned to 0 or 180 degrees, two end stops) is [I]. SysAp's position: 2 = CamAng0Closed,
4 = CamAng180Closed, 3 = otherwise (0x50EA4B26..).

## 2. Who reacts [V Ghidra + run]

- **SysAp** captures FoldOpen/Closed (0xF84C/D) and the four camera-angle codes (key handler 0x50EA4834, a table on
  code - 0xF840). Fold: shared-data state object +0x9C set 1/0 and keypad lights; camera angles: shared data
  `CameraAngle0Open` / `CameraAngle180Open` (category 0x10005943). The twist codes 0xF84A/B fall to the default case:
  SysAp does not switch screen modes.
- **CuiWs.exe** (the cover's own window server, starts gcuiapp.dll) watches shared data `Flip`: 1 -> cover UI on
  (state 0x12), 0 -> off (0x13) (0x5065B540, 0x5065B6C8), and the camera-angle keys (cover timers).
- **Screen mode 2** (landscape) follows the twist line [V run: register 34 = 3 each time GPIO 42 went low, 0 when
  high]. Which component calls SetScreenMode for it (avkon / eiksrv / the camera app) was not traced [I]; it is not
  SysAp and not the window server's ini.

## 3. Main display rotation [V]

`Wsini.ini` (ROFS `System/Data/Wsini.ini`) has three screen modes:

| mode | size | rotation | other |
|---|---|---|---|
| 1 | 352x416 | 0 | COLOR16MU |
| 2 | 416x352 | 90 | twips swapped |
| 3 | 176x208 | 0 | `SCR_XSCALE3 2`, `SCR_YSCALE3 2`, COLOR64K (old applications, doubled) |

The rotation is done by the **display controller**, not by the window server: genericlcd_lcd.ldd keeps an
orientation 0..3 (0x50043034: 1 or 3 -> 416 wide x 352 high) and on a change (0x50043194) writes
- register **34** = 0 (0 degrees), **3** (90), 2 (180), | 0x10 when 0x5004D84C says so; 270 goes another way
  (0x5004E1D0) [V 0x5004DCEC];
- register **36** = 1 when the scale is 2 (screen mode 3: the controller doubles pixels) [V 0x5004DD3C; I not seen in a run];
- register **38**, the window, with the axes swapped: in portrait `0 0 351 415`, in landscape the long axis is 415
  (seen: `00 00 00 00 9f 01 1f 00` = 416 x 32 status pane band, `00 00 40 01 9f 01 5f 01` = rows 320..351).

The frame memory then arrives in landscape order (416 pixels a row). The board's display core
(`nokia6630_lcd.h`, controller mode) clamps the window to 352x416 and ignores 34/36, so landscape frames come out
garbled (`main-switch-steps.png`). To show them: keep the frame memory in the controller's own order and rotate on
output by register 34 (3: incoming 416x352 shown turned 90 degrees in the 352x416 panel; which direction [I], to be
chosen by eye on text), and treat 36 = 1 as 2x2 replication.

Also seen: the main panel's power-save screensaver goes over **SPI device 0** as DCS (PTLAR/PTLON partial area,
RAMWR bands of 8800 bytes, IDMON), not over SoSSI; the board does not draw those either [V run].

## 4. The cover display (cuilcd.ldd / cuilcdpdd.pdd on SPI device 1) [V]

A DCS controller like the 9500's cover (`nokia9500_cover.c`): SLPOUT, NORON, INVOFF, MADCTL, GAMSET 04, COLMOD 05
(RGB565; the PDD also knows 02 = 8 bpp, 03 = 12 bpp for display modes 6/10), PASET/CASET 1..130, RAMWR of 33 800
bytes = 130 x 130 x 2, high byte first; DISPON. After boot it changes to **27-bit SPI words = three 9-bit DCS
words, most significant first**, a command in the low group (`[NOP 00, NOP 00, 2B]`, `[y0, y1, 2A]`, `[x0, x1, 2C]`,
then pixel bytes three a word). Low-power idle uses IDMON (39), PTLON (12) and PTLAR (30) 1..130.

MADCTL: 0xD4 with the display in its normal position, **0xB0 when twisted** (the cover's picture turned by 90
degrees by the controller) [V run]. The frame as sent, read row by row with no mirroring, is upright for 0xD4
(NOKIA logo, cover home screen) [V]; how 0xB0 should be shown [I].

Seen on the cover (replayed from the SPI log by `cover.py`): the boot logo; while the fold is open, a black/white
idle pattern (idle mode); with the fold closed, the cover home screen (signal, battery, date "Fr 27/11/2026", headset
icon, an analogue clock); twisted, a camera icon (idle mode). The clock face sits left of centre: whether that is the
real layout or the visible area is smaller than the 130x130 window is [I].

To show it: a second graphic console fed from `spi_send` dev 1 (split each transfer into 9-bit groups, MSB first)
into the 9500 cover's DCS parser, which already handles 132x132 RAM, CASET/PASET, RAMWR, COLMOD 5; add NOP, IDMON
(8 colours) and the 27-bit packing; `ngage_phone.py` gets a 'cover' view as for the Communicators.

## 5. What a board change needs (not done)

1. N90 machine properties `fold-closed` and `twisted` (+ a camera angle) driving GPIO 61 / 42 / 54 / 43 with the
   levels above; **default GPIO 42 high** (normal open phone). Buttons in `ngage_phone.py` (as the 6260's swivel).
2. The cover console (section 4).
3. Rotation (34) and 2x scaling (36) in the controller mode of `nokia6630_lcd.h`, with unit tests
   (`tests/nokia6630_lcd_test.c`).
4. Optional: SPI-device-0 partial-mode frames for the main screensaver.
5. The camera (imaging mode needs it, or the camera app shows "Hardware failure").
