# eka1/: N-Gage QD (RH-29), N-Gage (NEM-4) and other EKA1 firmware: extraction and analysis

The N-Gage work as a whole (status, running, debugging, next steps) is in
[docs/eka1-ngage.md](../docs/eka1-ngage.md). This directory has the tools that turn the
phone's flash files into the inputs the `ngage` QEMU board and the analysis need. Each phone's
extraction has its own subfolder (`rh29/` for the QD, `nem4/` for the N-Gage, `nhl10/` for the 6600, ...).

## What is tracked and what stays local

| Path | In git | What it is |
|---|---|---|
| `tools/` | yes | extraction, naming, runner, session and window tools |
| `dct4_extract.py`, `test_dct4_extract.py` | yes | the first DCT4 container extractor: it writes the PPM segments the board loads (`rh29/extracted/RH2941.026/`) |
| `analysis/kernel-api-map.{json,tsv}` | yes | ekern's export and exec tables (addresses, ordinal labels) from a Ghidra pass; input to `identify_kernel.py`; no tool regenerates it |
| `ngage_rom/` | no | the flash files `RH2941.0C1` (MCU) and `RH2941.026` (PPM) |
| `rh29/extracted/`, `rh29/output/` | no | the QD's extracted firmware bytes (ROM.bin, the Z: tree, PPM segments) |
| `nem4_rom/`, `nem4/` | no | N-Gage (NEM-4 V 4.03) flash files `nem440.3c1` / `nem440.326` and their extraction ([docs/devices/nokia-ngage-nem4/bring-up.md](../docs/devices/nokia-ngage-nem4/bring-up.md)) |
| `analysis/` (the rest) | no | generated tables (commands below; `name_from_lib.py`, `identify_kernel.py`); eka1_extract.py's reports go to `rh29/analysis/` |
| `tmp/` | no | the Ghidra project (`tmp/ghidra`) |
| `references/` | no | third-party sources (TriX, EKA2L1, dumprom): naming references only, no code is copied |
| `nhl10_rom/`, `nhl10/` | no | Nokia 6600 (NHL-10) flash files and their extraction ([docs/devices/nokia-6600-nhl10/bring-up.md](../docs/devices/nokia-6600-nhl10/bring-up.md)) |

Firmware bytes, extractions and screenshots are never committed (CLAUDE.md).

## Regenerating everything from the flash files

Put `RH2941.0C1` and `RH2941.026` (RH-29 V 04.10) in `eka1/ngage_rom/`, then from `eka1/`:

```sh
# 1. ROM.bin, the Z: tree and the PPM overlay, with the reports in rh29/analysis/
PYTHONDONTWRITEBYTECODE=1 python3 tools/eka1_extract.py \
    ngage_rom/RH2941.0C1 ngage_rom/RH2941.026 -o rh29/output --report
# 2. the PPM segments the board loads (-M ngageqd,ppm=eka1/rh29/extracted/RH2941.026)
python3 dct4_extract.py ngage_rom/RH2941.0C1 ngage_rom/RH2941.026 -o rh29/extracted
# 3. names of user-side exports from the Series 60 1.2 SDK import libraries
python3 tools/name_from_lib.py <SDK>/epoc32/release/armi/urel      # -> analysis/user-exports.tsv
# 4. kernel names (from analysis/kernel-api-map.json and the evidence file)
python3 tools/identify_kernel.py                                    # -> analysis/kernel-*
python3 tools/ekern_align.py <Series 60 0.9 SDK WINS EKERN.DLL>     # -> analysis/ekern-align.tsv
```

`--apply` on steps 3 and 4 also labels the open ROM.bin in Ghidra (REST plugin, port 8089).
`ngage_run.py`, `ngage_sess.py` and the stack scans name addresses from
`rh29/output/rom/manifest.json` and `analysis/user-exports.tsv`, so run steps 1 and 3 first.

The N-Gage (NEM-4) the same way, into its own folder (`-M ngage`, `--model ngage`):

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/eka1_extract.py \
    nem4_rom/nem440.3c1 nem4_rom/nem440.326 -o nem4/output --report
python3 dct4_extract.py nem4_rom/nem440.3c1 nem4_rom/nem440.326 -o nem4/extracted
```
The SDKs are in the owner's archive (docs/eka1-ngage.md).

`python3 -m unittest -v test_dct4_extract test_eka1_extract_report` tests the container extractor and
the report helpers on synthetic data.

## Verified facts about the image

- `RH2941.0C1` is the MCU image. `RH2941.026` is a PPM/variant image (a second directory
  tree and localized data), not more MCU ROM.
- ROM header at flash 0: `rom_base=0x50000000`, `rom_size=0x01170000`,
  `rom_root_dir_list=0x50311000`, kernel data `0x80000000..0x80400000`.
- Flash: chip 0 (`0x00000000..0x00F00000`) is ROM offset 0; chip 1 (`0x02000000..`) is ROM
  offset `0x00F00000`.
- The ROM tree has 1156 entries (96 directories, 1060 files), all verified byte for byte
  against the source records. The header lists 16 primary and 388 secondary modules.
- The kernel `ekern.exe` is at virtual `0x5031AF9C` (size `0x2786C`).
- The PPM overlay has 1576 files, all verified.
- The NOS (cellular) code at ROM `0x00C000..0x2AC000` is scrambled (docs/eka1-ngage-questions.md
  Q20); everything Symbian is plain, uncompressed XIP.

Type-14 records use the legacy "+1" checksum adjustment; the extractors record it and go on
(`--strict-checksums` rejects it).
