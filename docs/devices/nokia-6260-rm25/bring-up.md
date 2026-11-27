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
**Infrared** (2026-11-26): two 6260s beam a Calendar entry to each other (receiver: Connect., Infrared, about one
minute of listening; sender: Send, Via infrared), same use as the 6600 (docs/devices/nokia-6600-nhl10/infrared.md,
`--model nokia6260`). The driver is the 6600's SIR.PDD; two board fixes were needed: the BlueCore UART's bits in the
shared on/off words (H-nhl10-sir) and a 0-count one-shot timer (H-ngage-timer-zero, ecomm.ldd's turnaround timer,
because the 6260's irda.esk sets no irMinLinkTurnaroundTime). Notes: docs/research/6600-irda.md "Nokia 6260".

## Swivel (2026-11-27)

The fold and the swivel display are five sensor lines, GPIO 0x0C120C04 bits 22-26 (active low) [V code]. ekeyb's
read helper 0x50394854 inverts them into virtual bits 25, 26, 27, 29, 30 (virtual bit 28 is not a GPIO line: a driver
call, `(dev 1, reg 0x1F, mask 0x10)` bit 1, the UEM's power key [I]). 0x50392CA4 decodes a position, A..E = bit 22..26
high:

| D | E | other lines | position | scan code (0x50395690) | the phone [V run] |
|---|---|---|---|---|---|
| high | high | A, B, C high | 1 | 0xB7 | normal (power-on state) |
| high | high | B, C high, A low | 2 | 0xB8 | no visible effect; keys work |
| high | high | B or C low | 0 | 0xB6 | lights off (ISI PN_LIGHT 0x3A msg 05 `03 02 01 01`; 1 sends `03 01 01 01`): **closed** |
| low | high | - | 3 | 0xBB | Camera starts, its view turned to the left (texts read bottom to top) |
| high | low | B, C high | 4 | 0xBA | Camera starts, view turned to the right |
| high | low | B or C low | 5 | 0xB9 | Camera, view turned right, a smaller viewfinder (128x96 frames instead of 176x144) |
| low | low | - | none | - | - |

The keypad DFC (0x503926C4) checks it on every scan; a new position must hold for 200 scans (position 4: 800; table
0x503956B0) before ekeyb queues its scan code (EStdKeyApplication2..7) and keeps the scan timer running while it
counts [V code]. The status pane stays upright in 3-5; only the application's area turns. Which physical position is
2, 3, 4, 5 (open with the display turned 90 or 180 degrees, folded with the display outwards) is [I]; the line pattern
the board uses for each position is one that decodes to it, not one read from a phone [I].

Board: machine property `swivel` (0-5, ekeyb's numbers; 1 at power-on): `qom-set /machine swivel 0`, in ngage_phone.py
the Fold/swivel list. A change raises the keypad's CTSI bit so ekeyb scans (H-rm25-switches). Tests:
`tests/nokia6260_board_test.py` `Swivel` (a transcription of 0x50392CA4 pinned to the ROM's instructions; each board
pattern decodes to its position).

## Open

- Swivel positions 2-5: which physical position of the fold and display each one is (see "Swivel"); the pattern of
  sensor lines the phone's magnets make in each one.
- 0xA2 / 0xA3 / 0xAC on F7 / F8 / F6: names [I] (EStdKeyIncVolume / DecVolume, EStdKeyDevice8).
- Sound, network: not tried. Camera (Menu > Camera) works with the test pattern (2026-11-26; H-rm25-camera,
  H-dct4-selftest-stop); its image is turned 90 degrees by the phone, the model compensates [I why]. In swivel position 5 Camera asks for
  128x96 frames, which the model sends since 2026-11-27 (before: 160-pixel rows, slanted stripes; regression
  `rm25_swivel5_viewfinder_garbled` in `tests/ngage_cam_test.c`). In 3-5 the pattern is turned against the view: the sensor
  is in the swivel part and turns with it [I], and the model does not turn the scene per position.
