# Nokia E90: two displays (cover 240x320, inner 800x352) — what to model

Static research, 2026-11-17, firmware RA-6 300.34.84 (`build/firmware/RA-6/300.34.84/16`). No QEMU run.
[V] = read in the firmware's code or data; [I] = inferred, not checked. Ghidra programs used:
`e90_d_display.bin` (base 0x800d9e20), `e90_Hal.bin` (0x80171c70), `e90_keypad.bin` (0x8010c160), raw dumps of
`core-flat.img` (copies in `/tmp/e90g/`, not kept).

## 1. Screen 1 is the cover display, a real second screen of the DisplayDriver

- `d_display.ldd` (Nokia's real one, 118516 bytes, built Aug 27 2008, product dir
  `display\omap\2420\client\maxi\display_driver_product.cpp`: "Maxi" = E90) creates **two** display
  entries in `CreateDisplays` (`FUN_800f1e04`) [V]:
  - device+0x6a4 = screen 0: 0x680-byte entry, product 0x830000, panel id 0x46, `DisplayHandlerHassi` /
    `controller_blizzard.cpp` (external Blizzard controller): **the inner 800x352 panel** [I by size: wsini
    mode 1 and `boot_figure_800x352.dll`].
  - device+0x6a8 = screen 1: 0x490-byte entry, product 0x290000, panel id 0x44, the MeSSI/RFBI handler (same
    product code as the N95 8GB's 320x240 panel): **the cover 240x320 panel** [I strong:
    `boot_figure_240x320.dll` is the second boot figure; there is no `tv_out.dll` in this ROM, so screen 1 is not
    TV-out]. Panic texts "Cannot Create Primary Display" / "Cannot Create Secondary Display" [V].
- Screen 1 has its **own frame buffer**: `FUN_800f1efc` sizes it from screen 1's largest mode (w x h x bytes per
  pixel) and places it in screen 0's chunks (+0x70, +0xa4) after screen 0's buffer [V]. So the cover is not a
  window into the inner buffer at the driver level.
- `Hal.dll` config: EDisplayNumberOfScreens = 2 (Hal.dll+0xdd8, file 0xe50) [V]. The driver registers a HAL
  display handler per screen: `FUN_800dea30` calls `Kern::AddHalEntry(EHalGroupDisplay=4, thunk, ...)` with
  thunk 0x800dea14 for screen 0 and 0x800de9f8 for screen 1 [V]; the device number argument is not shown by the
  decompiler [I: = screen index].

### Channel protocol per screen (same numbers as the N95 doc, `docs/devices/nokia-n95-8gb-rm320/nokia-d_display.md`)

DoControl `FUN_800e49ac`, DoRequest `FUN_800e5330`, 0x8xxx `FUN_800e5210` [V]. Every control/request acts on
`device + 0x6a4 + channel.screen*4`, the screen selected by **ctl 10**:

| fn | E90 behaviour |
|---|---|
| ctl 10 (a1 = screen by value) | `FUN_800e2ff0`: **0 and 1 accepted**, >= 2 -> -5 (KErrNotSupported) [V]. Our stand-in accepts only 0 (comment "only 0") |
| ctl 11 caps | **25 words / 100 bytes** (`FUN_800db990`, written with length 100) [V], filled per screen: w0 = present, w2 = DSB (+0x264), w4/w5, **w6 = height (+0x58), w7 = width (+0x54)**, w8 = max partial lines (+0x218), w9..11 rotation 90/180/270, w12..16 = 4K/64K/16M/16MU/16MA, w17 = panel flag, w22 = w23 = 1, w1 = caller is WSERV (priority 0x28a = 650) |
| ctl 3/4/5/6/7/8/9/0x19/0x1a/0x1e/0x1f/0x21 | as on the N95, on the selected screen; ctl 9 posts only to that screen's panel (`FUN_800dcb2c`, no reference to the other screen) [V] |
| ctl 0x1d | only on screen 0 [V] |
| req 0x8001/0x8002/0x8003 | self-test / colour mode / rotation of the selected screen (rotation checked against its own +0x268/+0x26c/+0x270 flags) [V] |
| req 0x2003/0x2004 | DSB EndUpdate on the selected screen [V] |

Values of screen 1's caps (rotation flags, colour modes, partial lines) are in its 0x490 entry built at run time;
not read [I: 240x320 portrait, 16MU and 64K, rotation as the N95's MeSSI panel (0/1/1), partial lines 320 or 0].

### HAL per screen (`FUN_800de0bc`, one instance per screen) [V]

0 ScreenInfo, 2 WsSwitchOnScreen (= on), 10 SetBacklightOn -> **-5** (backlight not via display HAL on the E90),
0xc/0xd/0xe brightness (only if the panel has it, +0x248), 0xf **ModeCount** (`FUN_800dba6c`), 0x10 SetMode
(cap MultimediaDD), 0x11 Mode, 0x14 **SetState: 0 -> panel off (`FUN_800dbaec`), else on (`FUN_800dbaac`)**,
0x15 State (+0x2a0), 0x17/0x18 Current/SpecifiedModeInfo (TVideoInfoV01, 0x34 bytes), 0x1c (cap MultimediaDD).

**Why the window server died:** with 2 screens it asks HAL for screen 1 (`HAL::Get(1, ...)` ->
`Kern::HalFunction(EHalGroupDisplay, fn, ..., device 1)`). Our display HAL (BSP `lcd_portrait`) registers
device 0 only, so ModeCount/ModeInfo for screen 1 fail -> a CScreen with 0 modes -> KERN-EXEC 3 [I: matches the
observed crash; the HAL side, not ctl 11, is the first gap].

## 2. Who switches the panels and the backlights

- The driver's own pairing: re-initialising one display turns the other off first and back on after
  (`FUN_800dbb40`), and the power-down path (`FUN_800da9e4`) turns off both screen 0 and screen 1 [V]. Nothing in
  the channel interface "moves" the UI from one panel to the other [V: no such ctl/req].
- The panel on/off per screen is HAL 0x14 SetState (cap PowerMgmt) or WsSwitchOnScreen [V]; **who calls
  `HAL::Set(1, EDisplayState, ...)` on a lid change is not found** [I: WSERV per screen on power events, or SysAp
  through the flip plugin `SysapPluginFlipBehaviorController.dll` (ROFS), whose ini lists apps exempt from the flip
  behaviour: autolock, telephone, video telephony, BLID, landmarks, startup, camera, maps, BT toggle, SWIS [V ini]].
- Content on the cover: wsini has **one** screen whose mode 2 (240x320) is the cover layout, and ctl 9 only
  feeds the selected screen's panel [V both]. So either (a) WSERV/ScDv for the cover mode opens a channel with
  ctl 10 = 1 and posts into screen 1's buffer, or (b) the S60 UI renders mode 2 on screen 0 and the Blizzard
  controller (screen 0) feeds both glasses. Static code does not settle it [I; (b) leaves screen 1 unused by the
  UI, which matches the single-screen wsini; (a) is required for a 240x320 picture to reach a MeSSI panel through
  this driver]. Cheapest runtime check: give screen 1 a HAL entry + caps in the stand-in, boot, close the lid,
  and log which screen each ctl 9 arrives on (`sydb log --grep 'DisplayDriver'`).
- `omaplcd.ldd` / `pvromaplcd.dll` / `libomaplcd.dll`: PowerVR display-class glue ("PVRGetDisplayClassJTable",
  imports `r_display_kernel` "PriDisp") for the 3D driver [V strings]; nothing about the lid. Not needed while
  sygl replaces the GPU path [I].
- Lights: no `LightLdd`/`LightDrv.pdd` in this ROM although `SALightsService.01.DLL` (HWRM light plugin) names
  them [V]; its targets are **KBD, KBD2, DISPLAY, DISPLAY2, EMAIL, FLASHLIGHT, MMKEYS, PWRSAVELED** [V]:
  DISPLAY2/KBD2 = HWRM `ESecondaryDisplay`/`ESecondaryKeyboard` = cover backlight and cover keypad [I]. Display
  HAL SetBacklightOn returns -5 [V], so the backlights go through the light plugin's own path [I: the plugin's
  fallback when LightLdd is missing is not read]. HWRM light policy ini lists plugin UIDs 0x100058F3 (SysAp),
  0x101F8F6A (LedController), 0x20003b74, 0x10205030 [V].

## 3. Lid state at boot

- `SysAp.exe` holds KPSUidHWRM 0x10205041 and 0x101F8773 (the category akncapserver writes keys 3/8/9 into on a
  lid close) [V: the UIDs are present]; HWRMServer holds 0x10205041 [V]. Keys inside them not read.
- Best guess [I]: KPSUidHWRM 0x10205041 key `KHWRMFlipStatus` (EPSHWRMFlipOpen = 1 / Closed = 2 on S60 3.x;
  key number 1 or 5 in the SDK headers, not checked here) and `KHWRMGripStatus`; HWRMServer publishes it from its
  power-state plugin, fed by the keypad/lid switch driver ("Switch" in the GAPE name table of
  `r_display_kernel.dll`/`d_display.ldd` [V]); the window server is told by the EKeyDeviceA/B key events
  (wsini `S60_HWSTATE_KEYCODE0/1` [V]), which the keypad driver sends on a switch change. `keypad.dll` contains
  neither UID [V], so it only produces key/raw events.
- A phone booted closed: [I] the first EKeyDeviceB must arrive after WSERV is up (WSERV's HW-state table only
  reacts to the key), and the P&S flip key must say closed before SysAp starts (SysAp reads it at start-up).

## 4. Minimal faithful design for the emulator

1. **Keep one QEMU frame buffer, add a second panel view** [design]: the display memory holds screen 0
   (800x352, stride 3200) and screen 1 (240x320, stride 960) back to back, as the real driver lays screen 1's
   buffer after screen 0's. The phone window shows screen 0 while open and screen 1 while closed (a register
   `ACTIVE_PANEL` or two `syborg-fb` instances; one device with two panel geometries is the smaller change).
2. **d_display stand-in, screen-aware** [design from V protocol]: ctl 10 accepts 0 and 1 (-5 otherwise); every
   control/request uses the channel's screen; ctl 11 returns per-screen caps (screen 1: w0 = 1, w6/w7 = 320/240,
   w2 = 1, w13 = 1, w15 = 1, rotation flags 0/1/1 [I]); ctl 31/stride per screen; ctl 9/8 copy into the selected
   screen's panel buffer (already the `d_display.panel = 1` path).
3. **HAL for device 1** [V needed]: register `EHalGroupDisplay` for device number 1 with ModeCount 1..2,
   ModeInfo 240x320 16MU (stride 960), ScreenInfo, State/SetState. This is what the window server reads before it
   opens any channel; then drop the `Hal.dll+0xdd8` patch (H-e90-one-screen) and its comment in
   `models/nokia-e90-ra6/code-patches.txt`.
4. **The lid switch** [I]: on close send EKeyDeviceB (as now), set KPSUidHWRM 0x10205041 `KHWRMFlipStatus` =
   closed, switch the active panel; on open EKeyDeviceA, flip status open, inner panel. At boot, the instance's
   lid state sets the P&S key before SysAp (bootprops) and queues the matching EKeyDevice key once WSERV runs.
   Verify the key number in SysAp before writing it.
5. **What to drop**: the Hal patch (step 3), the window-side crop `[skin.slides.lid] closed_view` once the frame
   buffer itself shows the active panel. Keep the crop as a fallback only if the runtime check in section 2 shows
   option (b) (cover = mode 2 of screen 0): then the faithful model is "screen 1 exists but is idle", the panel
   switch shows screen 0's top-left 240x320 at the cover's geometry, and only step 3 (HAL device 1 with modes) is
   needed to remove the patch.
6. Profile: `tv_out = true` in `model.toml` is doubtful: no `tv_out.dll` in the ROM [V].

## Outcome (2026-11-17)

Done as in steps 1-3 and the window part of 5 (H-e90-two-panels): the runtime check showed option (a), screen 1
is the cover. The Hal patch and the `closed_view` crop are dropped. Open: step 4 (flip status, lid closed at boot),
a white screen when the lid closes before idle, step 6.
