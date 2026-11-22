# Nokia 7710 (RM-12) bring-up

Firmware V 4.10.0 (`RM12mPR4.10.0.c1u` core, `.V10` / `.V12` variant packs, build "0538 RM-12", 2005-09).
Series 90 (the only S90 phone), Symbian 7.0s, EKA1; 640x320 touch screen in landscape, 5-way navigation
key, a few hardware keys, MMC. Goal: the phone's own ROM to its S90 home screen ("Desk") on `-M nokia7710`.

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## State (2026-11-18)

Boots to the **S90 Desk** in about 25 s to the first-boot touch calibration, then the Setup wizard ("Finish")
and the Desk; 51 processes, no crash. Touch (the host mouse is the pen) and keys work. Window:

```sh
python3 eka1/tools/ngage_phone.py --model nokia7710 [--phone NAME] [--fresh]
```

Hacks and gaps: [../../hacks.md](../../hacks.md) "Nokia 7710". The method and lessons:
[how-it-was-done.md](how-it-was-done.md).

## Files (git-ignored)

| What | Where |
|---|---|
| owner's archive | `Nokia_7710_RM-12.zip` |
| flash files | `eka1/rm12_rom/` (the .c1u, .V10, .V12, .300 and the 05193xx.ini product descriptors) |
| records and unpacked images | `eka1/rm12/images/<file>/` (`manifest.json`) |
| the core ROM, its file list, manifest, export names | `eka1/rm12/core/ROM.bin`, `files.json`, `output/rom/manifest.json`, `analysis/user-exports.tsv` |

```sh
python3 -I eka1/tools/ra2_extract.py eka1/rm12_rom/RM12mPR4.10.0.c1u eka1/rm12_rom/RM12mPR4.10.0.V10 \
    eka1/rm12_rom/RM12mPR4.10.0.V12 -o eka1/rm12/images
mkdir -p eka1/rm12/core && cp eka1/rm12/images/RM12mPR4.10.0.c1u/r06-*.img eka1/rm12/core/ROM.bin
python3 -I eka1/tools/rom_manifest.py eka1/rm12/core          # files.json + output/rom/manifest.json
python3 -I eka1/tools/port_exports.py eka1/rm12/core/ROM.bin eka1/rm12/core/files.json \
    eka1/rm12/core/analysis/user-exports.tsv
```

## Step 1: inventory

### Container [V]

The 9500's Phoenix A2 container (`eka1/tools/ra2_extract.py`). One difference: the sub-header's tag is a
fixed 20-byte field and the data starts at +56; the 9500's tags were 19 characters, so "data after the tag's
NUL" happened to be right there. The 7710's tags are shorter ("Tetra v7") and reading from the NUL gave
"invalid stored block lengths" (regression `test_short_tag_data_at_56` in `eka1/test_ra2_extract.py`).

| File | Record | Content |
|---|---|---|
| .c1u | r00-r02 | text "Disable check CRC o..." |
| .c1u | r04, r05 | "User Disk C:" descriptor; a FAT image (OEM "EPOC", 0x1E000 bytes) [I: the factory C:] |
| .c1u | r06 `V4.10.0 0538 RM-12` | deflate -> 0x1A39000: **the EKA1 core ROM**, loaded at 0x10600000 |
| .c1u | r07 `V4.10.0 0443 RM-12` | deflate -> 0x369000, a second EKA1 ROM (rom_size 0x400000, 136 files: SelfTestServer, TestServer, CameraTestServer, BTTestServer) [I: the factory test image, as the 9500's T record] |
| .c1u | r08 | ROFS image 0xAFC9F0 (stored) |
| .c1u | r09 `Tetra v7` | deflate -> 0x16A40 at 0x13B00000 [I: the loader, as the 9500's L records] |
| .c1u | r10 `StartUp Logo` | deflate -> 0x64044 |
| .V10 / .V12 | r02 `V4.10.0 ID10/ID12 RM-12` | deflate -> 0x1E9000 / 0x1E2000 at 0x12100000: an **extension ROM** (header: base 0x51B00000, right after the core; its own directory) [V header], language variant |
| .300 | - | the DCT4 modem MCU flash ("DCT4 MX/ORCA ALGORITHM", McuSwId in the .ini files); not an A2 container, not needed (the modem is modelled) |

The 9500 had a separate ROFx (language ROFS extension) in its .l18; the 7710 has none: its language
files live in the V extension ROM.

### Core ROM header [V]

rom_base 0x50000000, rom_size 0x1B00000, root directory 0x50003660, kernel data 0x80000000..0x80400000,
1657 files (21 .app in the core ROM: Shell (the Desk [I]), phoneApp, sysap, ControlPanel, Cntapp, Opera,
VisualRadio, MusicPlayer, ...; more in the ROFS). Primary list: EKern.exe (0x50014F9C, 181936 bytes);
secondary: EFile.exe. Variant (+0xCC): **ECust.dll** (0x500416B0). Kernel extensions (+0xD0, then each
image header's word 23 = the next extension's **directory entry**), in load order:

```
7710: epower -> bcs -> euwire -> ei2c -> eusbcc -> Videodriver -> epbusm -> DiskOnChip -> ekeyp -> exyin -> espi -> ISA_IF_DRIVER -> ISA_IF_DRIVER_2ND
9500: EPower -> Bcs -> EuWire -> Ei2c -> EUsbcc -> VideoDriver -> Espi -> DiskOnChip -> EpBusp -> Epbusm -> EKeyp -> EKeyb -> Isa_if_driver -> Isa_if_driver_2nd
```

New: **exyin.dll** (XY input: the digitiser; the P910i has an Exyin.dll too). Gone: EKeyb (the 9500's
I2C keyboard controller, there is no QWERTY), EpBusp. Same order otherwise.

### SoC: TI OMAP1510, as the 9500 [V]

The bootstrap's literals are the 9500's OMAP1510 set (MPU interrupt handler 0xFFFECB00, EMIFS 0xFFFECC00,
clocks 0xFFFECE00, DPLL1 0xFFFECF00, JTAG ID 0xFFFED400, UART2 0xFFFB0800, GPIO 0xFFFCE000) at nearly
the same offsets (0x50000870.. vs 0x500008A8..). Bootstrap memory table (0x500007C0, 9 entries
`[phys, size, attr, virt]`):

| phys | size | attr | 9500 |
|---|---|---|---|
| 0x00000000 | 64 MB | b2000041 (ROM) | same (CS0) |
| **0x0C000000** | **32 MB** | b2000041 (ROM) | 9500: 8 bytes, I/O [I: a NOR flash on CS3 on the 7710?] |
| 0x10000000 | 64 MB | a2000042 (RAM) | same: SDRAM 64 MB |
| 0x20000000 | 192 KB | b6000025 | same (internal SRAM) |
| 0x10200000 | 0x564020 | 9 | same |
| 0xFFF00000 | 1 MB | b1000023 (I/O) | same |
| 0xE1008000, 0xE1017000 | 2 KB each | I/O | same |
| 0x097FFFFE | 2 | I/O | same; the 9500 also had 0x08000000 (1 KB -> 0x58102000) |

### Kernel-side modules vs the 9500 [V: names; addresses differ]

Every kernel-side module of the 9500 is there except EKeyb.ldd, EUart3.pdd, ECodec.ldd, WLAN (WlanLdd/Pdd),
the cover display (CuiLcd.ldd/pdd), MedInt/MedAta. None is byte-identical (all relinked), several have the
same size (Ehwaomap.pdd 106304, Bal.dll 4024: the DSP bridge), others differ (DiskOnChip.dll 103272 vs 118392:
another TrueFFS build; VideoDriver.dll 16584 vs 25048; XbusDma.pdd 8260 vs 7780).

| Function | 7710 module | 9500 equivalent / note |
|---|---|---|
| storage | DiskOnChip.dll (0x500AC160), MedDoc.pdd, EComp.fsy, ERofs.fsy | same scheme (H-ra2-doc applies, new addresses) |
| display | Videodriver.dll (extension, 0x50098470) + videodriver.ldd | the OMAP LCD controller [I], 640x320 |
| digitiser | **exyin.dll** (0x500C78C0, 5056 bytes) | new |
| keys | ekeyp.dll (0x500C6E10, 2624 bytes) | the 9500's MPUIO keypad (patch 0017) |
| modem | XbusDma.pdd, XbusProtocol.ldd, Isa_if_driver(_2nd).ldd, IsaApi.dll | same (H-ra2-xbus-modem) |
| DSP / audio | Ehwaomap.pdd, Bal.dll, Aic23.ldd, Lm4855.ldd | same (H-ra2-dsp-boot, H-ra2-bal) |
| other | MedMmc.pdd (MMC), Bc02 (BT), CamLdd, EUsbc, Euart1/2 | |

### Kernel words (ported from the 9500, `eka1/tools/ngage_run.py` MODELS `nokia7710`) [V by code match]

| Word | 7710 | 9500 | How |
|---|---|---|---|
| Kern::Fault | 0x50028304 | 0x500212C4 | export 102 in both, 16/16 words (`port_addr.py`) |
| current thread | 0x800018BC | 0x800018C0 | literal voting, 46 users agree |
| object containers | 0x8000042C | 0x80000430 | 17 users agree |
| EUser RThread::Panic | 0x5006A844 | 0x500D9D48 | 12/12 words |

## Board (step 2-3)

`-M nokia7710` is the 9500's board (`src/qemu-ngage/hw/arm/nokia9500.c`) with a per-phone description
(`Ra2Phone`: images, partitions, the TrueFFS stand-in's addresses, relocation deltas of the DSP patches,
the devices around the SoC). The 7710 shares with the 9500: QEMU's OMAP310/1510 model and patches
0003-0017, the boot shortcut (H-ra2-loader: core at 0x10600000; the V extension ROM at 0x12100000), the
TrueFFS API stand-in (H-ra2-doc, assembled at the 7710's DiskOnChip.dll entry + 4 = 0x500AC1C8), the XBUS
modem on UART2 (H-ra2-xbus-modem) with its own reply table `src/qemu-ngage/isi-replies-rm12.txt` (seeded from
the 9500's), and the DSP-bridge patches (H-ra2-dsp-boot, H-ra2-bal): Ehwaomap.pdd, BridgeApi, Bal.dll,
Dasfgwc.dll and DeviceManagementServer are the 9500's binaries relocated (only relocated words differ), so
the 9500's patch addresses + one delta per module apply, each old word checked (tests in
`tests/nokia7710_board_test.py`). Partitions: 0 = C: (the c1u r05 FAT16 record, 174048 sectors), 1 = the
directory + ROFS (no ROFx), 2 = blank.

```sh
python3 eka1/tools/ngage_sess.py -s NAME --model nokia7710 start --catch fatal,pabt,dabt,undef,panic
```

Tools: `objstride=2` in the model (the 7710 kernel's object-container entries are `{DObject*, key}`, 8 bytes;
`objects` read garbage with the 9500's 4-byte stride).

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | EStart::Main panics IsaApi 1 ("checksum not correct") at 0.9 s | CheckCrc (IsaApi.dll, cmp at 0x50142F54) wants the `00 01 -> 02` reply's data[1] = 0x6B (the 9500: 0xA7, the 6600: 0x82) [V] | `reply 00 01 02 00 6b`; the kernel, EFile/TrueFFS, the XBUS link (sync, frames) all worked unchanged from the 9500 |
| 2 | 17 processes; Wserv prefetch abort at pc 0 (21.8 s) | Wserv's clicksoundplugin.dll: `RSharedDataClient::Assign(0x101F9691)` returned -12 (KErrPathNotFound), User::LeaveIfError, then a call through a NULL pointer [V by an exec trace on User::Leave 0x50058DA8]. CommonEngine's SharedData server writes `C:\System\Shareddata\`; partition 0 had been sized from the record (240 sectors) while the FAT16 boot sector says 174048, so writes past it failed | partition 0 = 174048 sectors (regression `test_rm12_c_drive_full_size`) |
| 3 | EikSrvs data abort in EKern 0x5002860C (23 s) | the exec reads the kernel's security object `[0x800003F8]` = NULL: Security::NewL(TDesC8) (EUser ord 736) on the super-page blob 0x40000904 failed, a path taken only when the start-up type `[0x8000102C]` is 3/4/5. The bootstrap (0x5000032C) sets reset reason 2 when ARM_SYSST (0xFFFECE18) bit 1 GLOB_SWRST is set; 2 -> start-up type 4 (warm reset, 0x5000126C). QEMU powered on with ARM_SYSST = 0x3A (POR, EXT_RST, ARM_MCRST and GLOB_SWRST) [V by watchpoint on the super page, physical 0x1056601C] | QEMU patch 0019: power-on ARM_SYSST = 0x20 (POR only): start-up type 0, `Security::NewL()`; the 9500 keeps type 1 (its bootstrap's reason comes out 1 either way [V by run]). **51 processes** (Desk, LockSrv, phoneApp, sysap, ...), the 640x320 display shows the first-boot touch calibration ("Tap the centre of target 1") |
| 4 | the first-boot touch calibration waits for taps ("Tap the centre of target 1") | exyin.dll drives an ADS7846-class controller on Microwire CS 0 (commands D1 X, 91 Y, B9 Z1, C9 Z2, 94 end), /PENIRQ on MPUIO 4 (edge flipped by the driver), self-test wants Z1 = 0, Z2 = 0xFF0 with the pen up; calibration words in the super page 0x40000994 (docs/research/rm12-digitiser.md) [V] | `nokia7710_input.c`: the controller (pen up: Z1 0 / Z2 0xFF0; down: Z1 = Z2 = 0x800), the host pointer as the pen, screen -> raw through the phone's current calibration words (`nokia7710_touch.h`, host test `tests/nokia7710_touch_test.c`). Keys: ekeyp's 5x6 MPUIO matrix (docs/research/rm12-keys.md; column 0 is real here). The four targets, then the Setup wizard ("Finish"), then **the S90 Desk** (build/evidence/7710/03-desk.png); the 5-way key moves the Desk focus |

## Stall 5 (owner's report, fixed 2026-11-19)

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 5 | the owner's phone, started from its kept C:, stays black: 72 processes, Desk running, the LCD controller configured (640x320) but LCD_EN off; a fresh phone showed its first logo only late | reproduced on the owner's own flash file; a trace of the kernel's ModifyRegister (0x5003D8AC) on 0xFFFEC000 showed the fresh boot ending with an enable (0x5009A17C) and the kept boot with an extra power-up then two disables (0x5009A860, 0x5009AE20). VideoDriver's power-up state machine (+0x15C) reads the panel controller over Microwire CS 3 (words `reg << 8 | value`; register 0x0A = status) and in state 4 (0x5009A834) wants 0x40 (state 3: 0x20), else state 8 and the LCD off [V by runs + code]. Nothing answered on CS 3 (reads 0). A first suspect, the 9500's once-per-address palette-loaded interrupt (patch 0013), was tried (a re-arm after a power cycle) and was not needed [V by run without it] | H-rm12-panel: a panel controller on CS 3 that logs the words and reads 0x40 for register 0x0A (`nokia7710_input.c`); regression `test_rm12_kept_c_panel_status`. The owner's phone boots to the Desk again; the NOKIA logo appears after ~3 s on both fresh and kept boots |

## Open

- **Fixed 2026-11-19 (stall 5 below):** the owner's phone did not show anything when started from its kept C:,
  and a fresh phone showed its first logo late.
- Battery / charger: the status bar shows power icons at top right; which modem indication drives them is not
  read yet (on the P910i it was the GSM side's battery indication; here probably an ISI PN_BATTERY/EM message).
- Unanswered modem requests at the Desk: `35 02`, `1C 10`, `17 08/0A` (obj 0x51, the DSY), `19 0A`, `09 1F`,
  `5A 00`, `1B 00 {41}` (IMEI), `09 0F`, `31 30`, `54 0D`, `DB 01/05`, `1C 2D`, subscriptions of 0x40, 0x4D,
  0x53, 0x64.
- Clock 12:00 am at boot (no RTC hand-over); no network, sound, MMC.
- Short taps (0.4 s) were sometimes judged inconsistent by the calibration app while the window was also
  screendumping 10/s; 0.8 s taps always worked (exyin needs two sample bursts, 10 ticks each).
