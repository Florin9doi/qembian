# qembian: notes for Claude sessions and subagents

qembian: Nokia S60 phone user sides (first and most tested: N95 8GB, S60 3rd FP1, Symbian 9.2),
unmodified, on our EKA2 kernel with stand-in drivers and a fake modem, on our own Syborg board in
QEMU 11.1.3. Start with `docs/tools.md`
(debugging tools), `docs/hacks.md` (every workaround), `docs/roadmap.md` (status and queue).

## Live phone: use sydb first

For anything on a running phone (install/run/kill apps, files, processes, window groups, keys,
screenshots, keyguard, P&S, IPC strace, gdb), use the sydb bridge (`docs/sydb.md`), not
hand-made QEMU/gdb command lines:
- MCP tools `mcp__sydb__*` (server in `.mcp.json`), or the CLI `tools/sydb/sydb.py` (`--help`).
- The owner's phones are device instances (`tools/instance.py list`; `build/instances/<name>/`:
  instance.toml + drives, snapshots, logs; docs/sydb.md "Which phone"). For the owner:
  `python3 tools/sydb/sydb.py phone NAME [--fresh]` is ONE command (boots, opens the window, ends the
  session when the window goes away); never hand over start + phone. Their list: `phones.txt`.
- Agents never use the owner's instances: `sydb --session build/sydb-<name> start` (`--seed
  build/seeds/<id>` = a phone past first boot, `--fresh` = factory-fresh; `--gui` shows the window) or
  an ephemeral instance (`tools/instance.py create NAME DEVICE --ephemeral --replace`). A bare `sydb`
  (no `--instance`/`--session`) is the primary instance of `$SYBORG_DEVICE`: the owner's phone. A
  session refuses another phone's drives; `sydb stop` ends it.
- `sydb dbg ...` is the persistent gdb: hw breakpoints (`dbg cmd 'condition N $r0 == -4'`),
  bt/regs/mem/eval, app ELF symbols from `SYBCODE:` lines. `sydb strace MATCH` traces IPC.
- `sydb log --grep RE` / `events` read the session's serial log (`<session>/serial.log`).
- `sydb ui` (MCP `ui_tree`) gives the foreground app's UI as data (title, softkeys, list/menu items and
  selection, control tree), and `ui select TEXT --open` / `ui wait TEXT` drive it. Prefer it to a
  screenshot for "where am I / what is selected / what does the screen say" (docs/ui-automation.md:
  summary keys, recipes, platform status). The app answers itself through uiprobe.dll, an ECom plugin
  in every Avkon app; `--gdb` reads memory instead.
- Real modem (N95 8GB): `sydb start/phone ... --modem real` runs the phone's own modem firmware on an emulated
  modem CPU instead of the reply tables (logs in `<session>/modem/`; docs/sydb.md, docs/research/n95-cmt-emulation.md).
- Video: `sydb record` / the phone window's Record button (Ctrl+R) = screen + sound to MP4 (`docs/recording.md`;
  `src/qemu-syborg/ui/avrec.c`, identical copy in ../k750i).
- Crashes and races: `sydb catch add ...` (freeze/dump/snapshot when a thread dies as described,
  call stack included), `sydb snap save` / `sydb fork SNAP` (resume a saved machine as a separate
  session, again and again). Prefer this to breakpoints on hot paths, which change the timing.
- `tools/syb_run.py` (scripted boots with timed keys and a report) and `tools/syb_gdb.py` are still
  right for one-shot boot experiments and ROM composition (`docs/tools.md`). They kill only their
  own `sybtool` QEMUs, so they don't stop a sydb session.

## Debugging method

Before investigating a crash, hang, restart, black screen or a new firmware, load the project skill
`symbian-debugging` (`.claude/skills/symbian-debugging/SKILL.md`): triage order (deaths, unanswered
modem requests, who is blocked on whom), catches, trace builds, gdb with kernel symbols, reading ROM
and ROFS code, and the traps that gave wrong conclusions before.

## Devices (declarative device model)

One QEMU binary serves every phone; everything phone-specific is declared in
`models/<model>/model.toml` + `models/<model>/firmware/<fw>/firmware.toml` (device model v2,
`models/README.md`) (+ `keymap.toml`, `isi-replies.txt`, `code-patches.txt`, `ESTARTCOMP.TXT`, `gl/`).
Read `docs/devices.md` (schema, layering) before touching `models/`, `src/standins/`,
`personalities/` or the run scripts. For a new phone or firmware load the project skill `adding-a-device`
(`.claude/skills/adding-a-device/SKILL.md`; full flow in `docs/adding-a-device.md`;
`tools/fw/identify.py`, `tools/fw/store.py import`, `tools/device.py firmware-add`).
- No new N95 literal in a script or tool: put it in the profile and read it through `tools/device.py`
  (`list`, `firmwares`, `check`, `show`, `get`, `file`, `paths`, `qemu-args`, `identify`, `build`, `gen`).
  Device: `$SYBORG_DEVICE`, else the instance's (`$SYBORG_INSTANCE`), else `models/registry.toml`
  `default`; ids are a model, `MODEL@FW` or a firmware's own id (`nokia-n8-rm596-v13`).
- Compose a device ROM with `tools/device.py build [ID] [--variant V] [--kernel]` (hybrid-boot.sh /
  compose-exp.sh are wrappers). It refuses firmware whose `[firmware.identity]` (sw.txt, image hashes,
  hardware drivers) does not match; `tools/device.py identify DIR` tells which profile a firmware is.
- Families (`family = "bb5-s60"` -> `platform/families/bb5-s60.toml`, merged under the profile) and
  `platform/isi/replies-bb5.txt` hold only what is true of every member for a reason (BB5 modem, S60
  keys, our board/kernel set-up). Never move a value there because the devices so far happen to
  agree (screen, hardware, drives, stand-ins). When a new device differs from a family value, move
  that value out of the family into each member's profile in the same change; do not override it
  in one device and keep a false "base". Rules: `docs/devices.md` "Families".
- A stand-in that must speak another phone's protocol variant gets it declared
  (`[[standins]] config = { key = value }` -> `BoardConfig("<name>.<key>")`), never guessed from
  the arguments a client passes.
- Stand-ins: `src/standins/<driver>/` (shared headers in `_shared/`); the profile's `[[standins]]` picks
  them (bld.inf and ROM order generated). Keys: `keymap.toml` -> EKEYB table, sydb aliases.
- Personalities (`personalities/sym92/`: exec map, kernel patches, `kabi.toml`) make our EPL kernel look
  like the phone's own; verify with `tools/personality/check.py --device ID`.
- Smoke test after every platform change: `tests/device-smoke.sh [ID]` (throw-away `smoke` instance;
  idle, Carousel, Switch off), unit tests `tests/device_py_test.py`.
- Flash files are in the ROM store `roms/` (git-ignored), extractions in `build/firmware/`, each device's
  build dir in `build/roms/<id>/<fw key>/` (`tools/device.py paths`: never spell these paths in a
  script), the owner's phones in `build/instances/`. Never boot the owner's drives for a test (an
  instance in `tools/instance.py list`, or `run-gui.sh`, which boots one): clone it
  (`tools/instance.py clone NAME NAME-test`) or use a `--session build/sydb-<name>` session.

## EKA1 phones (N-Gage QD, 7650, 6600, 3230, 6630, N70, 9210, 9500, 7710, P800, P910i): their own kernel, separate boards

Symbian 6.1-7.0s phones run their own EKA1 kernel on QEMU boards in `src/qemu-ngage/` (built by
`scripts/build-qemu-ngage.sh` into `build/qemu-ngage/`), outside the device model and sydb: `-M ngage`
(N-Gage QD), `nokia7650`, `nokia6600`, `nokia3230` (DCT4 variants of ngage.c), `nokia9500` and `nokia7710` (QEMU's OMAP1510, one board with a per-phone description), `nokia6630` (the same board file,
QEMU's OMAP1510 made an OMAP1610 by patches 0021-0027; its devices in `nokia6630_soc.c`), `nokian70` (the 6630's board with the
N70's phone description: OMAP1710 IDs, ISC modem replies), `nokia9210` (Symbian 6.0; its own board modelled
from the phone's code, `nokia9210.c`), `p910i` and `p800`
(a PNX4000 board modelled from the phone's code). For a new board or chip load the project skill
`new-board-bringup` (`.claude/skills/new-board-bringup/SKILL.md`). Start with `docs/eka1-ngage.md` (tools, running,
debugging) and the phone's `docs/devices/<id>/bring-up.md`; overview in `docs/device-matrix.md` 2.2b; the
method for a new SoC and an unknown modem protocol: `docs/devices/se-p910i/how-it-was-done.md`.
Window: `python3 eka1/tools/ngage_phone.py --model M` (a fresh phone's first boot shows a blank/white screen while
C: is formatted: up to ~150 s on the 3230, not a hang; docs/eka1-ngage.md "First boot is slow"); debug sessions: `eka1/tools/ngage_sess.py`; to reproduce
an owner's report, boot their phone directly (`-s phone-NAME --flash build/ngage/phones/NAME.flash`) once no window or
session uses it (two emulators on one flash file corrupt it). After changing shared code (ngage*.c, patches/qemu-ngage/, the
build script) run `tests/ngage-boot-check.py --model ngage / nokia7650 / nokia6600 --idle`.

## Hosted EKA2: the kernel as a Linux process

The EPL kernel runs as a 32-bit ARM Linux process with the phone's ROM code native (no CPU emulation of
Symbian code); the N95 boots to its home screen in a QEMU aarch64 test VM. Start with
`src/hosted/README.md` (diagram, choices, layout), `docs/hosted-eka2/` (`roadmap.md` status and speed,
`tests.md`, `future.md`: SMP, one process per Symbian process, JIT/AOT for arm64-only CPUs; `debugging.md`: ROM code
right on QEMU but wrong on hosted, ABI checklist first) and the
"Hosted EKA2" section of `docs/hacks.md`. Window: `python3 tools/hosted/phone.py`; tests:
`python3 tools/hosted/htest.py` (env values are hex). Every shortcut goes into docs/hacks.md.

## Subagent research rules (owner's instruction)

Research agents deliver **just enough API/protocol knowledge to mock a driver or system**, never a
full reverse-engineering of it.
- Deliverable: per control/message, give the number, the argument layout, what must come back
  (values and handle kinds) and the accepted results. Add proposed stand-in code, plus a short doc
  that marks each claim [verified] or [inferred].
- Budget: about 30–40 tool calls. When it is reached, stop and report what you have.
- If something is unclear, mark it [inferred], give a best guess and move on. Don't dig.
- If the subsystem needs heavy emulation, stop early. Report in 3–4 lines with the cheapest
  alternative.
- Out of scope: internals not needed for the mock, naming everything, neighbouring subsystems.
- Don't run QEMU (`tools/syb_run.py`, `tools/syb_gdb.py`, `scripts/run-modern.sh`, `sydb start`)
  unless the brief says so. `syb_gdb` kills every running qemu, so runs collide with the main session.

## Project rules

- Don't commit proprietary binaries, firmware bytes or firmware screenshots
  (keep them in `build/evidence`).
- oFono/phonet and other GPL sources are naming references only; copy no code.
- Local commits only, no push.
- Log missing tools in `docs/tooling-ideas.md` after long debugging loops.
- Tests grow with the code: new work comes with tests, edge cases included. They need not be exhaustive
  on the first attempt (don't block progress), but the test base must grow quickly. **Every bug fixed
  gets a regression test for exactly that case** (named after the bug, saying what broke). Homes:
  `tests/` (host-side Python/C), `src/hosted/tests/` (hosted-EKA2 kernel tests, `tools/hosted/htest.py`).
