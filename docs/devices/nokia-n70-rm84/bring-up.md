# Nokia N70 (RM-84) bring-up

Firmware V 5.1003.3 (`RM84_51003301.C0` core, `RM84_51003301.V05` variant). S60 2nd Edition **Feature Pack 3**,
Symbian **8.1a**, EKA1, TI **OMAP1710**, BB5 modem: the 6630's successor and the last Symbian flavour without a phone
here. Goal: its own ROM to the S60 home screen and the Menu grid, as the 6630 (FP2) reached them on 2026-11-19.

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## State (2026-11-20)

**The S60 home screen and the Menu grid** on `-M nokian70`, from the phone's own unmodified ROM, in one session, 10
stalls: first boot ~3.5 min (NOKIA logo while C: is formatted), then "Select current city", the Time and Date queries,
the Active Standby home screen (clock, shortcut bar, "No calendar entries for today", Messag./Music softkeys), the Menu
(F5) with its grid (Messaging, Contacts, Calendar, Radio, Gallery, Imaging, Clock, Web, My own), folders open (Imaging:
Camera, Movie, RealPlayer, Img. print). No sound, network, SIM, DRM, secure environment or memory card. Clock: the RTC
time read `19 0A` is answered with QEMU's clock in UTC (H-rm84-rtc, [docs/research/n70-rtc.md](../../research/n70-rtc.md));
later boots skip the Time and Date queries. **Camera** (2026-11-28, test pattern, fresh boot): Imaging > Camera or
hold F10 (0xE3) shows the viewfinder; F10 takes a 1600x1200 picture ("01012007", 56 kB, shown after the capture)
[V run]; not checked in Gallery (the Menu stayed blank after leaving Camera in that run [I cause]); docs/devices/nokia-7650-nhl2/camera.md "Nokia N70". **Not checked
yet: a second boot from a kept NAND** (the 6630's open stall 26). How it was done: [how-it-was-done.md](how-it-was-done.md).

    python3 eka1/tools/ngage_phone.py --model nokian70 --phone NAME [--fresh]   # the window (NAND kept in build/ngage/phones/NAME.flash)
    python3 eka1/tools/ngage_sess.py -s NAME --model nokian70 --catch pabt,dabt,undef,fatal start   # a debug session

Keys (the 6630's): digits, F7 or keypad * = *, F8 = #, arrows = joystick, Enter = select, F1/F2 = softkeys, F3 call,
F4/Esc end, **F5 Menu**, Backspace C, F6 edit, F9 0xAA, F10 0xE3 and F11 0xBA (the N70's two extra matrix keys [I which]).
Evidence: build/evidence/rm84-home-city.png, rm84-home-screen.png, rm84-menu-grid.png, rm84-menu-imaging.png.

Prepare the images once (all git-ignored):

    python3 -I tools/fiasco.py eka1/rm84_rom/RM84_51003301.C0 --out eka1/rm84/fiasco/core
    python3 -I tools/fiasco.py eka1/rm84_rom/RM84_51003301.V05 --out eka1/rm84/fiasco/v05
    python3 -I eka1/tools/xzip_core.py 'eka1/rm84/fiasco/core/SOS*CORE@00070000.hdr' 'eka1/rm84/fiasco/core/SOS*CORE@00070400.bin' \
        eka1/rm84/core/ROM.bin --rofs eka1/rm84/output/rom/rofs.img
    python3 -I eka1/tools/rom_manifest.py eka1/rm84/core
    python3 -I eka1/tools/port_exports.py eka1/rm84/core/ROM.bin eka1/rm84/core/files.json eka1/rm84/core/analysis/user-exports.tsv
    python3 -I eka1/tools/rm1_nand.py eka1/rm84/core/nand.img --rofs eka1/rm84/output/rom/rofs.img \
        --rofx eka1/rm84/fiasco/v05/_pre@01ae0000.bin --user-start 0x2600000

## Files (git-ignored) and their SHA-256

| What | Where | SHA-256 |
|---|---|---|
| owner's archive | `Nokia_N70-1_RM-84_V14.0_SW_5.1003.3.0.1.zip` (Firmware/RM-84/: the files below, an older 5.0737 folder, a 157 MB product data package, not used) | `b56ef6c82bacc748d651638900c36087870da3225a3f8db425e9790ab06c1576` |
| core flash file (V 5.1003.3) | `eka1/rm84_rom/RM84_51003301.C0` | `b74582bd8052906ae9d7b22a53b6b01a86b574fb11493da70a3099bc4776af4f` |
| variant (used) | `eka1/rm84_rom/RM84_51003301.V05` | `e6893e9f19fd1a93e0b36136fa89d61c174233bb29328fe1f6cd15051337e190` |
| user-data area (not used) | `eka1/rm84_rom/RM84_51003301.UDA` | `489baeebca6fde592e88be39152644316a1b53ebc88460099528541ad45b94f3` |
| older core 5.0737 (not used) | `eka1/rm84_rom/5.07370/RM84_50737301.C0` | `dbda739f4052e3b86c8e3ca05bf12bd99b3784af4f2ad5dee2e8d72b2ef06f30` |
| older variants (not used) | `eka1/rm84_rom/5.07370/RM84_50737301.V02`, `.V05` | `d31f3380b9ebc74bbcd16a2035efd70ea50e00f8f10106dd96c8feb96643d54d`, `e77c6f2ba52d4996413bf3b7160bd743765ae0f393e334c79ac937dfe116443a` |
| PA sub-TOC (not used) | `eka1/rm84_rom/5.07370/PASUBTOC.bin` | `50b622fea9135b58812607bc9fa4100bb9e3679954bc66444e8c93cf10d013a8` |
| FIASCO partitions | `eka1/rm84/fiasco/core/`, `v05/`, `uda/` (`tools/fiasco.py`) | |
| inflated core ROM (VA 0x50000000) | `eka1/rm84/core/ROM.bin` (+ `files.json`, `output/rom/manifest.json`, `analysis/user-exports.tsv`) | `ca45cdd6e5c1652636688b0332f0d1c41cf88d625fbc534553e0d6db3d2afd3c` |
| core ROFS | `eka1/rm84/output/rom/rofs.img` | `fcd499775d8b2cb84e8ec094189f15203d76da8cb6e43d139177e4c63f02a43f` |
| variant ROFx | `eka1/rm84/fiasco/v05/_pre@01ae0000.bin` | `3e46ae111f6373087dc93a48db4e0d9b9bb7d67674b9e051818234b97ff7866f` |
| UDA image (a FAT16 C:, "NOKIA") | `eka1/rm84/fiasco/uda/_pre@00000000.bin` | `446b5d6ec8f982b5504fe90cfd3647ff857e678cbc77b91c2aecb8c30208c011` |
| NAND image (pristine) | `eka1/rm84/core/nand.img` (`rm1_nand.py --user-start 0x2600000`, 69 206 016 bytes) | built, not hashed |
| Ghidra inputs | `build/evidence/ghidra-rm84/` (modules cut at their VA, decompilations `*.c`) | |
| screenshots | `build/evidence/rm84-*.png` | |

## Step 1: inventory [V]

**The 6630's format, unchanged tools.** The flash files are BB5 FIASCO containers (header strings "BB5 ALGORITHM",
"OMAP1610 GBBM-1.8.1 ALG": the loader's algorithm name, not the SoC); `tools/fiasco.py` splits them; the core is
XZIP-compressed exactly as the 6630's (one raw deflate stream from partition offset 0x3D0) and
`eka1/tools/xzip_core.py` inflates it unchanged:

| Partition | Flash | Size | Content |
|---|---|---|---|
| NOLO, X-LOADER, PRIMAPP, SOS_LOADER | 0x6C0 .. 0x10600 | small | boot loaders |
| SOS_XZIP | 0x50400 | 29 696 | the core's decompressor |
| **SOS*CORE** | 0x70400 | 27 MB | the Symbian core, compressed: ROM 0x50000000 size **0xB00000** (root 0x50003C70), then a plain ROFS (0x1346ED4 bytes, iMaxImageSize 0x13F0000) |
| MCUSW, DSP0 | 0x200400, 0x20400 | 5 MB, 1.3 MB | the modem side |
| .V05 | 0x01AE0000 | 12 MB | a plain ROFx (the variant) |
| .UDA | 0 | 3.9 MB | a FAT16 boot sector ("NOKIA", "FAT16") and data: C: as shipped [I]; not used (C: is formatted at first boot, as on the 6630) |

ROM: 461 files, 386 primary and 349 secondary modules, the applications in the ROFS. Kernel ekern.exe 0x50008F9C (196 648
bytes, not the 6630's build: Kern::Fault is export 278 at 0x5001FA84, which adds 0x10000 to the reason and falls into the
fault routine 0x5001FA8C [V Ghidra]); kernel data words ported from the 6630 by matching code: current thread
**0x8000295C** (34 users agree), containers 0x8000048C (11 agree) [V run]; container entries are {DObject*, key} pairs
(`objstride=2`, as the 7710's kernel) [V run].

**The SoC: an OMAP1710 that looks like the 6630's 1610.** The bootstrap's register literals are the 6630's, page for page;
so are ekern's [V scan]. The kernel types the chip from PRODUCT_ID_MSB (0xB5F7 = 1710) and custom.ldd names "1610 1611
1710" [V]. Board: the 6630's (`-M nokian70` beside `-M nokia6630` in `nokia9500.c`, same `nokia6630_soc.c` devices).

**Modules: the 6630's set with three differences that mattered** [V module list]: (1) the modem stack is **ISC**
(IscApi.dll, IscDriver.ldd, IscMultiplexerBase.dll, IscIsiMultiplexer.ldd, IscDataTransmissionBase.dll,
IscDriverExtension.ldd) over the same xbus.ldd + ssi.pdd, where the 6630 had isa_if_driver; (2) ekeyp.dll is twice the
size (a different keypad); (3) a memory-card driver (medmmc.pdd: the RS-MMC slot) and camera drivers (camparifpdd,
campdd, camldd). mednand.pdd is the same size as the 6630's (same chip table [I]).

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | (board) the N70 machine | the bootstrap and kernel use the 6630's SoC addresses [V] | `-M nokian70`: `nokia6630_init`'s body as `rm1_board_init(machine, phone)`, a phone = name, images, core patches, OMAP1710 IDs; `ngage_run.py` MODELS `nokian70` |
| 2 | the bootstrap restarts for ever (pin-mux and clock writes repeat in the log; a breakpoint on 0x10100000 hits with sp = 0x140EF308) | the bootstrap's memory table says **64 MB at 0x10100000** (ROM +0xD20; the 6630's: 32 MB) [V]: its stack, at the top of that bank, was past the board's 64 MB from 0x10000000 | `-M nokian70` has 128 MB |
| 3 | NAND image | the ROFS (iMaxImageSize 0x13F0000) and the 12 MB ROFx end at 0x2591800, past the 6630 image's C: start (0x1000000) | `rm1_nand.py --user-start 0x2600000` (C: 25 MB) |
| 4 | EStart's Main waits in IscApi; the SSI never used (no hello) | ssi.pdd's Start (0x50150208) sets AP wake (GPIO4 bit 3, SET_DATAOUT, mask 8) but, unlike the 6630's PDD (its +0x44), never makes the pin an output: the direction register keeps 0x6F7F (bit 3 input) [V Ghidra + xp], and the board's hook passed `out & ~dir` | the GPIO output hook passes the output latch (`nokia6630_soc.c`); the link comes up (hello, link up, header/ACK) |
| 5 | the first ISI message, `res 00 msg 05` (media 0, objects 0, no data), unanswered | IscIsiMultiplexer.ldd's configuration request; reply message 06 accepted while unconfigured: [10..11] BE max message size (length + 6 checked against it), [12..13] BE the **ISC peer object** (+0xCB4), [14] a byte (+0x2E); a zero u16 faults the kernel (0x5014C4xx) [V Ghidra] | `reply 00 05 06 20 00 00 01 01` (0x2000, peer object 0x01 [I]) |
| 6 | channel open `res 00 msg 01 {40 00}` (media 01) answered with the 6630's CheckCrc reply {00 1D}: EStart waits for ever (revlonestart.dll -> Custom.ldd request 0, completed only from the ISA_KERNEL_IF open callback 0x50151D7C, never run [V catch]) | the router (0x5014BCE4) takes a message as ISC control when it comes from the peer object (+0xCB4) with media 0; control message 02 {channel, object[, status]} completes the channel's open (0x5014C274) [V Ghidra] | reply table: `rq10` (the request's byte 10), `media=`, `sobj=` (`ngage_isi.c`); `reply 00 01 02 rq10 rq10 media=00 sobj=01` (object = the channel number [I]): channels 0x40, 0x41, 0x0B open, the MTC state query is asked and answered |
| 7 | the CPU in `b .` at PA 0x100FF124 (Undefined mode, MMU off) ~70 s in | SecPdd's physical monitor (VA 0x500A9928) reads **0xFFFED400** and calls the secure ROM (0x3C00) only for 0x03330000/0x03330100/0x03330200, else `b .`; QEMU answers the OMAP310's 0x03310315. The kernel types the chip from 0xFFFE2004 (0xB5F7 = OMAP1710) [V Ghidra] | H-rm84-omap1710: the N70 board answers 0xFFFE2004 = 0xB5F7, 0xFFFED400 = 0x03330000 [I which of three]: **29 processes** at 90 s (EwSrv, Starter, DosServer, PhoneServer, DMS, ...), NOKIA logo |
| 8 | the CPU always in EHWAOMAP.pdd 0x504FE188..94; SecEnvInit reports KERN-EXEC (`38 0E` to the modem) | the 6630's DSP boot wait and SecEnvInit stalls (13, 19) [V samples, log] | the 6630's patches at the N70's addresses (code match, old words identical): H-rm84-dsp-boot, -dsp-timeout, -bal-run, -eapgwc, -secenvinit, -drmhelper (`rm1_patches_n70`) |
| 9 | DeviceManagementServer panics USER 21 four times (`38 0E` reports; restarted each time) | in asc.dll (catch on User::Panic): the N70's parser of the audio reply `1C 2E` (0x5049CEB0) keeps the 6630's [10] [11] [12] and then (0x5049D2A8) looks up **sub-block 01** through isimsg: for messages 2E/2F the count is at frame byte 15, sub-blocks from 16 (isimsg vtable 0x50143AE0 +0x10/+0x14), and reads u16 at +2 and +4 (flags 0x80, 0x800) [V Ghidra]; the 6630's 13-byte reply was overrun | `reply 1C 2D 2E 01 01 00 00 00 01 01 06 00 00 00 00` (flags 0 [I]) |
| 10 | CTelServer panics USER 22 in NOKIATSY (0x5042288C) | the SIM reply `09 32` to `09 31 {3B ..}`: the TSY wants status 01 and the SIM message class's fields 0x7F = Mid(12,4) and 0x84 = Mid(16,60) (isimsg 0x5013CF64): a frame of 76 bytes, as the 9500's; the N-Gage form had 23 [V catch + Ghidra] | the 9500's `reply 09 31 32 len=70 3B 01` |
| 11 | (keys, Menu) | ekeyp.dll (0x50085970) scans the 6630's matrix with the 6630's column words and scan-code table, plus row 4 col 2 = 0xE3 and row 5 col 2 = 0xBA [V Ghidra] | `RM84_KEYS` (`nokia6630_keys.h`), `rm1_keys_init(mpu, n70)`: city list, Time, Date, **the home screen**; F5 opens **the Menu grid** (the 6630's stall 25 fixes, `09 13` per sub-type and H-rm84-drmhelper, were already in) |

## Next

1. A second boot from a kept NAND (`--phone NAME` without `--fresh`): the 6630 faults there (its stall 26: DosServer,
   `23 04` item 0x133 and the MTC state change); check the N70 the same way.
2. Unanswered modem requests (`02 06`, `02 16`, `09 BA`, `09 DE`, `08 01`, `31 11`, `31 30`, `43 07`, `5A 0D`, `54 0D`,
   `DB 01/05`, `17 2F`, `19 0A`, `D7 09`, `41 08`; the `38 0E` panic/trace reports): each only when something waits on it.
3. Sound (DSP), network/SIM, DRM/secure environment, the memory card (medmmc.pdd), the camera cover switch (GPIO4 bit 5).
