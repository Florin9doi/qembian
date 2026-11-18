# Sony Ericsson (UIQ) firmware: formats, what each phone holds, how to read it

State of 2026-11-13. Covers the two firmwares held: P1i (UIQ 3.0) and P910i (UIQ 2.1); since
2026-11-13 twelve more UIQ 3 sets come out of the Emma service tool archive — what is in it, and how
to get the files out, is [semc-emma.md](semc-emma.md). Every claim
is marked [V] (read from those files) or [I] (inferred: from names, strings or public specs). The
tool is `tools/fw/semc.py`; its docstring repeats the formats. Tests: `tests/semc_test.py`.
Phase 2 (how to run each phone) is in "Next" below; [uiq.md](uiq.md) has the platform overview.

## Reading them

```sh
python3 -I tools/fw/identify.py se_roms/P1/*.rar          # name the archives (a directory's archives are not expanded)
python3 -I tools/fw/store.py import se_roms/P1/*.rar      # -> roms/SE-P1i/R6E30/ (rom.toml, roles)
python3 -I tools/fw/semc.py identify roms/SE-P1i/R6E30    # what each file is
python3 -I tools/fw/semc.py extract roms/SE-P1i/R6E30 -o build/firmware/SE-P1i/R6E30
```

`extract` writes each layer's tree (`core/`, `rofs1/`, `rofx/`, `rofs-<CDA file>/`, `rolf-<file>/`), the
merged Z: drive `z/` (hard links, upper layers win), the FAT drives as sparse images plus their files
(`drive-<file>.img`, `drive-<file>/`, mtools), the modem's S-record files flattened (`modem/`), and
`manifest.json`: per input file its kind and header facts, per layer every file (path, address, size,
sha256), the identity and any problems.

Identity [V]: there is no type designator in these files. The store and the registry use
`SE-<product>` as the type code and the PDA (application CPU) software revision as the version:

| | P1i | P910i |
|---|---|---|
| store id | `SE-P1i/R6E30` | `SE-P910i/R5B01` |
| PDA software (`version.txt`) | `CXC162108 R6E30` (Z:\private\102030EE) | `CXC162041 R5B01 050509` (Z:\System\Data; baseline "layla Inc8_PA4") |
| customisation (`cda_version.txt`) | `CDA162024/70 R6A` (Trade_Russia) | `CDA162005/4 R5A02` (EAST-EMEA, on A:) |
| product | `P1i` (EMMA script, GDFS unit 0x06D4) | `P910i` (A:\System\Data\ProductInfo.rsc) |
| other versions | CCPU (modem) `CXC162037 R9H004`, `CXC162175 R3A01` [I: tags 3/4 unknown] | modem `R4E001` (from the file names) |

## P1i (UIQ 3.0, Symbian 9.1, EKA2)

Philips Nexperia PNX4008 (ARM926EJ-S 208 MHz, PowerVR MBX), 2 Gb NAND + 1 Gb DRAM [I: public specs].
One file per NAND image, no container around the Symbian images [V]:

| File | Kind | Contents | Drive |
|---|---|---|---|
| `BbyoneSW_CXC162058` | core | EKA2 XIP ROM, TRomHeader at 0: romBase `0xF8000000`, size `0x500000`, kernel data `0x64000000..0x66200000`, 221 files incl. `ekern.exe` (193608 bytes) [V] | Z: (bottom) |
| `Rofs1Ext_R6E30R` | rofs | ROFS1, iMaxImageSize `0x01AE0000`, 1401 files [V] | Z: (ESTART media 7) |
| `ACPU_SW_CXC162108_BASELINE_R6E30kJ_ROF` | rofx | ROFS extension on top of ROFS1 (base `0x01AE0000`), 2810 entries, many pointing into ROFS1 [V] | Z: |
| `ACPU_CDA162024-70_R6A_Trade_Russia_ROFS` | rofs | customer ROFS ("ROFS2" in ESTART.TXT, media 8), 880 entries; one has address `0xFFFFFFFF`, size 0 (hides a lower file: `playnow_reg.rsc`) [V, meaning I] | Z: (top) |
| `EPOC_Trade_Russia` | fat | FAT32 "EPOC", 160 MB file system, 24 MB of it in the file, 158 files (`install/language/NN/NN.zip` ...) [V] | C: (ESTART: "NAND - USER DATA") [I: size matches] |
| `EPOC2`, `EPOC3` | e32image | Symbian EXEs run during flashing: USB NAND flash loader "Suzi USB flash loader (XSR)" (names MiniBoot, CoreLoader, BootSection, ROFS image mask) and an ISP loader [V: strings] | none |
| `CCPU_*.babe`, `*FLASHLOADER*`, `*CS_LOADER` | babe | modem (CCPU, Ericsson DB2000) software and loaders: magic `BA BE`, version 4, 0xFF fill, payload at `0x224` [V]; payload looks signed/encrypted [I]; not decoded | modem |
| `Created ... by GDFtoEMMA 1.31` | emma | modem parameter (GDFS) writes: `w_free BLOCK UNIT INDEX bytes;` [V]: versions (2/0x079E), product (2/0x06D4), product number (2/0x06D3), SIM lock, A5 cipher (0/0x94) | modem |
| `GDFS_*` | gdfs | service scripts (reads/writes of GDFS units) [V] | modem |
| `Camara_Software` | other | 8051 code for the camera module [I: opcodes] | camera |

Z: as the file server builds it [V: core/sys/Data/ESTART.TXT]: `Z: 7 EROFS ROFS FS_COMPOSITE` (ROFS1),
`Z: 8 EROFS ROFS FS_COMPOSITE` (ROFS2), C: = NAND user data (ELOCAL FAT), D: = Memory Stick, E: = IRAM.
The ROFS layers replace 11 core files (`c32exe.exe`, `btmanserver.exe`, `CNTSrv.exe`, `Dial.dll` ...)
[V: same names in both]; the merged `z/` keeps the ROFS version (composite file system, `ecomp.fsy` is in
the core) [I]. Merged Z: 3887 files.

Kernel side [V: core file list]: `nandftl.fxt`, `mednand.pdd`, `medint.pdd`, `medms.pdd`, `lcd_ctrl.ldd`,
`clcdc.ldd`, `GPIODRIVER.LDD`, `HALKE.LDD`, `dspbootldd`/`dspcontrolldd`, `bridgedrv.ldd`, `ExtSoundLdd/Pdd`,
`camdrv`/`DCam0`/`DCam1`, `JPEGEnc/Dec`, `MPEG4Enc/Dec`, `YUV2RGB.LDD`, `videomgrdrv.ldd`, `radiodrv`,
`btdrv.ldd`, `eusbc.ldd`, `wificard.ldd` + `WifiPhilipsSa2443Spi.pdd`, `crypto.ldd`, `services.ldd`,
`isp_loader.ldd`, `euart.pdd`/`ecomm.ldd`, crash tooling `exmon*.dll`, `crashread.exe`. User side to modem:
`ccpuproxysrv.exe` [V]. PowerVR user libraries `PVR2D.dll`, `libgles_cm.dll` in ROFS1 [V].

## P910i (UIQ 2.1, Symbian 7.0, EKA1)

Philips Nexperia PNX4000, ARM9 156 MHz [I: public specs]. Each `.bin` = a small header, the payload,
then zero padding so that size + 0x20 is a 4 KB multiple [V, all six files; purpose unknown].

| File | Kind | Header | Contents | Drive |
|---|---|---|---|---|
| `P910i_R5B01_EU-EMEA-APAC_Symbian.bin` | semc-eka1 | `[u32 0x90000000][u32 0x01A9A000]` = flash address, length [V] | EKA1 XIP ROM: rom_base `0x50000000`, size `0x1B00000`, kernel data `0x80000000..0x80500000`, one root (hardware variant `0x05050505`), 1572 entries, 1468 files, all inside the payload; `System/Libs/EKern.exe` 167772 bytes [V] | Z: |
| `EAST-EMEA_Rolf_File.bin` | semc-rolf | `[u32 0x91D00000][u32 0x000F4DEC]` [V] | ROLF image: ROFS header layout, entries without the UID block, 4-byte aligned, iStructSize without the padding [V]; 29 files: `cda_version.txt`, `ProductInfo.ini/.rsc`, `SystemLogo.mbm`, themes, start-up/shutdown videos, default sounds, bookmarks | **A:**: EStart mounts `EAROLF.fsy` ("Rolf") on `ROLFDRV.PDD` and reads `A:\System\Data\productinfo.ini` [V: EStart strings] |
| `EAST-EMEA_HU_RU_TR_PL_CS.bin` | semc-fat | `[u32 0][u8 0xB0][u32 0x008C8800]` [V; 0xB0: I, an image type] | FAT16 "EPOC", 1 FAT, 64 MB file system, 9 MB of it in the file; every used cluster is inside it [V]; 1388 files (`documents/...`, language data) | C: [I] |
| `*_avr.bin` (3) | srec | `S003`, then binary S-records `S` + type digit + count + address (2/3 bytes BE for S1/S2) + data + checksum; `S8` with count 0 ends [V] | GSM modem software `0x0..0x195696` (OSE process names such as `HW_Equalizer_Done_Process`) and its GDFS backup `0x300000..0x3F0000` [V]; flattened into `modem/` | modem |
| `neosphere910.SIS` | sis | | EKA1 install package (Russian input) | |

Kernel side [V: ROM list]: `EKern.exe`, `EUser.dll`, `EFile.exe`, `EStart.exe`, `PlpVariant.dll`,
`rolfdrv.pdd`, `ramdrv.pdd`, `MEDMS.PDD`, `DocSupportDrv.ldd`, `TouchScreenCtrl.ldd`, `Dhal.ldd`,
`videodriver.ldd`, `Euart1.pdd`/`EComm.ldd`, `ESound.ldd`/`Esdrv.pdd`/`pcmdrv.pdd`/`EMixinSound.ldd`,
`ampctrlldd`, `pwmdrv`, `pwrnotif`, `BTCTRLDRV`, `cspdrv`, `eesp`, `elh4drv`, `edldrv.pdd`. The ROM's
interrupt names (`IrqHsTimer0..2`, `IrqUart1..6`, `IrqDispFifo`, `IrqCamSync`, `IrqTouchscreen`,
`IrqKeypad`, `IrqMemoryStick`, `IrqPcmData0` ...) are in the kernel image [V] and name the board's devices.
EStart also starts `startosesvr.exe` and waits for a "StartSignal" from the GSM side ("take dump from
GSM plat") [V: strings]: the first modem message a fake peer must send [I].

## Phase 2: how each phone would run (2026-11-13, analysis only, nothing booted)

### P1i: our EKA2 kernel, the N73's set-up, SE stand-ins

- **Kernel ABI: fits as is** [V]. `tools/fw/probe.py --device build/firmware/SE-P1i/R6E30 stubs --vs
  nokia-n73-rm132`: EUser.dll has 258 SWI sites (27 fast, 231 slow), all 9.1-shaped (`mov ip, lr; swi`),
  and the ordered sequence is identical to the N73's: the `sym91` exec map applies unchanged. Slow exec
  table 233 entries in both. `exec-sizes` lists 17 handlers whose detected copy-in sizes differ, but
  where both kernels show a size they agree (CodeSegCreate 0x17C, ProcessCreate 0x1A0); the rest are
  one-sided detections in handlers that copy no structure (ChunkBase, HandleAttributes) [I: heuristic
  noise from another compiler, check on first boot].
- **Memory model**: moving, ROM DLL data `0x40000000`, kernel limit `0x66200000`: the N73's layout
  (`syborgmv`) [V]. Fixed processes (data below the kernel limit): efile, SettingSvr,
  centralrepositorysrv, EventSvr, c32exe, AtServer, EComServer [V] (the N73 had five; romcompose handles them).
- **Stand-ins** (SE kernel-side files the user side opens by name [V: name found in the user-side binary]):

  | Priority | Driver (users) | Notes |
  |---|---|---|
  | boot | `HALKE.LDD` (EStart, Hal.dll), `GPIODRIVER.LDD` (EStart, Hal.dll, AtCommands, Servicemenus) | SE's HAL extension and GPIO channel |
  | boot | `lcd_ctrl.ldd` (Hal.dll, startuphandler.aps, lcdhandler.aps, bco.exe), `clcdc.ldd` (lcdhandlerclient, ScreenCP) | display control; the frame buffer itself is ours |
  | boot | `dspbootldd.ldd` (startuphandler.aps, M3SAudioDevMgr) | DSP boot at start-up |
  | modem | `vtcaifdrv.ldd` (semc.tsy), `bridgedrv.ldd` (caifmux.csy) | CAIF link to the CCPU |
  | later | `ExtSoundLdd/Pdd`, `dspcontrolldd` (audio), `texstream.ldd` + PVR (libgles_cm), JPEG/MPEG4/YUV2RGB, camdrv, radiodrv, btdrv, wificard, threadpriority | features |
  | ours | `ecomm`/`euart`, `eusbc`, `elocd`, `minkda`; touch `exyin.dll` (the 5800's `exyin` stand-in), keys `keyboardscan.dll`/`jogdial.dll` (our keyboard + a keymap) | exist |

- **Modem: a new protocol** [V names, I semantics]. `semc.tsy` (550 KB) -> `CAIFMUX` (`caifmux.csy`,
  `AT*SECAIF=` to open the mux) -> channel `CAIF_OPA`: **OPA**, an object RPC (`OpaClient.dll` 415 KB in
  the core, about 150 interfaces: `OPA::ICallManager`, `ISimManager`, `ISmsManager`, `ICopsManager`,
  `INetworkNitz`, `IAudioDevice`, `IClockManager`, `IRadioControl`, ...). `ccpuproxysrv.exe` and
  `LCServer.exe` use the CCPU proxy too. A fake CCPU must answer OPA calls over CAIF: the BB5 ISI peer's
  job again, with a new family (not `bb5-s60`). The GDFtoEMMA scripts give real parameter values.
- **Start-up**: SysStart, eight start-up modes (`private/10205C44/SSCForStartupMode1024..1031.RSC`); every
  mode starts the same core set (ewsrv, fbserv, eiksrvs, LinShell, LCServer, AtServer ...) [V]. Whether
  the UI comes up before the CCPU answers: [I], the first boot tells.

### P910i: its own EKA1 kernel on a new board

2026-11-18: done as `-M p910i`: boots to the UIQ set up wizard and the start-up Power menu;
[devices/se-p910i/bring-up.md](devices/se-p910i/bring-up.md).

- Kernel side from the ROM header lists [V]: EKern, EUser, EPower, epbusz, Hal, EFile, EStart, EComm,
  `udiskonchip.dll` (M-Systems DiskOnChip flash disk), `rolfdrv.pdd` + `earolf.fsy` (A:), `DocSupportDrv`,
  `ramdrv`, `ECust`, `Exyin` + `TouchScreenCtrl.ldd`, `Dhal.ldd`, `videodriver.ldd` + `_ext`, `Euart1.pdd`,
  `scdv`, `ESound`/`Esdrv`/`pcmdrv`/`EMixinSound`, `critajogdial`, `ekeypad`, `ampctrl`, `pwmdrv`,
  `pwrnotif`, `BTCTRLDRV`, `MEDMS.PDD`, `prodriver`; GSM link `cspdrv.ldd`, `i2ccontroller`, `eesp.ldd`,
  `elh4drv.ldd`, `edldrv.pdd`, `eose.dll`.
- Like the N-Gage, the kernel is the phone's own (rom_base `0x50000000`, kernel data `0x80000000` in both),
  so the work is a QEMU board for the Philips PNX4000: interrupt controller, timers, UARTs, LCD, keypad +
  jog dial, resistive touch (ADC), DiskOnChip, Memory Stick, RTC, PCM, PWM, I2C, plus the GSM link. No
  QEMU model and no public datasheet for the PNX4000 [I]; its peripheral map could not be read from
  literals (Thumb code, virtual addresses) [V: tried]. For scale: the N-Gage board is ~3700 lines and took
  56 logged questions over three days.
- Modem: `erigsm.tsy` -> `ecmux.csy` (`MUXCOMM::0`) -> OSE signals over a link driver (`eose.dll`,
  `startosesvr.exe`, `elh4drv`) [I]; EStart waits for a "StartSignal" from the GSM side [V: string].
  Also a new binary protocol.

### Verdict: start with the P1i

Both phones need a fake modem speaking a new binary protocol (OPA over CAIF; OSE signals over a mux), so
that does not separate them. What does:
- **P1i** reuses a whole working stack: our kernel, the `syborgmv` baseport and the `sym91` personality,
  whose exec map fits the P1i's EUser exactly [V]; ROM composition, ROFS media, sydb, the exyin touch
  stand-in. New work: about six boot-path LDD stand-ins, the CAIF/OPA fake CCPU, a profile.
- **P910i** needs a complete new SoC board (a dozen undocumented devices) before its kernel gets anywhere,
  and the modem on top. It is the N-Gage effort again, without the N-Gage's prior art.

Order for the P1i: profile `models/se-p1i/` (moving model, 9.1, new family `semc-uiq3`) -> ROM composition
from the SE layers (inventory.py reads FIASCO only; feed it semc.py's layers) -> boot to EStart with the
HAL/GPIO stand-ins -> display (`lcd_ctrl`, `clcdc`) -> CAIF link + the smallest OPA set start-up needs.
Not decoded and not needed for either: the BABE payloads, the 8051 camera code.
