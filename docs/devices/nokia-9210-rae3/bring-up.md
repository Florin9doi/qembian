# Nokia 9210 Communicator (RAE-3) bring-up

Firmware V 4.13 (`RAE33004.130` + a language file `RAE33004.13x`, December 2001). Series 80 1st edition, **Symbian
6.0** (the only Symbian OS version with no phone here before it), EKA1, TI925T core on a Nokia ASIC with no public
datasheet. The board is modelled from the phone's own code, as the P910i's was.

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## State (2026-11-20)

**The Series 80 Desk** on `-M nokia9210` (`src/qemu-ngage/hw/arm/nokia9210.c`), from the phone's own unmodified ROM:
first boot ~95 s to the first-boot wizard (Home city, Time and date, My contact card, "Startup completed"), then the
Desk; a second boot from the kept DataFlash (C:) goes straight to the Desk in under a minute. The inner 640x200
screen, the QWERTY keyboard, the command buttons and the application keys work: Desk, Telephone (its directory,
"No contacts"), Messaging (Inbox, Outbox, Drafts, Sent), Internet, a note typed on the Desk. Evidence:
`build/evidence/rae3/01-home-city.png` .. `09-note-typed.png`.

Not modelled: the cellular side's radio (cmt.ext runs on this CPU; its DSP is a boot-handshake stand-in, no network,
no SIM), sound, the cover display and keypad (phone side), the memory card (the door reads open: no card), IrDA,
the serial ports, the RTC (the clock starts at 01:01 or the last saved time).

    python3 eka1/tools/ngage_phone.py --model nokia9210 --phone NAME [--fresh]   # the window (C: kept in build/ngage/phones/NAME.flash)
    python3 eka1/tools/ngage_sess.py -s NAME --model nokia9210 --catch pabt,dabt,undef --flash FILE start   # a debug session

Keys: the QWERTY as typed (letters, digits, Enter, Backspace, Space, Esc, Tab, arrows, Shift, Ctrl, Alt = Chr,
CapsLock, comma, full stop, slash); **F1-F4 the command buttons beside the screen (F1 = the top one, OK)**; F9 Menu,
F10 Help; F5 Desk, F6 Telephone, F7 Messaging, F8 Internet, F11, F12, Insert, Delete the other application keys.
Scripts: hold keys >= 300 ms (`mon 'sendkey e 400'`): QEMU's default 100 ms press is lost between two keyboard
scans about half of the time.

Prepare the image once (git-ignored):

    mkdir -p eka1/rae3_rom && (cd eka1/rae3_rom && unzip /path/to/Rae3_mcu_v4.13.zip)
    python3 -I eka1/tools/rae3_extract.py eka1/rae3_rom/RAE33004.130 eka1/rae3/core/ROM.bin
    python3 -I eka1/tools/rom_manifest.py eka1/rae3/core
    python3 -I eka1/tools/port_exports.py eka1/rae3/core/ROM.bin eka1/rae3/core/files.json eka1/rae3/core/analysis/user-exports.tsv

## Files (git-ignored) and their SHA-256

| What | Where | SHA-256 |
|---|---|---|
| owner's archive | `Rae3_mcu_v4.13.zip` (the main file, 21 language files, Phoenix .ini/.pp/.gen/.wug/.trc) | `cc3cbfb06d87e34a51552029878ff8c00f6f9ad4a91a14a038bbaa2d7513d4fe` |
| core ROM (VA 0x50000000, 0xBD0000 bytes, 684 files) | `eka1/rae3/core/ROM.bin` (+ `files.json`, `output/rom/manifest.json`, `analysis/user-exports.tsv`) | built by `rae3_extract.py` |
| Ghidra inputs and decompilations | `build/evidence/ghidra-rae3/` (`ekern.c`, `Medata.c`, `epbus.c`, `ecust.c`, `ekeyb.c`, cmt.ext windows) | |
| screenshots | `build/evidence/rae3/` | |

Ghidra: the MCP (port 8089) was down during this bring-up; everything went through headless Ghidra,
`tools/ghidra/rawdec.sh BIN BASE OUT.c [arm|thumb] [ENTRY...]` (`LISTING=1` also writes the disassembly listing).

## Step 1: the container and the ROM [V]

The files are the DCT3-era Phoenix container ("DCTL", "LINDA ALGORITHM", the 0xA2 header of `eka1_extract.py`) with a
record type of its own (`eka1/tools/rae3_extract.py`, test `eka1/test_rae3_extract.py`):

| Record | Layout | Content |
|---|---|---|
| 0x19 | type, u24 sector, u16 device (1 or 8), u24 word address, checksum, u24 size, header checksum | two 8-bit lanes: even bytes chip A, odd bytes chip B; the word address is the chip offset |
| 0x14 | type, u32 bus address, checksum, u24 size, header checksum | plain bytes |

The EKA1 core ROM is chip A of device 1 (ROM 0..4 MB), chip B of device 1 (4..8 MB), then device 8 chip A from offset
0x24DE00 (ROM 0x800000) continued by the 0x14 records at bus 0x02000000 + X (ROM 0x800000 + X from 0x9D0100). Read as
one stream the data looked like Thumb with its opcode bytes in two lanes; one lane alone had the ROM header (base
0x50000000, size 0xBD0000, root 0x5089AAAC). Mapping device 8 from its offset 0 left 26 files blank; the right offset
came from Calicovw.app's image header. The language files add a language area (device 8 below 0x24DE00, both chips)
not needed for the core.

Modules: EKern, EUser, EpBus (peripheral bus: the memory card), HAL, EFile, ECust (the variant), Medata.pdd
("Media.SFlash": C:), medcrm.pdd ("Media.MmcF"), enos.ldd/PHONET/isimsg/phonetsy (the cellular link), **cmt.ext
(2.3 MB: the cellular software runs on this CPU as a kernel extension)**, EKeyb/Ekdata, ScDv, FbServ, EwSrv, Desk,
Telephone, Messaging ... The kernel names its interrupts: 57 sources, "Fiq..." (controller 0x05006000) and "Irq..."
(0x05005000), the same sources in the same order (Timer0-4, DMA0-3, LidClosed/Open, MediaLid, IrDA, Media, SerialFlash,
Ctsi, Acc, KeyPress/Depress, Battery, FlashCopy, LcdMatch, Dtr, FlashProtect, MpuGenIo6, Gpt32k, Gpt32kWb).

Kernel words (code match against the N-Gage's 6.1 kernel): current thread **0x80007214** (32 users agree), containers
**0x80000264** (11 agree); a DThread's saved SP is at **+0x160** (+0x158 on 6.1 and later) [V].

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | the ROM is not a plain stream | two byte lanes; the upper part on device 8 at 0x24DE00 and bus 0x02000000 [V above] | `eka1/tools/rae3_extract.py` |
| 2 | (board) CPU, memory map | the bootstrap's CPU table accepts Main ID 0x54009250 & 0xFF00FFF0 (TI925T) [V 0x50000A50]; flash at 0, ROM banks {0, 0x01000000, 0x02000000} [V 0x50000DD0], EMIF at 0x04000000, peripherals at 0x05000000 (kernel VA 0x58000000 = PA + 0x53000000) | `-M nokia9210`, a logging catch-all with "ready" and self-clearing bit tables |
| 3 | the bootstrap blinks its error LED (0x05007000 writes, for ever) | its flash probe (copied to and run at 0x05011000): strb 0x90 / ldr / strb 0xFF per chip select; bank 0 = **0x88640089** ends the probe, else every chip select must answer 0x88960089 [V 0x50000B30..B6C] | Intel ID mode in the flash model; bank 0 = device 0x8864 |
| 4 | the bootstrap's bank test on chip select 2 | it reads the bank between instruction words with known patterns: with no chip the floating data bus returns the last fetched instruction [V listing 0x500006F0] | (first a floating-bus region; then stall 6 put a chip there) |
| 5 | the bootstrap's stack and RAM size | stack at 0x03FFFFF0, size probe at 0x03800000 [V log] | 16 MB at 0x03000000 |
| 6 | prefetch abort at 0x50C00000 (past the ROM) | the kernel maps ROM VA 0x50BCF000 to PA 0x023CF000 [V gva2gpa]: the upper ROM is at 0x02000000 + offset | ROM 0x800000.. behind chip selects 1 and 2 |
| 7 | the kernel waits for timer 3 | timers at 0x05007000 + 0x20 n (+0x04 run, +0x08 expired, +0x10 count, +0x18 reload), a clock/power block at 0x05002000, two 64-source interrupt controllers (+0x08 enable, +0x0C disable, +0x10 pending, +0x14 pending index + 1) [V accessors] | timers (13 MHz [I]) and controllers in the board |
| 8 | cmt.ext polls 0x05003000 bit 11 | after a clock enable at 0x05002004 [V 0x50A8F4A8] | ready bit |
| 9 | cmt.ext polls 0x050121B4 | a byte serial interface to the power/control ASIC (+0xB0 out, +0x1B0 in, status bits 1/2) [V 0x50A1DEF2]; it was inside my guessed SRAM | ready bits; the "SRAM" is 4 KB at 0x05011000 |
| 10 | data abort writing VA 0x5A050000 (pmmwrite.dll) | the cellular side's permanent memory (PMM) is Intel flash at the top of chip select 2 (VA 0x5A040000 = 0x023F0000, continuing past 4 MB) [V gva2gpa] | chip select 2 = 8 MB, Intel program / block erase / suspend / status in the flash model |
| 11 | pmmwrite.dll suspends its erase for ever | it suspends while 0x05006010 / 0x05005010 show anything pending, and the model returned raw levels (a never-enabled source) [V 0x500BA4B4] | +0x10 / +0x30 = pending & enabled |
| 12 | cmt.ext waits for API+0xFE | **the DSP boot**: 127 blocks of 512 halfwords to the DSP's API RAM (0x05011000 + 0x200), an alternating handshake on API+0x100 / API+0xFE, then API+0x002 != 0xFFFF [V 0x50A1EF9E..F06A] | H-rae3-dsp: the API RAM answers each handshake |
| 13 | cmt.ext polls 0x0501203C, then an interrupt storm | the **CTSI** timing block: +0x40/+0x44 counter, +0x48/+0x4C compare, +0x20 raw status, +0x24 interrupt status (write 1 clears) masked by +0x2C, +0x3C a sync byte; the handler loops while +0x24 & ~+0x2C [V 0x50A1D9DE] | CTSI model, source 33 = +0x24 & ~+0x2C |
| 14 | cmt.ext waits for 0x05012060 bit 7 | a conversion start/busy bit (an ADC [I]) [V 0x50A1F59E] | self-clearing bit |
| 15 | EFile starts, C: mount stops | **C: is an Atmel AT45DB321-class DataFlash** (528-byte pages) on a byte serial controller at 0x0500B000 (+0x0C select, +0x04 transmit, +0x08 receive, +0x10 busy, +0x14 data there), page data by **DMA** (4 channels at 0x05009000: +0x14 source, +0x20 destination as 24-bit bus addresses, +0x2C bytes - 1, +0x08 start); commands 0x52/0x53/0x55/0x60/0x61/0x84/0x87/0x83/0x86/0x50/0x57; the bootstrap reads its status once after 0x57 and wants (status & 0x38) == 0x30 [V Medata.pdd, kernel 0x5000130C] | `nokia9210_dataflash.h` + controller + DMA in the board (test `tests/nokia9210_dataflash_test.c`); the driver scans all 8192 spare areas and initialises its 63-page reserve |
| 16 | the kernel's start-up waits on source 55, later only 56 is enabled | the 32 kHz timer (0x05016000) drives "Gpt32k" (55) and "Gpt32kWb" (56) [V intc in runs] | both |
| 17 | every kernel timer stops after one tick | the tick handler writes the next period to +0x14 and clears +0x08 [V 0x500038F4]; the model kept +0x14 as plain storage | +0x14 = the next period (reloads at 0, restarts a stopped timer) |
| 18 | the file server waits in TBusLocalDrive::Caps of the memory-card drive | EUser reconnects a not-ready drive with an asynchronous kernel call; the card stack (EpBus.dll: host at 0x0500A000, status +0x18, interrupts MediaCmdIdle/DataIdle/Response/LidOpen/LidClosed) waits for a card-door state [V EpBus 0x5005EDF8, intc]; with the door closed it powered the absent card up, timed out, and still never completed the reconnect | the lids as levels: the Communicator lid open (source 10), **the card door open (11: no card)**; DataIdle/CmdIdle high, a response timeout for any command: **28 processes, EwSrv, EikSrvs, Desk, Telephone ...** |
| 19 | the inner screen | the LCD is in the 0x04000000 block: +0x20 frame start 0x03000000, frame data at +0x20 from it, 640 x 200 EColor4K (0000RRRRGGGGBBBB) [V ECust + the bootstrap; docs/research/rae3-display.md] | a QEMU console: **the first-boot Home city dialog** |
| 20 | keys | EKeyb.dll: a 10 x 10 matrix at 0x05013000 (+0x20 column drive, +0x04 rows active low), source 39 = any key down, scan codes table[c * 10 + r] at 0x508A17F1 [V docs/research/rae3-keys.md] | `nokia9210_keys.h`: the wizard (F1 = OK) and **the Desk**; a second boot from the kept DataFlash goes straight to the Desk |

## Next

1. The cellular side: cmt.ext runs the GSM stack on this CPU against a DSP stand-in; no SIM, no network. Its SIM
   interface, the DSP's per-frame protocol and the CTSI frame interrupts are the next layer (the phone application
   opens, "No contacts").
2. The cover display and keypad (phone side), sound, IrDA, the RTC (time at boot), the memory card (a card image
   behind the 0x0500A000 host).
3. The language area (device 8 below 0x24DE00 in the .13x files) is not loaded; the core ROM's English is used.

## Nokia 9210i (RAE-5, firmware 6.00) on `-M nokia9210i`

State (2026-11-26): **the Series 80 Desk**, from the phone's own unmodified ROM, on the 9210's board with three
differences. First boot ~2 min to the Home city wizard (the phone formats its blank C: itself: EPOC FAT12,
15360 sectors), F1 through the wizard to the Desk; a second boot from the kept C: goes straight to the Desk (with
the "Time and date" query: no RTC, as on the 9210). Evidence: `build/evidence/rae5/01-home-city.png`,
`02-desk.png`, `03-second-boot-desk.png`.

    python3 -I eka1/tools/rae3_extract.py RAE53006.000 eka1/rae5/core/ROM.bin --rae5 [RAE53006.00A]
    python3 eka1/tools/ngage_phone.py --model nokia9210i --phone NAME [--fresh]   # C: in build/ngage/phones/NAME.flash

| # | Stall | Evidence | Fix |
|---|---|---|---|
| i1 | the bootstrap's chip select 2 test | CS2 is a device, not ROM: an address-line test, then byte 0x02002000 must read 0x30 after 0xA5 twice to +4 [V 0x50000C8C] | no flash on CS2; 0x30 as a ready value (the DiskOnChip Millennium's chip ID) |
| i2 | prefetch abort at 0x51000000 (the CPU in a forward slide from 0x50DC4C46) | cmt.ext unlocks its PMM (0x60 + 0xD0 + 0xFF on each 8 KB block of bank 1 0x7F0000..0x7FE000) **while running from bank 1** [V 0x50DC3AEC]; with one command mode per chip, every fetch after 0xD0 read status 0x0080 (Thumb `lsls`, a NOP sled). On the 9210 the PMM is on chip select 2 | H-rae3-rww: per-partition (512 KB [I]) command modes; 8 KB parameter blocks in the chip's top 64 KB. Test `Rae5PmmUnlockRunsFromSameBank` |
| i3 | white screen; EFile's C: mount stops on the DiskOnChip | C: is a **DiskOnChip Millennium** behind **TrueFFS 4.x linked into Medata.pdd** ("Media.DiskOnChip", ARM). Its wrapper object (0x5008CBEC..0x5008D6F8: init 0x5008CC54, read 0x5008CC9C, write 0x5008CEAC, format 0x5008D10C) calls one dispatcher, **flCall(ctx, func, IOreq) at 0x5008C730**, which runs the DOC init 0x5008CA84 on first use; func 4 mount, 0x12 info (irData word 0 = sectors), 5 read, 6 write, 8 delete, 3 defragment, 0x10 format, 1 dismount, 0x13 after each; 0x37 = not mounted [V listing] | H-rae5-doc: `doc_standin_rae5.S` over flCall, block device at 0x0503F000 (VA 0x5803F000), C: in `flash=FILE`. Tests `Rae5DocStandin` |

Decompilation: `build/evidence/ghidra-rae5/Medata.c` (+ `.lst`). The DataFlash controller of the 9210 is still
instantiated but unused (the 9210i's Medata.pdd never touches 0x0500B000).

