# Nokia 6260 (RM-25) bring-up

Firmware V 5.0536.0 (01-09-05): core image `RM25PE505360.C1`, PPM `RM25PE505360.26`. S60 2nd Edition FP1, Symbian
7.0s, EKA1, DCT4 UPP: the 3230's and 7610's generation. Board `-M nokia6260` (`ngage_rm25` in `ngage.c`), a variant of
the `ngage` board like the 3230 and 7610.

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## Files (git-ignored)

| What | Where |
|---|---|
| flash files | `roms/6260/` (outside the repo), linked from `eka1/rm25_rom/` |
| ROM.bin, Z: tree, PPM tree | `eka1/rm25/output/` |
| raw PPM segments (dct4_extract) | `eka1/rm25/extracted/` |
| screenshots | `build/evidence/6260/` |

```sh
cd eka1 && python3 -I tools/eka1_extract.py rm25_rom/RM25PE505360.C1 rm25_rom/RM25PE505360.26 -o rm25/output --report
python3 -I dct4_extract.py rm25_rom/RM25PE505360.C1 rm25_rom/RM25PE505360.26 -o rm25/extracted
```

## Step 1: comparison with the 3230 [V]

- ROM 0x1D00000 bytes at 0x50000000 (3230: 0x1B00000), PPM tree at 0x51D00000 (flash 0x02D00000); 19 primary modules.
- **ekern is the 3230's byte for byte, 0x4000 higher** (the 2194 differing words are relocations, as on the 7610): the
  same kernel data (current thread 0x800017D4, containers 0x800003B8, screen chunk pointer 0x80001780), Kern::Fault
  0x50342B34, the NOS stand-in's ekern exports at 3230 + 0x4000.
- Bootstrap bank table (ROM 0x650): the 7610's, with 13 MB of ROM in chip 1 from 0x02000000 and an 8 MB device window
  at 0x04000000 (C:). The chip-ID tables moved; the 3230's Samsung 0xEC/0x22FA (type 14) on CFI02 is accepted [V run].
  ROM digest entry 0x8F18 (0x50009000..0x519BFFFF) as the others (H-ngage-md5).
- NOS: nosstarter entry 0x52894194, data 0x80002230 (0x5314 bytes from 0x52B286A4), bss 0x80007544 + 0xC6568. Hidden
  NOS module header 0x504573F0: exports 1/2/4 at 0x5289A6C0 / 0x5289A690 / 0x5289A654. enosdriver (the 3230's +0x1800)
  export 2 = 0x50453C84.
- medint.pdd is the 3230's (only the device window VA differs).
- isaapi CheckCrcMsgL: Thumb `cmp r0, #0xE4` at 0x5037AB78 (3230: 0xC0, 7610: 0x5B).
- ekeyb.dll (0x50391E40, 0x38D0 bytes) is neither the 3230's nor the 7610's: see stall 2.

## Board

`ngage_rm25` in `ngage.c`: the 7610's model with this ROM map and the NOS stand-in assembled for V 5.0536
(`build.sh`: `ngage_rm25_nos_stub.h`), ISI table `src/qemu-ngage/isi-replies-rm25.txt` (the 7610's, CheckCrc 0xE4),
keypad layout `NGAGE_KPD_RM25` (`ngage_keys.c` `rm25_keys`).

```sh
python3 eka1/tools/ngage_sess.py -s NAME --model nokia6260 --flash build/ngage/rm25-test.flash start --catch fatal,pabt,dabt,undef
python3 eka1/tools/ngage_phone.py --model nokia6260
```

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | (slow, not a stall) first boot: ~155 s white screen while C: is formatted (EStart, then RfsServer) | owner's run; a debug session paused repeatedly during the format can leave medint polling for an erase that already finished (0x50399730, DQ7 wait), so don't sample PCs during the first boot [V run] | none; keep the flash (`--flash`) |
| 2 | with the 7610's keypad, **0, Clear and End** did nothing (owner's report) | ekeyb's own 6x6 table (0x503955D8, row * 6 + line): Clear at (1, 1), edit at (2, 1), 0xA2/0xA3 at (2, 2)/(2, 5), 0xAC at (3, 0); the read helper 0x50394854 takes **sense line 5 (0xA3, 0, Call, End) from GPIO 0x0C120C04 bit 19** (the 7610: 18), the joystick from GPIO bits 6, 3, 4, 5, 7 as the 7610, and five more lines from GPIO bits 22-26 (switch entries 0xB6-0xBB) [V code] | `NGAGE_KPD_RM25`: `rm25_keys`, line 5 on bit 19, bits 22-26 idle high [I]; [V run: 0 and Clear at the time query, End from Menu back to the home screen]. Regression `test_rm25_zero_clear_end_dead_with_7610_layout` |

## State (2026-11-26)

Boots to the S60 home screen after the time and date queries; Menu grid; keys (0, Clear, End verified after stall 2)
and joystick; Camera (test pattern). No sound or network (as the 7610), time and date asked at every boot.

## Open

- GPIO bits 22-26 and the switch read through a driver call (virtual bit 28): the swivel/fold positions [I]; held at
  "not asserted". Which position that means on the phone, and whether the screen or the keys depend on it, is not known.
- 0xA2 / 0xA3 / 0xAC on F7 / F8 / F6: names [I] (EStdKeyIncVolume / DecVolume, EStdKeyDevice8).
- Sound, network: not tried. Camera (Menu > Camera) works with the test pattern (2026-11-26; H-rm25-camera,
  H-dct4-selftest-stop); its image is turned 90 degrees by the phone, the model compensates [I why].
