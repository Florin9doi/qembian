# Sony Ericsson P910i bring-up (UIQ 2.1, Symbian 7.0, EKA1, Philips PNX4000)

Firmware `CXC162041 R5B01 050509` (baseline "layla Inc8_PA4"), customisation `CDA162005/4 R5A02`
(EAST-EMEA). The phone's own ROM and kernel on a new QEMU board in the EKA1 tree (`src/qemu-ngage/`),
like the N-Gage, 6600, 7650 and 9500.

Marks: **[V]** verified in the firmware bytes or code, **[I]** inferred.

## State (2026-11-18)

The phone's own bootstrap and EKA1 kernel run on `-M p910i` (`src/qemu-ngage/hw/arm/p910i*.c`, a PNX4000 board
modelled from the phone's code: interrupt controllers, HsTimer, UARTs, LCD FIFO + panel chip + video RAM
console, PL080 DMA, I2C, touch and keypad panels, GPIO pins for jog / side key / flip). C: is on a
DiskOnChip API stand-in (H-p910-doc), the GSM side is an OSE peer with a reply table (H-p910-gsm). First boot:
start-up logo -> Language dialog -> set up wizard (calibration, flip keyboard, location, time) -> the start-up
Power menu; C: is kept (`flash=`), so later boots go straight to the Power menu. "Phone on" brings up 48
processes (Phone, Applications, Appicker, SYSTEMAMS, ...) and, with the SIM-related OSE replies, **the UIQ home
screen** (the Applications launcher, build/evidence/p910i/home-screen.png). Battery: full, from the GSM side's periodic report (stall 18). Open: the Memory Stick (D:,
"Disk corrupt" at each boot), sound, network, a few OSE requests (13E0, 0370, 0533/1E).
Method and lessons: [how-it-was-done.md](how-it-was-done.md). Hacks: docs/hacks.md "Sony Ericsson P910i". Tests: `python3 -m unittest tests/p910i_board_test.py`,
`tests/p910i_gsm_peer_test.c` (build line in its header).

```sh
python3 eka1/tools/ngage_phone.py --model p910i [--phone NAME] [--fresh]       # the window (mouse = pen)
python3 eka1/tools/ngage_sess.py -s NAME start --model p910i [--flash FILE] --catch "dabt,fatal,panic"
```

Debugging aids that worked here: `--debug unimp,exec,cpu,nochain --dfilter VA+4` (CPU state at every entry
to one block, at full speed: found stall 14's Leave), `break 'VA=LABEL?EXPR'` (conditional, armed at run
time), the peer's `p910i-gsm:` log (every OSE frame, UNANSWERED marked), and the swbp recipe for a new OSE
request (docs/research/p910i-gsm-link.md s. 6.6: the stub allocates with r1 = the request number; the
awaited signal list is the literal before `bl 0x508a6db4`; the call after it decodes the reply).

## Files (git-ignored)

| What | Where |
|---|---|
| flash files | `roms/SE-P910i/R5B01/` (`rom.toml`) |
| extraction (`tools/fw/semc.py extract`) | `build/firmware/SE-P910i/R5B01/` (`z/` = ROM + ROLF merged, `drive-*.img` = C:, `modem/`) |
| core ROM, file list with addresses | `eka1/p910i/core/ROM.bin` (VA = 0x50000000 + offset), `files.json`, `files.tsv`, `output/rom/manifest.json` (for the ngage tools) |

The ROM image is the flash file's payload after its 8-byte header (`[0x90000000][0x01A9A000]`). Export
names for the stack tools (ported by ordinal from the N-Gage, [I]):
`python3 eka1/tools/port_exports.py eka1/p910i/core/ROM.bin eka1/p910i/core/files.json eka1/p910i/core/analysis/user-exports.tsv`.

## Step 1: inventory

### ROM header [V]

rom_base 0x50000000, rom_size 0x1B00000, root directory 0x50003620, kernel data 0x80000000..0x80500000,
one root (hardware variant 0x05050505). Reset branch at 0 -> bootstrap at 0x50000220. Primary file list
(+0xA0) starts with EKern.exe (0x50012F9C, 167772 bytes), secondary (+0xA4) with EFile.exe.
Variant (+0xCC): **ECust.dll** (0x500B2A10). Kernel extensions (+0xD0, chained through each image
header's iNextExtension, word 23), in load order:

EPower.dll -> epbusz.dll -> Exyin.dll -> TouchScreenCtrl.ldd -> videodriver_ext.dll -> critajogdial.dll
-> ekeypad.dll -> ampctrl.dll -> yuvtransfer.dll -> i2ccontroller.dll

Other kernel-side modules [V: ROM list]: udiskonchip.dll (M-Systems DiskOnChip), rolfdrv.pdd + earolf.fsy
(A:), DocSupportDrv.ldd, ramdrv.pdd, Dhal.ldd, videodriver.ldd, Euart1.pdd + EComm.ldd, scdv.dll,
ESound.ldd / Esdrv.pdd / pcmdrv.pdd / EMixinSound.ldd, ampctrlldd.ldd, pwmdrv.ldd, pwrnotif.ldd,
BTCTRLDRV.ldd, MEDMS.PDD (Memory Stick), prodriver.dll, TRKKERNELSTUB.DLL; GSM link cspdrv.ldd, eesp.ldd,
elh4drv.ldd, edldrv.pdd, eose.dll.

### Bootstrap memory table [V layout, I meanings]

At 0x50000314: `[count][id]` then `count` entries `[phys, size, attr, virt]` (id 1, 20 entries). Physical
pages it maps: 0xD0000000 (0x41000 and 32 MB), 0x50000000, 0x58000000, 0x70000000, 0x78000000,
0x80000000, 0x10000000, 0x18000000, 0x18200000, 0x20000000, 0x22000000, 0x28000000, 0x30000000,
0xB0000000 (32 MB -> 0x5E000000, 8 KB -> 0x5C000000), 0x90000000 (128 KB -> 0x5CF00000; 32 MB = the
NOR flash with this ROM [I: the flash file's address]), 0x68000000 (64 KB -> 0x5CF40000). The CPU id
check accepts an ARM926 (part 0x926) and one other core. The CP15 control word sets V: high vectors.
The SoC map from the bootstrap and the variant: [docs/research/p910i-soc-map.md](../../research/p910i-soc-map.md).

### Kernel words (ported, for `eka1/tools/ngage_sess.py --model p910i`)

| What | Address | How |
|---|---|---|
| Kern::Fault (r0 category descriptor, r1 reason) | 0x50026468 (EKern export 119) | the 9500's body, word for word [V] |
| current thread | 0x80001E94 | 242 ported users of the 9500's 0x800018C0 agree |
| object containers | 0x800000D0 | 73 ported users of the 9500's 0x80000430 agree |
| EUser RThread::Panic | 0x5005BEC0 | ordinal 812, as in the N-Gage's EUser (ordinals 621/812 0x58 apart in both) [V]; 0x5005B9A4 = RProcess::Panic (813) |
| User::Panic / User::Exit / RThread::Kill / RThread::Terminate | 0x50058570 / 0x500585A8 / 0x5005BE68 / 0x5005BE94 | ordinals 809 / 397 / 621 / 1132 |
| exception vectors | 0xFFFF0000 | control word 0x627A / 0x5E27A (V set) [V] |

### Start-up (EStart and after) [V: strings]

EStart loads BTCTRLDRV and pwrnotif, mounts RAMDRV.PDD, MEDMS.PDD (Memory Stick), `EAROLF` "Rolf" on
ROLFDRV.PDD (A:, reads `A:\System\Data\productinfo.ini`), loads docsupportdrv.ldd, starts
`Z:\System\Programs\startosesvr.exe` and waits on `OSE_SEM` ("EStart didn't recive StartSignal take dump
from GSM plat"), runs POPCORN.EXE and the "LinneaStartUpSvr" with the `Z:\System\Data\Linnea\*.lsu`
plug-ins (batpwr, gsmbuttoneventhandler, startuphandler; `.asp`/`.scy` handlers for the accessory, LCD,
profiles, phone, comms db). Failure text: "EStart Failed to start StartUpHandler, re-flash PDA, format
drive". UIQ side: QStart.exe, QEikStartPhase2, QPhoneStart, LinShell, splash, Sysagx.

Telephony: `erigsm.tsy` (+ erigsmapi.dll) -> `ecmux.csy` ("Multiplexer CSY", port `MUXCOMM::0`) ->
the OSE link (eose.dll, startosesvr.exe, elh4drv.ldd, cspdrv.ldd) [V names, I roles].

### Compared with the 9500 / 6600

| | P910i | 9500 | 6600 |
|---|---|---|---|
| OS | 7.0 (UIQ 2.1) | 7.0s (S80) | 7.0s (S60 2nd) |
| EKern | 167772 B at 0x50012F9C | 176600 B at 0x5000DF9C | |
| kernel data | 0x80000000..0x80500000 | ..0x80400000 | |
| ROM | XIP in NOR at 0x90000000 [I] | copied from DiskOnChip into SDRAM | XIP NOR |
| SoC | PNX4000 (no QEMU model) | OMAP1510 (QEMU omap1) | DCT4 UPP (ngage board) |
| C: | DiskOnChip (udiskonchip.dll) FAT16 64 MB | DiskOnChip (TrueFFS) | NOR |
| modem | OSE GSM platform over a UART (new protocol) | XBUS + ISI | NOS + ISI |

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | bootstrap polls 0x58000700 bit 4 (0x900004C8), then 0x50000000 bit 2 (0x900007A4) | catch-all log | ready bits: 0x58000700 in the catch-all table, 0x50000000 in the sys page (PLL locked) |
| 2 | first SVC enters the vector in Thumb state, undefined-instruction loop at 0xFFFF0004 | `-d int`: SVC then Undefined at once, CPSR T set; the bootstrap ORs 0x40001000 into SCTLR (0x50000878): bit 30 is ARM920T's nF clock bit, reserved on the ARM926, but QEMU took it as SCTLR.TE (Thumb exception entry) on every core [V] | QEMU patch 0018 (TE only from ARMv6T2 on) |
| 3 | kernel waits in EKern 0x50014C78 for +0x10 bit 10 of the phys 0x50000700 block to rise | it synchronises each write there to a slow clock (low, then high) [V] | bit 10 = a 32768 Hz square wave [I]; interrupt controllers, HsTimer, debug UART modelled (`p910i_soc.c`) |
| 4 | 2 processes (EKern, EFile); EStart starts and panics ESTART 2 at 0.8 s | EStart (0x5009CA20) loads docsupportdrv.ldd, then mounts "Fat" on C: (0x5009C8A4): RFs::MountFileSystem returns -2, so panic 2 [V by catches]. C: has no media: udiskonchip.dll (the media PDD with TrueFFS inside) probes the DiskOnChip at 0xB0001000 (control 0x1006/0x1076 = 1C/E3, 1D/E2, chip id 0x1000, 0x1FFE) and gets zeros | H-p910-doc: udiskonchip's TrueFFS dispatcher (0x500A27C4) -> a stand-in over a board block device with the FAT image (docs/research/p910i-diskonchip.md) |
| 5 | the stand-in's first call aborts (data abort writing its block device) | udiskonchip runs in EFile's thread in **user mode** (CPSR 0x10 at the dispatcher); 0x5C000000 (the window's table mapping) and 0x58000F00 (kernel peripherals) are kernel-only; TrueFFS itself writes the window through 0x5E000000 (`P910_NO_DOC_STANDIN=1`, catch at 0x500A33B0: r0 = 0x5E000000) [V] | block device at window + 0x1800, used through 0x5E001800 (regression doc_blk_user_mapping). C: reads, EStart runs, startosesvr starts |
| 6 | startosesvr blocked in elh4drv's DoCreate; the CPU spends its time in elh4drv's ISR (IRQ 7) with IIR reading 0xC1 | the interrupt controller model latched every rising edge until a write to +04; the kernel acknowledges only HsTimer0/1 there (edge-select register +0x10 = 6) [V by the ISR and the IC registers] | only lines set in +0x10 latch, the others are levels (regression ic_level_irq_latched) |
| 7 | 4 processes (EKern, EFile, EStart, startosesvr); the link syncs ("L1" -> "LA"), OSE_SEM is signalled, EStart sits in its active scheduler (0x5009D5B8); the phone sends nothing more on the link | EStart's scheduler waits for OSE signal 0xC384 through ereq.dll (observer slot 48 -> StartUp 0x5009DF94); the GSM side sends it unasked; the 24 s watchdog is not armed (nothing in 45 s) [V static + run] | peer sends `00 03 00 00 84 C3 00 00` at +1 s and +3 s after "LA": StartUp runs at 1.89 s, 7 processes (appsvr, Sysagx, eventsvr) |
| 8 | appsvr (LinneaStartUpSvr) in startuphandler.lsu blocked in a synchronous swbp.dll (OSE client) call; EStart waits on it via appcli; the link looked silent | the peer's log buffer was flushed only on an LSR read: the phone had sent `62 02 00 E3 98 43` = OSE request 0x4398 from pid E3 to GSM process 62; swbp's stub 0x508B0C10 waits for 0x4F4E {value 0..4, status} [V static] | log flushed 5 ms after the last byte (regression gsm_log_not_flushed); the peer became a table (`p910i_gsm_peer.c`, `src/qemu-ngage/p910i-ose-replies.txt`, frame `dst len16 src signo16 data`), `reply 4398 4F4E 00 00` |
| 9 | Kern::Fault at 2.1 s: kernel-side data abort in Esdrv.pdd 0x500CE00C for POPCORN.EXE (start-up animation/sound): [[obj+12]+0xDC] is NULL | catch dabt / fatal [V]; the sound hardware object (Esdrv 0x800038A8) gets its state objects' owners only in the I2C completion callback (0x500D0558); the codec init's ten I2C writes (device 0x34) were never completed: no I2C controller (docs/research/p910i-sound.md) [V static + board log] | I2C controller at phys 0x58000800, IRQ 17, every transfer completes with ACK (`p910i_i2c.c`, H-p910-i2c): **13 processes** (EwSrv, FbServ, QStart, splash, EikSrvs, settingsvr, EDbsrv ...), **the start-up logo on the screen** (build/evidence/p910i/startup-logo.png) |
| 10 | start-up stops at the logo, 13 processes; OSE requests unanswered: 0x0A3C (pid E3 -> 30, `00 4B`), 0x42CC (E3 -> 61), 0x4FB0 (E2 -> 3A), 0x13A0 (E1 -> 3A) | EikSrvs' main thread waits in RWindowGroup::SetName (Wserv), QStart waits on EikSrvs; Wserv idle; the CPU always in EFile's DOCBGTH (absolute-background priority): ELocal's DiskOnChip background thread calls the media driver's defragment (TrueFFS 0x32F) in a loop and sleeps on its semaphore only when it returns -4 = TrueFFS status 63 (0x500AFB84); the stand-in answered 0 [V by stacks + exec trace + code] | 0x32F answers 63 (regression docbgth_busy_loop): **27 processes** (Flipkeypad, LinShell, C32exe, shutdownsrvs ...) and **the UIQ first-start "Language" dialog** (UK English, Türkçe, Русский, Magyar; build/evidence/p910i/language-dialog.png). Replies for 0x42CC/0x4FB0/0x13A0 added from docs/research/p910i-gsm-link.md s. 6.6 (0x0A3C is send-only) |
| 11 | Language dialog needs input | docs/research/p910i-input.md: touch and the flip keypad are two resistive panels (ADC blocks phys 0x50000800 / 0x50000900, IRQ 23 / 26), jog / side key / flip are GPIO pins (sys +0x100 bits 1/0, 2, 18) whose interrupt polarity the drivers flip (IC +0x0C) [V mostly] | `p910i_input.c` + pin lines in the IC; taps land on the right rows (default calibration inverted) |
| 12 | after "Yes": a grey frame; OSE 0x137D (E3 -> 30, `74 93 90`) unanswered (0xC39F is send-only) | swbp stub 0x508A8644 waits for 0x137E, decoder 0x508A86B4 [V] | `reply 137D 137E 00 00 00` [I values]: the **P910i set up wizard** (build/evidence/p910i/setup-wizard.png) |
| 13 | set up wizard (calibration, flip keyboard, location, time, "tell me about", complete), then the start-up Power menu (Phone on / Flight mode / Power off); OSE 0x49D4, 0x49D5, 0x4FB3, then 0x4397, 0x413F unanswered | swbp stubs (docs: p910i-ose-replies.txt comments) [V] | replies added; C: kept across boots (`flash=`): a second boot goes straight to the Power menu |
| 14 | after "Phone on" (Done): Tele.app (thread Phone) panics "TeleApp" -1 ~2 s later, every run; Phone is critical: Kern::Fault, restart | the start-up engine's step 2 (0x50998010, docs/research/p910i-teleapp.md) leaves -1; found with a QEMU CPU-state trace of User::Leave's first block (`--debug unimp,exec,cpu,nochain --dfilter 0x5004b184+4`: every Leave's r0/sp/lr at full speed): lr 0x5030F8F7 = Gsmadv.dll `RAdvGsmPhone::RetrieveSimEmergencyNumbersLC`, ETel IPC 0x858 returned 0 numbers -> Leave(KErrNotFound) [V]. The TSY (erigsm.tsy -> ecmux.csy -> eesp/edldrv) has not touched any UART: telephony is not up. (Runs that "did not panic" under breakpoints were runs where the Done tap never registered: no race.) | the TSY speaks OSE signals over the same link (ereq -> swbp), no AT [V: docs/research/p910i-tsy-link.md]: IPC 0x858 = OSE 0x413F -> 0x22C9 {status, count, 13 x (3 BCD + 1)}; my guessed all-zero reply meant count 0. Reply with 112 and 911 (regression teleapp_emergency_numbers_empty): no panic, **48 processes** (Applications, Appicker, SYSTEMAMS, alarmserver, worldserver, ...); a "Disk corrupt D:" notice (the Memory Stick, not modelled) |
| 15 | 48 processes, all idle; the screen keeps the start-up logo (splash) | QStart kills splash after its start-up list's semaphores; it waits on TELE_2_SEM, which Tele.app signals in its start-up engine's step 6; steps 3-6 start only when Tele's internal state manager (0x50A2EE04) reports "on" through callbacks 0x509A5928 / 0x509A59F4 [V: docs/research/p910i-splash.md]; logging catches show the callbacks never run [V by run]. OSE 0x0533 (E3 -> 49) and 0x0A3C / 0xC39F (send-only) are the only unanswered frames | the phone's own requests, decoded one by one (swbp stubs, `/SUB` table entries for the generic 0x0533): `0533/18 -> 1681`, `0533/73 -> 16A3`, `4331 -> 173D`, `4339 -> 173E` [V layouts, I values]: **the splash goes; the phone shows "No SIM card / Insert SIM card"** (build/evidence/p910i/no-sim.png) |
| 16 | "No SIM card": the SIM state from the GSM side reads as absent | `4339 -> 173E`'s byte is the SIM presence [I, strong: 0 = the "No SIM card" screen, 1 = the phone goes on to read the SIM with 0533/5E] | `173E 01`, `0533/5E -> 16B8` (OK + a 43-byte block, zeros [I]): **the UIQ home screen** (the Applications launcher: CommuniCorder, Pictures, Video, Music Player, Internet, Messages, Contacts, Phone, Calendar; shortcut and status bars; build/evidence/p910i/home-screen.png), under the "Disk corrupt D:" notice |
| 17 | the "Disk corrupt D:" notice does not take the pen or the Confirm pad on the home screen | the touch block reported the taps [V: its sample register]; the CPU was always in the SocketServer thread polling UART unit 1's LSR for "transmitter empty" (Euart1.pdd 0x500B84CC) on the catch-all [V by sampling]. Also new OSE requests (0533/0C, /52, /9A, 09AD, 12FA answered; 13E0, 0370, 0533/1E open; 4CF4 send-only) | units 0, 1, 3, 5 as 16550s (regression socketserver_uart_spin): Continue works, **the clean home screen** (build/evidence/p910i/home-screen.png) |
| 18 | the status bar shows the battery at 0 % (owner) | the level is not measured on the PDA side: the baseband reports it as OSE indication 0xC39C {percent, charger}; batpwr.lsu subscribes and raises event 0x10202 for the status bar (LSBLib, 11 steps); pwrnotif/HAL are not involved [V: docs/research/p910i-battery.md] | `every 10000 C39C 64 00` (a periodic report, new `every` table entries): full battery, not charging (regression battery_level_zero) |
