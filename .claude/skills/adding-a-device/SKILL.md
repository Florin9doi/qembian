---
name: adding-a-device
description: Procedure for adding a phone to qembian's EKA2 path (Symbian 9.1 and later: S60 3rd/5th, Symbian^3, Anna, Belle, UIQ 3) or a new firmware version of a phone we already have. Use when someone brings new flash files, asks to support a new model, type code or software version, or asks whether an unknown firmware will run. Covers import, inventory, the profile, the personality check, stand-ins, the build and boot loop, validation and the "done" levels. For EKA1 phones (Symbian 6.0 to 8.1) or a chip QEMU does not model, use new-board-bringup instead.
---

# Adding a device or a firmware (EKA2)

The full reference is `docs/adding-a-device.md` (read the section you are at); the device model is
`docs/devices.md`. This skill is the order of work and the rules that cost time when skipped.

## 0. Ground rules

- Firmware never goes into git: flash files live in the ROM store `roms/`, extractions in `build/firmware/`;
  commit only names, addresses, sizes and checksums. No firmware screenshots in the repo.
- Never test on the owner's instances (`tools/instance.py list`): use `sydb --session build/sydb-<name>` or a
  throw-away instance (`tools/instance.py create NAME DEVICE --ephemeral --replace`).
- No phone literals in scripts or tools: everything device-specific goes into `models/<model>/` and is read
  through `tools/device.py`. Family files hold only what is true of every member for a reason.
- Every shortcut is a hack row (`models/<id>/hacks.md` or `docs/hacks.md`) the day it is taken; every fixed bug
  gets a regression test.

## 1. Identify and import

```
python3 tools/fw/identify.py PACKAGE_DIR_OR_ARCHIVE    # type code, version, file roles (from .vpl / sw.txt / containers)
python3 tools/fw/store.py import PACKAGE               # into roms/<RM>/<version>/; archives are left where they are
python3 tools/device.py registry RM-xxx                # known type code? which model?
```

**A new firmware of a model we have** (the common case):
```
python3 tools/device.py firmware-add MODEL RM-xxx/<version> [--rofs2 ID] [--rofs3 ID]   # writes firmware/<version>/firmware.toml
python3 tools/fw/inventory.py --device MODEL@<version>
python3 tools/device.py build MODEL@<version> --kernel
bash tests/device-smoke.sh MODEL@<version>
```
Byte patches are module+offset with the original bytes verified, so a build that stops with "expected X, found
Y" means a patched module changed in this build: `tools/fw/patches.py check FILE --device ID --search` shows where
the bytes moved. Then go to step 6 (validate). Only if something breaks, continue with the loop in step 5.

**A new model**: copy the nearest model directory to `models/<id>/` (keep `model.toml`, `keymap.toml`, the data
files that apply; drop `firmware/`), set `[device]`, `[platform]`, `[firmware] default`, empty `[status]`, add
the registry row, run `firmware-add`, then continue.

## 2. Inventory and the ROM header

```
python3 tools/fw/inventory.py --device ID              # core-flat.img, core.json, rofs*.img, symindex.json, abi/, kernel-drivers.txt
python3 tools/fw/probe.py --device ID header           # romBase/kernData -> memory model, user_window_start, fixed processes
```
0x80000000/0xC8000000 = multiple memory model (`baseport = "syborg"`); 0xF8000000/0x64000000 = moving model
(`syborgmv`). Put the printed `[firmware.rom]` numbers into the firmware overlay. `tools/device.py identify` fills
`[firmware.identity]` (the build refuses firmware that does not match).

## 3. Personality

```
python3 tools/personality/check.py --device ID
python3 tools/fw/probe.py --device ID stubs --vs <known device>        # ordered EUser stub sequence (decisive)
python3 tools/fw/probe.py --device ID exec-sizes --vs <known device>   # loader copy-in sizes
```
Zero unknown SWIs and no vtable deltas: keep the nearest personality. A few: `[personality.overrides]`. Many or a
different memory model: fork `personalities/<nearest>/` (kernel rebuild). A call mapped `!invalid` but used by
the firmware returns 0 silently and shows up later as odd behaviour: check "used but invalid" before the first boot.

## 4. Describe the hardware from the firmware

Display (`probe.py display`, wsini.ini, the display LDD's caps), keys (the keypad driver's matrix table,
`keymap.toml`, `device.py check`), timing (hal.dll's fast counter and tick: `[board.timing]`), drives
(`probe.py estart`: `[[board.drives]]` + the device's `ESTARTCOMP.TXT`, CR LF), stand-ins (compare the *client*
side, the requests it issues and buffer sizes, before reusing a stand-in of the same name; unknown -> leave it
out), modem (BB5: `family = "bb5-s60"`, own `isi-replies.txt` starts empty), GL (`[gl]` or `gl.impl = "none"`).
Details: `docs/adding-a-device.md` section 4.

## 5. Build and boot loop

```
python3 tools/device.py build ID --kernel
python3 tools/sydb/sydb.py --session build/sydb-<x> start --fresh --catch-panics --timeout 120
python3 tools/sydb/sydb.py --session build/sydb-<x> catch hits
```
Start every early boot paused with catches armed (early faults happen before a catch can be added). For any
stall, crash, hang or black screen, load the `symbian-debugging` skill first (triage order). Quick map:
unanswered modem requests -> `tools/isi.py log <session>/serial.log`; who waits on whom -> an IPC-traced ROM
(`--trace 0x80000800`) and `tools/syb_report.py`, or `tools/sydb/kwalk.py --user N`; a dying thread -> a catch
with `--action freeze --snapshot`. One wait at a time: each fix moves start-up to the next state. Patterns that
recur from phone to phone: `docs/adding-a-device.md` section 5a.

## 6. Validate and record

- `bash tests/device-smoke.sh ID` (idle, Carousel, Switch off) and `python3 tools/device.py check ID`.
- `python3 tools/isi.py log <smoke serial.log>`: unanswered requests before a stall are the ones that matter.
- Devices with `[modem_cpu]` (the N95 8GB): a `--modem real` session and `tools/cmt/isirecord.py
  <session>/modem/access.log` compare the fake modem with the phone's own modem firmware.
- Levels L0 to L6 (`docs/adding-a-device.md` section 6) go into `[status]`; `models/<id>/README.md` gets the
  identity, level, hacks and open items; `docs/device-matrix.md` and `docs/roadmap.md` get a line.
- Commit locally with explicit paths (`git commit -- PATHS`); never anything under `roms/`, `build/` or
  `firmware/`.
