# Nokia 7610 (RH-51) and 6670 (RH-67) bring-up

Firmware V 4.20: one MCU core image for RH-51/52/67/68 (`RH51_52_67_68PE706420.C1`) and a PPM per phone
(`RH51PE706420.26` the 7610, `RH67PE706420.26` the 6670). S60 2nd Edition **Feature Pack 1**, Symbian 7.0s, EKA1,
DCT4 UPP: the 3230's generation. Boards `-M nokia7610` and `-M nokia6670` (one `NgageModel`, `ngage_rh51` in
`ngage.c`; the 6670 is the same model under its own name), variants of the `ngage` board as the 3230.

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## Files (git-ignored)

| What | Where |
|---|---|
| flash files | `roms/7610/`, `roms/6670/` (outside the repo), linked from `eka1/rh51_rom/`, `eka1/rh67_rom/` |
| ROM.bin, Z: tree, PPM tree | `eka1/rh51/output/`, `eka1/rh67/output/` (the two ROM.bin are identical) |
| raw PPM segments (dct4_extract) | `eka1/rh51/extracted/`, `eka1/rh67/extracted/` |
| screenshots | `build/evidence/7610/`, `build/evidence/6670/` |

```sh
cd eka1 && python3 -I tools/eka1_extract.py rh51_rom/RH51_52_67_68PE706420.C1 rh51_rom/RH51PE706420.26 -o rh51/output --report
python3 -I dct4_extract.py rh51_rom/RH51_52_67_68PE706420.C1 rh51_rom/RH51PE706420.26 -o rh51/extracted
# 6670: the same with rh67_rom/RH67PE706420.26 -> rh67/
```

## Step 1: comparison with the 3230 [V]

- ROM 0x1900000 bytes at 0x50000000 (3230: 0x1B00000), PPM tree at 0x51A00000; 19 primary modules, the 3230's list.
- **ekern is the 3230's byte for byte, 0x22000 lower** (the 2194 differing words are all relocations): the same
  kernel data (current thread 0x800017D4, containers 0x800003B8, screen chunk pointer 0x80001780), Kern::Fault
  0x5031CB34, the ekern export ordinals of the NOS stand-in at 3230 − 0x22000.
- Bootstrap: the same chip-ID table (ROM 0x78A8) and combination table: the 3230's Samsung 0xEC/0x22FA (type 14) on
  CFI02. Bank table (ROM 0x650, 17 entries): ROM 0..16 MB in chip 0, 9 MB in chip 1 from 0x02000000 (PPM at
  0x02A00000), an 8 MB device window at 0x04000000 (C:; the 3230: 6 MB). ROM digest entries at 0x8F00/0x8F18.
- NOS: nosstarter entry 0x528929E4, data 0x80002230 (0x5054 bytes from 0x52B0357C), bss 0x80007284 + 0xC55D0. Hidden
  NOS module header 0x5042D620: exports 1/2/4 at 0x52898F10 / 0x52898EE0 / 0x52898EA4. enosdriver export 2 =
  0x50429EC4.
- isaapi CheckCrcMsgL: Thumb `cmp r0, #0x5B` at 0x503520F8 (3230: 0xC0).
- ekeyb.dll (0x503689F0, 0x23C8 bytes) is **not** the 3230's: it has the 6600's 20-entry key list (0x5036AD50) and
  joystick list (0x5036ADAC), but no joystick port; see stall 2.

## Board

`ngage_rh51` in `ngage.c`: the 3230's model with this ROM map and the NOS stand-in assembled again for V 4.20
(`build.sh`: `ngage_rh51_nos_stub.h`), ISI table `src/qemu-ngage/isi-replies-rh51.txt` (the 3230's, CheckCrc 0x5B),
keypad layout `NGAGE_KPD_RH51` (`ngage_keys.c` `rh51_keys`).

```sh
python3 eka1/tools/ngage_sess.py -s NAME --model nokia7610 --flash build/ngage/rh51-test.flash start --catch fatal,pabt,dabt,undef
python3 eka1/tools/ngage_phone.py --model nokia7610      # or nokia6670
```

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | (slow, not a stall) first boot: ~3 min white screen while C: is formatted, as the 3230 | 3 processes for 60 s, then 44 and the time query [V run] | none; keep the flash (`--flash`) |
| 2 | the time query takes no key: not the soft keys, not the joystick (with the 6600's keypad model) | ekeyb's read helper 0x5036A608: lines 0-4 from 0x0C150400, rows from 0x0C15040C (the 6600's matrix), **sense line 5 = GPIO 0x0C120C04 bit 18** (the 6600: 19) and the **joystick on GPIO bits 6, 3, 4, 5, 7** (active low; virtual bits 16-20 = left, up, right, down, select). The 6600 model left those GPIO bits at 0 = all joystick lines held. The lines are bound as IrqGPIO3Low..IrqGPIO7Low (ekeyb 0x50369FD4..), which the board does not model [V code] | `NGAGE_KPD_RH51`: GPIO bits 3-7 and 18 idle high, a joystick key pulls its bit low; **H-rh51-joy-ctsi**: a joystick edge raises the keypad's CTSI bit, whose ISR (0x50368F2C) scans with a read that covers the GPIO bits [V run: up/down change the time, right/down/left move in Menu, select opens Gallery]. Regression `test_rh51_keys_dead_with_6600_joystick` |

Everything after that came from the 3230's work: the BlueCore UART bit, the ALS read, the DSY audio and DevSound
0xE8 replies. The 7610 reached the **S60 home screen** right after the time and date queries
(build/evidence/7610/01-home-screen.png), **Menu** and Gallery; the 6670 the same with its own theme (62 processes).

## State (2026-11-26)

Both phones boot to the S60 home screen and Menu; keys and joystick work. No sound (DevSound answered "ended" at
once), no network, no camera (the board's camera model is for the 7650/6600/3650/3660 sensors; not tried), time and
date asked at every boot.

Unanswered ISI at the home screen (6670): TSY `43 07` (x4), `5A 00`, `31 30`, `31 11` (x4), `02 1E`, `02 16`;
`41 10` (object 0x42), `54 0E` (object 0x59), DSY `09 29 {37}`.

## Open

- Camera: both phones have one (CIF on the 7610's 1 MP sensor [I]); `camera` is off.
- The side keys (0xAA in the matrix at row 1, line 1 = F6 [I which key]); the power key (UEM) as on every board here.
- GPIO interrupts (IrqGPIOnLow/High) are not modelled; H-rh51-joy-ctsi stands in for the joystick's.
