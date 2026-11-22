# Nokia 6630: how it got to the Menu, and what we learned

The Nokia 6630 (RM-1, S60 2nd Edition FP2, Symbian 8.0a, EKA1, TI OMAP1610, BB5 modem) went from flash files to the S60
home screen and the Menu grid in two sessions (2026-11-19), 25 stalls. The stall record is [bring-up.md](bring-up.md);
every workaround is in [../../hacks.md](../../hacks.md) "Nokia 6630". This page is for the next phone on a new SoC
variant, a BB5 flash file or an FP2-era S60.

## 1. The shape of the work

| Layer | What it took | Where |
|---|---|---|
| Container | BB5 FIASCO, split by the N95's `tools/fiasco.py` unchanged; the core is **compressed** (SOS_XZIP: one raw deflate stream starting *inside* the 1 KB record header) | `eka1/tools/xzip_core.py`, docs/research/rm1-xzip.md |
| Board | the core goes to SDRAM at 0x10100000 (where the bootstrap is linked); CS0 = a 2-word jump stub | `nokia9500.c` `nokia6630_init` |
| SoC | QEMU models the OMAP310/1510 only; the 1610's differences one by one: 4-bank level-2 interrupt controller, GP timers (the tick), GPIO modules, 32 kHz watchdog, ULPD registers and widths, the APLL lock bit, no LCD controller | QEMU patches 0021-0027, `nokia6630_soc.c` |
| Storage | a NAND controller + Samsung small-page chip model; partitions come from a TOC the boot loader leaves in SRAM (the board writes it); the ROFS and the variant ROFx read straight from NAND | `nokia6630_nand.h`, `eka1/tools/rm1_nand.py` |
| Display | SoSSI + a DCS smart panel (176x208) + the 1610 LCD DMA channel | `nokia6630_lcd.h` |
| Modem | SSI (32-bit control words on channel 0, ISI payloads by DMA on channel 3), not the 9500's byte XBUS; the ISI peer (`ngage_isi.c`) unchanged | `nokia6630_ssi.h`, `isi-replies-rm1.txt` |
| Keys | ekeyp.dll's MPUIO matrix, 4 columns x 7 rows | `nokia6630_keys.h`, QEMU 0026 |
| No DSP, no secure ROM | checked one-word core patches (`rm1_patch`, old word verified): DSP boot wait, DSP request timeout, Bal ords 19/22, two plug-in reply loops, SecEnvInit, DRMHelper; a 4-word "secure ROM" at 0x3C00 | `nokia9500.c` |
| S60 start-up | ISI replies read from the parsers (CheckCrc 0x1D, NET status sub-block, audio 1C 2D, SIM service table sub-types, self-test 59 42) | `isi-replies-rm1.txt` |

## 2. Techniques that paid off

- **Ghidra MCP for all reverse engineering** (localhost:8089, schema at `/mcp/schema`). Cut a module out of ROM.bin at
  its VA, `import_file` + `set_image_base`, decompile everything into a JSON once, then grep the decompilation for an
  error constant (`0xffffffdf`, `-0x12`, ...). That found the -33 and -18 sources in minutes. Thumb code needs the
  TMode context register set (a `run_script_inline` that clears the listing, sets TMode, disassembles and creates
  functions at every `push {.., lr}`); without it Ghidra finds no functions or decodes ARM garbage.
- **RAM-loaded code (ROFS executables) from the live phone**: find the code chunk in `objects` (e.g.
  `Startup[100058f4]`), read its home address from the chunk object (+0x28, here 0xFF500000), dump it with
  `ngage_sess x`, load the dump into Ghidra at that address and name its import stubs by reading their slots live.
  This found Startup's self-test call and the Menu's DRM plug-in wait.
- **Name everything through import stubs**: ARM `ldr ip,[pc]; ldr pc,[ip]`, Thumb `ldr r3,[pc,#4]; ldr r3,[r3]; bx r3`,
  and a third form in Bal.dll (`ldr ip,[pc,#4]; ldr ip,[ip]; bx ip`) -> slot -> target -> `user-exports.tsv`.
- **Catches with Python conditions** (`VA=label?r0 == 0xffffffee`): conditions compare *unsigned* register values
  (`-18` never matches). Catch User::Exit with `r0 != 0` to see who dies and with what; catch User::Panic to read the
  category; the exit code of a thread whose E32Main TRAPs is the leave code.
- **Panic reports over ISI**: this firmware sends every panic to the modem as `38 0E` (thread name, category,
  reason): decode them from the log instead of catching.
- **"Who blocks whom"**: stacks of every server (`stack PROC::THREAD`) until one is not idle in its scheduler; then
  sample the PC of the current thread a few times. A Supervisor spinning on a register read stalls every DFC (keys
  died that way).
- **Kern::Fault "PLAT" 91** is not a kernel bug: a thread marked system-critical panicked (DThread exit 0x5001DC08).
  Catch User::Panic to find which.
- **Same-binary check**: Bal.dll is the 9500's relocated (933 of 1006 words equal), DMS is not; compare before
  porting stand-ins.
- **Grep the ROM for a code pattern** once a fix works: the EapGwc reply loop's byte pattern found the only other
  one (MuMdrcGwc) at once.

## 3. Traps

- The research doc gave the SSI DMA channels at 0x2800; the real offset was 0x1800 (a VA offset leaked in). Check
  agents' addresses against the first run.
- The modem's hello must be answered with hello (that clears the phone's "Ctrl pending" flag), and **on the next tick**:
  an immediate answer was eaten by ssi.pdd's own start-up drain read.
- QEMU ends a translation block at an SCTLR write; ARMv5 code that switches the MMU relies on the two prefetched
  instructions (patch 0025). Symptom: the CPU running through empty memory with the MMU off.
- The variant ROFx must sit at the ROFS header's iMaxImageSize (+0x2C), not at the next block (Wserv died with
  WSERV-INTERNAL 13 because Wsini.ini was missing).
- Seeded ISI replies from other phones can be **worse than no reply**: the N-Gage NET status, the 9500's SMS `02 16`
  and a SIM-table reply for the wrong sub-type each made CTelServer panic USER 21, and C32Start (system-critical) then
  took the kernel down. Read the parser (Ghidra) for every reply the boot depends on.
- Leave catches slow the boot several-fold (the timer interrupt still runs, user code doesn't): the target (DMS at
  ~60 s) was never reached in a 190 s wait. Catch the cold path that produces the error instead.
- `pkill -f PATTERN` in a shell command that contains the pattern kills that shell.
- A frozen session (a catch hit) ignores keys sent with `mon sendkey`: check `status` before concluding keys are dead.
- The first boot formats the NAND (~3.5 min to the home-city list); not a hang.

## 4. Next

Stall 26 (second boot from a kept NAND), sound (DSP), network/SIM, DRM: bring-up.md "Next".
