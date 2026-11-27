# qembian roadmap

Decision (2026-11-04, by the project owner): **option 1 first, then option A.** Options are
defined in [n95-compatibility.md §6](devices/nokia-n95-8gb-rm320/n95-compatibility.md).

1. **Option 1: our EKA2 kernel with a Symbian 9.2 exec personality, running the N95 8GB user side
   byte-identical** on the Syborg virtual platform. This continues until the N95 user side is
   "working": the home screen, Avkon/CONE/Uikon/WSERV up, and apps launchable.
2. **Option A: emulate the N95's OMAP2420 platform** (QEMU `n800`-family machine) and run the
   unmodified Nokia kernel and ROM.

Why this order:
- Option 1 runs on a kernel we can build, trace and debug, so user-side problems can be told apart
  from hardware problems.
- What it produces carries over to option A:
  - ROM composition, unpaging and inspection tools
  - the exec trace
  - the inventory of Nokia hardware-facing servers and the fake peers they need (modem ISA,
    DOS/SysAp power, accessories)
  - a known-good reference boot to compare against

**Firmware as is (2026-11-10):** the list of every change our build makes to the phone's files, and the
QEMU device that would replace each (VideoCore/KHRN, RAPIDO DSP, Bluetooth HCI + FM, drive map):
[research/rom-as-is.md](research/rom-as-is.md).

**Other Symbian UIs (2026-11-07):** UIQ 3 phones (Sony Ericsson P990, P1i, M600, W950/W960, Motorola
Z8/Z10; Symbian 9.1-9.3, EKA2) fit the existing kernel, personalities and tools; they need a new
modem front end, stand-ins and a firmware container reader. UIQ 2 (P800/P900/P910, EKA1) is out of
scope. [docs/uiq.md](uiq.md).

**SE P1i: Bluetooth controller (open work, 2026-11-17).** The P1i boots to its home screen, but c32's port handler
panics `!PortHandler 4` at first boot ("Application closed" note; the phone runs on). Cause: no Bluetooth stack.
bt.prt loads erihci.dll (`z:\private\101F7989\Esock\bt.bt.esk`: `hcidllfilename= erihci.dll, port= 6`), erihci opens
the LDD BTDRV (`btdrv.ldd`), which our hybrid lacks and whose phone binary needs SE's UART layer (ecust.dll,
GPIODRIVER.LDD). The port handler's set-up (porthandlersvr.dll 0x831c -> 0xa8c8 -> 0xa79c) then fails on
`RHCIDirectAccess::Open` and `RSdp::Connect` (-1) and leaves. To do: a `btdrv.ldd` stand-in (factory BtDrvDRV;
requests 0 read, 1 write one frame <= 2048 bytes, 2 signal notify, 3, 4; controls 0..10) forwarding H4-like HCI frames
to a host-side responder: Command Complete status 0 with fixed parameters for the init set (Reset 0x0C03, Read Local
Version 0x1001, Features 0x1003, BD_ADDR 0x1009, Buffer Size 0x1005, Class of Device, Local Name, Scan Enable,
timeouts, event mask/filter, host buffer size), plus erihci's vendor commands for the Philips BGB203/204 patch
download (0xFC0F, 0xFC22, 0xFC2B; handlers at 0xf83079bc / 0xf8307fb6) and its hardware-type query to the CCPU
(ccpustorage.dll / OpaClient). Estimate 1-2 days with tests. Not working (tried): NOPing the port handler's leaves
(it then uses the unopened HID session, KERN-EXEC 0); `BtBearer.ph` = 0. Details: docs/research/p1i-bluetooth.md,
docs/devices/se-p1i/bring-up.md item 27.

## Option 1 work plan

| Step | Content | Exit criterion |
|---|---|---|
| E1.1 | ROM composer: our kernel-side files + the N95 user-side files at their original XIP addresses + a new root directory/TRomHeader; paging off | composed image passes `romimg.py`; every N95 user file byte-identical at its original address |
| E1.2 | 9.2 exec table in our kernel: same dispatch mechanism, 9.2 numbering; unknown/unimplemented calls trace + fault; per-call serial trace switch | kernel builds; our own text shell still boots with a matching EUSER (regression check) |
| E1.3 | Boot the composed ROM: N95 `efile.exe` as file server | `efile` reaches its first IPC/loader calls; every stall named by the SWI trace |
| E1.4 | 9.2 semantics shims as hit (active scheduler / trap handler fast calls, DLL lock and attach, `MutexWait`, loader structures) | N95 `EStart.exe` runs |
| E1.5 | Hardware-facing stubs (DOS/SysAp, modem, accessories), display 240×320, keypad mapping | N95 `ewsrv.exe` draws, then Starter/SysAp, then the home screen |

The status of each step is tracked in [build-log.md](build-log.md) (option 1 section) and the
README.

**Status 2026-11-04 (end of day):**
- E1.1–E1.3 done.
- E1.4: shims added as hit (HandleDuplicate, MutexWait, SafeInc/Dec, loader struct size).
- E1.5 in progress. Hardware stand-ins exist for ISC (silent modem), Custom, the display driver,
  the ROFS media and a 240×320 32 bpp LCD. Nokia's WSERV, FBSERV, ECOM, Starter and DosServer run.
- Fake modem peer (ISI over ISC) done for the boot-critical messages; Starter runs its start list.
- The next blocker is `DATAPORT.CSY` leaving in the comms server; needs a ROFS E32-image loader for analysis.

## Option A (later)

Start from the QEMU 0.9.1 `n800` machine (OMAP2420, Menelaus, OneNAND, CBUS, I2C, SPI already
modelled). Boot the flat N95 core image directly, skipping NOLO and XZIP. Then add Nokia-specific
devices in the order Nokia's EKERN touches them: GAPE, STI, the display and keypad path, the ISC
modem link (fake CMT peer), DVFS, the PowerVR and DSP bridge stubs.

**Status 2026-11-05 (overnight, unattended):** runs 20–31 in [build-log.md](build-log.md); every
shortcut is in [hacks.md](hacks.md).
- Starter now runs its whole start list without crashing: `sae`, `SysAp`, `Phone`, `Idle`,
  `screensaver`, `startup`, `alarmserver`, `aknnfysrv`, `PhoneServer`, … (Avkon servers stable).
- Unblocked on the way: DRM key storage (dummy device cert), IMEI/IMSI/light/energy-management ISI
  replies, MmfAudioServer's permanent thread dying (no DSP; kernel spares it, key-sound server
  patched to run without MMF).
- **Display:** the path works (WSERV → ScDv 16MU device at our frame buffer, hardware rotation via
  `DisplayDriver`). The screen stays dark because (a) the splash logo renders empty and (b) no
  other window is drawn yet: SplashScreen keeps being dismissed and restarted by Starter, i.e. the
  startup sequence (Startup.exe / SysAp) has not reached the idle state.
- Next: find who sets `KPSUidStartup` (0x101F8766) key 0x301 and why the startup app does not
  finish (kernel `SYBPROP:` trace, patch 0004); then why AknIconServer renders SVG icons empty.
- Tooling added: `tools/pcsample_hmp.py` (guest PC sampling), `SYBORG_GDB=1|wait` for
  `scripts/hybrid-boot.sh`, `romcompose.py --patches` (verified byte patches), stub control hooks.

**Audio (paused 2026-11-05, owner's call):** QEMU `syborg-audio` device (PCM, tones, MP3/AAC via
FFmpeg; `scripts/run-gui.sh` plays through PipeWire), DSP stand-in streams the raw PCM node (startup
jingle works on `build/n95/hybrid-exp.img`, composed with the key-sound/startup-tone byte patches off by `scripts/compose-exp.sh`).
Open: key tones (DevSound request refused; audio-route reply 77/01→09 added, unverified), AAC/MP3
nodes untested (trigger: Profiles → ringing tone preview), working ROM still has the audio patches.
Details: docs/devices/nokia-n95-8gb-rm320/nokia-dsp-playback.md, docs/hacks.md (H-dspbridge).

**Queue (2026-11-05, from the project owner):**
- Done (run 42): Startup "hands" animation, first-boot region/date/time queries and the idle screen (fake modem answers a definite "no SIM"). Done (run 44): Phone.exe comes up (DSP bridge + DVFS stand-ins let MmfAudioServer start), publishes 213, the phone-ready skip patch is gone and the full Active Standby idle screen stays up.
- Screensaver partial display: implemented (DisplayDriver ctl 3/4/5 → Syborg fb PARTIAL_* registers, rows outside the band black, 8-colour idle mode); verified by poking the registers, not yet seen from the real screensaver (the idle screen does not stay up long enough: Phone/audio).
- White band at rows 240–319 and a stuck "SysAp: Feature not supported" note: half-initialised UI;
  expected to go away as SysAp/Startup get their missing features. Not a display bug.
- Done: host mouse input to the Syborg pointer is off by default on modern QEMU
  (`-global syborg-pointer.input=on` to enable); S60 3rd edition has no touch screen.

**Fixed (2026-11-06): home screen lost focus after the menu ("frozen idle").** Cause: no SIM, so
Startup switches to the Offline profile, which sends `MTC_STATE_REQ` (RF_INACTIVE) via DosServer;
the fake modem did not answer, the switch failed ("failed to activate offline mode"), the phone
stayed in General. Phone.exe (FUN_8170b898) brings "Standby mode" back to the front on a foreground
event only when SIM status (P&S 0x101F75B6/0x100052E9) != 5 (removed) or the Offline profile is
active; otherwise Phone itself (window group "Telephone", nothing drawn) keeps the focus. Found
with `src/n95apps/wgprobe` (focus/window-group order) and `syb_gdb brk 0x8170b898`. Fix: MTC_STATE_RESP
0x64 OK in `iscpeer.inl`. Keyguard unlock (F5 then F7 or `*`) was never broken.
**Open: keyguard emergency-number check.** While the keypad is locked every key sends
`CALL 0x28` (emergency number check, UCS-2 key string, from NOKIATSY in c32exe) and waits ~9 s for
a reply the fake modem does not give; keys pressed meanwhile seem lost. Needs the `0x29` reply
layout (NOKIATSY parser not located yet). Screensaver band clipping fixed 2026-11-06 (WSERV
rotation index 0 = landscape picture, now 1280-byte lines and rotated by the fb; H-lcd-native-landscape);
orientation confirmed by the owner.

**Open: no 3D/GPU (PowerVR MBX) — "Carousel: Memory full" (found 2026-11-06 with sydb).** Carousel
(`carousel.exe` UID 0x10282bf6, the N95 8GB multimedia menu) renders through `hitchcock.dll`
(Nokia's Hitchcock UI toolkit) on OpenGL ES 1.x: `libgles_cm.dll` → `libimgegl.dll` →
`services_um.dll`, which does `User::LoadLogicalDevice("services")`. Nokia's kernel half of the
PowerVR stack (`services.ldd`, `pvrsrv.dll`, `pvromaplcd.dll`, `omaplcd.ldd`) is not in our kernel
ROM, so that returns −1 [verified, gdb], `eglInitialize` fails with `EGL_BAD_ALLOC` 0x3003
[verified], and Hitchcock (hitchcock.dll+0x3942c) maps that to `User::Leave(-4)`. Eikon shows that
as "Memory full. Close some applications and try again". It is not a real memory shortage. It
happens when Carousel builds its UI (it is in the foreground, e.g. `sydb launch 0x10282bf6`), not
while it idles in the background. Other GLES/M3G users will fail the same way. Fix = a GPU model
(PowerVR services driver or a GLES stand-in), not planned yet.

**Planned: GPU (OpenGL ES 1.1 + EGL), 2026-11-06.** Plan in [research/n95-gles-design.md](research/n95-gles-design.md):
- keep the ROM's `libgles_cm.dll` and redirect its 175 entry points (generated code-patches) to our
  own GLES/EGL implementation, built for a fixed address in our part of the ROM;
- software rendering on the phone first (owner: no host GPU for now), split into a front end and a
  back end so a QEMU device can take over the back end later;
- frames shown through direct screen access (as Nokia's WSEGL does), vsync-paced;
- milestones: ZigGLTri (triangle) → ZigGT2Car (textures/lighting) → Carousel/Hitchcock → M3G.
Owner decisions pending: entry-point patches vs. replacing the DLL, own renderer vs. a base library,
colour depths, where the vsync wait lives.

**Planned: phone skin (emulator window with hardware buttons), 2026-11-06.** Built as
`sydb phone` ([sydb.md](sydb.md) "Phone window"): a PyQt6 `sydb phone` window (VNC screen, button bar, slider). Side
keys need only keymap entries. The slider and accelerometer need stand-ins for Nokia's kernel-side
`keypad.dll` / accelerometer drivers, which are not in our ROM.

**Fixed 2026-11-06: game audio played too fast** (the DSP stand-in now takes each raw PCM stream's rate and channels from the N95's own EAP 0 / RawGwc messages, docs/research/dsp-pcm-format.md; Crash Bandicoot = 22050 Hz mono, jingle 44100 Hz stereo). Was: Crash Bandicoot Nitro Kart 3D's sounds
(MediaClientAudioStream, CMdaAudioOutputStream) play sped up on run-gui. Suspects: the stream's
sample rate not honoured by our audio path (DevSound/DSP stand-in or syborg-audio resampling), or
buffers consumed faster than real time. Check with `sydb strace` on the audio server and the
syborg-audio device's rate handling.

**Keys (2026-11-06, patch 0015 fixed):** QEMU sends F13-F19 as set-1 0x5d/0x5e/0x5f/0x55/0xe003/
0xe077/0xe004 (not 0x64-0x69), so the first 0015 filled unreachable slots. Now: F13 = EKeyDeviceA,
which akncapserver turns into the **landscape** layout switch (the N95's media slide; whole UI),
F14 = EKeyDeviceB = **portrait** (sydb key aliases `landscape`/`portrait`; `sydb phone`'s Landscape
button). F15/F16 = EStdKeyApplication19/1A (camera app captures them; camera untested).
F17 = EStdKeyApplication7: akncapserver sets P&S 0x101f876e/4 = 1, F19 = App8 sets it to 0 (meaning
unknown, not the Multimedia key; F18 = 0xe077 would index past the 0xf0-entry table).
**Multimedia key found (patch 0016):** the N95 keypad.dll matrix table (0x8014f6d4, three slide
variants) gives the codes; F20 = EStdKeyApplication6 opens Carousel, F21 = Application1D Gallery,
host Play = Application2 the music player; App3/4/5 (stop/prev/next), App1B/1C (variant keys) and
App19/1A (camera) not verified. The
"sideways Carousel" was the landscape layout left on by an F13 test; Carousel itself uses no fixed
orientation. Also open: Carousel's boot instance still dies with USER-EXEC 3 on some boots
(the race at its ConstructL); `sydb fg` on its window group then shows only the backdrop.

**RomPatcher+ (2026-11-06, owner's device): works.** It hung the phone because its `patcherShadow.ldd`
(built for the 9.2 kernel) crashed our kernel; fixed by matching the N95 kernel's driver ABI
(H-kernel-abi-92, `tools/personality/kabi.py`); it now lists and applies its patches. Old notes: Its drivers only use `Epoc::AllocShadowPage` / `CopyToShadowMemory` /
`FreeShadowPage` / `Epoc::RomHeader` / `Kern::SuperPage`. Options: (1) a 9.2 kernel-ABI personality
for RAM-loaded drivers (loader marks the code segment; imports of DObject/DLogicalDevice/
DLogicalChannelBase constructors, destructors, SetName, Close... go to 9.2-layout shims; the kernel
talks to the driver's device/channel through proxy objects that call the 9.2 vtable slots); a
contained but real job. (2) Apply the patches RomPatcher+ would apply at ROM composition
(tools/romcompose.py `--patches`), which needs no driver at all. Also seen in the log: many recovered
kernel faults in `__magic_address_readdesheader1` from `SessionSendSync` (clients passing small
integers as descriptor pointers, e.g. TmScanServer, Anykey): worth checking against exec92's IPC
argument-flag translation.

**In progress: a safe graphics boundary, then a faster one (2026-11-11).** Plan, exit tests and status in
[gpu-boundary-roadmap.md](gpu-boundary-roadmap.md) (findings in it). Phases 1
(authority: user-permission guest access, kernel-only registers, owner and compositor rights from
d_display) and 2 (lifetime and bounds) are done; then the speed phases for slower hosts (phones): OpenVG
batching (3), fewer copies (4), a renderer worker (5), each measured with `tools/gl_bench.py`. Hostile tests:
`tools/probes/glsec`.

**Later: host GPU presentation and the rest of the settings (parked 2026-11-08 by the owner).**
Design and reasons in docs/gpu-host-review.md (sections 2 and 3, status table at the top). In order:
1. Device side of GL scan-out: the syborg-gl context shared with QEMU's display EGL context, a
   `gl_scanout` path in syborg-fb behind a property (`syborg-fb.scanout=memory|gl`), a fence per
   present; check with `-display gtk,gl=on` in a throwaway session (review 2.2).
2. `-display dbus,gl=on,p2p=on` from the daemon and a DMA-BUF listener in `sydb phone` (a small ctypes
   EGL helper importing the buffer into a QOpenGLWidget); shared memory stays the fallback
   (`--screen shm|dbus`) (review 2.3). Screenshots and the guest-visible frame buffer keep working.
3. Supersampling (`scale` = 2: host surfaces at twice the guest size, guest frame buffer downsampled)
   for a sharp window at 2x, once step 2 shows frames straight from the GPU (review 2.4).
4. Remaining settings (review 3.2/3.4): `mipmaps`, `pacing=vsync` (gcebe completes `displayed` on the
   next vsync), `overlay`, `debug`, `epoch` + `reset`, CAPS/CFG registers and a protocol version,
   MCP tools `gl_get`/`gl_set`, a Rendering menu in the phone window over the same daemon op.
5. Only if measurements change: host tessellation, zero-copy uploads, rectangle fast path (review
   4.1; parked because the guest idles 74% during UI work and libEGL is under 2% of samples), and GPU
   state in snapshots (review 1.1).

## Open issues reported by the owner (2026-11-08, after manual runs)

Not investigated yet; start each with the `symbian-debugging` skill (unanswered ISI requests first, then
the IPC trace build to find who waits on whom).

| Where | Symptom [owner] | First step |
|---|---|---|
| every N8 firmware (011.012, 013.016, Anna, Belle) | a "widget removed" note at boot: a home-screen widget fails to initialise and the home screen drops it | find which widget (home screen / content publisher logs, `cpserver` IPC results: the IPC trace showed cpserver answering the home screen with -1 several times on Anna) and what its publisher waits for |
| N8 Belle 111.040.1511 | every home-screen widget shows a loading spinner forever | same publisher path as above; also RES_C9 msg 24 is unanswered on Belle only |
| N8 Anna 025.007 | opening the menu turns the screen fully black and the phone seems to hang | catch/IPC trace while opening the menu from the seeded home screen; compare with the menu on 011.012 (works) |
| N95 (oracle, 2026-11-08) | the real phone boots with no abnormal thread exits; qembian has 9 (central repository restart, SplashScreen E32USER-CBase 46, WLAN, USB, Bluetooth SDP/FaxModem, lbdtracker, MobileCrash servers) | docs/real-phone-tracing.md "First real recording": fix them one by one, re-run `tools/oracle.py diff` |
| 5800, N8 (all S60 5th / Symbian^3 phones) | random UI actions take seconds, e.g. Settings, then any item | likely a request to a driver or server that only completes on a timeout: time the action with the IPC trace, look for requests completing after ~1-3 s or with errors, and for unanswered ISI requests sent at that moment (the 3 s haptics stall, H-haptics-fail, was this kind) |

## Nokia 9500 Communicator (RA-2, S80, EKA1): both screens live (2026-11-18)

State (commit 320ed9e): boots on `-M nokia9500` to 58 stable processes. Cover: idle screen (wallpaper, clock,
indicators, softkeys), keypad works with the lid closed. Inner: S80 UI, lock code 12345 accepted, first-start
wizard (language, time/date, contact card); Desk runs. QEMU OMAP1 fixes 0009-0017, audio without a DSP,
stalls 20-38 in docs/devices/nokia-9500-ra2/bring-up.md. Window: `python3 eka1/tools/ngage_phone.py --model
nokia9500`. Open: finish the wizard (F9 = command button 1 = OK, untested), Desk home screen, cover scroll keys
unconfirmed and a cover-UI freeze after some of them / Select->Save ("Contacts not ready"), no flash
persistence (the wizard returns every boot), isolation checks not re-run after 0015-0017 (OMAP only).

### Original plan (owner, 2026-11-18)

Firmware RA-002 v.06.27 (extracted to eka1/ra2_rom/, git-ignored). Two displays (cover and
the inner 640x200 one) and the cover open/close: **model them properly, as the E90 (H-e90-two-panels): both
panels on the board, the cover as a real switch input read by the phone's own drivers, no hacks** (owner).
QEMU's own OMAP1 models (`-M sx1`, hw/arm/omap1.c, omap_sx1.c: OMAP310) are the starting point if the 9500 is
OMAP1510 [I]. Flash format: Phoenix A2 with 0x28 records; the core is one raw-deflate stream (offset 0x61,
0x1122000 bytes, an EKA1 ROM: rom_base 0x50000000, size 0x1200000) followed by ROFS images [V].

## Nokia 7650 (NHL-2NA, EKA1): boots to the home screen (2026-11-18), same level as the 6600

Same list as the 6600 below (ISI after idle, starting with the self-test `59 02`; menu and apps; time asked
every boot; first-boot hint; camera, IrDA, Bluetooth, sound). docs/devices/nokia-7650-nhl2/bring-up.md.
Camera done with a test pattern (2026-11-26, docs/devices/nokia-7650-nhl2/camera.md); the host camera as a source is next if wanted.
Camera also on the 6260, 7610 and 6670 (2026-11-26, same document); the 3230's camera module (a command port) is open.
Camera on the 6630 (2026-11-27, OMAP1610 board: same document, "Nokia 6630"); the 6680 with the same model (photos only,
"Nokia 6680"). N90: frames reach the driver, the viewfinder stays blank (video set-up, likely the DSP; "Nokia N90").

## Nokia 6600 (NHL-10, EKA1): boots, not really working yet (owner, 2026-11-18)

It reaches the home screen on `-M nokia6600` (docs/devices/nokia-6600-nhl10/bring-up.md), but the owner's
verdict is "it does boot but that's mostly it". Parked for the 7650; to make it really work:
1. Answer the unanswered ISI requests after idle (TSY 5A 00, SIM 09 09/29/23, SMS 02 1E/16, DSY 0B 11 {01 00})
   and give the phone a network state (MTC/NET replies), as the N-Gage table does.
2. The menu and the main apps (Phone, Messaging, Camera, Gallery), each until it opens without a stall.
3. Time and date: no longer asked on a restart (H-nhl10-rtc); every boot comes up on the host's local time (H-uem-rtc,
   2026-11-26).
4. A first-boot hint in the phone window: the screen is white for ~60 s while C: is formatted.
5. Memory card (mmc.pdd, new driver), power key (UEM), sound, Bluetooth. Camera done (7da861e). IrDA done
   (2026-11-26): two phones beam to each other (docs/eka1-ngage.md "Infrared"); the 6260 beams too (2026-11-26,
   H-ngage-timer-zero); the 7650, 3650/3660 and 3230 have it on, not tried; the N-Gage QD's is off. Contacts: blank list, one "PLAT 3" kernel fault (bring-up page).
6. Fixed 2026-11-18: the restart hang (owner: hangs on the home screen when not --fresh). Phone.app was killed by
   the view server (ViewSrv 11) while blocked in GetALSLine (SIM 09 29, unanswered); not a regression (8be36d8
   does the same). Replies 09 29 / 09 23 and H-nhl10-rtc (no time query on restart). Details:
   docs/devices/nokia-6600-nhl10/bring-up.md "State".

## Fake BB5 modem realigned to the N95 modem firmware (2026-11-18)

The shared reply table (`platform/isi/replies-bb5.txt`), the N95's no-SIM `0A 00` line and the modem core
(`syborg_modem_core.c`) now send what the N95 8GB's own modem firmware encodes (docs/research/n95-isi-encoders.md
10, 11.5): MTC RF status `79 {00 01}` in NORMAL (from the MTC state), INFO `1B 2F` (real layout, see below), EM `17 01/0B/09` built
from the request mask, SEC `12 {05 16 00 00 00 00}`, NET 8-byte sub-blocks 00 and 2C (`0A 01/02/37`), `0A 40` -> F0
not supported (the S60 3.2/5th phones keep `41` in their own tables), SIM `1A {2F FB}` without and `{2F 24 00 00 00 00}`
with a SIM, SIM_STATUS `C1` 6-byte forms, RTC `19 0B` with a date/time sub-block from the host clock. Host tests (`tests/isi_replies_test.py`, `tests/modem_unit_test.py`).

Same evening regression (owner: N95 8GB no boot, logo ~15 s late; 5230/5800/E52 black screen; 808 and N8 fine): the
138-byte `1B 2F` (148-byte message) reached SAServer's 128-byte ISC channel-35 receive; ISCPEER completed
KErrOverflow, SAServer re-posted the same buffer at once, 1.5 million times, and start-up starved. Closed the same
night (docs/research/n95-isi-encoders.md 6, 12):
- **ISC overflow handling** [V]: the N95's NokiaISCDriver.ldd answers a too-small receive with KErrNoMemory (-4) +
  the needed length and keeps the message; SAServer (N95 and E52) and NokiaDSY grow their buffer on -4 only. Our
  stand-in now does the same (`src/standins/_shared/iscrx.h`, `iscpeer.inl`, ISC and ISA, Receive and DataReceive),
  logs the first refusal once per channel, and drops a message a client refuses more than 3 times in a row so nothing
  can spin (docs/hacks.md H-isc-rx-overflow). Test `tests/iscpeer_overflow_test.py`. Verified: N95 session with the
  138-byte form forced in its own table: idle at 15.3 s, one `KErrNoMemory` line, `deliver (148)`.
- **The real `2F`** [V]: the modem sends all 138 data bytes (`00 01 F0 00 88 00`, flags u32, 128-byte PP block);
  the phone (SAFeatureInfoService) reads the F000 sub-block (id16, len8) up to byte 27. Table: `len=18 00 01 F0 00 10
  00` (the real layout cut to the 16-byte sub-block, flags 0; the old 16-byte form had the length in the filler).
  Tests `SAServerSpinsOnLongProductProfile` (118-byte cap kept), `test_product_profile_bits_fits_saserver_buffer`.
- **Seeds**: `build/seeds/nokia-5800-rm356` was a broken link to `../runs/5800-seed` (build/runs is gone); recreated
  as drives of a fresh boot past the region/date/time queries and the welcome note (idle from it at 8.0 s). New
  `build/seeds/nokia-e52-rm469` the same way (region, date, time, Phone setup skipped). The 5730 and N79 seeds (also
  broken links to build/runs) recreated the same way: 5730 region (emergency-call note OK'd), date, time, Phone setup
  skipped; N79 region, date, time, emergency-call note OK'd, welcome note declined. Booted from the new seeds on
  sessions: 5730 idle at 15.5 s, N79 idle at 15.0 s, home screens drawn.
- Smoke after the change: N95 8GB PASS (idle 15.8 s, Carousel, Switch off), 5800 PASS (idle 7.8 s), E52 PASS (idle
  8.0 s; Switch off now declared `smoke_skip`: no power menu from F4 or a held End key [V]).
  5730 PASS (idle 15.6 s), N79 PASS (idle 15.3 s); both have no GL (no Carousel check) and keep their existing
  Switch off `smoke_skip` (no power menu).
- **`SosPmmHandler -5` is pre-existing, not from this change** [V]: `Thread sospmmhandler.exe::Main Panic
  SosPmmHandler: N -5` ("No PMM partition found from NAND.", the product-data partition our flash lacks) is in the
  5800-v20 instance log of 2026-11-14, the N79/E72 logs of 2026-11-14, the 5730/E52/6720 logs of 2026-11-17 and the N86
  catch of 2026-11-10 (build/evidence/bringup-2026-11-10/n86-rm484/catches/001-sospmmhandler.exe_Main.txt), all before
  the realignment. Non-fatal (the boot goes on); recorded in docs/devices/nokia-5800-rm356/bring-up.md. Like
  `mc_useragent KERN-EXEC 0` (docs/build-log.md), it is a known start-up panic on the BB5 S60 phones checked (and the N8).
- Host tests after the seeds and smoke runs: `tests/isi_replies_test.py` (22), `tests/modem_unit_test.py` (33),
  `tests/iscpeer_overflow_test.py` (5) OK; `tools/isi.py lint` ok. No open items left from this change.

## Check later: display panel model (d_display.panel) on the phones not yet confirmed

2026-11-13: the display stand-in can keep a panel buffer filled only by WSERV's posted updates (ctl 9/8), as the
real N95-family driver; scanning out the video RAM directly showed every redraw half done (black blinking, fixed on
the 5800/5230, owner-confirmed "drastically better"). Enabled (`config = { panel = 1 }` on `d_display`) where logs
show steady ctl 9 traffic: N95, N95 8GB, E5-00, N97, E52, 5730, N79, E72 (+ 5800, 5230).

2026-11-17: N86 enabled (748 ctl 9 in 160 s; verified on a clone). N73 ME posts whole-screen ctl 8 steadily (1010 in
6 min) - a candidate, not enabled (its boot is slow, not verified). Not enabled, to check when they run well enough:
E71 (rm346), N81 8GB (rm179), N73 (rm132), 6720 (rm424). Their logs had 0-3 ctl 9 calls (short or missing runs). Check: `grep -c "ctl 9 " <session>/serial.log`
after a minute of UI use; steady calls -> add `config = { panel = 1 }` to the profile's d_display entry and the
device to tests/device_py_test.py. Caution: with the panel on, a phone whose WSERV does not post with ctl 9 would
keep showing its first frame. GPU phones (N8, 701, 808) compose on the host and stay off.

## UI as data (`sydb ui`): platform sweep (planned, owner 2026-11-20)

`sydb ui` (in-app probe + gdb reader, [ui-automation.md](ui-automation.md)) works on the N95 8GB. Next: run
`tests/uiprobe_smoke.py --device ID` on every S60/Symbian^3 profile and fill in the table in
ui-automation.md "Platforms", working through its assumption checklist where a device fails (first suspects:
imports on 9.1, the Avkon icon record and the drawing path on Symbian^3). UIQ (P1i, P990i) needs a Qikon
variant of the probe; the gdb reader needs the moving and flexible memory models.

## Small open items carried over (2026-11-22)

- Profile cleanup (from the 2026-11-07 cleanup review, item 1.1): `[props]` vs `milestones.idle`, `[status]`
  fields that drift, `kernel.trace_mask`, `rom.manifest`, `[skin]`, `[composition]` docs, the LCD build rename.
- Idea, not started: start Linux from a running Symbian on the virtual N95 the way it would be done on a real
  one (a user-side loader app + a kernel driver that takes over the machine), as a rehearsal for the real phone.
