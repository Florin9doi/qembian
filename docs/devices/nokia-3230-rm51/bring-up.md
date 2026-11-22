# Nokia 3230 (RM-51) bring-up

Firmware V 05.07 (`RM51PE507170.C1` MCU core, `RM51PE507170.26` PPM/variant). S60 2nd Edition **Feature Pack 1**,
Symbian 7.0s, EKA1, DCT4 UPP SoC: the era between the 6600 (2nd Edition) and the 8.0a/8.1a phones. Board
`-M nokia3230`, a variant of the `ngage` board (as the 6600 and 7650).

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## Files (git-ignored)

| What | Where |
|---|---|
| owner's archive | `RM-051 v.05.07 3230.rar` |
| flash files | `eka1/rm51_rom/RM51PE507170.C1` (27 948 499 B), `.26` (5 585 733 B) |
| ROM.bin, Z: tree, PPM tree, reports | `eka1/rm51/output/`, `eka1/rm51/analysis/` (+ `user-exports.tsv`), `eka1/rm51/files.json` |
| raw PPM segments (dct4_extract) | `eka1/rm51/extracted/` |

```sh
cd eka1 && python3 -I tools/eka1_extract.py rm51_rom/RM51PE507170.C1 rm51_rom/RM51PE507170.26 -o rm51/output --report
python3 -I dct4_extract.py rm51_rom/RM51PE507170.C1 rm51_rom/RM51PE507170.26 -o rm51/extracted
```

## Step 1: inventory and comparison with the 6600 [V]

- Same Phoenix A2 / DCT4 container; `eka1_extract.py` reads it unchanged: ROM 0x1B00000 at 0x50000000, 1717 files
  (all verified), 19 primary and 666 secondary modules; PPM tree at virtual 0x51B00000.
- **Same generation as the 6600:** the primary list is the 6600's without testbluetooth/testhci; ekern 0x29790 bytes,
  478 exports in the same order (Kern::Variant 426, Power 344/451, Kern::Add 7, DPlatChunkHw 454/93 all match by
  ordinal); the kernel data layout is the same (current thread 0x800017D4, containers 0x800003B8, screen chunk
  pointer 0x80001780: literal voting, 48/17/4 users agree); Kern::Fault 0x5033EB34 (16/16 words).
- **Flash:** three chips at `n << 25` (the bootstrap's chip loop checks 0..2; the 6600: 0..3). ROM 0..16 MB in chip 0,
  the rest in chip 1 from 0x02000000 (PPM at 0x02B00000); a 6 MB device window at 0x04000000 (C:) [bank table ROM
  0x64C]. **Chip types:** the bootstrap accepts only types 2, 8, 10, 13-17 (combination table ROM 0x7A60; ID table
  0x7910: AMD 0x01/0x257E.., Samsung 0xEC/0x22F0..0x22FE) and medint.pdd's type switch (0x503959BC) has no ST/Intel
  types: the 6600's ST part (type 11) halts the bootstrap. The bootstrap's ID read tries an Intel query, then the
  AMD autoselect (unlock 0xAA at 0xAAA, 0x55 at 0x554).
- **NOS:** nosstarter calls the entry 0x528943A4 with data 0x80002230 (0x532C bytes from 0x52B247AC) and bss
  0x8000755C + 0xCD894 (6600: 0x52882024, 0x80002210, 0x80006D44). The hidden NOS module enosdriver links
  against: header 0x50455BF0, exports 1/2/4 at 0x5289AA88 / 0x5289AA58 / 0x5289AA1C. enosdriver export 2 =
  0x50452484.
- ROM digest entry at 0x8F18 (0x50009000..0x519BFFFF) as the 6600 (H-ngage-md5 default).

## Board

`ngage_rm51` in `ngage.c`: three chips, ROM map {0..16 MB chip 0, 16 MB.. chip 1}, the 6600's NOS stand-in
(`nos_standin_nhl10.S`, now parameterised: build.sh assembles `ngage_rm51_nos_stub.h` with the 3230's addresses),
trace mask bit 30, **AMD/Samsung NOR** (`flash_amd`: QEMU's pflash_cfi02, Samsung 0xEC/0x22FA = type 14 [I which part
the phone has]; QEMU patch 0020 adds `pflash_cfi02_get_memory` so the board can load the image), the 6600's keypad
model for now, ISI table `src/qemu-ngage/isi-replies-rm51.txt` (the 6600's, CheckCrc byte 0xC0).

```sh
python3 eka1/tools/ngage_sess.py -s NAME --model nokia3230 start [--flash FILE] --catch fatal,pabt,dabt,undef
```

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | bootstrap halts at 0x63B0 (chip check) | the 6600's ST chip ID gives type 11, not in the 3230's combination table (0x7A60) [V] | Samsung 0xEC/0x22FA (type 14) on QEMU's CFI02 (AMD command set): medint supports it (switch 0x503959BC: types 8/13/14) [V] |
| 2 | (EStart) the CheckCrc reply | isaapi's Thumb `cmp r0, #0xC0` at 0x50376B88 (6600: 0x82) [V] | `reply 00 01 02 00 c0` |
| 3 | (slow, not a stall) first boot: C: is formatted twice (EStart's format, then the restore-factory-settings script's `FORMAT C:` from RfsServer, after Starter's `RRfsClient::PerformRfs`) at ~0.7 s per 64 KB sector: ~150 s to the splash | session-call trace (EUser SendReceive/CreateSession) + QEMU `pflash_*` trace: every sector erased and completed [V] | none; keep the flash (`--flash`): the next boot reaches the time query in ~20 s |
| 4 | 17 processes, the NOKIA splash; the CPU always in BTTestServer | BC02_UART1.pdd (CSR BlueCore on a UPP UART) polls 0x0C150572 bit 7 before each byte (0x5041385C, no timeout) [V] | H-rm51-bt-uart: the board's known-register table forces that bit [I: transmitter ready]: **42 processes, the first-boot time query** |
| 5 | the time query ignores OK: F1 increments the hour, Enter/F2/F5/Up do nothing | the 3230's ekeyb reads one 6x6 table (0x503905F0, row * 6 + line) on the N-Gage's ports with the joystick inside the matrix (rows 0-1); it never reads the 6600's joystick port 0x0C15020A; switch line GPIO 0x0C120C04 bit 20 (docs/research/rm51-keys.md) [V] | `ngage_keys.c` `rm51_keys` (the N-Gage layout with edit and C swapped, 0xAC side key on F6), layout enum `NgageKpdLayout`; regression `test_rm51_keys_match_scan_table`. The time and date queries take OK: **59 processes, the status pane** (clock, signal, battery) |
| 6 | the idle area stays white: Phone.app blocks in vmbx `RVmbxNumber::GetAlsLine` | the TSY (object 0x45) sends SIM `09 0F {0B 00 00}` (the N-Gage's ALS read `09 31 {3B 00 00}` renumbered); the N-Gage's reply layout under the new numbers is taken (the TSY reads a second record `{0B 00 00 01}`) but GetAlsLine does not return [V by run]. Also answered from the 9500's table: `09 09`, `09 1D` [I] | MMGSMWCDMA's reply handler 0x507C1C7C: [11] 01 = "record present, ask for the next one", anything else completes the request (vmbx ignores the error, ALS line stays 1) (docs/research/rm51-tsy-als.md) [V]: `reply 09 0F 10 len=13 0B 00`. **The S60 idle screen** (wallpaper, clock, date, signal, battery; softkeys Camera / Go to; build/evidence/3230/02-home-screen.png), 61 processes |
| 7 | Menu (F5) opens its title bar but never draws the grid | Menu's CAknAppUi::BaseConstructL -> CAknKeySoundSystem::NewL blocks connecting to the key-sound server, which is inside MMF audio initialisation; the DSY's `0B 23 {00 00}` and `0B 11 {01 00}` are unanswered (the 6600 sent `0B 00` instead) Nokia.dsy's audio receiver (0x50839D94): `0B 11 {01 00}` (tid 9) waits in a nested scheduler, handler 0x5083A800 caches the value on the first reply and ends the wait on the second; `0B 23` has no reply handler (docs/research/rm51-dsy-audio.md) [V] | `reply 0B 11 12 @01 01 01 02 08 00 00 00 00 00 00`; the DSY's audio waits end. **Still open:** the key-sound server (EikSrvUi, a fixed process: stack at 0x01B00000) waits inside `CMdaAudioToneUtility::NewL` for the audio device; the sound-device objects 0x55/0x56 send resource **0xE8** (`E8 20 {82 02 15 E0 ...}`, `E8 2A {00 00}`), new on this phone, unanswered: research docs/research/rm51-audio-e8.md |

Unanswered ISI at the time query: `02 1E`, `09 09`, `09 0F`, `09 1D`, `5A 00`, `31 30` (object 0x45, the TSY),
`09 29 {37}`, `0B 11 {01}` (x2), `0B 23` (object 0x54, the DSY).

| 8 | keys stop after the first key clicks: Menu (F5) never opens | Wserv's key-sound animation (AknAnimDll) calls the KeySoundServer synchronously for each key; the server plays a click through MMFDevSound, which talks to the modem on ISI resource **0xE8** (objects 0x55/0x56) and waits synchronously at play end and close: `E8 20` play needs `21` started then `2B` ended, `E8 2A` stop, `0B 19` (x2 at play end), `0B 04` close; any reply ends each wait (docs/research/rm51-audio-e8.md, rm51-keysound.md) [V]; found by a logging catch on the server's ServiceL (0x509E8C44): keys A4, A5 handled, B4 (Menu) never reached it | table lines for all of them, `reply+` (new: extra frames after a reply, ngage_isi.c, test reply_plus): **the Menu grid opens, the joystick navigates** (build/evidence/3230/03-menu.png) |

## State (2026-11-19)

The 3230 boots to its **S60 home screen** and **Menu** on `-M nokia3230`: keys and joystick work, no sound (the
modem side answers DevSound's 0xE8 protocol with "ended" at once), no network. First boot ~150 s (C: formatted
twice); with a kept flash file about 30 s to the time query, which every boot asks (no RTC hand-over yet).

```sh
python3 eka1/tools/ngage_phone.py --model nokia3230 [--phone NAME] [--fresh]
```

Keys in the window: F1/F2 softkeys, F3/F4 call/end, F5 Menu, arrows + Enter the joystick, 0-9, `*`, `#` (F10),
Backspace C, Left Shift edit, F6 the side key 0xAC [I].

## Open

- RTC: the time and date queries at every boot (the 6600 has the same; the NOS's RTC hand-over passes no time).
- Unanswered: `0B 14` (x3, object 0x56, DevSound volume/routing [I]), TSY `02 16`, `02 1E`, `31 11`, `31 06`,
  `31 30`, `43 07`, `5A 00`, DSY `09 29 {37}`, `0B 23`, `1C 29`; `54 0E` (object 0x5B).
- Guessed replies [I]: `09 09`, `09 1D` (from the 9500's table), the DSY audio value 0, `0B 04 -> 05` (id),
  everything seeded from the 6600's table.
- Hardware not modelled: the BlueCore (only its UART ready bit), camera, MMC, radio, sound.
