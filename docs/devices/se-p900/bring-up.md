# Sony Ericsson P900 bring-up

Firmware: PDA CXC162021 **R5B02** (2004-05-06, "Baseline: linn MR4_PB2"), language pack EMEA2 (IT, FR, DE; CDA162003/2
R5A07) with its ROLF. UIQ 2.1, Symbian 7.0, EKA1. Between the P800 and the P910i: the P910i's storage (DiskOnChip with
TrueFFS in udiskonchip.dll) and memory map, the P800's display driver generation (videodriver.dll) and 16 MB of SDRAM.
Runs on `-M p900`, the P910i's board with a third `P910Phone` description.

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## State (2026-11-27)

Boots to the **UIQ home screen** (Applications launcher, battery full) with no faults: start-up logo, the first-start
Language dialog (UK English, Français, Deutsch, Italiano), the P900 set up wizard (location, time and date, "tell me
about", complete), the start-up Power menu, "Phone on", home (46 processes). The pen works. C: is kept (`flash=`): the
second boot goes straight to the Power menu. No tool or board stall of its own: every piece came from the P910i or the
P800 (below). Window:

```sh
python3 eka1/tools/ngage_phone.py --model p900 [--phone NAME] [--fresh]
python3 eka1/tools/ngage_sess.py -s NAME --model p900 [--flash FILE] start --catch dabt,fatal,panic
```

Hacks: [../../hacks.md](../../hacks.md) "Sony Ericsson P900". Tests: `python3 -m unittest tests/p900_board_test.py`.

## Files (git-ignored)

| What | Where |
|---|---|
| flash files | `../roms/P900/` (outside the repo): `P900_R5B02_Symbian.bin`, `P900_EMEA2_Rolf_File.RLF.bin`, `P900_EMEA2_IT_FR_DE.LNG.bin`; no GSM-side (.AVR) file |
| extraction (`python3 -I tools/fw/semc.py extract ../roms/P900/ -o build/firmware/SE-P900/R5B02`) | `build/firmware/SE-P900/R5B02/` (core/, rolf-*/, drive-*.img, z/) |
| board images | `eka1/p900/core/ROM.bin` (= `core-eka1.img`; + files.json, output/rom/manifest.json from `eka1/tools/rom_manifest.py`), `eka1/p900/rolf.bin` (the RLF file without its 8-byte `[flash address][size]` header), `eka1/p900/c.img` (= `drive-P900_EMEA2_IT_FR_DE.LNG.bin.img`, the factory C:, whole sectors) |

## Inventory [V]

| File | Kind (`semc.py identify`) | Flash address (header) | Content |
|---|---|---|---|
| Symbian | semc-eka1 | 0x90000000, 0x17FB000 bytes | EKA1 XIP ROM, base 0x50000000, 1398 files; EKern 0x50011F9C, EUser 0x5003AE30 |
| RLF | semc-rolf | 0x91D00000 (the P910i's +0x1D00000) | ROLF (drive A:), 30 files |
| LNG | semc-fat | 0xB0000000 | FAT16 "EPOC" image of C:, 16 MB (the P910i's is 64 MB) |

Storage modules as the P910i's: udiskonchip.dll, DocSupportDrv.ldd, rolfdrv.pdd, ramdrv.pdd, MEDMS.PDD; display
videodriver.dll (10296 bytes; the P800's 10572, the P910i has videodriver_ext.dll instead); keypad critakeypad.dll
(the P800's driver; the P910i: ekeypad.dll); swbp.dll and EUser the P910i's sizes.

Bootstrap memory table (ROM 0x314): entry for entry the P910i's (DiskOnChip window 0xB0000000 8 KB at 0x5E000000,
NOR 0x90000000 at 0x5C000000 / 0x5CF00000, SRAM 0x68000000), except SDRAM **16 MB** at 0xD0000000 and the first
entry (0x24000 bytes; the P910i 0x41000).

## Ported values

| Value | P900 | P910i | How |
|---|---|---|---|
| TrueFFS dispatcher (stand-in) | 0x500A1678 | 0x500A27C4 | the P910i's first 32 bytes, unique in the ROM [V]; the stub is position-independent |
| TrueFFS FAT filter (-> 0) | 0x500A376C | 0x500A4868 | the P910i's first 24 bytes, unique [V] |
| flExit-like (-> 0) | 0x500A2084 | 0x500A31D8 | same prologue and body shape, one constant loaded differently [I] |
| Kern::Fault | 0x50025364 | 0x50026468 | export 119, 16/16 words (`eka1/tools/port_addr.py`) [V] |
| EUser RThread::Panic | 0x5005AD90 | 0x5005BEC0 | the P910i's 32 bytes, unique [V] |
| current thread | 0x80001E88 | 0x80001E94 | literal voting, 15 agree (as the P800's) [V: process list reads] |
| object containers | 0x800000C4 | 0x800000D0 | literal voting (as the P800's) [V: process list reads] |
| page directory (TTB) | 0xD0028000 | 0xD0044000 | RAM search: maps 0x80000000 and the vector page; the P800's 0xD0024000 read empty [V: dumpx = live reads] |

## Board choices

| Piece | P900 | Why |
|---|---|---|
| C: | the P910i's DiskOnChip block device and stand-in (H-p910-doc) at the P900's addresses | same udiskonchip code [V] |
| display | RGB565, DMA finishing 2 ms after enable (H-p800-dma) | videodriver.dll is the P800's generation [V: module]; the logo and the wizard show correct colours [V by eye] |
| GSM side | `src/qemu-ngage/p900-ose-replies.txt` = the P910i's table | swbp.dll is the P910i's size; `eka1/tools/swbp_waits.py eka1/p900/core --table ...` finds every stub-checked reply number the same [V] |
| input | the P910i's model (touch, keypad panel, jog, flip pins) | only the pen tested; the P900's keypad driver is critakeypad.dll (P800), not checked |

## Open

Unanswered OSE at the home screen (the P910i's and P800's open set): 0x0370 (E3 -> 30), 0x0533/B1, 0x13E0 (8D -> 23),
0x0A3C, 0x4CF4 and 0xC39F (send-only on the P910i). Not checked: jog dial, flip keypad and the flip switch, Memory
Stick, sound, network. The clock starts at 01/01/2004 in the wizard.
