# P800: how it got to the home screen, and what we learned

The Sony Ericsson P800 (UIQ 2.0, Symbian 7.0, EKA1) reached its UIQ home screen in about two hours (2026-11-19) on
the P910i's board, with six stalls. The record is [bring-up.md](bring-up.md); workarounds in
[../../hacks.md](../../hacks.md) "Sony Ericsson P800".

## 1. P800 vs P910i: one platform, three differences

The P800 and the P910i run the same Symbian (7.0, EKA1) on the same SoC: the kernel's interrupt name table is
identical but for one unused line, the bootstrap memory tables differ only in RAM size and the 0xB0000000 window,
the GSM link is the same OSE-over-UART protocol, and the P910i's input and display models worked with no or small
changes. What differed:

| | P910i | P800 |
|---|---|---|
| RAM | 32 MB | 16 MB |
| internal storage | DiskOnChip + TrueFFS (FAT C:) | Intel StrataFlash NOR with LFFS, driven by the phone's own medint.pdd |
| display | videodriver_ext, RGB565 | older videodriver.dll, **RGB444**, completes its power-up only on a DMA interrupt that arrives after a state change |
| OSE reply numbers | e.g. 4398 -> 4F4E | **renumbered**: 4398 -> 4E86 (most moved by -0xC8) |

The P990/P1i are another generation altogether (Symbian 9.1, EKA2, UIQ 3.0): they run on our own EKA2 kernel.

## 2. What paid off

- **Reuse by description:** a `P910Phone` struct (machine name, DiskOnChip yes/no) and a second machine type; the
  P910i keeps its exact behaviour (checked by a boot to its Language dialog, pixel-identical).
- **Three research agents, each right first time:** storage (medint's command set, CFI values, IRQ 45, the LNG =
  an LFFS volume), display (why request 0 never completed: a race against the driver's own state machine), and the
  earlier P910i docs as their starting point.
- **A tool for renumbered protocols:** `eka1/tools/swbp_waits.py` reads each request stub's wait list from the ROM
  and checks a reply table against it. Seeding the P800 table from the P910i's would otherwise have cost a stall
  per request; the tool turned it into one pass (validated on the P910i's own table).
- **Sub-command stubs (0533/xx):** search swbp for `movs rX, #SUB` and the sigsel literal right after; the
  decoder's shape (count + 3-byte entries) identified the emergency-number list, which must not be empty.

## 3. Traps

- **A model that completes too fast can be wrong:** the instant DMA completion that suited the P910i made the
  P800's display driver drop its completion (it changes state after starting the transfer). Delays that look like
  "realism" can be correctness.
- **Colour depth by eye:** a white screen that shows cyan is 12-bit RGB444 decoded as RGB565.
- **QEMU's pflash_cfi01 is not a general Intel flash:** two command cycles in one 32-bit store and
  interrupt-driven completion need their own model.

## 4. Next

Keys (jog dial, flip keypad) are untested; sound, network, Memory Stick, and the unanswered OSE requests in
bring-up.md. The P900 (in the same archive) is the P800's direct successor and should need even less.
