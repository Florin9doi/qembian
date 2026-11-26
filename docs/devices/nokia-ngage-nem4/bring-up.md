# Nokia N-Gage (NEM-4) bring-up

The first N-Gage (2003, "taco"), firmware V 4.03 (`nem440.3c1` MCU, `nem440.326` PPM/variant). Symbian 6.1, S60 1st
Edition, EKA1, DCT4 UPP SoC: the N-Gage QD's generation. Board `-M ngage` (`ngage.c` `ngage_nem4`); the QD moved to
`-M ngageqd` (`ngage_rh29`). Everything that applies to both (the board file, its options `ppm=`, `isi=`, `flash=`,
`mmc=`, the ISI table, `ngage_*` names) keeps its name.

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## Files (git-ignored)

| What | Where |
|---|---|
| owner's copy | `roms/N-Gage/nem440.3c1` (18 777 938 B), `nem440.326` (1 361 703 B), outside the repository |
| flash files | `eka1/nem4_rom/` |
| ROM.bin, Z: tree, PPM tree, reports | `eka1/nem4/output/`, `eka1/nem4/analysis/` (+ `user-exports.tsv`, ported by ordinal) |
| raw PPM segments (dct4_extract) | `eka1/nem4/extracted/nem440.326/` |

```sh
cd eka1 && PYTHONDONTWRITEBYTECODE=1 python3 tools/eka1_extract.py nem4_rom/nem440.3c1 nem4_rom/nem440.326 -o nem4/output --report
python3 dct4_extract.py nem4_rom/nem440.3c1 nem4_rom/nem440.326 -o nem4/extracted
```

The version: Nokia flash names are type + version + suffix (`RH2941.0C1` = RH-29 V 4.10), so `nem440.3c1` = NEM-4
V 4.03 [I].

## What differs from the QD, and where each value comes from

Every value is re-read from the ROM by `tests/ngage_nem4_board_test.py`, which reads the QD's known values the same
way first (the method check).

| | N-Gage (NEM-4) | N-Gage QD (RH-29) | How it was read |
|---|---|---|---|
| ROM.bin | 0x011C0000 B, root dir 0x50317000 | 0x01170000 B | eka1_extract [V] |
| Flash | 3 chips: ROM 0..7 MB chip 0, 0x700000.. chip 1 (flash 0x02000000), 0xB00000.. chip 2 (0x04000000): the 3650's layout | 2 chips, split at 0xF00000 | the MCU file's flash blocks [V] |
| ekern | 0x50320F9C, 506 exports | 0x5031AF9C, 506 | export table [V] |
| Stub ekern calls (ordinals 469, 394, 493, 7, 479, 64) | 0x503346B4, 0x5033C8F8, 0x5033C584, 0x503451F4, 0x50341A90, 0x50334FCC | 0x5032E7AC, ... | by ordinal [V] |
| NOS entry / NOS bss | 0x5149D284 / 0x8000C680 | 0x5149E4C4 / 0x8000C378 | nosstarter's literal pool [V] |
| nosdriver's NOS imports (open, ctor) | 0x514A0A50, 0x514A13CC (slots 0x511B4028/30) | 0x514A1C90, 0x514A260C | the slots before nosdriver's header [V] |
| Kernel data: screen chunk word, current thread, containers | 0x800066EC, 0x80006740, 0x80000398 | same | each referenced as often as in the QD's ROM [V static] |
| Bootstrap digests | ROM 0x8F00 / 0x8F18 (the board fixes the stored one, H-ngage-md5) | same | [V] |
| Trace mask (+0xD8) | 0xC020A003 (bit 30, the debug port, already on) | 0xC0008003 | [V] |
| Kern fatal-fault routine (tools) | 0x50324240 | 0x5031E340 | `port_addr.py`, unique 12/12 match [V] |
| Keypad | the 6600's ports (joystick port 0x0C15020A, read by ekeyb [V run]) with its own grid | 6x5 matrix | below |

The NOS stand-in is `nos_standin.S` (the QD's) built for NEM-4's addresses (`nos-standin/build.sh`,
`ngage_nem4_nos_stub.h`); the other stubs rebuilt byte-identical with `build/toolchains/arm-2009q3`.

### Keypad [V static, positions; I key names]

ekeyb 0x50418DD0 scans two sub-matrices as the 6600's does: the routine 0x50419D14 looks up
`list_g[line + (row << g)]` (g = 1: rows 0-1 x lines 0-1, list 0x5041B10C; g = 2: rows 2-5 x lines 2-5, list 0x5041B11C):

```
row 0: lsk   menu                          row 3: .  .  1  2  3  0
row 1: C     0xB6                          row 4: .  .  4  5  6  call
row 2: .     .     rsk   *     #   edit    row 5: .  .  7  8  9  end
```

The joystick is on the 6600's port (0x0C15020A, active low). Unmapped, the board answered 0 there, that is, all five lines
held: the reason the NEM-4 needs `NGAGE_KPD_NEM4` (the 6600's plumbing, `nem4_keys`) and not the QD's matrix. Down and
select are taken from the 6600 [I]. 0xB6 is a key the QD lacks (host F6); where it sits on the phone is not known.

## Tools

- `eka1/tools/ngage_phone.py --model ngage` (window), `ngage_sess.py --model ngage`, `ngage_run.py --model ngage`;
  the QD is `--model ngageqd` (its phone flash `build/ngage/phones/ngageqd.flash`).
- `tests/ngage_nem4_board_test.py` (no emulator): the values above, the key grid, and the machine/model/folder links.
- `tests/ngage-boot-check.py --model ngage` (reference `build/evidence/nem4/boot-ref-timequery.png`, made with
  `--save-ref` once the first UI screen is known good).

## Status

See the end of this file (updated with each run).
