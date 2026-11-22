# Nokia 6630 (RM-1) bring-up

Firmware V 6.03.40 (`RM1_6.050340_B4.C0R` core, `RM1_6.050340.v27` variant; a Turkish `.v23` too). S60 2nd Edition
**Feature Pack 2**, Symbian **8.0a**, EKA1: the first Symbian 8 phone and Nokia's first 3G (WCDMA) phone. Goal: its own
ROM to the S60 home screen, covering the FP2 era (the 3230 covers FP1, the 6600 2nd Edition).

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## State (2026-11-19)

**The S60 home screen and the Menu grid** on `-M nokia6630`, from the phone's own unmodified ROM: first boot
(~3.5 min: the NAND is formatted, then the home-city list, Time and Date queries), the idle screen (wallpaper, clock,
indicators, Messag./Camera softkeys), every keypad key, Calendar from the idle shortcut, the Menu (F5) with its
grid. No sound, network, SIM, DRM or secure environment. **Open: a second boot from a kept NAND** (`--phone NAME`
without `--fresh`) faults (stall 26). How it was done, techniques and traps: [how-it-was-done.md](how-it-was-done.md).

    python3 eka1/tools/ngage_phone.py --model nokia6630 --phone NAME --fresh     # the window (NAND kept in build/ngage/phones/NAME.flash)
    python3 eka1/tools/ngage_sess.py -s NAME --model nokia6630 --catch pabt,dabt,undef,fatal start   # a debug session

Keys: digits, F7 or keypad * = *, F8 = #, arrows = joystick, Enter = select, F1/F2 = softkeys, F3 call, F4/Esc
end, **F5 Menu**, Backspace C, F6 edit, F9 0xAA (a side key [I]).

Prepare the images once (all git-ignored):

    python3 -I tools/fiasco.py eka1/rm1_rom/RM1_6.050340_B4.C0R --out eka1/rm1/fiasco/core
    python3 -I tools/fiasco.py eka1/rm1_rom/RM1_6.050340.v27 --out eka1/rm1/fiasco/v27
    python3 -I eka1/tools/xzip_core.py 'eka1/rm1/fiasco/core/SOS*CORE@00070000.hdr' 'eka1/rm1/fiasco/core/SOS*CORE@00070400.bin' \
        eka1/rm1/core/ROM.bin --rofs eka1/rm1/output/rom/rofs.img
    python3 -I eka1/tools/rm1_nand.py eka1/rm1/core/nand.img --rofs eka1/rm1/output/rom/rofs.img \
        --rofx eka1/rm1/fiasco/v27/_pre@01050000.bin

## Files (git-ignored) and their SHA-256

| What | Where | SHA-256 |
|---|---|---|
| owner's archive | `Nokia_6630_RM-1.zip` (Firmware/: the .C0R, .v27, a Turkish variant folder, a Russian "Light" installer .exe, not used) | `7592d9644698d9d35dcd86bcb2802ef5ad4f3a0e48e8a73d2c1b541b5d1a0c09` |
| core flash file (V 6.03.40) | `eka1/rm1_rom/RM1_6.050340_B4.C0R` | `5fa64f072c060646cde277560bc2c6bb11cddbcef75b1a5d04e7be212b4cae3a` |
| variant (English, used) | `eka1/rm1_rom/RM1_6.050340.v27` | `ce31ab3101d19a2e0aaf2a58a3c71d813f713d59138c9c2ce1d58b15127d0ee4` |
| variant (Turkish, not used) | `eka1/rm1_rom/RM1_6.050340.v23` | `3dc770775d53f1dd61e09a9fc7319da9b9380e5f760684b6ae6419dcd85fddf4` |
| FIASCO partitions | `eka1/rm1/fiasco/core/`, `eka1/rm1/fiasco/v27/` (`tools/fiasco.py`) | |
| inflated core ROM (VA 0x50000000) | `eka1/rm1/core/ROM.bin` (+ `files.json`, `analysis/user-exports.tsv`) | `ba6ffa97974aa275f4faf7d3f62c5a1f290c1d145a1f958729c6443a45b805f4` |
| core ROFS | `eka1/rm1/output/rom/rofs.img` | `ebd24b1b1f2bd4463ecd149706460d1ef007192d8ed5ed41a5aaef7b8ea7f9ec` |
| variant ROFx | `eka1/rm1/fiasco/v27/_pre@01050000.bin` | `a76ff31ec5018b04ef42371e04f1039ea6218a936cba6fe90198281c18d0ca34` |
| NAND image (pristine) | `eka1/rm1/core/nand.img` (`eka1/tools/rm1_nand.py`, 69 206 016 bytes) | built, not hashed |
| Ghidra inputs | `build/evidence/ghidra-rm1/` (modules cut at their VA; Startup/Menu RAM code dumped from the phone) | |
| screenshots | `build/evidence/rm1-*.png` | |

## Step 1: inventory [V]

**Not DCT4: BB5 with a TI OMAP1610.** The flash files are BB5 "B2" FIASCO containers (header strings "BB5 ALGORITHM",
"OMAP1610"), the format of the N95-era phones, not the Phoenix A2/DCT4 records of the 6600/3230. `tools/fiasco.py`
(written for the N95) splits them unchanged:

| Partition | Flash | Size | Content |
|---|---|---|---|
| NOLO, X-LOADER, PRIMAPP, SOS_LOADER | 0x6C0 .. 0x10600 | small | boot loaders |
| SOS_XZIP | 0x50400 | 29 696 | ARM code with zlib 1.2.1's inflate: the core's decompressor |
| **SOS*CORE** | 0x70400 | 16 566 272 | the Symbian core, **compressed** (7.97 bits/byte); no plain zlib/deflate stream at its start |
| MCUSW, DSP0 | 0x200400, 0x20400 | 5 MB, 1.3 MB | the modem (cellular) side |
| .v27 | 0x01050000 | 3.7 MB | a plain ROFx (the language/variant ROFS extension) |

Board: the SoC is an OMAP1610; the closest board here is the 9500/7710's OMAP1510 (QEMU's omap1.c models the
OMAP310/1510 only) [I until the bootstrap is read].

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | the core ROM is compressed | SOS_XZIP (Thumb, runs at 0x10090000) copies the partition's first 0x3D0 bytes and inflates the rest as **one raw deflate stream** (zlib 1.2.1, -15 window bits): it starts at partition offset 0x3D0, inside the 1 KB record header, 0x30 bytes before the `.bin`, which is why a scan of the `.bin` found nothing; the first 0x30 output bytes are zero, then the EKA1 ROM; after the stream, a plain ROFS (partition offset 0x630000, 0x99CAC0 bytes) (docs/research/rm1-xzip.md) [V] | `eka1/tools/xzip_core.py HDR BIN ROM.bin --rofs rofs.img`; tests `eka1/test_xzip_core.py` (regression rm1_stream_in_hdr) |

ROM: base 0x50000000, size 0xC00000, root 0x50003C60, kernel data 0x80000000..0x80400000; 594 files in the core
(453 primary, 420 secondary modules), the applications in the ROFS. Kernel ekern.exe 0x50009F9C (184 260 bytes), variant
ecust.dll 0x50036FD0. Extensions: epower -> LCDSTARTUPEXT -> epbusm -> ekeyp -> usbext -> eusbcc -> wd32khz.ext.

**Closest relative: the 9500/7710 (OMAP), not the 3230/6600 (DCT4)** [V module list]: the modem is XBUS (`xbus.ldd`) +
`ssi.pdd` (OMAP SSI) + the ISA_IF drivers, the DSP bridge (EHWAOMAP.pdd, Bal, BridgeApi), keys through ekeyp (OMAP
MPUIO keypad), C: on NAND (`mednand.pdd`, `nandftl.fxt`), lcd.ldd/lcddev, BlueCore on UART1. The bootstrap uses the
OMAP1510 register set (0xFFFECB00 interrupt handler, EMIFS 0xFFFECC00, clocks 0xFFFECE00) plus OMAP1610-only blocks
(second interrupt controller 0xFFFE0000, GPIO 0xFFFBE400 / 0xFFFBEC00, 0xFFFBBC00). Plan: `-M nokia6630` from the
9500 board, with the OMAP1610 differences as QEMU patches (QEMU models only the OMAP310/1510).
| 2 | (board) where the core runs | the bootstrap is linked to run at physical **0x10100000** (its jump tables hold 0x10102ED8..; memory-table entry 0x10100000/32 MB) [V]: XZIP inflates into SDRAM there | `-M nokia6630` (`nokia9500.c` `nokia6630_init`): ROM.bin loaded at 0x10100000, CS0 = the 9500's two-word stub, ARM926 on QEMU's OMAP310/1510 model, catch-alls for the 1610-only blocks; kernel words ported from the 3230 (current thread 0x800018C8, containers 0x8000048C; Kern::Fault 0x5001e65c (reason + 0x10000) -> the fault routine 0x5001e664 [V]: the session's `fatal` catch) |
| 3 | the kernel (MMU on, Supervisor only) loops in an ARM926 wait-for-interrupt (0x50010848, from 0x5000DF0C) for the first tick | it programs the **OMAP1610's 4-bank level-2 interrupt controller** (0xFFFE0000/0100/0200/0300; QEMU's 1510 model has one bank) [V log]; its dispatcher (0x5000e5f4) reads level 1's SIR, and for line 0 level 2's SIR & 0x7F + 32: interrupts 32..159, the 1610 map (L2 5 MPUIO, 14/15 UART1/2, 40/41/48 GPIO banks 2/3/4) [V]. The tick is **GP timer 2** (0xFFFB1C00, the OMAP2 dmtimer layout): TIOCP 0x34, TWER 3, TLDR 0, TTGR, TMAR 0x200, TIER 3, TCLR \|= 0x43, polls TWPS (0x5000e05c) [V]; only level-1 line 18 is unmasked (MIR 0xFFFBFFFF) = GPTIMER2 in the 1610 map; after each interrupt it reads TCRR (0x5000ea80) and moves TMAR on by 0x200 (1/64 s at 32 kHz) [V run]. The 32 kHz OS timer (0xFFFB9000, CR 0x0B, no IE) is not the tick | QEMU patch 0021 (omap-intc `nbanks` 4 via `omap1610_mpu_init`, level-2 FIQ to level-1 line 2); `nokia6630_soc.c` GP timers 1-8 (L1 17/18, L2 34..39); tests `nokia6630_gpt_test.c` (rm1_no_tick), `nokia6630_board_test.py` |
| 4 | Kern::Fault in epower.dll: "Someone has disabled GPIO func.clock !" | epower reads 0xFFFE087C (ULPD SOFT_REQ_REG2 on the 1610) and needs bit 2 (0x5003b5b8) [V]; the kernel set it with a 32-bit RMW that QEMU's 1510 ULPD dropped (width 4, register unknown) [V log]. The fault then crashed in the variant's fault hook (ecust vtable +0x210 reads `this+8` = NULL): a fault before the variant is ready | QEMU patch 0022 (1610 ULPD: 32-bit accesses, 0x54..0x9C stored); test `Ulpd` (rm1_gpio_func_clock) |
| 5 | lcd.ldd spins at 0x5003ebd8 for bit 2 of 0xFFFBAC08 | the 1610's **SoSSI** (smart-panel interface) at 0xFFFBAC00, absent on the 1510 [V literals: lcd.ldd uses 0x00..0x20 only] | H-rm1-sossi: a register file, 0x08 bit 2 set; the panel protocol: docs/research/rm1-lcd-sossi.md (open) |
| 6 | mednand.pdd (Media.Nand2) polls 0xFFFBCC68 bit 0 after setting 0xFFFBCC64 bit 1 | the 1610's **NAND flash controller** at 0xFFFBCC00, all polled, no IRQ/DMA, hardware ECC in TI packing; chip ID table: Samsung small-page (512+16, 32 pages a block) only; partitions from a **TOC in OCP SRAM 0x20000100** left by the boot loader ("SOS-ROFS", "SOS-USER"; without it "User disk start address still not found - CRITICAL FAILURE") (docs/research/rm1-nand.md) [V] | H-rm1-nand: `nokia6630_nand.h` (controller + chip EC 76, 4096 x 32 x 528), `eka1/tools/rm1_nand.py` builds `eka1/rm1/core/nand.img` (TOC in pages 0x100-0x101, the ROFS + v27 ROFx at 0x100000 with ECC, C: blank at 0x1000000), the board copies the TOC to SRAM at reset; `flash=FILE` keeps the NAND; tests `nokia6630_nand_test.c` (rm1_nand_controller), `eka1/test_rm1_nand.py` |
| 7 | wd32khz.ext polls 0xFFFEB014 for 1 after a soft reset of 0xFFFEB010 | the 1610's 32 kHz watchdog (OMAP2 WDT layout), absent on the 1510 [V 0x500aba0c] | H-rm1-wdt: a register file (SYSSTATUS 1, WWPS 0), never expires |
| 8 | EStart's Main waits in isaapi.dll (RBusLogicalChannel::DoRequest on the ISA channel); the CPU idles. EKern, EFile, EStart (Main, InitCompleteThread) [V objects/stack] | the modem link: xbus.ldd over ssi.pdd; **SSI** at 0x30000000 (SST +0, SSR +0x800, GDD DMA channels **+0x1800** + 0x40 n [V run; the research said 0x2800, a VA offset]); 32-bit control words on channel 0 (hello 0, link up 0x10000000, header 0x73HHLLCC with the length in words, ACK 0x83000000, NAK 0xC3000000), ISI payloads by DMA on channel 3; interrupts L1 29 (SSRFullCh0), L2 90 (SSRCtrl) [V MIR]; not the 9500's byte XBUS (docs/research/rm1-ssi-xbus.md). Two traps [V run]: the modem must answer the phone's hello with **hello** (only that clears xbus's "Ctrl pending" flag, 0x50149f5c), and **not at once**: the phone says hello from its Ctrl interrupt while ssi.pdd's start-up still drains SSR channel 0, which ate an immediate answer | H-rm1-modem: `nokia6630_ssi.h` (the modem's end of the word protocol) + `nokia6630_soc.c` `rm1_ssi_*` (registers, GDD copies, IRQs, AP-wake hook on GPIO4 bit 3, GPIO1 bit 12 held high) feeding ngage_isi.c; tests `nokia6630_lcd_ssi_test.c` (rm1_ssi_hello_eaten, rm1_ssi_ctrl_pending) |
| 9 | IsaApi CheckCrc (EStart, object 0x1F, `res 00 msg 01 {00 00}`) unanswered | isaapi compares the reply's data[1] with **0x1D** (Thumb `cmp r0, #29` at 0x50142B88) [V] | `isi-replies-rm1.txt`: `reply 00 01 02 00 1d`, the rest seeded from the 3230 [I] |
| 10 | the phone ran off into empty memory ~25-45 s in (pc 0x59.. 0x6.. rising, Undefined mode, MMU off) | not a reset (no second bootstrap entry [V catch]). Two causes found on the way: (a) QEMU's 1510 MPU watchdog dropped the bootstrap's 32-bit "watchdog off" writes (0xF5/0xA0 to TIMER_MODE) [V log]; (b) **SecPdd.pdd** (security environment, started for SecEnvInit) copies its monitor below the core (PA 0x100Fxxxx, VA 0x5977xxxx) and runs it in Undefined mode: it turns the MMU off with `mcr p15,0,r2,c1,c0,0; nop; mov pc, r1` (0x500AADE8) relying on the ARM926 having prefetched the next instructions through the old mapping; QEMU ended the TB at the mcr and fetched from PA = old VA [V disassembly + catch: SCTLR written, then pc in empty memory] | QEMU patch 0024 (MPU watchdog: 32-bit accesses as 16-bit); QEMU patch 0025 (pre-v6 cores: the two instructions after an SCTLR write are translated in the same TB, as prefetched); tests `LaterPatches` (rm1_watchdog_32bit, rm1_secpdd_mmu_off) |
| 11 | SecPdd's physical monitor jumps to **PA 0x3C00** (`mov r1, #0x3C00; mov pc, r1` at 0x500AB234), the OMAP1610's on-chip (secure) ROM, which the board does not have (CS0 = boot stub + zeros) | every secure service goes there (MMU off, physical sp, return address at [sp], r4 = return kind, r0 = 1 OK); callers map anything but 1 to KErrGeneral; EStart does not wait for SecEnvInit, which exits on an error (docs/research/rm1-secenv.md) [V] | H-rm1-secrom: 4 words at 0x3C00 in the board's boot ROM: r0 = 0, r4 = 1, pop [sp], bx. SecEnvInit then dies with KERN-EXEC 0 (reported to the modem, harmless so far) |
| 12 | Wserv dies at start-up: WSERV-INTERNAL 13 (reported in `38 0E` messages to the modem) | Wsini.ini is in the ROFS; the variant ROFx sat at the next 16 KB block after the ROFS, but it extends the ROFS at its **iMaxImageSize** (header +0x2C = 0x9B0000; the .v27's directory offset 0x9B0030 counts from the ROFS start) [V], as on the N95 | `rm1_nand.py` puts the ROFx at ROFS + iMaxImageSize; test rm1_rofx_place. **27 processes** (EwSrv, FbServ, Starter, EikSrvs, AknSkinSrv, PhoneServer, DosServer, SplashScreen ...); still the NOKIA logo |
| 13 | the CPU always in EHWAOMAP.pdd (0x50242d78..84), DeviceManagementServer started last | the DSP/BIOS bridge waits for the DSP's boot code to clear a shared word, no timeout (the 9500's H-ra2-dsp-boot loop, same code) [V] | H-rm1-dsp-boot: the loop's `bne` (0x50242D84, old word 0x1AFFFFFB checked) -> nop, applied to the core image before it goes to SDRAM (`rm1_patch`) |
| 14 | 23 processes, the CPU idle, still the NOKIA logo; Starter idle in its active scheduler; DeviceManagementServer, RfsServer, EDbsrv, CdlServer no longer listed; no panic reported to the modem but SecEnvInit's | Nokia.dsy is ISA object 0x77 (its subscription = the 9500/3230 DSY list) [V]: MTC/SIM/security indications added [I]. Unanswered: `19 0A`, `D7 09 {1B 04}`, `09 1F {02/01/2E 00}` (obj 0x7E), `17 2F {00 01 28 04 0F A0}`, `DB 05` (obj 0x58), `DB 01` (obj 0x56), `41 08` (obj 0x6B) [V log] | open: DMS's fate (the 9500 needed H-ra2-bal next), the unanswered requests |
| 15 | DeviceManagementServer's thread returns **-33** at ~65 s (no panic, no leave with -33), EikAppUiServer exits with the same code right after; RfsServer/EDbsrv/DBRECOVERY exit 0 (normal) [V exit catches] | DMS loads its DSP plug-ins (TestServerGwc -> Gwc.dll -> Bal.dll -> EHWAOMAP.pdd); EHWAOMAP's DSP request timer (0x502387E0) completes the pending request (types 1-6) with KErrTimedOut since no DSP answers [V Ghidra: `mvn r2, #0x20` at 0x50238888 before Kern::RequestComplete 0x50248038]. (Gwc.dll's own timer RunL 0x50288098 also reports -33; changing it alone did not help [V run]) | H-rm1-dsp-timeout: 0x50238888 -> `mov r2, #0` |
| 16 | DMS (then EikAppUiServer) now exit with **-18** (KErrNotReady); still 23 processes, the NOKIA logo | a `User::Leave(-18)` in DMS's ConstructL (0x5021C794, TRAPped by E32Main 0x50220720, whose result is the thread's exit code): each DSP plug-in (TestServerGwc, then EapGwc, ...) creates its node and calls Bal.dll ord 22 (0x50285BD8, DoSvControl 0x80000007); EHWA.ldd answers KErrNotReady (`mvn r0, #0x11` at 0x5024ADF8) because the node never left state 0 without a DSP [V catches at both sites + Ghidra; docs/research/rm1-dms-not-ready.md]. EikAppUiServer is DMS's client (DeviceManagementServerProxy: Create, Logon, Resume, wait for the rendezvous) and exits with DMS's reason [V Ghidra]. Trap: catch conditions compare unsigned registers (`r0 == 0xffffffee`, not -18), and leave catches slow the boot so much that DMS is not reached in 190 s | H-rm1-bal-run: Bal ord 22 = `mov r0, #0; bx lr` (old words checked); no non-zero exits in 300 s, **26 processes** (DMS stays), the panel turns white (the UI draws); test rm1_dms_not_ready |
| 17 | white panel, 26 processes; EikAppUiServer blocked in `CAknKeySoundSystem::NewL` (session connect); DMS's main thread blocked in Bal.dll ord 19, then in EapGwc 0x502287A0 and MuMdrcGwc 0x50229700 | ord 19 = DoSvControl 0x80000014 + synchronous wait for the DSP; the plug-ins' command call sends through Gwc and loops on `User::WaitForRequest` for the DSP's reply (the only two such loops in the ROM [V scan]) [V stacks + Ghidra] | H-rm1-bal-run extended (ord 19 returns 0), H-rm1-eapgwc (both loops: `bne` after the send -> `b` to the return); DMS goes on to asc.dll's ISI audio request `1C 2D` (object 0x78, open) |
| 18 | DMS waits in asc.dll for an ISI reply to `1C 2D {00 00}` (object 0x78) | parser 0x501EE2E0: resource 0x1C, message >= 0x2E, (data[0], data[1]) in (1,1) (2,2) (1,3) (1,6) [V Ghidra] | `reply 1C 2D 2E 01 01 00` |
| 19 | Kern::Fault("PLAT", 91) at 22-83 s (timing-dependent) | DThread exit 0x5001DC08: a **system-critical thread panicked** [V Ghidra]: SecEnvInit's Main (KERN-EXEC, its cleanup after the failed secure calls closes a sub-session never opened) [V catch] | H-rm1-secenvinit: 0x5030652C returns at once |
| 20 | CTelServer panics USER 21 (isimsg under NOKIATSY 0x501A2E43), C32Start (system-critical) follows: PLAT 91 | the TSY (object 0x56) read past the seeded NET_MODEM_REG_STATUS reply (N-Gage form, 1 sub-block) [V catch + log] | the 9500 form `reply 0A 00 01 00 00`: **39 processes** (SysAp, Phone, Startup, Autolock, ScreenSaver, sae, watcher, ...); the S60 note **"Phone start-up failed. Contact the retailer."** (open) |
| 21 | "Phone start-up failed. Contact the retailer." (Startup's note), SysAp/Phone/Startup running, CPU idle | Startup.app (ROFS; its RAM code at VA 0xFF500000, dumped and loaded into Ghidra; imports named from the live IAT) calls `RDosSelfTest::PerformSelfTest` -> DosServer -> Nokia.dsy (object 0x77) -> ISI `59 42 {00 00}`, unanswered [V log + Ghidra]. (Not SysAp's "StartFail" shared-data path: no hit on its handler 0x508E4734 [V]) | `reply 59 42 43 00 00` [I payload]: **the first-boot "Select home city" list** (build/evidence/rm1-home-city.png) |
| 22 | (keys) | ekeyp.dll (0x50080960) scans the OMAP MPUIO matrix in 4 steps (KBC bits 0..3 low: column words 0x7E 0x7D 0x7B 0x77 at 0x5008147A), 7 row bits, scan code = table[step + row*4] (0x5008145E): digits, * 0x2A, # 0x7F, arrows 0x0E-0x11, softkeys 0xA4/0xA5, select 0xA7, call/end 0xC4/0xC5, Menu 0xAA, C 0x01, edit 0x12, 0xB4 [V Ghidra] | `nokia6630_keys.h` + `rm1_keys_init` (QEMU's omap_mpuio_key): the home-city list scrolls and selects, then **Time and Date queries** answer OK (build/evidence/rm1-keys-city-time.png); after the date: a white screen (open). Keys: digits, F7 *, F8 #, arrows, F1/F2 softkeys, Enter select, F3 call, F4/Esc end, F5 Menu, Backspace C, F6 edit |
| 23 | after the date query: white screen, then Kern::Fault PLAT 91 (a system thread panicked) | CTelServer panicked USER 21 in SIMATKTSY.DLL 0x501C9FFC, the SIM Toolkit TSY's handler of NET_MODEM_REG_STATUS (`0A 01`): it finds sub-block 0 without checking (none in the 9500 form) [V catch at the parser entry: the message is our `0A 01 {00 00}` + Ghidra] | `reply 0A 00 01 00 01 00 08 05 00 00 00 00 00` (sub-block 0 of 8 bytes, status 05 no service [I]): **the S60 home screen** (wallpaper, clock, date, Messag./Camera softkeys; build/evidence/rm1-home-screen.png) |
| 24 | keys dead on the home screen (the MPUIO saw them; ekeyp's interrupt stayed disabled) | (a) QEMU's MPUIO keypad had 5 rows, the 6630 scans 7 (End, Menu, 0, 4, C on rows 5-6) [V log]; (b) the Supervisor spun in ekern 0x5000D820: after epower enabled a module clock for bc02_uart1.pdd (Bluetooth) it polls ULPD APLL_CTRL (0xFFFE084C) bit 15, the APLL lock, with no timeout; QEMU keeps bits 0-3 only, so no DFC ran any more (ekeyp's scan is one) [V samples + stack + Ghidra] | QEMU 0026 (8 keypad rows), 0027 (1610: APLL_CTRL reads with bit 15 set): keys work on the idle screen (joystick right -> Calendar, Down moves a week; build/evidence/rm1-idle-calendar.png); **F9 (0xB4) opens the Menu**: its title shows, the grid does not (open; build/evidence/rm1-menu-title.png) |
| 25 | the Menu shows its title, the grid stays empty; CPU idle | (a) the TSY asks the SIM service table `09 13` with sub-types 28 and 74; only the N-Gage sub-type 11 was answered [V log] -> replies per sub-type (62 processes instead of 49); (b) Menu's ECom plug-in MenuDRMPlugin -> DRMHelper.dll starts DRMHELPERSERVER.EXE and waits on "DRMHelperServerSemaphore" with no timeout (0x50ABD41C); the server panics ("CDRMHelperServe" 1, no secure environment) before it signals [V Ghidra + run] | `reply 09 13 14 @28 / @74 ..`; H-rm1-drmhelper (the start-server routine returns KErrNotFound): **the Menu grid** (Messaging, Contacts, Log, Gallery, Camera, Media, Calendar, Web, Clock; build/evidence/rm1-menu-grid.png) |
| 26 | a **second boot from a kept NAND** faults: Kern::Fault "PLAT" 91 after ~75 s | DosServer (system-critical) panics E32USER-CBase 46 (stray signal) [V catch]; on a second boot Nokia.dsy asks `23 04 {01 33 ..}` (another permanent-data item; our reply is fixed to item 0xD4) and `15 01 {01}` (MTC state change) just before [V log] | open |

## Next (hand-over: docs/prompts/nokia-6630-bringup.md)

1. Stall 26: the second boot (kept NAND): read Nokia.dsy's parsers for `23 04` (item 0x133) and the MTC state change
   (`15 01` -> `64`, then the 15 C0 indication?), and why DosServer gets a stray signal.
2. Unanswered modem requests (TSY object 0x56: `01 AA`, `1B 00`, `02 06`, `02 16`, `09 BA`, `09 DE`, `08 01`, `31 06/11/30`,
   `43 07`, `0A 36`; DSY 0x77: `09 29`, `19 19`; obj 0x7F `19 0A`, `19 01`, `D7 09`, `17 2F`; `DB 01/05`, `41 08`):
   each only when something waits on it (the 9500's `02 16` reply crashed the TSY here).
3. Sound (DSP: the DMS plug-ins are stubbed), network/SIM, DRM/secure environment: not started.
4. The LCD DMA interrupt line (level-1 25 [I], never bound in the runs).
