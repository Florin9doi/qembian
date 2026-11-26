# Nokia N90 (RM-42) bring-up

Firmware V 5.0607.7 (`5060773_RM42.C0` core, `5060773_RM42.V05` variant). S60 2nd Edition **Feature Pack 3**,
Symbian **8.1a**, EKA1, TI **OMAP1710**, BB5 modem: the N70's platform with the N90's own display (352x416 main panel
behind a display controller, a small cover panel on SPI). Board: the 6630's (`-M nokian90`, `rm1_board_init` with
`rm1_phone_n90`, `src/qemu-ngage/hw/arm/nokia9500.c`); method and tools as the N70's
([../nokia-n70-rm84/how-it-was-done.md](../nokia-n70-rm84/how-it-was-done.md)). Workarounds: [../../hacks.md](../../hacks.md)
"Nokia N90".

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## State (2026-11-26)

From the phone's own unmodified ROM on the **352x416 display**: NOKIA logo, first boot (C: formatted; the owner saw
it done at ~138 s, after which the screensaver shows: white with the date bar and the clock), "Select current city",
the Time and Date queries, the **Active Standby home screen** (date, shortcut bar, "No calendar entries for today",
Messag./Gallery softkeys) and the **Menu grid** (Messaging, Contacts, Calendar, RealPlayer, Gallery, Imaging, Clock,
Web, Office; it fills in a few seconds after the Menu title). No sound, network, SIM, DRM or secure environment; the
cover display is driven (its frame goes over SPI) but not shown. Clock: the RTC time read `19 0A` is answered with
QEMU's clock in UTC (H-rm84-rtc, docs/research/n70-rtc.md); a second boot from a kept NAND asks no Time or Date and
shows the host's date in the home city's zone [V run 2026-11-26]. Evidence: build/evidence/rm42-*.png.

    python3 eka1/tools/ngage_phone.py --model nokian90 --phone NAME [--fresh]   # the window (352x416 at scale 1)
    python3 eka1/tools/ngage_sess.py -s NAME --model nokian90 --flash build/ngage/NAME.flash --catch fatal,pabt,dabt,undef start

Keys: the N70's list for now (Enter select, F1/F2 softkeys, F5 Menu, arrows) [V: wake, select, Menu, Exit worked];
the N90's ekeyp.dll is twice the N70's size, so its matrix may differ [I] (not read yet).

Prepare the images once (all git-ignored):

    mkdir -p eka1/rm42_rom eka1/rm42/output/rom && cp ../roms/N90/* eka1/rm42_rom/
    python3 -I tools/fiasco.py eka1/rm42_rom/5060773_RM42.C0 --out eka1/rm42/fiasco/core
    python3 -I tools/fiasco.py eka1/rm42_rom/5060773_RM42.V05 --out eka1/rm42/fiasco/v05
    python3 -I eka1/tools/xzip_core.py 'eka1/rm42/fiasco/core/SOS*CORE@00070000.hdr' 'eka1/rm42/fiasco/core/SOS*CORE@00070400.bin' \
        eka1/rm42/core/ROM.bin --rofs eka1/rm42/output/rom/rofs.img
    python3 -I eka1/tools/rom_manifest.py eka1/rm42/core
    python3 -I eka1/tools/rm1_nand.py eka1/rm42/core/nand.img --rofs eka1/rm42/output/rom/rofs.img \
        --rofx eka1/rm42/fiasco/v05/_pre@01b70000.bin --user-start 0x2600000

(`port_exports.py` needs the N-Gage's named list `eka1/analysis/user-exports.tsv`, not on this machine: skipped.)

## Files (git-ignored) and their SHA-256

| What | Where | SHA-256 |
|---|---|---|
| core flash file (V 5.0607.7) | `eka1/rm42_rom/5060773_RM42.C0` (from `../roms/N90/`) | `09d391c5d4cc2361d7ff5d6c5939ef9e988b2ac9c878ed13e590c2feee9696a9` |
| variant | `eka1/rm42_rom/5060773_RM42.V05` | `5171868a96f650da45aba327a71b0239676951743fbfec8e9d582c7683121a32` |
| inflated core ROM (VA 0x50000000, 16 MB, root 0x50003C60) | `eka1/rm42/core/ROM.bin` (+ `files.json`, `output/rom/manifest.json`) | |
| core ROFS (0x12B40FC bytes) | `eka1/rm42/output/rom/rofs.img` | |
| variant ROFx (7 MB) | `eka1/rm42/fiasco/v05/_pre@01b70000.bin` | |
| NAND image | `eka1/rm42/core/nand.img` (ROFS at 0x100000, C: at 0x2600000) | |
| Ghidra output | `build/evidence/ghidra-rm42/` (`genericlcd_lcd.c`, `espi.c`: headless `tools/ghidra/rawdec.sh`, `GHIDRA_HOME=/snap/ghidra/49/ghidra`, `JAVA_HOME=/snap/ghidra/49/usr/lib/jvm/java-25-openjdk-amd64`) | |

## Step 1: inventory [V]

The N70's container and tools unchanged. The core ROM is larger (733 files, 564 primary; many applications are in the
core instead of the ROFS). Same drivers as the N70 byte for byte in size: ISC (IscDriver, IscIsiMultiplexer), xbus,
ssi.pdd, mednand, SecPdd, custom.ldd, medmmc, EUser, EFsrv. New: **genericlcd_lcd.ldd** (90 KB) + genericlcd_lcddev +
lcdstartupext (the N70's lcd.ldd is gone), **cuilcd.ldd/cuilcdpdd.pdd** (the cover UI), **espi.dll** (an SPI
driver). The bootstrap's memory table (ROM +0xD04) has the N70's 64 MB at 0x10100000. The kernel is another build:
current thread **0x800018C4** (40 of 40 code users of the N70's 0x8000295C agree), containers **0x80000488** (16 of 16),
Kern::Fault 0x5002039C (reason + 0x10000) falling into the fault routine **0x500203A4**. The N70's eleven patch words
all found at RM-42's addresses by a unique 16/16 masked code match, old words identical.

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | (board) the N90 machine | as the N70 | `-M nokian90` (128 MB, OMAP1710 IDs, `rm1_patches_n90`), `ngage_run.py` MODELS `nokian90`, `isi-replies-rm42.txt` (the N70's table, unchanged so far) |
| 2 | Kern::Fault 1.5 s in, in EKern's extension start-up: "Hw-error:SPI reset failed", reason -18 | espi.dll (VA 0x5003E5E0) soft-resets the SPI at VA 0x580B0C00 (= PA 0xFFFB0C00) and polls +0x14 bit 0 fifty times [V 0x5003F688..C4]; nothing there | an SPI block at 0xFFFB0C00 (`rm1_spi_init`, OMAP1710 phones) |
| 3 | EwSrv: Kern::Fault "LCD.LDD panic: " 0x1000011 (55 s) | genericlcd_lcd.ldd's controller set-up (0x5004E094) switches on the panel type (0x2000, 0x4000, 0x1000000, 0x2000000), was 0; the type comes from the main panel's RDDID over SPI (0x5004C4C8: ID low byte 0x15/0x31, bits 16-23 0x10/0x29, default 0x2000) [V Ghidra]; the SPI was modelled with the McSPI layout and the driver's transfers never completed. The real layout comes from ekern's accessors espi imports (0x5000F778..0x5000F7FC, base + offset): 18 ISR, 1C IER, 24 SET1, 28 SET2, 2C CTRL (bit 0 read, 1 write, bits 2-6 length - 1, bits 7-9 device), 30 STATUS, 34 TX, 38 RX; Lv2_SPI = entry 81 = level-2 line 49 [V] | the layout; transfers complete at once; RDDID answers 0x290015 |
| 4 | still panel type 0: device 1 (the cover panel) sent nothing after SET1 bit 5 + CTRL 0xA2 | espi's DMA mode: system DMA channel 0 (CSDP 0x602, destination 0xFFFB0C34 = TX, CCR sync 1) waits for the SPI's request [V catches on the kernel's DMA helper 0x5000E640, PA 0xFFFED800 + channel * 0x40] | the SPI raises DMA request 1 while CTRL bit 1 and SET1 bit 5 are set and sends each word the DMA writes (cut to the word length: 32-bit elements carried stray bits); RDDID read (25 bits), the panels' DCS set-up and the cover's frame go out; no more faults |
| 5 | black 176x208 screen | the main display is a controller on SoSSI's B channel (register byte + value bytes: 04..2A timing/mode, 16 = width / 8 = 0x2C, 1A = 0x1A0 = 416, 38 = window {x0, y0, x1, y1} 16-bit LE, 48 = memory write) [V SoSSI trace], not a DCS panel; 352x416 | per-phone panel size and controller mode in the display core (`nokia6630_lcd.h`); the console is 352x416 |
| 6 | the later logo twice as wide, every other column black; once that was "fixed" as RGB666 pairs, the UI (city list) in noisy colours | with register 2A = 0x87 (the UI's set-up, also 58 = 7) the LCD DMA sends 16-bit units (CSDP 0x141, 704 units a 352-pixel row) from a 0x00RRGGBB frame buffer: a pixel is two units, low half first [V trace]. (RGB666 only looked right on a white/blue logo) | 2A & 7 == 7 -> unit pairs as 0x00RRGGBB: city list, time, date, home screen, Menu grid |
| 7 | the start-up logo (first seconds) twice, side by side in the top half, pink (owner's report) | that logo is written by the CPU through the FIFO with 2A = 0x81 (SoSSI bus format 0x2F): each 32-bit word is two RGB565 pixels (0873 dark blue, EF7F a light edge) [V trace] | 2A & 7 != 7 -> one RGB565 pixel a unit |

## Next

1. The N90's own keypad (ekeyp.dll 10.8 KB vs the N70's 5.9 KB): read its matrix and scan-code table; the N90's
   extra keys (camera, the hinge/fold switches).
2. The cover display (device 1 on SPI: 128x128 [I], CASET 1..0x82): show it as a second view, as the Communicators'
   cover panel; the fold/twist state (which screen is active).
3. Panic reports `38 0E` (three, `61 01 01 5A..5C`) and unanswered requests (`02 06`, `02 16`, `31 11`, `43 07`,
   `77 01/07`, ...): each only when something waits on it.
4. A second boot from a kept NAND; sound (DSP), network/SIM, DRM.
