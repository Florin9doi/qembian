# Nokia 6600 (NHL-10) bring-up

Firmware V 05.53 (`NHL105530.C1` MCU, `NHL105530.26` PPM). Symbian 7.0s, S60 2nd Edition, EKA1
kernel. It is the first 7.0s phone in qembian. It follows the N-Gage QD path: the phone's own kernel,
unmodified, on a board derived from `ngage` ([../../eka1-ngage.md](../../eka1-ngage.md)).

Marks: **[V]** verified in the firmware bytes or code, **[I]** inferred (a best guess, with its reason).
Addresses are ROM virtual (0x5xxxxxxx), physical (0x0xxxxxxx) or kernel data (0x80xxxxxx).

## State (2026-11-18)

**Owner's verdict: it boots, but that's mostly it.** Parked for the 7650; what "really working" needs is the
"Nokia 6600" section of docs/roadmap.md and "Open (next)" below. A fresh phone shows a white screen for about
60 s while C: is formatted (the window does not say so yet).

- Steps 1 (format) and 2 (comparison with the N-Gage) are done.
- Step 3: the board is `-M nokia6600`, a variant of `ngage` (an `NgageModel` in `ngage.c`). The
  N-Gage still boots as before (`tests/ngage-boot-check.py`).
- Step 4 (first boot): bootstrap -> kernel -> EFile -> EStart (C: formatted on the first boot, ~60 s)
  -> window server -> Starter (51 processes) -> Startup -> first-boot time and date queries -> **the
  home screen** (wallpaper, clock, date, softkeys Camera / Go to). See [First boot](#first-boot-step-4).
- Step 5: keypad and joystick modelled (H-nhl10-keys); the window is
  `python3 eka1/tools/ngage_phone.py --model nokia6600` (flash in build/ngage/phones/nokia6600.flash;
  `--fresh` = factory-fresh). ekeyb debounces: a press must last about a second in scripts (a human
  press is fine).
- Checks: `python3 tests/ngage-boot-check.py --model nokia6600` (and without `--model` for the
  N-Gage) after every board change; `tests/ngage_isi_test.c` for the ISA framing.

### Open (next)

- Unanswered ISI after idle: phonetsy `5A 00`, SIM `09 09`, SMS `02 1E` / `02 16` (no MMSMSGSM consumer
  for 02 17 / 02 1F found: their response ids are unknown); the DSY's `0B 11 {01 00}` (audio subtype 01).
- The menu, the Phone app's network state (no MTC/network answers yet), the camera, the memory card
  (mmc.pdd is a new driver), Bluetooth (the N-Gage's LPRF model is untried here).
- Contacts: the list stays blank, and in one run the kernel faulted "PLAT 3" (Kern::Fault from the supervisor
  thread, stack through isa_if_driver.ldd 0x50410224) about a minute after Contacts opened [V run, 2026-11-26, with
  IrDA off]. Cause not looked at (an unanswered SIM phonebook request is the first guess [I]).
- IrDA: done 2026-11-26 (H-nhl10-sir; docs/research/6600-irda.md; how to use it: [infrared.md](infrared.md)). Two phones beam a Calendar entry to each other
  (`ngage_phone.py --irda a` / `--irda b`, docs/eka1-ngage.md "Infrared"). The first FIQ the phone ever took froze it
  until 0x0C120050 stopped keeping the kernel's start-up 7 (it reads 0 now, H-upp-fiq-status). Checks: `tests/ngage_sir_test.c`,
  `tests/ngage_nhl10_irda_test.py`.
- Restarts (a kept flash): fixed 2026-11-18 (owner: "hangs on the home screen when not --fresh"). Phone.app
  calls RMobilePhone::GetALSLine synchronously while building the idle view (sssettings 0x51032D44 ->
  etelmm 0x50632B18 -> SIM `09 29 {35 00}`); unanswered, the view server killed Phone (ViewSrv 11), the idle
  area stayed unpainted ("Locked" or the NOKIA animation frame on top). A fresh boot did not hit it: there the
  value comes from CommonEngine's store and the start-up is longer. Fixed with the replies `09 29 -> 2A {35 FF
  00}` and `09 23 -> 24 {25 01 00}` (docs/research/6600-tsy-sim-replies.md) and H-nhl10-rtc (no time query on a
  restart: docs/research/6600-rtc-clock.md). Not a regression: the first home-screen build (8be36d8) does the
  same. Checks: `tests/ngage-boot-check.py --model nokia6600 --idle`, `tests/ngage_isi_test.c`
  (test_nhl10_als_line). Dead ends on the way (docs/research/6600-autolock.md): the security indication
  08 10, the start-up reason 105, Autolock.
- The clock starts at rtc.dll's default date after a restart (no time query any more).
- The power key (UEM), the 0xAA key and the switch line (0xA8/0xA9).
- The NOS's real hardware chunks (H-nhl10-hwchunk) and flash part (H-nhl10-flash-st).

Running it (an agent session; the owner's command comes with step 5):

```sh
python3 eka1/tools/ngage_sess.py -s NAME start --model nokia6600 [--flash build/ngage/phones/NAME.flash]
python3 eka1/tools/ngage_run.py --model nokia6600 --secs 5      # one-shot experiment with a report
python3 eka1/tools/port_addr.py VA...                           # N-Gage address -> 6600 (code match)
```

## Files and extraction

| What | Where (git-ignored) |
|---|---|
| owner's archive | `NHL-10 v.05.53 6600.rar` (the flash-file archive) |
| flash files | `eka1/nhl10_rom/NHL105530.C1` (23 285 863 B), `NHL105530.26` (1 525 735 B) |
| ROM.bin, Z: tree, PPM tree | `eka1/nhl10/output/` |
| reports (`report.md`, `ghidra-notes.md`, `files.tsv`, ...) | `eka1/nhl10/analysis/` |
| raw PPM/MCU segments (dct4_extract) | `eka1/nhl10/extracted/` |

From `eka1/`:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/eka1_extract.py \
    nhl10_rom/NHL105530.C1 nhl10_rom/NHL105530.26 -o nhl10/output --report   # analysis -> nhl10/analysis
python3 dct4_extract.py nhl10_rom/NHL105530.C1 nhl10_rom/NHL105530.26 -o nhl10/extracted
```

SHA-256: C1 `12ca6e0d…f53c6f1`, .26 `d5452212…28d1e2`.

## Step 1: the format

- **[V] Same container.** Phoenix A2 / DCT4 records (types 0x14, 0x20, 0x21), the same header tokens
  ("WD2 ALGORITHM", "WD2 CBUS BT"). All 3003 type-14 data checksums carry the legacy "+1", as on the
  N-Gage.
- **[V] `eka1_extract.py` reads it unchanged.**
  - ROM: 0x1600000 bytes at 0x50000000.
  - Tree: 1614 entries (106 directories, 1508 files), 0 incomplete, every file re-verified byte for
    byte. 21 primary and 561 secondary modules.
  - The only fix: its report and Ghidra notes quoted the N-Gage's kernel address, flash split and PPM
    windows for any firmware. They are now read from the image (`kernel_header`, `bank_lines`,
    `variant_lines`; regression test `eka1/test_eka1_extract_report.py`, bug "report_hardcoded_ngage").
- **[V] `dct4_extract.py`** parses the containers (raw segments) but, as on the N-Gage, finds no tree.
  `eka1_extract.py` is the extractor to use.
- **ROM header** (flash 0; the N-Gage value in brackets):

  | Field | 6600 | N-Gage QD |
  |---|---|---|
  | rom_base / rom_size | 0x50000000 / **0x01600000** | 0x50000000 / 0x01170000 |
  | root dir list | **0x50300000** | 0x50311000 |
  | kernel data | 0x80000000..**0x80300000** (3 MB) | ..0x80400000 |
  | primary / secondary list | 0x50301EC4 / 0x50302148 | 0x503112A8 / 0x50311488 |
  | trace mask (+0xD8) | **0x80008003** (bit 30 off) | 0xC0008003 |
  | screen x/y/bpp | 0 (not set) | 0 |
  | last two words | 0x0000A000, 0x0029C000 | 0x0000A000, 0x002AD000 |

  [I] The last two words are the NOS image start and size (they match the scrambled range below).
- **[V] Flash layout: four chips, not two.** The flash file programs:

  | Flash range | Contents |
  |---|---|
  | 0x00000000..0x00800000 | ROM offset 0 (bootstrap, NOS, Symbian start) |
  | 0x04000000..0x04700000 | ROM offset 0x800000 |
  | 0x06000000..0x066FF000 | ROM offset 0xF00000 (data ends at ROM 0x15FF000) |
  | 0x02000000..0x02164000 | PPM (.26 file): tree at virtual 0x51700000 |

  - [V] The bootstrap's one bank table (ROM 0x64C, 19 entries of {physical, size, attributes}) maps,
    in this order from virtual 0x50000000: chip 0 0..8 MB, 0x04000000 7 MB, 0x06000000 8 MB,
    0x02000000 2 MB (PPM, so virtual 0x51700000, matching the extracted tree), 0x04700000 0xC0000,
    then 0x0C000000 2 MB, 0x0D000000 16 MB, 0x02200000 6 MB twice (two attribute sets; [I] the
    C: flash file system, as 0x02400000/0x02600000 on the N-Gage), 0x08000000 4 KB (EMIF),
    0x0A000000 16 MB (SDRAM), 0x0C120000 4 KB.
  - [V] The chip loop (ROM 0x61F4) identifies chips 0..3 at `n << 25` and **halts (bit 3 of
    0x0C0047FD, spin at 0x624C) if any of the four is unknown**. The N-Gage needed chips 0 and 1.
  - [V] Accepted IDs (table at ROM 0x74E0, {manufacturer, device, ext1, ext2, ...} → type): AMD
    0x01 / 0x22D1, 0x247E, 0x227E, 0x257E, 0x267E, 0x277E (with extended IDs); Intel 0x89 / 0x8872,
    **0x8874 (type 4)**, 0x8876; Samsung 0xEC / 0x22F0, 0x22F8, 0x22FA; ST 0x20 / 0xA4, 0x88C0,
    0x88BE. The ID read now also reads words +0x1C/+0x1E (AMD extended IDs). The board's Intel CFI01
    0x8874 is still accepted.
  - [I] Four 8 MB chips (each table range fits in 8 MB per chip).
- **[V] ROM digest check, as Q12.** The entry at ROM 0x8F18 covers 0x50009000..0x515FEFFF, and the
  stored MD5 (`88e6c388…`) does not match the image (`5fb5d92b…`). H-ngage-md5 applies unchanged.
- **[V] NOS scrambled, as Q20.** ROM 0x00C000..0x29C000 has ~8 bits/byte of entropy; the bootstrap
  below and the NOS data above (0x29C000..0x300000) are plain. `nosstarter.ext` (same 0x94 bytes of
  code) calls the NOS entry **0x52882024** with data 0x52AFACB8 (0x4B34 bytes) copied to
  **0x80002210** and bss **0x80006D44 + 0xC0358** (N-Gage: 0x5149E4C4, 0x80006EF0, 0x8000C378).
  [I] The NOS alias is virtual 0x52800000 → physical 0 (entry at ROM 0x82024, scrambled; data at
  ROM 0x2FACB8, plain), as the N-Gage's 0x51400000 alias.
- **[V] PPM.** One tree at flash 0x02000000 (virtual 0x51700000), 2189 entries (110 directories, 2079
  files), all verified. No separate data window (the N-Gage had one at flash 0x00F00000): the file data
  sits in the same chip-1 window or in the MCU ROM.

## Step 2: comparison with the N-Gage

### Kernel

| | 6600 | N-Gage QD |
|---|---|---|
| ekern.exe | 0x5030FF9C, 0x2974C bytes | 0x5031AF9C, 0x2786C |
| code / entry | 0x50310000 / 0x50310054 | 0x5031B000 / 0x5031B054 |
| tools version | 1.00 build 183 | 1.00 build 175 |
| exports | **478** (dir 0x50338FC0) | 506 |
| exec table | **0x50314AEC, 273 slots**, bounds-checked (0x444) | 0x503210DC, 237 slots, unchecked |
| exec dispatcher (SWI vector) | 0x50314F30 | 0x50321490 |
| data / bss | 0x500 / 0x1C58 | 0x4E4 / 0x69E4 |

- [V] The vector page and the SWI dispatcher are the same code; 7.0s adds a bound check and 36 exec
  slots (4 null slots in both).
- [V] ekern's hardware literals are the N-Gage's: interrupt controller 0x58120114 / 0x581201D4 /
  0x58120050, 0x0C000432, 0x0C000C00, 0x58200000, the trace port 0x591CFFFF / 0x591D0000 /
  0x59010000. New: 0x5814D006, 0x59020000, 0x59030000; 0x59E00000 replaces 0x59A00000 (also in
  medint.pdd: [I] the C: flash window moved).
- [I] Kernel addresses found by matching N-Gage code bytes (to confirm before use as catches):
  fatal-fault routine 0x50312B34, CBus 16-bit accessor 0x50312F00, EUser RProcess::Create 0x5039EAC0.
- [V] **Trace mask bit 30 is off**: the kernel prints nothing to the trace port unless the board sets
  it (in the served image's header or in the superpage).

### Kernel-side modules (primary list)

6600: ekern, ecust, ecomm.ldd, ramdrive.pdd, rtc.dll, nosstarter.ext, pnmsg.dll, isimsg.dll,
**isaapi.dll, testbluetooth.dll, testhci.dll, ekeyb.dll, ekdata.dll**, cbus.dll, helper.ldd,
medint.pdd, ELocal.fsy, ELocl.dll, EUser.dll, Hal.dll, **Dhal.ldd**. The N-Gage's primary list has
**phonet.dll** and none of the bold ones.

Drivers in the whole ROM (size 6600 / N-Gage):

| Same driver, both phones | 6600 only | N-Gage only |
|---|---|---|
| audio.ldd, btldd.ldd, btcomm.csy, cbus.dll, ecomm.ldd, ecuart.csy, elocal.fsy, eram.fsy, euart1.pdd, hal.dll, helper.ldd, lcd.ldd (0x9348 / 0x6B20), medint.pdd, nokia.dsy (0xA5E0 / 0x7134), nosstarter.ext, phonetsy.tsy (0x10A74 / 0x40180), ramdrive.pdd, rtc.dll | **enosdriver.ldd, isa_if_driver.ldd** (+ isaapi.dll, isa_if_protocol_base.dll), camldd.ldd, campdd.pdd, dhal.ldd, ircomm.csy, mmc.pdd | **nosdriver.ldd**, phonet.dll, gd1drv.ldd, cameraldd.ldd, camerapdd.pdd, medmmc.pdd, hotswapdrv.ldd |

- [V] **The cellular transport changed.** The N-Gage's `nosdriver.ldd` + `phonet.dll` (which
  `ngage_isi.c` decodes) are gone. The 6600 has ISA_IF_DRIVER ("ISA KERNEL-IF") + ENOSDRIVER
  + `isaapi.dll` (RIsaApi, opening PhoNet objects such as PN_OBJ_EPOC_MONSERV_SERV) and the 7.0s
  multimode ETel (etelmm, mmcustomtsy, a much smaller phonetsy). The messages are still ISI; the
  driver, its channel calls and the variant/NOS interface it uses must be read anew.
- [V] The memory card is a new driver pair (mmc.pdd, no hotswapdrv/medmmc): `ngage_mmc.c` may not
  fit as it is.
- 319 of the N-Gage's 388 secondary modules exist on the 6600; the 6600 adds 242 (MMF, ECom, Java
  MIDP 2, SyncML, camera, IrDA, multimode ETel, ...).

### Board hardware (the bootstrap)

- [V] The bootstrap's MMIO literals (ROM 0..0x8000) are nearly the N-Gage's set: EMIF 0x08000000..
  0x08000014, clock 0x0C120004/0x0C120020, 0x0C149000, interrupt controller and timers at 0x0C120xxx,
  CBus 0x0C150420, LCD interface 0x0C121200..08, 0x0C14F008/10, 0x0C150202..12, internal SRAM stubs at
  0x0C004748.., SDRAM 0x0A000000. **Same SoC (UPP)**. New: EMIF 0x08000018/0x0800001C (chip selects
  for the extra chips), 0x08018000 (N-Gage 0x08010000), 0x0C14B000.
- [V] Reset at 0: `b 0x320` (N-Gage `b 0x32C`).
- [V] SDRAM 16 MB at 0x0A000000 (bank table).
- [I] Screen 176x208 as the N-Gage; the 6600 panel is 65k colours (N-Gage 4096), so the screen chunk
  format and lcd.ldd (larger) may differ. Not read yet.
- Every firmware address in the board (about 60 in `ngage.c`, the NOS stub and `ngage_keys.c`/`ngage_lprf.c`:
  fatal routine, screen chunk pointer 0x800066EC, NOS DFC 0x8000C578, item tables, ...) is
  N-Gage-specific and must be found again for this kernel.

## Recommendation

**A variant of `ngage`, not a new board**: `-M nokia6600` in the same QEMU tree
(`src/qemu-ngage/`), sharing `ngage.c`'s peripherals (interrupt controller, timers, CBus/UEM, LCD
interface, trace port, catch-all, persistent flash), because the bootstrap and the kernel touch the same
SoC registers. The per-phone part becomes a config struct: flash chip count, chip map and ROM split,
the PPM placement, the MD5 entry, and the kernel-data and ROM addresses the board's workarounds use
(today `#define`s). The 6600 gets its own stub (`nos-standin/` per phone or a parameterised one), its own
`isi-replies` file and its own keymap. The N-Gage's boot check runs after every board change.

First boot will need, in order:
1. **Four flash chips** at 0, 0x02000000, 0x04000000, 0x06000000 (all must answer an ID), loaded per
   the flash file (ROM split 0 / 0x04000000 / 0x06000000, PPM at 0x02000000, the rest erased).
2. **H-ngage-md5** on the 0x8F18 entry (0x50009000..0x515FEFFF).
3. **Trace mask bit 30** set in the served image (a new board workaround) so the kernel's debug port
   speaks.
4. **NOS stand-in** at the new entry 0x52882024 (data 0x80002210, bss 0x80006D44), registering
   whatever variant interfaces the 7.0s `ecust`/`enosdriver`/`isa_if_driver` call (a research brief:
   the variant vtable slots and the ISA_IF channel calls, just enough to mock).
5. **ISI over ISA_IF**: the transport decoder (the N-Gage's `ngage_isi.c` reads NOSDRIVER sends) and a
   6600 reply table, filled from the UNANSWERED requests.
6. The kernel addresses for catches and the board's hooks (fatal routine, panics, screen chunk), then
   keypad (ekeyb is now a primary module) and the display.

## First boot (step 4)

Each stall, in order, with what fixed it. Kernel addresses ported from the N-Gage were found with
`eka1/tools/port_addr.py` (masked code match; unique full-score hits) and confirmed by the run.

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | Exception in ekeyb's `AddPowerHandler`: NULL power model | data abort 0x503A2638 (TDblQue at model+16), as N-Gage Q22 | stub builds the power model: DPowerModel ctor 0x5032AFF8 (export 451), Power::SetPowerModel 0x5032B33C (export 344) |
| 2 | ecust Fault 13 | ecust 0x5033B210 maps the screen (physical 0x0A097000, 0x12000) with DPlatChunkHw::New (export 65) and requires address 0x5A152000; hardware chunks are allocated consecutively from 0x5A000000 (list 0x800016FC, base [0x80001730]); ours ended at 0x5A10C000 | H-nhl10-hwchunk: the stub reserves 0x46000 (ctor export 454, create export 93) [I: the NOS's share] |
| 3 | ecust polls variant vtable +0x134 (word +0x94) for bit 1 | ecust 0x50339A38, as N-Gage Q24 (+0x90/+0x12C there) | stub sets 3 through setter +0x130 |
| 4 | ecust Fault 11: Get(3) = NULL | variant Register = vtable +0x104, Get +0x108, slots at +0x1C (0..7) | stub registers slots 3, 6, 7 (trap objects); slot 3 item 5 read is left pending as on the N-Gage |
| 5 | medint Fault "FLASHDD" 5 | medint switch 0x5036C32C: flash types 0, 2, 3, 9, 10, 15 (AMD), 11, 12 (ST); not Intel (type 4) | H-nhl10-flash-st: chips answer ST 0x20/0x88C0; medint's ST driver (0x5036B018..) uses Intel commands |
| 6 | Exception in the NOS (0x52888798) called by enosdriver.ldd | an unnamed NOS module (header 0x504128B0, data 0x80002210, exports at 0x50412968) is in enosdriver's DLL references; enosdriver calls exports 1, 2, 4 | stub: export 4 = NOS object ctor (vtable only, kept at 0x80002184), export 1 = send (object, TDesC8&, length), export 2 = plain call; the board puts jumps at the three NOS addresses |
| 7 | EStart panics "IsaApi 0" | isaapi `RIsaApi::CheckCrcMsgL` (0x50349974): sends `01 00 00 00 06 00 10 37 00 01 00 00` (resource 0, object 0x10, msg 1), waits 10 s for a reply whose data[1] = 0x82 ("checksum correct, cmt_ape_if ok") | reply table `isi-replies-nhl10.txt`; the stub's DFC hands frames up through enosdriver export 2 (0x5040F164) -> ISA_IF_PROTOCOL_BASE export 12 -> isa_if_driver 0x5040F62C (raw frame pointer, routed by receiver object) |
| 8 | (slow, not a stall) EStart formats C: | "auxstart 105" (item 12 = 105, as N-Gage Q44); RFormat loop 0x503DA53C; medint erases block by block, completion by kernel timer | none: about 60 s once; keep the flash (`--flash`) |

### The ISA_IF transport (NOS <-> Symbian), as far as seen

- Frame [V: isaapi's RIsaApi::Send logging and CheckCrcMsgL]: media, receiver device, sender
  device, resource ("Function"), length (16-bit little-endian, from the receiver object on),
  receiver object, sender object, transaction id, message id, data. The N-Gage's frames are the same
  without the media byte being 1.
- Down (Symbian -> NOS): isa_if_driver channel -> protocol object (+0x3C) -> enosdriver 0x5040F12C ->
  NOS export 1 (object, const TDesC8& frame, length).
- Up (NOS -> Symbian): enosdriver export 2 (frame pointer) -> ISA_IF_PROTOCOL_BASE export 12 ->
  isa_if_driver 0x5040F62C: queued, then a DFC (0x5040F690) dispatches by receiver object
  ((frame[1] & 3) << 8 | frame[6]); objects 0x37..0x58 are the RIsaApi channels (0x37 = EStart's
  init channel).
- Resource 0 / object 0x10 is the NOS-side ISA service: msg 1 = interface checksum (reply data[1]
  0x82 = match), msg 0 = indication subscription (data: count, then resource/message pairs) [I].
- EStart then reads the SIM language (resource 9, msg 0x1F, {2E 00}).

### Start-up after Starter (2026-11-18)

- [V] With the C: format done once (`--flash`), Starter's full list runs (37 processes: SplashScreen,
  Phone, SysAp, sae, Startup, ...); the splash shows (RGB565 screen chunk, `screen_chunk_ptr`
  0x80001780).
- [V] DosServer's Nokia.dsy opens ISA object 0x54 and subscribes (res 00 msg 00) to 17 21, 0A 1E,
  1C 12, 1C 15, 09 EF, 09 ED, 08 10, 15 C0, ... The board answers a subscription with the `ind`
  lines for that object (MTC_STATE_INFO_IND, SIM_IND, security 08 10: the N-Gage's values [I]).
- [V] Phone.app blocks in PhoneServer, which blocks in a synchronous DOS call (DSClient op
  0x4B3); Startup waits on DOS events too (DSClient). Both are Nokia.dsy waiting for ISI answers.
- Nokia.dsy replies (static analysis by a research agent; [V] = read in the code):
  - receive switch 0x50419580 (resource jump table 0x504195F4): 08 -> 0x5041A558, 09 -> 0x50419DAC,
    1C -> 0x504199A0.
  - SIM 09 C0 {00 00} -> C1, handler 0x50419F14 reads [11]: 01 = OK (then 09 2D and 09 2B {3A 00}),
    06/24/1C/4B = error states. Minimum frame 12 bytes.
  - 09 2D -> 2E (0x5041A170): [10] 3A, [11] 01, [12] picks a DOS value; 09 2B -> 2C (0x5041A1FC):
    [10] 3A, [11] 01, [12] bit 0, plus a 16-bit field (frame padded) [I].
  - Security 08 11 {00 00} (sent once after indication 08 10) -> 12, handler 0x5041A5A4 switch on
    [10] (table 0x5041A5DC): 05 = start-up OK, no code [I strong]; 01/06 code query, 02/2C PIN,
    03/2D PUK, 1A rejected SIM. [11] 12/13 set two flags.
  - After security 05 the DSY sends resource 0x53 msg 00 {04} (reply handler 0x5041A4D4, not
    traced) and 09 C0 again.
- The TSY (EtelServer) subscribes from object 0x45 and sends 5A 00; phonetsy's resource-5A code
  0x5146FF36 is an indication filter (ids 0x20-0x67), not that response [V].
- DosServer was then stuck on `53 00 {04 00}` (so Startup's `RDosServer::Connect` and Phone's DOS
  call waited): reply 53 01, handler 0x5041A4D4 reads [10] only (00 silent, 01/03/>05 ->
  DosSrv 0x5041C278(0), 02 retry, 04/05 other states) [V].
- Audio `0B 11` comes from two clients: Nokia.dsy's second class (subtype 05, parser 0x504159CC:
  [10] 05, [11] count, sub-block {02, len, 08, 00, u16}) and phonetsy (subtype 06, the N-Gage
  GetHandsfreeMode). The reply table gained `@XX` (match request byte 10); a subtype line wins over
  a generic one (`tests/ngage_isi_test.c` isi_reply_subtype).
- [V] rtc.dll (96% the N-Gage's code) needs the NOS's RTC hand-over (N-Gage Q49): DosServer's
  first alarm request faulted in rtc.dll 0x50340960 through `[rtc+0x120]` = NULL. The stub makes
  request 1 (vtable +0x20) on the RTC object from variant slot 1 ([variant+0x20]).
- [V] Startup reaches the first-boot time query (state 4) with the replies above.
- Keypad (research agent's static RE of the 6600 ekeyb, `ngage_keys.c` H-nhl10-keys): same ports
  and scan as the N-Gage; two sub-matrices driven together (rows 0-1 x lines 0-1: left soft, menu,
  right soft, 0xAA; rows 2-5 x lines 2-5: edit * # C / 1 2 3 0 / 4 5 6 call / 7 8 9 end), line 5 on
  GPIO 0x0C120C04 bit 19; a separate joystick on 0x0C15020A (bits 13 left, 1 up, 2 right, 10 down,
  11 select, active low; interrupt enables 0x0C150214/16 = CTSI status-2 bits [I]); a switch line
  0x0C15020C bit 9 (0xA8/0xA9). Scan-code lists 0x5036742C (group 1), 0x5036743C (group 2),
  0x50367488 (joystick).

- 2026-11-18 (owner): the joystick did nothing (number keys worked, so the menu looked stuck). The keypad
  model raised the joystick's CTSI status-2 bit only for lines enabled in 0x0C150214/16, and the values
  ekeyb leaves there do not match. Every edge now raises it; menu navigation works [V 7650 run].
