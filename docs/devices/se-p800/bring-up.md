# Sony Ericsson P800 bring-up

Firmware: PDA CXC162001 **R2D02** (2003-06-30, "Baseline: M3_PA4"), phone (GSM side) CXC162002 R2F, language pack 5
(EN) with its ROLF. UIQ 2.0, Symbian 7.0, EKA1. The first UIQ phone; the P910i's predecessor two generations back
(P800 -> P900 -> P910i). Goal: its own ROM to the UIQ home screen on `-M p800`, the P910i's board.

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## State (2026-11-19)

Boots to the **UIQ home screen** (Applications launcher, battery full): start-up logo, set up wizard (location,
time and date, "tell me about", complete), the start-up Power menu ("Phone on"). The pen works; settings are kept
in the phone's NOR image (`flash=`): the second boot skips the wizard (Power menu, then home). About 12 s to the
wizard. Window:

```sh
python3 eka1/tools/ngage_phone.py --model p800 [--phone NAME] [--fresh]
```

Hacks and gaps: [../../hacks.md](../../hacks.md) "Sony Ericsson P800". Method: [how-it-was-done.md](how-it-was-done.md).

## Files (git-ignored)

| What | Where |
|---|---|
| owner's archive | `Proton Drive Download - 2026-11-17(1).zip`, folder `P800/P800_R2F/` (rar files; the archive also holds P900, P990, W960, T610, T630) |
| flash files (unrarred) | `P800_Symbian_CXC162001_R2D02.sym.bin`, `P800_Phone_CXC162002_R2F.AVR.bin`, `P800_Language_5_EN.LNG.bin`, `P800_Language_5_Rolf_File.RLF.bin` |
| extraction (`tools/fw/semc.py extract`) | `build/firmware/SE-P800/R2F/` (core/, rolf-*/, modem/, z/) |
| board images | `eka1/p800/core/ROM.bin` (+ files.json, output/rom/manifest.json, analysis/user-exports.tsv), `eka1/p800/lng.bin`, `eka1/p800/rolf.bin` (payloads without the 8-byte `[flash address][size]` headers) |

`tools/fw/store.py import` skips these files ("type code/version unknown"): the store identifies SE firmware by
product (EMMA scripts / ProductInfo.rsc), which this package lacks; `semc.py identify` names every file. To do:
teach the store the P800 (type code SE-P800, version from version.txt).

## Step 1: inventory [V]

| File | Kind (`semc.py identify`) | Flash address | Content |
|---|---|---|---|
| .sym | semc-eka1 | 0x90000000, 0xFF7000 bytes | EKA1 XIP ROM, base 0x50000000, size 0x1000000, root 0x50003520, kernel data 0x80000000..0x80500000, 1182 files |
| .RLF | semc-rolf | 0xB0C00000, 0x3FFA00 | ROLF (drive A:), 54 files |
| .LNG | (unknown) | 0xB0000000, 0xC00000 | 12 MB, not a FAT boot sector: the image of the second NOR's first 12 MB [I: C: under a flash translation layer, docs/research/p800-storage.md] |
| .AVR | srec | - | the GSM side (Ericsson baseband); not needed (modelled) |

Kernel EKern.exe 0x5000EF9C (167020 bytes), variant ECust.dll 0x500AB4C0. Kernel extensions (each image header's
word 23 = the next extension's directory entry):

```
P800:  EPower -> epbusz -> Exyin -> videodriver -> critajogdial -> critakeypad -> ampctrl -> yuvtransfer -> i2ccontroller
P910i: EPower -> epbusz -> Exyin -> TouchScreenCtrl.ldd -> videodriver_ext -> critajogdial -> ekeypad -> ampctrl -> yuvtransfer -> i2ccontroller
```

Kernel-side storage: **medint.pdd, medram.pdd, efat.fsy, msfat.fsy** (+ medms.pdd Memory Stick, earolf.fsy A:);
the P910i's DiskOnChip pieces (udiskonchip, docsupportdrv.ldd, rolfdrv.pdd, ramdrv.pdd) are absent. The GSM link
modules (cspdrv, eesp, elh4drv, edldrv, erigsm.tsy, ecmux) are the P910i's set.

**Same SoC as the P910i** [V]: the kernel's IRQ name table (0x50010778, 64 names) is identical but for line 31
(P910i IrqMemoryStick, P800 IrqNotUsed0). Bootstrap memory table (ROM 0x314, 18 entries) = the P910i's with:
SDRAM **16 MB** at 0xD0000000 (P910i 32 MB); phys **0xB0000000 32 MB** mapped at 0x5E000000 and 0x5C000000 (the
P910i: 8 KB DiskOnChip window); NOR 0x90000000 16 MB; no 0x68000000 SRAM entry, no 0x90000000 128 KB I/O page.
The bootstrap issues Intel CFI commands to 0xB0000000 (0x98 query, 0x50 clear status, 0xFF read array) [V by run].

### Kernel words (ported from the P910i, `eka1/tools/ngage_run.py` MODELS `p800`)

| Word | P800 | P910i | How |
|---|---|---|---|
| Kern::Fault | 0x500221C8 | 0x50026468 | export 119, 16/16 words |
| current thread | 0x80001E88 | 0x80001E94 | literal voting, 47 agree |
| object containers | 0x800000C4 | 0x800000D0 | 18 agree |
| EUser RThread::Panic | 0x5005788C | 0x5005BEC0 | 12/12 words |

## Board

`-M p800` = the P910i's board (`src/qemu-ngage/hw/arm/p910i*.c`) with a `P910Phone` description: 16 MB RAM, the
core in the NOR at 0x90000000, a second NOR at 0xB0000000 (32 MB: lng.bin at 0, rolf.bin at +0xC00000; for now a
ROM device that logs writes, `p800-nor2:`), no DiskOnChip window. The GSM side is the P910i's OSE peer with its
own table `src/qemu-ngage/p800-ose-replies.txt` (seeded from the P910i's).

```sh
python3 eka1/tools/ngage_sess.py -s NAME --model p800 start --catch fatal,pabt,dabt,undef,panic
```

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | 5 processes (EKern, EFile, EStart, startosesvr, popcorn); black screen; the OSE link syncs (L1/LA), the StartSignal 0xC384 and battery 0xC39C go out | popcorn.exe opens the VideoDriver channel, DoRequest 0 (0x50AD4728), WaitForRequest: never completed; video RAM all zero; the panel chip got 02=01, 38=82, 1C=20, 01=20 [V by stacks + log]. The P800's videodriver.dll is older than the P910i's (binds IrqDispFifo 11, IrqDispSync 47, IrqCamErr 13) | research: docs/research/p800-display.md |
| 2 | (same) | the panel driver's power-up (0x500AE104) starts the full-frame DMA in state 3 and takes its completion (IrqDma -> 0x500AE980, result 1) only in state 4; the board's DMA finished inside the Config write, before the state change, and the completion was dropped (docs/research/p800-display.md) [V code] | H-p800-dma: on the P800 a channel finishes 2 ms after it is enabled (`p910i_lcd.c` `dma_finish`): **the start-up logo** ("Sony Ericsson P800"), popcorn exits; 7 processes |
| 3 | the logo in wrong colours (white shows cyan) | the P800's frame buffer is 12-bit RGB444 in 16 bits (EColor4K): white 0x0FFF read as RGB565 is cyan [V by eye] | the console decodes RGB444 on the P800 |
| 4 | 7 processes (appsvr, Sysagx, eventsvr); the CPU idle | startuphandler.lsu's OSE request 0x4398 was answered with the P910i's 0x4F4E; the P800's swbp stub (0x506F1FC8) waits for **0x4E86** (wait list 0x50701130) [V]. The OSE reply numbers differ between generations | `reply 4398 4E86`, `4397 4E85`; new tool `eka1/tools/swbp_waits.py ROOT --table FILE` checks every reply line against the phone's own stubs (P910i table: all found stubs agree). **20 processes, the P800 set up wizard** (build/evidence/p800/02-setup-wizard.png) |
| 5 | set up wizard (location, time and date, "tell me about", complete) and the Power menu with the pen; after "Phone on" the start-up logo stays; OSE 0x4EE8, 0x4EEB unanswered | the P910i's 0x4FB0 / 0x4FB3 shifted by -0xC8, their replies too (0x4F4C, 0x4F4F) [V: swbp_waits.py] | replies with the P910i's payloads [I] |
| 6 | still the logo; OSE 0x0533 sub 0xA6 unanswered | stub 0x506F5B5C waits for 0x16D3 (sigsel 0x50701514); decoder 0x506F5BC8: sig[3] != 0 OK, count sig[6], count x 3 bytes [V]; the shape of the P910i's SIM emergency-number list (0x413F there, where a count of 0 crashed Tele.app) [I] | `reply 0533/A6 16D3 A6 01 00 00 02 11 F2 FF 19 F1 FF` (112, 911): SIM presence and SIM reads follow (the P910i's 4331/4339/0533 replies fit [V: stubs' sigsels]), **the UIQ home screen** (Applications launcher, battery full; build/evidence/p800/03-home-screen.png), 27+ processes |

Unanswered at the home screen (probably harmless so far): 0x0A3C (send-only on the P910i), 0x0370 {E3 ...}
(to 0x30), 0x0533/B1 (waits for 0x16DE [V]), 0x13E0 (pid 8D -> 23), 0x4C2C (E1 -> 4A), 0xC39F (send-only).
