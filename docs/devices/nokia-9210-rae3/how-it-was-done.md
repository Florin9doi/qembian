# Nokia 9210: how it got to the Desk, and what we learned

The Nokia 9210 Communicator (RAE-3, Series 80 1st edition, Symbian 6.0, EKA1, TI925T on an undocumented Nokia ASIC)
went from a Phoenix flash package to the Series 80 Desk in one session (2026-11-20), 20 stalls, on a new board
modelled entirely from the phone's code. The record is [bring-up.md](bring-up.md); the workarounds are in
[../../hacks.md](../../hacks.md) "Nokia 9210". This page is for the next phone with no datasheet.

## 1. The shape of the work

| Phase | Stalls | What it took |
|---|---|---|
| Container | 1 | a new record type, two byte lanes, a ROM split over three flash places; found by byte statistics (opcode bytes of Thumb code in two lanes) and by validating every executable's image header against its own address |
| Bootstrap | 2-6 | the flash ID probe, a floating-bus bank test, the RAM size, the upper ROM on chip select 2 (found with `gva2gpa` after a prefetch abort) |
| Kernel and cmt.ext start-up | 7-14 | timers, interrupt controllers, the cellular software's hardware: clock ready, a serial control bus, its PMM flash (program/erase/suspend), the DSP boot handshake, the CTSI timing block, an ADC busy bit |
| Storage | 15-18 | a DataFlash behind a byte serial controller with DMA; the tick's +0x14; the memory-card door |
| UI | 19-20 | the LCD and the keyboard from two research agents; the wizard; the Desk |

## 2. Techniques that paid off

- **Byte statistics before structure.** Lane-by-lane histograms of the record data showed Thumb opcode bytes (0x1C,
  0x46, 0x68, 0xF0) in two of four lanes; a UTF-16 string read across two streams ("TDTBAMCSH-" = "DBMS-" + "TTACH")
  gave the lane layout.
- **Validate the ROM by its own image headers**: every executable's first word (UID1) must be 0x10000079/7A at the
  address the directory gives; a table of good/bad per megabyte pointed at the one missing 0x60000-byte gap, and one
  image header with its code address found the right offset.
- **The kernel's page tables tell the physical map**: `mon 'gva2gpa VA'` showed where the upper ROM and the cellular
  PMM flash really sit.
- **Floating bus**: a bank test that reads between instruction words with patterns expects, for an empty chip select,
  the last fetched word. A catch-all that returns 0 makes a bank "present".
- **Levels, not edges**: this kernel never clears a pending bit; every source is a device level. "Ready" and "idle"
  lines are simply high while idle, and drivers enable them only while they wait. Lid and door pairs re-enable the
  opposite line in their handlers.
- **Follow the waiting thread, not the busy one**: the file server waited in a reconnect of the *memory-card* drive
  while all the debugging effort went into the DataFlash driver that had finished its work. Reading the EUser path
  (`TBusLocalDrive::Caps` -> reconnect -> WaitForRequest) and the kernel's enabled sources (lids) found it.
- **Headless Ghidra** (`tools/ghidra/rawdec.sh`, ExportListing.java, RawSeed.java) replaced the MCP when it was down:
  a whole module decompiled in seconds, the listing with resolved literals (`lits.py`) for register offsets.
- **Research agents for the leaf devices**: the display and the keyboard came back right first time (both run
  checked), while the critical path stayed in the main session.

## 3. Traps

- `objects` said "processes (3)" with names missing on a new kernel: container entries can be {object, key} pairs
  (`objstride=2`) and a DThread's saved SP moves between kernel versions (+0x160 here).
- QEMU's `sendkey` holds a key 100 ms; a 10 ms scan with a debounce lost about half of them. Hold 300-400 ms.
- Macro parameter names in a key table (`Q(Q)`, `K(K)`) collide with key names.
- The DataFlash "programs" that looked like bad-block remaps were the driver initialising its reserve table; the
  driver was idle and correct; the stall was elsewhere.
