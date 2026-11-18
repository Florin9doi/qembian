# Nokia E90 Communicator (RA-6) bring-up

S60 3rd FP1 on Symbian 9.2, OMAP2420. Two displays: the cover display (240x320, lid closed, phone keypad) and the inner
display (800x352, lid open, QWERTY keyboard). Firmwares in the ROM store: RA-6/300.34.84 (default) and RA-6/210.34.75
(`nokia-e90-ra6@210.34.75`, not booted yet). Profile started from the N95 8GB's; `[I N95]` values are unchecked.

1. **Import** (2026-11-17): the type code RA-6 was refused by the ROM store and device.py (they took only RM-/SE-):
   widened to `R[A-Z]-\d+` (test `test_store_takes_ra_type_codes`). ROM header: multiple model at 0x80000000 as the
   N95; kernel-side and user-side files interleaved -> `kernel.place = "after-firmware"`.
2. **Personality `sym92l91`** [V]: with `sym92`, EStart's ProcessCreate fields were garbage (DProcess::Create -6,
   "Fserv fault 11", KDLL trace) and the loader copies 0x17c bytes for CodeSegCreate: the N81/E71 case (9.2 exec
   tables, 9.1 loader layout). 237 slow execs (N95: 241). `tools/personality/check.py` crashed on every personality
   without a personality.toml (fixed, test `tests/personality_check_test.py`).
3. **Display** [V]: wsini has one screen with modes 1 = 800x352 (inner), 2 = 240x320 and 3 = 320x240 at 90 (cover).
   d_display: portrait (upright) panel 800x352, stride 3200, 25 caps words as the E71 (26 overflowed WSERV,
   KERN-EXEC 3), LCD HAL `lcd_portrait`.
4. **Two screens in HAL** [V]: Hal.dll+0xdd8 (file 0xe50) says EDisplayNumberOfScreens = 2; the window server built
   screen 1 with no display modes and died (FAR 0x28, as the 6720). First patched to 1 (H-e90-one-screen), then
   modelled (step 9).
5. **First boot reaches the region query on the inner display** (2026-11-17); Select x5 and Exit from the Welcome
   app's options give the idle screen (Offline).
6. **Lid and cover display** (2026-11-17) [V]: wsini `S60_HWSTATE`: EKeyDeviceA -> mode 1 (inner 800x352),
   EKeyDeviceB -> mode 2 (cover 240x320; alternative mode 3 = 320x240). The keymap's `lid-slide` switch sends them
   (F13 open / F14 closed). On EKeyDeviceB the window server lays out the whole UI for the cover (softkeys at the
   bottom) and draws it at the frame buffer's top-left; no display-driver call. (First design, replaced by step 9: the phone window cropped
   that part of the inner frame buffer; a lid closed during boot showed a cut-off inner picture, the cover dimmed
   with screen 0.) Opening the
   lid brings the inner layout back. One command: `python3 tools/sydb/sydb.py phone e90 --fresh`.

7. **QWERTY** (2026-11-17) [V typing, I matrix]: the keymap's `qwerty` group (letters, space, enter, shift, Fn,
   Ctrl, punctuation; copied from the 5730) types into the E90's editors: "hello" in a new text message's To field.
   The E90's own keypad.dll matrix table is not read yet (scan codes are the EStdKey letters).

8. **GPU** (2026-11-17) [V]: `gl.impl = "sygl"` as the N95 (same PowerVR MBX): the N95's libgles_cm ordinals and
   import list resolve against this firmware; the identity is the E90 driver's own ("1.2.17 HW PowerVR MBX with VGP",
   EGL "1.1 build 3.3.33.561.1.1.5"); the blob sits at 0x80090000 (free gap below the user side at 0x800a0ec0);
   the entry patches are per firmware (`firmware/300.34.84/gl/gles-patches.txt`). sygl's present clipped to a fixed
   320x240 screen (only that corner was drawn): it now takes the screen's sides from `[board.display]`
   (SYGL_SCREEN_LONG/SHORT, test `tests/sygl/present_test.c`). Checked with `src/n95apps/glprobe1` (a GLES 1.1
   client: EGL init, window surface, 120 frames): full-screen frames, no GL error, host GPU kicks counted.

9. **Two panels** (2026-11-17) [V runtime]: the cover is HAL screen 1 (H-e90-two-panels). `lcd_e90` (patch
   qemu-baseport 0028) registers a second display HAL device with its own 240x320 frame buffer; d_display serves
   channels for screen 1 (`d_display.screen1 = 240x320`, log "DisplayDriver channel ... serves screen 1"); with
   screen 1 present, EKeyDeviceB moves the UI to screen 1. QEMU's syborg-fb has a second panel (registers 19..22)
   with its own backlight; the window's Lid button sets `/machine/fb active-panel` and follows the panel's size.
   Owner's test: switching works correctly once booted.

Open:
- closing the lid during boot (before idle) gives a white screen; boot with the lid closed (QEMU has
  `syborg-keyboard.boot-keys=f14` + `syborg-fb.panel=1`, sydb has no option yet); the lid state is not kept;
  KHWRMFlipStatus P&S not set; the cover's modes are ours, not read from the phone's LCD driver [I];
- the cover's key set when closed (phone keypad, cover softkeys) vs. the QWERTY when open;
- the E90's own keypad matrix (Chr, the application keys over the keyboard, the cover keypad's keys);
- `nokia-e90-ra6@210.34.75` (its own gles-patches.txt: run src/sygl/build.sh for it); GL on the cover display
  (mode 2) not tried; stand-ins checked only by name.
