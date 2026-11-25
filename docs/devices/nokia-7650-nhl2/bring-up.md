# Nokia 7650 (NHL-2NA) bring-up

Firmware V 5.06 (`NHL23250.6C1` MCU, `NHL23250.677` PPM, 2003-06). Symbian 6.1, Series 60 1.0, EKA1:
the first S60 phone, and the same software generation as the N-Gage QD (phonet/nosdriver, the same
kernel-data layout). It runs on the N-Gage board as `-M nokia7650` (an `NgageModel` in
`src/qemu-ngage/hw/arm/ngage.c`; the stub is `nos_standin.S` built with `NHL2`).

## State (2026-11-18)

- Boots to the **home screen** (wallpaper, clock, date, softkeys Camera / Favourit.; 41 processes) after
  the first-boot time and date queries. Keypad: the 6600's model (the 7650's ekeyb is 94% the 6600's
  code with the same key lists) [V: OK accepted at both queries].
- Window: `python3 eka1/tools/ngage_phone.py --model nokia7650` (flash build/ngage/phones/nokia7650.flash;
  the first boot formats C:, white screen for about a minute).
- Check: `python3 tests/ngage-boot-check.py --model nokia7650` (all three phones after a board change).
- **Camera (2026-11-26):** works with a board test pattern (no host camera): viewfinder, still capture saved as
  JPEG under Images, stand-by with the slide closed. The phone's own camerapdd drives a modelled sensor (I2C)
  and CCP receiver; details in [camera.md](camera.md) (H-nhl2-camera). Check:
  `python3 tests/ngage-boot-check.py --model nokia7650 --camera`.
- **Keypad slide (2026-11-26):** the switch line 0x0C15020C bit 9 (H-nhl2-slide); the phone starts with it open.
  Closing it (F7, the window's Slide button, `qom-set /machine slide-closed true`) locks the keypad as on the
  phone; opening unlocks.
- Open: self-test `59 02` from object 0x36 is unanswered (the N-Gage table answers 59 01); the reply
  table is the N-Gage's, unchecked for the 7650; IrDA, Bluetooth and sound are untried. Same "boots,
  not really working yet" level as the 6600 (docs/roadmap.md).

Marks: **[V]** verified in the firmware or a run, **[I]** inferred.

## Files

| What | Where (git-ignored) |
|---|---|
| owner's archive | `NHL-02 v.05.06 7650.rar` (the flash-file archive) |
| flash files | `eka1/nhl2_rom/` |
| ROM.bin, Z: and PPM trees, reports | `eka1/nhl2/output/`, `eka1/nhl2/analysis/` |
| PPM segments for the board | `eka1/nhl2/extracted/NHL23250.677/` |
| agent test phone | `build/ngage/phones/nhl2-agent.flash` |

```sh
cd eka1 && PYTHONDONTWRITEBYTECODE=1 python3 tools/eka1_extract.py \
    nhl2_rom/NHL23250.6C1 nhl2_rom/NHL23250.677 -o nhl2/output --report
python3 dct4_extract.py nhl2_rom/NHL23250.6C1 nhl2_rom/NHL23250.677 -o nhl2/extracted
python3 eka1/tools/ngage_sess.py -s NAME start --model nokia7650 --flash build/ngage/phones/NAME.flash
```

## Format and comparison (steps 1-2)

- [V] `eka1_extract.py` reads it unchanged: ROM 0xF00000 at 0x50000000 (flash 0..0xEFA000, one range
  in chip 0), 1010 entries / 927 files; PPM tree at flash 0x00F00000 (virtual 0x50F00000), 1402 files;
  all verified. Root dir 0x502FF000, kernel data 0x80000000..0x80400000, trace mask 0xC0008003 (on).
- [V] Kernel: ekern.exe 0x50307F9C, 0x273F8 bytes, 502 exports (N-Gage 506). Kernel data layout as the
  N-Gage: screen chunk word 0x800066EC, containers 0x80000398, current thread 0x80006740 (ported
  users agree). Fatal routine 0x5030B0F4.
- [V] Primary modules: the N-Gage's set (phonet.dll, isimsg.dll, nosstarter.ext, cbus, medint, ...)
  plus ekeyb/ekdata in the primary list. No memory card drivers (the 7650 has no slot), no
  Nokia.dsy; ircomm.csy added. phonetsy 0x3EEA4.
- [V] NOS scrambled (0xC000..0x2A8000), entry 0x5148C1C4 (alias 0x51400000 as the N-Gage), data
  0x516F98CC -> 0x80006EE0, bss 0x8000B6E4 + 0xBE60C. nosdriver.ldd imports two NOS functions:
  0x514903CC (channel ctor) and 0x5148FA88 (channel open).
- [V] Ported stub addresses (`eka1/tools/port_addr.py`): Kern::Variant 0x5031B3B8 (export 464),
  SetPowerModel 0x503235FC (389), DPowerModel ctor 0x50323288 (488), Kern::Add 0x5032BEF8 (7),
  DThread::Write 0x50328918 (475; the code order as N-Gage 479), DLogicalChannel::Complete 0x5031BCD0
  (63), DPlatChunkHw ctor 0x50316BF0 (491) / create 0x50316C64 (102).
- [V] Variant vtable (0x50330EDC): Register +0xFC (slots at +0x1C), status setter +0x120 / getter
  +0x124 (word +0x90; N-Gage +0x128/+0x12C).

## First boot (step 4)

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | undefined instruction at 0x200010 (bootstrap) | the Intel probe 0x4F64 takes 0x8872 (type 8) or 0x8876 (type 7) only; our 0x8874 fell through to AMD-style probes, whose unlock cycles left CFI01 out of read-array mode, and the CPU ran status words | chip IDs per model |
| 2 | bootstrap spins at 0x51D4 | EMIF set-up 0x51A8 takes chip 0 type 7 or 12 only | chip 0 = 0x8876 |
| 3 | bootstrap halts at 0x2A84 | digest check 0x2A8C, entries at ROM 0x7FD0 (PPM) / 0x7FE8 (MCU 0x50009000-0x50EF9FFF, stored MD5 wrong as N-Gage Q12) | H-ngage-md5 with per-model entry offsets |
| 4 | ecust Fault VARIANT 13 | 0x50330BD4 needs the screen (physical 0x0A0A5000) at 0x5A0E9000; ours at 0x5A0D7000, 0x12000 short | stub reserves a 0x12000 hardware chunk (H-nhl10-hwchunk, NHL2) |
| 5 | medint fault | 0x5036CBE8 places C: by chip 1's type: 4/8/9/13 -> 0x02000000 (4 MB, the bank table's), else 0x02600000 | chip 1 = 0x8872 (type 8): two different parts |
| 6 | (slow, not a stall) EStart formats C: | "auxstart 105" | keep the flash |
| 7 | none: Starter, Startup, time/date queries, home screen | with the 6600 keypad model | |

- 2026-11-18 (owner): the joystick did nothing (number keys worked, so the menu looked stuck). The keypad
  model raised the joystick's CTSI status-2 bit only for lines enabled in 0x0C150214/16, and the values
  ekeyb leaves there do not match. Every edge now raises it; menu navigation works [V 7650 run].
