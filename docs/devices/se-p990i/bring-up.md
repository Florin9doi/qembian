# Sony Ericsson P990i (UIQ 3.0, Symbian 9.1): bring-up log

Profile `models/se-p990i` (family `semc-uiq3`, personality `sym91r`, firmware R6E28 Generic World1), started
2026-11-17 from the P1i's profile ([../se-p1i/bring-up.md](../se-p1i/bring-up.md)): the same platform
(PNX4008, Ericsson CCPU behind the BridgeServer, the same ESTART.TXT and aps.ini), so every P1i stand-in, the
fake CCPU and the P1i's two patches apply as they are. [V] = checked on a run or in the firmware, [I] = inferred.

## How to rebuild and boot

```sh
tools/fw/store.py import se_roms/P990/P990i_DPY_Generic_World1_CDA162007-1-R6E28.rar   # roms/SE-P990i/R6E28
tools/fw/inventory.py --device se-p990i
src/semc/pvr2dif/build.sh se-p990i                  # the PVR2DIF.dll replacement (H-p1i-pvr2dif-stub)
tools/device.py build se-p990i --kernel
python3 tools/sydb/sydb.py phone p990i --fresh      # an instance named p990i (tools/instance.py create)
```

## Firmware [V]

| File | Kind | Notes |
|---|---|---|
| `BbyoneSW_CXC162058_R6E28` | core | EKA2 XIP ROM at 0xF8000000 (the P1i's layout), 210 files |
| `ACPU_SW_R6E28R_ROFS` | ROFS1 | 1389 entries |
| `BASELINE_CXC162036_R6E28_kJ_ROF` | ROFx | 2772 entries |
| `ACPU_CDA162007-1-R6E28_Generic_World1_ROFS` | customer ROFS | 848 entries |
| `EPOC_Generic_World1` | C: | FAT16, 61 MB |
| `CCPU_Signaling_SW_R9GA001_*.babe`, `BT_SW_R5A01_RED49.babe` | modem / Bluetooth | not decoded |

Identity: `version.txt` "PDA: CXC162036 R6E28", CDA162007/1 R6E28, product P990i (the EMMA scripts). HAL
(Z:\resource\apps\ProductInfo.ini): EModel 0x503DE69F "P990i", EMachineUid 0x1020E285 (KUidModelHermione),
software rev 5, family rev 0; the HALKE stand-in serves them.

Kernel side, differences from the P1i [V core lists]: `flip.dll` and `keylock.dll` (the keypad flip and the
keylock switch), `lcdtoshibadrv.dll` (P1i: two Sony LCD drivers), `camcover.dll`, `DMsoc2010.pdd` /
`DMsoc366.pdd` (two camera sensors; P1i: dcam0/1.pdd, camdrv_buffer.ldd).

## Boot log

1. **Tool fixes on the way** (2026-11-17): `tools/fw/store.py import` stored nothing, silently: `semc.quick_core`
   read 0x400 bytes and the SE cores' root list is at +0x1A90 (no SE core was recognised; the P1i had gone in before
   that check). It reads 64 KB now (test `QuickCoreTest`), and an import that identifies nothing says so.
   `src/semc/pvr2dif/build.sh` takes the device (the P990i's PVR2DIF.dll also exports 15 functions).
2. **First boot reaches the Language screen** [V], drawn right at 240x320 with the P1i's LCD build (256-pixel rows).
   The P1i's BServer patch applied (original bytes matched), and the aps.ini edit too (the same file in the ROFx).
   Known from the P1i and harmless here: DebugRouter panics -1, `drmagentsrv` is killed -1 repeatedly, `Exc 1`
   lines at kernel 0xf844b530 (the P1i has the same, 300-600 per boot).
3. **Language, Yes, Power menu, Flight mode** [V] as on the P1i. Then the Phone app died E32USER-CBase 63 and its
   ICL thread KERN-EXEC 3 (pngcodec reading the dead process's heap); before them CamServer panicked
   "CamSrv:start err" -1. Cause (docs/research/p990i-camera.md): the P990i's Phone app (CallControl.dll ->
   ECamSemcPlugin) starts CamServer; its render engine (intergraphapi.dll) loads `YUV2RGB.LDD`, a kernel-side file
   we drop. Stand-in `src/standins/yuv2rgbldd` (H-p990i-yuv2rgb-absent). The camera drivers themselves (camdrv.ldd,
   DMsoc*.pdd) are only opened when a camera is reserved; without them a reserve fails -1, no panic.
4. **The keypad flip** [V]: see below.
5. **The Phone app's start-up needs two more drivers** [V catch on User::Leave in Phone::Phone]: CallControl creates a
   camera (CCamera::NewL, for video calls) and leaves -1 without one; its error path then panics E32USER-CBase 63.
   Stand-ins `camdrv` (camdrv.ldd, units 3 and 4: controls succeed, requests stay pending) and load-only sensor PDDs
   `DMsoc2010.pdd` / `DMsoc366.pdd` (H-p990i-camera-absent): a camera that never delivers a frame. CallControl also
   opens `threadpriority.ldd` (kernel side, dropped) and raises four threads to priority 25 through it; stand-in
   `threadpriority` with the original's protocol (H-semc-threadpriority).
6. **Home screen** [V]: Language, Yes, Power menu (Flight mode), the Bluetooth note (`C32:CPortHandler 4`, as on the
   P1i), Setup wizard (No) -> the UIQ standby in Flight mode. One command: `python3 tools/sydb/sydb.py phone p990i
   --fresh` (instance `p990i`).

## The keypad flip [V unless marked]

The P990i's flip covers the lower part of the screen. Closed: the phone keypad on the flip, the UI in "softkey
style" on the visible top 240x256. Open (or the flip taken off): the whole 240x320 touch screen in "pen style" and
the QWERTY keyboard under the flip.

- wsini.ini: screen modes 2 = 240x320, 4 = 240x256 (and 3 / 5 = their turned forms).
- `flip.dll` (kernel extension, 1 KB) reads GPIO pin 5 through SE's GPIODRIVER and publishes it in two P&S
  properties: **0x100039EA/2** (SE's hardware-state category; read by QUiConfigServer.EXE, lcdhandler.aps,
  startuphandler.aps, the Qik controls) and **0x102086EF/1** (keyboardscan.dll's: the flip's keys press the QWERTY
  keys under it [I], so the scanner maps them by flip state). It starts with 1.
- **1 = open, 0 = closed**: with 0 published at boot the Language screen comes up in softkey style on 240x256 with
  the bottom 64 rows dark.
- Ours (H-p990i-flip): QEMU's syborg-sensor has a switch register (SWITCHES bit 0 set = closed, SWITCH_SEQ;
  `qom-set /machine/sensor switches 1`, `-global syborg-sensor.switches=1` to boot closed). The `flip` stand-in
  defines the two properties with flip.dll's policies and publishes the switch (polled every 100 ms, as flip.dll
  debounces with a timer). The phone window's Flip button sets the switch, shows the top 240x256 while closed (the
  flip covers the rest of the same panel) with the keypad keys, and the whole screen with the QWERTY keys while open.

## Keys [V unless marked]

The phone's key table is EKData.dll (core 0xf83957f0): scan-code blocks 0x00-0x11, 0x41-0x5A, 0x60-0xA0,
0xA4-0xAC, 0xB1, 0xB5, 0xB9, 0xD4-0xD7, then the key codes in order. 0x30-0x39 are not in it: the P1i draft's digits
did nothing. What each code does, from probes on a snapshot of the closed-flip home screen (`sydb fork`, one fork per
key, the lock key first):

| scan code | key code | key | host |
|---|---|---|---|
| EStdKeyDevice4..8 | EKeyDevice4..8 | 5-way up, down, left, right, centre (confirm: opens the Main menu) | arrows, Enter |
| EStdKeyDevice1 / 2 | EKeyDevice1 / 2 | jog dial (focus moves) [I which way] | PgUp / PgDn |
| EStdKeyDevice1B | EKeyDevice8 | jog press [I] | Home |
| EStdKeyDevice3 | EKeyEscape | Back [I] | Esc |
| EStdKeyDevice0 | EKeyBackspace | C (clear) [I] | Backspace |
| EStdKeyDeviceD (0xB1) | EKeyDeviceD | the keylock slide (keylock.dll adds 0xB1 down/up from GPIO 10) | F8 |
| EStdKeyDevice1D | EKeyDevice1D | camera key (starts the camera application) | F9 |
| EStdKeyNkp0..9, NkpAsterisk, Hash | '0'..'9', '*', '#' | the flip's digits (they open the dialer) | 0-9, F11, F12 |
| 'A'..'Z' (0x41-0x5A) | 'a'..'z' | QWERTY letters | letters |

Not found: the Calls / More softkeys of the softkey style (EStdKeyDevice9..C, E, F, Device1C/1E, Application0..7,
Yes, No, Menu showed nothing on the home screen). `tools/device.py` takes hex scan codes now (0xB1 has no name in
e32keys.h).

## Open

- The softkey keys (Calls / More) are unknown; the screen's softkey bar is not touchable while the flip is closed.
- The keylock slide is a key (F8), not a held switch; the phone locks its keys on its own after idle time.
- Booting with the flip closed (`-global syborg-sensor.switches=1` works, no sydb/window option yet).
- The camera application dies (CamServer KERN-EXEC 0): no camera, by design (H-p990i-camera-absent).
- Sound silent, as on the P1i.
- Bluetooth: `!PortHandler 4` as on the P1i (docs/research/p1i-bluetooth.md).
