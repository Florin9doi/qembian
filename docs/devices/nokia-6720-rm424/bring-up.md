# Nokia 6720 classic (RM-424, 032.001) bring-up

Profile `nokia-6720-rm424`: the E52's platform (S60 3rd FP2, RAPIDO, syborg6k), same wsini except one hardware-state key.

1. **Window server died on a second screen** (2026-11-17) [V]: the phone stayed black. EwSrv faulted KERN-EXEC 3 at
   EwSrv.exe+0x1b230: CScreen's display-mode choice (`FUN_805d6798`) indexed an empty mode list. CWsTop creates
   `HAL::Get(0x5b = EDisplayNumberOfScreens)` screens (EwSrv `FUN_805d4d5c`); the 6720's Hal.dll answers 2 from its
   own config (Hal.dll+0xdec = 2; the E52's is 1), screen 1 (TV-out) has no modes in our display stand-in. Code patch
   to 1 screen (H-6720-one-screen). Found by diffing the E52/6720 Hal.dll images word by word.
2. **Boots to the home screen** (2026-11-17): first boot stops on the screensaver (press a key), then region,
   date/time, Phone setup ("No thanks"), then the idle screen (Offline). `d_display` now has `panel = 1` as the E52.
3. **Photos turned the screen into stripes** (2026-11-17) [V]: ScDv draws 960-byte lines in portrait and 1280 in the
   320x240 mode, the frame buffer scans picture width x 4; the stand-in now reports the stride per rotation
   (`line_pitch_turned = 1280`, H-display-turned-pitch). Portrait, Photos and back are clean; the E52 too.
