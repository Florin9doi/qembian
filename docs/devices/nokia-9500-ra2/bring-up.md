# Nokia 9500 Communicator (RA-2) bring-up

Firmware RA-002 v.06.27 (`RA2n6270.c01` core, `.l18` language/variant, `.u18` user data; 2004-07-06 build).
S80 2nd edition, Symbian 7.0s, EKA1. Two displays (cover and the inner 640x200) and the cover open/close:
the owner wants both modelled properly, as the E90 (docs/hacks.md H-e90-two-panels), not hacked.

Marks: **[V]** verified in the firmware, **[I]** inferred.

## State (2026-11-18)

Steps 1-2 done. Step 3 (board, `-M nokia9500`, `src/qemu-ngage/hw/arm/nokia9500.c` on QEMU's OMAP310/1510
model): the kernel initialises, EFile mounts the DiskOnChip partitions (H-ra2-doc), EStart runs, the XBUS modem link
syncs and passes IsaApi's checksum check (H-ra2-xbus-modem), C: mounts, EStart finishes and starts 14 processes
(EwSrv, FbServ, SharedDataServer, Sysagx, C32, Starter, DosServer). DeviceManagementServer dies early (H-ra2-bal
known gap). Stalls 20-22 fixed: 31 processes (SplashScreen, EikSrvs, PhoneServer, EComServer, ...); no display yet. Next: the UI and the displays.
Hand-over prompt: docs/prompts/nokia-9500-homescreen.md. 2026-11-18 evening: 57 processes, the inner display shows the start-up splash, the cover UI draws; open: stall 31.

## Files (git-ignored)

| What | Where |
|---|---|
| owner's archive | `RA-002 v.06.27 9500.rar` (the flash-file archive) |
| flash files | `eka1/ra2_rom/` |
| records and unpacked images | `eka1/ra2/images/<file>/` (`manifest.json`) |
| the core ROM, its file list | `eka1/ra2/core/ROM.bin`, `files.json` |

```sh
python3 -I eka1/tools/ra2_extract.py eka1/ra2_rom/RA2n6270.c01 eka1/ra2_rom/RA2n6270.l18 \
    eka1/ra2_rom/RA2n6270.u18 -o eka1/ra2/images     # tests: eka1/test_ra2_extract.py
```

## Step 1: the format [V]

Not the DCT4 records the N-Gage/6600/7650 tools read. A Phoenix A2 container (0xA2, u32 BE header length,
4 zero bytes) of 0x28 records: 16-byte header with the data length as a big-endian u32 at +8, then the data.
Data is raw (ROFS/ROFx images, a FAT image) or starts with a sub-header (magic 0x99123456): uncompressed
size, compression (1 = raw deflate), stored size, load address, a tag string. Full layout:
`eka1/tools/ra2_extract.py`.

| File | Record | Content |
|---|---|---|
| .c01 | C `001C24070606.27` | deflate -> 0x1122000: the EKA1 core ROM, loaded at 0x10600000 |
| .c01 | ROFS | ROFS image 0x12A0000 (stored) |
| .c01 | T `001T...` | deflate -> 0x77A000, a second EKA1 ROM (rom_size 0x800000, kernel data to 0x80300000), also at 0x10600000 [I: a test/service image] |
| .c01 | L `000L` / `002L` | 0x18E78 / 0x19A88 at 0x13B00000 [I: loaders] |
| .c01 | D, S, text "001T CRC Disabled" | small blocks |
| .l18 | ROFx | 0x278000: the language ROFS extension |
| .l18 | V `016V` / `001V` | deflate -> 0xFB000 / 0xE9000 at 0x11800000 [I: variant data] |
| .u18 | FAT (OEM "EPOC") | 0x34000: the factory user disk [I: C:] |

The core ROM: rom_base 0x50000000, rom_size 0x1200000, root 0x50003650, kernel data
0x80000000..0x80400000; the EKA1 tree walker reads 980 entries (934 files, 14 apps).

## Step 2: comparison

- **SoC: TI OMAP1510 family** [V]. The bootstrap uses the OMAP1510 map: MPU interrupt handler 0xFFFECB00,
  EMIFS/EMIFF 0xFFFECC00, clock control 0xFFFECE00, DPLL1 0xFFFECF00, JTAG ID 0xFFFED400, the 0xFFFB/0xFFFE
  peripheral pages. Not the DCT4 UPP of the N-Gage board. The load addresses 0x10600000 / 0x11800000 /
  0x13B00000 are OMAP1510 SDRAM (EMIFF at 0x10000000) [I]: the ROM is unpacked into RAM and runs there.
- **Storage: DiskOnChip** (MedDoc.pdd) [V module], with a composite file system (EComp.fsy, ERofs.fsy):
  the core ROM, the ROFS and the ROFx are read from NAND-like flash, not executed in place [I].
- **Modem on its own processor** [V modules]: XbusDma.pdd + XbusProtocol.ldd + Isa_if_driver(_2nd).ldd,
  isaapi's "SECTION_XBUS". The stand-in is a peer on the XBUS link (board side), not a stub over the
  NOS entry as on the N-Gage/6600/7650.
- **Displays**: Cuilcd.ldd / Cuilcdpdd.pdd [I: the cover display, "Cover UI"]; the inner 640x200 through the
  OMAP LCD controller [I]. To read before the board: which interface the cover panel hangs on, and the
  cover open/close switch (a GPIO / keypad line read by EKeyb or the variant).
- Other drivers: WLAN (WlanLdd/WlanPdd), Bluetooth (Bc02 on UART1), USB (EUsbc, UsbIsa), audio (Aic23,
  Lm4855, Ecodec), camera (CamLdd/CamPdd), MMC (MedMmc), IrDA, OMAP hardware acceleration (Ehwaomap).
- Kernel: EKern.exe at 0x5000DF9C, 0x2B1D8 bytes (7.0s, the 6600's generation: its exec table, variant and
  ISA_IF work carries over [I]).

## Recommendation

A **new board on QEMU's own OMAP1 code** (`hw/arm/omap1.c`, the OMAP310 used by `-M sx1` /
`hw/arm/omap_sx1.c`): it models the MPU interrupt controller, timers, clocks, UARTs, the LCD controller,
MMC, I2C and GPIO this ROM touches. Our part: the board (memory map from the bootstrap, the DiskOnChip
or a loader shortcut that puts the C/ROFS/ROFx images where the bootstrap expects them), the two panels
and the cover switch (as the E90), the XBUS modem peer (BB5 ISI answers: platform/isi/replies-bb5.txt).

First boot will need, in order: how the bootstrap reaches the core (DiskOnChip read or the L loaders at
0x13B00000), SDRAM size, the core at 0x10600000 mapped to 0x50000000, then the kernel's peripherals on
QEMU's OMAP1510 models.

## Board and first boot (step 3-4)

```sh
python3 eka1/tools/ngage_sess.py -s NAME start --model nokia9500      # ppm= eka1/ra2/images
python3 eka1/tools/ngage_run.py --model nokia9500 --secs 5
```

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | QEMU "hardware error: omap_findclk: mcbsp3.clkx not found" | omap1.c's pin-mux code switches six clocks the clock tree lacks | QEMU patch 0003 (the six pin-output clocks) |
| 2 | kernel waits for its first tick | it starts the OS timer (0xFFFB9000) with 16-bit writes; QEMU's model dropped other widths | patch 0004 (16-bit access to 32-bit registers merged; 32-bit access to 16-bit ones = low half) |
| 3 | 110000 interrupts a second | the kernel acknowledges with a byte RMW of the interrupt controller's CONTROL_REG and sets ILRs by byte; QEMU's controller took 32-bit accesses only, so the line never fell | patch 0004 (byte/halfword access widened) |
| 4 | kernel polls ARM_SYSST bit 6 | IDLE_DSP after releasing the DSP (0x5003F38C); no DSP is emulated | patch 0005 (bit 6 set) |
| 5 | EFile's start-up waits on the media | DiskOnChip.dll (M-Systems TrueFFS, 0x50062370, 0x1CE78 bytes, 5 exports) probes the DiskOnChip registers at CS0 + 0x1000.. (the boot block at CS0 0..0xFFF); MedDoc.pdd uses exports 1, 3, 4, 5 | H-ra2-doc: a TrueFFS API stand-in backed by a board block device (ROFS 0x12A0000, ROFx 0x278000, C: FAT16 163808 sectors of which the flash file holds the first 0x34000 bytes). Two traps on the way (regression tests in `eka1/test_ra2_doc_standin.py`): the DLL's entry point is the first word of its text and the kernel calls it on load; flSetEnvAll takes two arguments |
| 6 | Kern::Fault "Bind DiskOnChip" -14 (without the stand-in), then the kernel restarts: it jumps to the ROM's physical alias at 0x140CC (0x50014090, MMU off) into empty CS0 | the restart routine assumes the core at physical 0 [I]; harmless as long as no fault | the `fatal` catch is Kern::Fault 0x500212C4 |
| 7 | EStart::Main panics IsaApi 4 "Timer expired, Sync not received!" at 4.9 s | IsaApi (EStart's ISA start-up) waits for the modem's sync over XBUS (XbusDma.pdd / XbusProtocol.ldd; protocol: docs/research/ra2-xbus-modem.md) | H-ra2-xbus-modem: UART2 + modem peer on the board |
| 8 | the phone ignores the sync ACK | XbusProtocol accepts an ACK only while no byte is in flight (link +0x48, cleared in TxDone); TxDone comes from the UART's modem-status interrupt (CTS back up, ISR 0x500B4898), the 70 ms timer is only a fallback and the sync resend every 5 ms restarts it | CTS dips 100 us per byte |
| 9 | XbusProtocol Kern::Fault reason 13 (DATA resent 6 times) | an ACK is taken only in the transmitter's waiting-for-ACK state, entered in TxDone; ours came before | the modem answers 500 us after CTS returns |
| 10 | receive reset (0x500B4394, RxDone(0, 0)) | a frame right behind another; and LSR data-ready when the PDD arms the payload receive = line error 8 (0x500B4B88) | 2 ms gap after every header and payload |
| 11 | IsaApi panic 0 "crc message not got" | the modem's first DATA had sequence bit 0; the phone expects 1 ([rx+4] = 8) and drops it as a duplicate | first DATA with bit 1 |
| 12 | IsaApi panic 1 "checksum not correct" | CheckCrc (0x501018BC) wants data[1] = 0xA7 (the 6600: 0x82) | `isi-replies-ra2.txt` |
| 13 | EStart waits for a key (raw events, scan code 0xA4 / 0xA7) after drawing an image | EStartavalon's KErrCorrupt path (0x50099F24): C: did not mount; EFile reads its boot sector from partition 0, which was blank | partition 0 = C:, partition 2 blank |
| 14 | QEMU exits by itself | QEMU's OMAP1 clock code shuts the machine down when ARM_IDLECT1 is written with WKUP_MODE clear (the 9500's idle code does) | QEMU patch 0007 (logged once, ignored) |
| 15 | XbusProtocol Kern::Fault 13 again | a phone frame resent because our ACK was late was handed on twice; a modem DATA frame the phone missed was never resent | duplicates ACKed only; resend after 100 ms (5 times) |
| 16 | MTC: state query (res 15 msg 02) | as the other DCT4/BB5 phones | `isi-replies-ra2.txt` seeded from the 6600/7650 tables [I] |
| 17 | 9 processes (EwSrv, SharedDataServer, Sysagx, ...); display not initialised; res 19 0A, D1 04 and two subscriptions unanswered | Wserv waits on DeviceManagementServer, which waits in BridgeApi.dll / Ehwa.ldd: Ehwaomap.pdd spins until the (absent) DSP clears its boot word (0x507F60C8) | H-ra2-dsp-boot (docs/research/ra2-dsp-bridge.md) |
| 18 | (after the keyboard controller) 2 processes: EFile's start-up thread stalls | the controller sent the lid state (0xE0) at power-on; EKeyb starts with the lid closed and the early ECaseOpen stalls start-up | nothing sent at power-on (tests/nokia9500_board_test.py) |
| 19 | DeviceManagementServer waits in Bal.dll (0x50925FAC) from Dasfgwc.dll: a DSP node call | | H-ra2-bal; DMS then dies (data abort, known gap), the boot goes on |
| 20 | 14 processes (FbServ, C32, Starter, DosServer); Wserv idle; no display; lid open changes nothing | Starter blocked in SysUtil 0x5009BEE4 -> DosServer function 301 (GetSWStartupReason) -> Nokia.dsy 0x5016E3DC -> Custom.ldd request 2 (ISA object 0x69) -> modem request `23 04 {00 D4 00 ..}` (tid 0x9A), unanswered [V static, docs/research/ra2-dsy.md] | `reply 23 04 05 ..` in isi-replies-ra2.txt ([10] 00 = OK, [24..25] start-up reason LE = 100 normal; bytes 1-13 [I]): Starter goes on |
| 21 | PhoneTsy (C32exe::CTelServer) panics USER 21 | Mmgsmwcdma.dll's security-state handler 0x50C0AAF4 reads data[0] (state, 5 = OK) and data[7] of the `08 12` reply; the 6600's 2-byte reply was too short [V] | reply padded to 8 data bytes |
| 22 | XBUSProtocol Kern::Fault 0x1000D (timing: some runs only); the phone resends one DATA frame, the modem's ACK and next frame are never read | Several causes, each seen in the byte log + the stall diagnostic (`N bytes unread ... rxdma`): (a) XbusDma arms each receive after checking LSR data-ready (0x500B4B68), set = line error 8 whose recovery (0x500B419C) leaves the receiver unarmed; (b) the modem's ACK came before the phone's TxDone (XbusProtocol takes an ACK only in TX state 4/5, 0x500B6508), the answer was timed 500 us after the CTS edge; (c) a modem DATA frame started while the phone's own DATA was arriving / its ACK still queued; (d) `rxdma 8` = request line high but no RX channel enabled: the PDD's RX-complete routine 0x500B4C94 disables the channel and delivers only with BLOCK set in CSR | (a) no LSR DR in DMA mode (`ra2_xbus_lsr`); (b) the answer waits for the phone's MSR read after CTS rose (`ra2_xbus_may_answer`, 70 ms fallback); (c) ACKs go first at frame boundaries, no new frame while a phone frame arrives (`ra2_xbus_ready`); (d) QEMU patch 0010 (CPC per frame for synchronised channels); patch 0009 `omap_dma_sync_state` for the diagnostic. Tests: xbus_answer_before_rx_armed, xbus_ack_before_txdone, xbus_data_crossing_phone_frame |
| 23 | DosServer (Nokia.dsy) waits in a nested scheduler (0x5016C584) after `0B 00` audio queries; PhoneServer and the Eikon server (PasswUi) block on it, Starter waits after RLock (record flags 0x12) | docs/research/ra2-isi-replies-2.md: `0B 00` reply msg 01 [V]; the `0B 11 {06 01}` reply handler 0x5016BF80 looks for sub-block id 02 (IsiMsg finder 0x500F5AE4), the N-Gage layout (id 06) never matched [V] | replies `0B 00 01 01 00`, `0B 11 12 @06 06 01 02 08 01 00 00 00 00 00`, SpeDeServer voltage `17 06 07 ...`; 45 processes (LightServer, CuiWs, Telephone, ...) |
| 24 | Kern::Fault "Exception" in the Supervisor / Ehwaomap.pdd; DeviceManagementServer restarted every few seconds | DMS dies at 0x50937328 (H-ra2-bal known gap): the audio plug-in's "ready" reaches the server's object +0x60 while ConstructL is still building it (+0x5C first); with that fixed, Gwc's message receive (Bal ord 11) and its cancel (ord 10) hung CActive::Cancel, Bal ord 9 (load the node from Dsp_image.dof) spun in BridgeApi's COFF parser, and the plug-in's open loops on -13 (its first command waits for a DSP reply message, -39) [V] | H-ra2-bal extended: `dms_ready` (NULL owner: drop), ord 11 pending / ord 10 KErrCancel, ord 9 stubbed, DMS accepts the failed open once (no audio); Wserv panicked WSERV-INTERNAL 13 loading its DevSound key-click plug-in; a Wsini.ini patch (no key click) was tried and dropped once Dasfgwc's commands were accepted (see stall 27). Open: the Supervisor still faults copying "devicemanagementserver" from 0x30000000 (DMS's fixed data) when DMS is started again |
| 25 | CuiWs (the cover's window server) panics "CuiWs" 201 = StartL left with KErrNotFound; 2 s later the Supervisor faults in DObject::FullName (an object whose owner field reads 0x30000000) while DMS's proxy looks up "devicemanagementserver" | CuiWs ConstructL creates a tone player (CMdaAudioToneUtility::NewL via MediaClientAudio, 0x50B72E1A) that fails without the audio path (docs/research/ra2-cuiws.md for the connects ruled out) [V by log catches]; the kernel fault follows CuiWs' death [I] | audio path: stall 27 |
| 26 | (after patches 0011-0012) 2 processes; the CPU stays in IRQ mode | VideoDriver's TVideoDriverInterrupt (Lv1_LCDCtrl) toggles LCD_EN for ever: QEMU's omap_lcdc assumed a 512-byte palette for 16 bpp (sync error) and re-raised palette-loaded on every re-enable [V by exec trace + LCDC registers] | QEMU patch 0013; the inner 640x200 screen is now enabled (blank) |
| 27 | DMS blocks in RBusLogicalChannel::DoCreate of the LM4855 amplifier (ASC1510 0x501353B8); Wserv waits on DMS (key click) | Lm4855.ldd writes over Espi.dll = McBSP2 (0xFFFB1000) in SPI (clock-stop) mode and polls SPCR2 for XRDY && !XEMPTY (0x5005EBE8); QEMU's McBSP had no SPI path and never cleared XEMPTY (docs/research/ra2-lm4855-spi.md) [V] | QEMU patch 0014 (clock-stop mode: a DXR1 write completes at once) |
| 28 | DMS panics "ASC internal error init set input divider" (-23) after 10 tries | the TLV320AIC23 codec is written over I2C at 0x1A (Aic23.ldd, 0x5005BE04); no device there [V] | `nokia9500-aic23` I2C slave (registers kept) in nokia9500_kbd.c |
| 29 | DMS vanishes (no panic/abort catch) while ConstructL powers the plug-in down; Wserv panics WSERV-INTERNAL 13 (key click) | ASC1510 power-down -> Dasfgwc ord 17 -> Bal ords 26/27 (Ehwaomap DoControl 0x40000014/15, DSP power): the PDD killed the client kernel-side [V by catches: the call never returned, the channel destructor ran in the Supervisor] | Bal ords 26, 27 -> bal_ok: 51 processes (SysAp, Startup, Telephone, Watcher, ...), the inner display shows the start-up splash |
| 30 | CuiWs still panics "CuiWs" 201 | CuiWs' LightClient connect (LightClient.dll 0x50098781) returns KErrNotFound: LightServer (ISA object 0x67) is blocked in its synchronous receive (0x500966E0) waiting for the `3A 03` (set brightness) reply (docs/research/ra2-isi-replies-3.md) [V] | replies `3A 03`, `3A 05`, `15 14`: 57 processes, CuiWs and CuiAppLauncher run |
| 31 | Telephone data abort at ~70 s (PhoneEngineGsm 0x50C5C2EC: event 0x307A handled while [engine+0x20] is NULL), then Starter panics 2 (critical process) and the kernel restarts | the engine's starter (RunL 0x50C539B4) leaves at step 10 (RunError 0x50C539A0, [starter+0x1C] = 10) with -1: Networkhandling NewL (ConstructL 0x50C72114) -> CbsMcnClient NewL connects to "CbsServer", not registered yet: CbsServer sits in a synchronous ETel call behind PhoneTsy's start-up (its `02 16` goes out only after Telephone's step 10) [V by catches]; RunError still reports "constructed" and +0x20 stays NULL (docs/research/ra2-isi-replies-4.md) | replies `09 23` (SIM CB topics: CbsServer's synchronous CustomApi call, Mmcustomtsy 0x50C3211C), `09 31` (a 76-byte frame: Mid(16,60) by field id, 0x500FBBCC), `09 09`, `09 1D` (docs/research/ra2-isi-replies-5.md); the reply table's 64-byte data limit raised to 200 (regression isi_reply_len_capped_at_64). Before: `02 16` [I layout] (CbsServer then goes on), `0A 00` (0 sub-blocks), `09 3D`, `31 19` (N-Gage layouts [I]), `09 31` with status 00 (the OK layout panicked USER 22 at Mid(38), Mmgsmwcdma ~0x50C09F2E). Open: what PhoneTsy's start-up still waits for (SIM 09 09/1D/23, 1B 00, 5A 00, DB 01 unanswered) |
| 32 | 58 processes, stable (no crash for minutes); inner display stays on the start-up splash; the cover UI shows "SIM card not valid" / "Unlock" | Nokia.dsy's `08 12` handler (0x50171E4C) took our data[1] = 00 as an error: SIM state 0 and `08 11` again [V] (docs/research/ra2-sim-state.md) | `08 12 {05 13 ..}` and `09 3B -> 3C {00 2A}`: the DSY now goes on (53 00, 09 3B, 09 29, 09 C0); **open:** the cover still shows "SIM card not valid" (which System Agent value it reads is not confirmed: KUidSimCStatus 0x100052E9 / KUidSIMStatus 0x100052C6 via Sae 0x5015D6E8); unanswered: 09 29 {37}, 17 08, 17 0A, 1C 10, 0B 14 |
| 33 | the cover still shows "SIM card not valid" | a catch on Sae's SIM-state copy (0x5015D6E8) shows 0 -> 7 -> 1; the change to 1 follows our `53 01 {01 00}` (the 6600's value) [V] | `53 01 {00 00}`: the state stays 7 and **the cover shows its idle screen** (wallpaper, clock, signal/battery, "Unlock"). Open: the inner display keeps the start-up splash, also with the lid open |
| 34 | the inner display keeps the start-up splash (also with the lid open) | Startup.app waits in SysLangUtil (0x5009CE74) -> SpeDeClient func 11 (language list) -> SpeDeServer `1B 1F` unanswered (docs/research/ra2-spede.md) [V] | `1B 1F -> 20` with the language list text ($001 = UK English): **the inner S80 UI runs**: side panel (clock, indicators) and "System locked - Enter lock code" |
| 35 | keys stop after the lid opens: EKeyb reads the lid byte (0xE0), then nothing | Ei2c's read routine polls I2C_STAT (0x500428D8) for RRDY/ARDY; its interrupt handler reads I2C_IV first, and QEMU's IV read cleared RRDY: the byte was never collected and the transfer never ended [V by an I2C register log] | QEMU patch 0016 (IV acknowledges RRDY/XRDY for the interrupt line only; they stay in STAT until the DATA access); 0015 = a bounded I2C register log (bring-up aid). Typing works on the inner display |
| 36 | "System locked": the lock code (12345) goes to the modem as `08 01 {06 00 '12345' 00}` | security code verify | `08 01 -> 02 {00 00}` (the BB5 table's OK [I]): unlocked, the first-start "Language selection: English" dialog appears. then Startup blocked again in SpeDeClient: SpeDeServer's product-profile write `1B 04` (the chosen language) was unanswered |
| 37 | first-start wizard | `1B 04 -> 05 echo` [I] | the wizard runs: Language (English) -> Time and date -> "My contact card" (Contacts form); **Desk (the S80 home) and LockSrv are running**. Open: which keys are the four command buttons (OK/Cancel...): F9-F12 in nokia9500_kbd.c send Device6..9 (0xAA..0xAD, indices 84/85/93/94), but one of them moved the form focus; the CBA mapping needs reading (EKeyb variants, the S80 key translation) |
| 38 | the command buttons and the cover keypad do nothing | they are a 5x5 matrix on the OMAP MPUIO keypad scanned by EKeyp.dll (0x5009D8B0, scan codes 0x5009E35C), not the I2C controller; buttons 1-4 = 0xA4..0xA7 = EKeyDevice0..3 (docs/research/ra2-command-keys.md) [V] | QEMU patch 0017 (`omap_mpuio_key` again, with a log line); F9-F12 -> the command buttons; with the lid closed the host digits, * #, F1/F2 softkeys, Enter select, F3 send, F4/Esc end, arrows (scroll, [I]) -> the cover matrix (positions found by pressing each one on the cover [V]); `keyp` machine property. Open: the four scroll keys are not confirmed, and the cover UI stopped reacting after some scroll presses / after Select->Save ("Contacts not ready") |

Displays: the inner 640x200 is the OMAP LCD controller (VideoDriver.dll, QEMU's omap_lcdc, console 0; enabled since patches 0012/0013; panel commands on Microwire CS3, ID read unanswered = zeros); the cover
128x128 is `nokia9500_cover.c` on Microwire CS0 (console 1; protocol docs/research/ra2-cover-display.md), built,
no traffic yet. Keyboard and lid: the keyboard controller on I2C 0x56 (bytes < 0xE0 keys: index = byte >> 1 into
a 3-variant scan-code table at [0x5009E8FC], handled only while the lid is open ([this+0x58]); 0xE0 / 0xE1 lid
open / closed -> TRawEvent 18 / 19) with its interrupt on MPUIO 2: `nokia9500_kbd.c` (I2C slave at 0x56, 7-bit [V:
EKeyb reads the lid byte at start]; machine properties `cover-open`, `kbd-byte`). Key codes to qcodes: to do (the
scan-code table, and whether bit 0 is "released", once the UI runs).

Kernel words (ported from the 6600, 7.0s): object containers 0x80000430, current thread 0x800018C0. Kern::Fault
0x500212C4 (r0 = category descriptor, r1 = reason), EUser RThread::Panic 0x500D9D48 (r1 = category, r2 = reason;
ported from the 6600), exception vectors high (0xFFFF0000). Session catches: `fatal,pabt,dabt,undef,panic`; a write
watchpoint is `VA=w:LABEL`, an address-filtered instruction trace `--debug exec,nochain --dfilter RANGES`. IRQ entry
0x500151D4, dispatcher 0x500125AC (level-1 SIR, then level-2 SIR + 32; line 22 = OS timer = 54).
Displays: Cuilcd.ldd / Cuilcdpdd.pdd (cover; touches 0xFFFB0000/0800/9800 and GPIO 0xFFFCE000 [V literals]),
strings "Cover LCD", CoverUI apps and "EFlip" (the cover state); the inner panel's driver is not identified yet.

## Hacks and unfinished work (state of 2026-11-18, commit 320ed9e and after)

### Hacks on the board (details and the real implementation in docs/hacks.md)

| Hack | What it replaces |
|---|---|
| H-ra2-loader | the bootstrap / DiskOnChip load of the core, ROFS, ROFx images: the board places them in RAM |
| H-ra2-doc | M-Systems TrueFFS (DiskOnChip.dll) as an API stand-in over a board block device; **no persistence**: C: is the factory FAT image each boot, so the lock code query and the first-start wizard come back every time |
| H-ra2-xbus-modem | the cellular modem: a peer on UART2 speaking XBUS framing, ISI answered from `src/qemu-ngage/isi-replies-ra2.txt` (a table, not a modem model: no network, no calls, no SMS) |
| H-ra2-kbc | the QWERTY keyboard controller (I2C 0x56) as a byte FIFO; other controller events (0xF0-0xF3) and writes from EKeyb not modelled |
| H-ra2-aic23 | the TLV320AIC23 codec on I2C 0x1A: registers kept, **no audio** |
| H-ra2-dsp-boot | the C55x DSP is not emulated: the boot-word poll in Ehwaomap.pdd is patched out |
| H-ra2-bal | Bal.dll node/stream/message/power exports return success without a DSP; DeviceManagementServer: "ready" guard and no open retry; Dasfgwc commands accepted without a DSP reply. **No sound** anywhere (key clicks, tones, ring) |

QEMU OMAP1 model fixes (not hacks; patches 0003-0017 in patches/qemu-ngage/): 0009 DMA channel state for
diagnostics, 0010 DMA CPC per frame, 0011 DMA interrupts masked by CICR, 0012 Microwire IRQs/DRQ (+ write log),
0013 LCD 16 bpp palette and palette-loaded interrupt once per palette [I: real LP clearing not modelled],
0014 McBSP SPI clock-stop mode, 0015 I2C register log (bring-up aid, can go), 0016 I2C IV keeps RRDY/XRDY,
0017 `omap_mpuio_key` (+ log line).

### ISI replies that are guesses [I]

Taken from the 6600/N-Gage/BB5 tables or chosen without reading the 9500 parser (each line's comment says so):
MTC `15 0B/02/01/04/12`, product profile `1B 02`/`1B 04` (echo), indications to objects 0x69/0x52
(`15 C0`, `09 EF`, `08 10`), DSY SIM `09 C0/2D/2B`, SIM lock `53 00` (00 chosen because 01 broke the SIM
state), DSY audio `0B 11`/`0B 00` values, `23 04` bytes 1-13, SpeDe voltage `17 06`, SMS `02 16 -> 17`, GPDS
`31 19`, SIM `09 3D`/`09 09`/`09 1D` (IMSI 001010000000001), lock-code verify `08 01 -> 02`, LightServer
`3A 03/05`, `15 14`. Unanswered and probably harmless so far: `19 0A`, `D1 04`, `1B 00 {41}` (IMEI), `5A 00`,
`DB 01`, `31 30`, `02 14`, `17 0C`, `17 08/0A`, `1C 10`, `0B 14`, `09 29`, `09 E0`, `01 A4` (PhoneTsy,
repeated), `1F 01/04` (object 0x54), `1C 2D` (object 0x65, data looks stale).

### Not done / known problems

- Desk home screen not reached yet: press F9 (command button 1 = OK) through the wizard; untested.
- Cover keypad: digits, * #, softkeys, select, send, end confirmed; the four scroll keys (matrix (0,1)
  (2,1) (3,1) (4,1)) unconfirmed; the cover UI stopped reacting after some scroll presses and after
  Select -> Save ("Contacts not ready": the SIM phonebook path is not answered).
- No persistence (see H-ra2-doc), no RTC model check (clock shows 10:14 at boot), no network/calls/SMS,
  no audio, no camera, no MMC, no Bluetooth/WLAN/USB/IrDA.
- The start-up logo appears only on the inner display; whether the real phone shows one on the cover
  is not known.
- The 0x1C 2D request of object 0x65 carries what looks like stale DSP-plug-in text; owner unknown.
- Isolation checks (N-Gage, 7650, 6600) passed after patch 0014 and the reply-table change; not re-run
  after 0015-0017 (OMAP-only code, which the other EKA1 boards do not use).
- Tools: `ngage_sess.py stack` cannot resolve threads whose process owner reads as 0 (the cover UI
  threads in CuiAppLauncher); monitor `xp` returns zeros for 16-bit OMAP registers (DMA, Microwire),
  hence the logs in patches 0009/0012/0015/0017.
