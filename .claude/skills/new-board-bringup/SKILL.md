---
name: new-board-bringup
description: Method for bringing up a phone that needs its own QEMU board, a chip QEMU does not model or a processor with no datasheet - an EKA1 phone (Symbian 6.0 to 8.1, S60 1st/2nd, Series 80/90, UIQ 2, N-Gage) on src/qemu-ngage, a new SoC variant, or a modem CPU running its own firmware (as the N95's n95-cmt). Also for an unknown baseband protocol (OSE, NOS, ISI peer). Use when starting such a bring-up, when a board's boot stalls on hardware, and before modelling a device. Covers phases, techniques that worked, traps, tools and where the per-phone records are.
---

# New board bring-up (EKA1 and unknown chips)

The phone's ROM runs unmodified, its own kernel included; we model the hardware it talks to, from its own code
when there is no datasheet. Records to read first, each written for the next phone of its kind:
- `docs/eka1-ngage.md` (start here: tools, running, debugging, performance traps)
- `docs/devices/se-p910i/how-it-was-done.md` (no-datasheet SoC, Ericsson OSE baseband)
- `docs/devices/nokia-9210-rae3/how-it-was-done.md` (board from code only, odd flash container)
- `docs/devices/nokia-6630-rm1/how-it-was-done.md` (new SoC variant of a QEMU model, BB5 flash, NAND, SSI modem)
- `docs/research/n95-cmt-emulation.md` (a modem CPU: loader, interrupts, power chip, self-tests, the link)
- each phone's `docs/devices/<id>/bring-up.md` (stall by stall)

## Rules

- Boards live in `src/qemu-ngage/` (own QEMU tree, `scripts/build-qemu-ngage.sh`), QEMU changes as
  `patches/qemu-ngage/` (one purpose per patch). After changing shared code (ngage*.c, the patches, the build
  script) run `tests/ngage-boot-check.py --model ngage / nokia7650 / nokia6600 --idle`.
- Never two emulators on one flash file. The owner's phones (`-s phone-NAME`) are for reproducing their reports
  only, once nothing else uses them.
- Every stand-in, patched word and modelled shortcut is a hack row in `docs/hacks.md` the day it is taken; every
  fixed bug gets a regression test (`tests/<board>_board_test.py`).
- Research agents get mock-level briefs (CLAUDE.md "Subagent research rules"), with the Ghidra MCP
  (localhost:8089) named. Give leaf devices (display, keys, sound) to agents; keep the critical path (boot,
  storage, modem link) in the main session. Check every agent address against the first run.

## Phases (typical: 15 to 25 stalls)

| Phase | What it takes |
|---|---|
| Container | split the flash file (`tools/fiasco.py`, `tools/fw/semc.py`, `eka1/tools/*_extract.py`); unknown formats: byte statistics per lane, validate every executable's header against its own address |
| Board to first tick | ROM/NOR where the bootstrap expects it, a logging catch-all for everything else, the bootstrap's polls answered one by one |
| Interrupts and timers | the dispatcher's code gives the controller layout; the kernel's IRQ name table names the lines |
| Storage | the media driver's single dispatcher is the stand-in point (TrueFFS, DataFlash, NAND); C: kept across boots |
| Modem/baseband link | sync handshake first, then each request the phone sends; the reply tables in the board's peer |
| UI and input | display = the framebuffer or video RAM the driver writes; keypad matrix from the driver's table |
| Start-up to the home screen | each request the phone waits for, one at a time |

## Techniques that worked

- **Start from the bootstrap's memory table** (`[phys, size, attr, virt]` entries): one rule translates every
  register address in the kernel and drivers. The kernel's page tables give the physical map (`gva2gpa`).
- **Catch-all first, models later**: one low-priority region that keeps written values, logs first accesses with
  the PC and ORs in "ready" bits; the first runs read like a to-do list. The n95-cmt probe's "learning memory" and
  map directives (`tools/cmt/n95-cmt.map`) are the same idea for a modem CPU.
- **Model the semantics the code depends on, not the chip** (level vs edge, the bits a driver polls, DMA that
  only has to complete). Floating-bus behaviour matters for bank probes.
- **Read the waiting thread, not the busy one**: the stall is where a request waits (stacks of every server,
  then PC samples), not where the effort went.
- **Debug sessions, not reboots**: `eka1/tools/ngage_sess.py` (catches that freeze with a dump, conditional
  breakpoints `VA=LABEL?expr` with unsigned compares, CPU-state trace `--debug exec,cpu,nochain --dfilter VA+4`,
  CPU sampling, reading a non-current process through its chunk's home address).
- **Name things through import stubs and frozen EUser ordinals** (`eka1/tools/port_exports.py`); port kernel
  words by literal voting from a known ROM.
- **Reverse engineering through Ghidra** (MCP at localhost:8089; headless `tools/ghidra/rawdec.sh` when it is
  down): cut a module out at its VA, set TMode for Thumb, decompile once, grep for the error constant.
- **Read the parser before answering a request**: zero is not a neutral answer, and a reply copied from another
  phone can be worse than none (USER 21 in a system-critical server takes the kernel down).

## Traps

- More than about six gdb breakpoints: none fire. Hot breakpoints change behaviour (virtual-time samples), not
  only timing. Leave catches slow a boot several-fold: catch the cold path that produces the error.
- Peripheral mappings differ by mode (a user-mode driver uses the user alias of a window).
- QEMU ends a TB at an SCTLR write (ARMv5 MMU switch code relies on prefetched instructions); shared CPU fixes
  need the boot checks of every board.
- A first boot that formats storage takes minutes with a blank screen: not a hang.
- `sendkey` holds 100 ms; scanned keypads with debounce need 300-400 ms.
- `pkill -f PATTERN` inside a command containing the pattern kills that shell: kill by saved pid.
