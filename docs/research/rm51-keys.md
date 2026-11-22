# Nokia 3230 (RM-51) keypad: ekeyb.dll, what the board must model

Static reading of the 3230's ekeyb.dll (primary module, VA 0x5038DB80, 11168 bytes, in
`eka1/rm51/output/rom/ROM.bin`), compared with the 6600's (0x50364F80). Research agent, 2026-11-19.
[V] = read in the code/data; [I] = inferred.

## Summary

- Same matrix hardware as the N-Gage / 6600 [V]: columns ("lines") 0-4 on 0x0C150400 (+00 input
  active low, +02 drive latch, +04, +08, +0A), line 5 on GPIO 0x0C120C04 bit 19 (active low), rows 0-5
  on 0x0C15040C (+00 input, +02 drive, +04/0x0C150410 enable, +08/0x0C150414, +0A/0x0C150416), keypad
  interrupt in CTSI 0x0C150100. Helpers: write 0x5038F5BC (virtual bits 0-4 -> 0x0C1504xx column
  regs, bit 5 -> 0x80000 GPIO, bits 8-13 -> row regs), read 0x5038FA84 (bits 0-4 from 0x0C150400,
  bits 8-13 from 0x0C15040C, bit 5 = GPIO 0x0C120C04 bit 19, bit 28 = UEM/CBUS reg 31 bit 1 [I: power]).
- **One 6 x 6 scan-code table**, not the 6600's two sub-matrix lists [V]: 0x503905F0, 36 words,
  index **row * 6 + line** (decoder 0x5038EE6C: `entry = table[(row*3)*2 + line]`; rows come from the
  read with mask 0x3F00, the set-bit list 0x5039042C = 7 bytes per row mask: count, 0-based indices).
  Word = scan code (byte 0) + a position id (byte 1, unused for the mock).
- **No joystick port** [V]: no literal 0x0015020x anywhere in ekeyb (the 6600 reads 0x0C15020A). The
  5-way joystick (arrows + select) sits in the matrix, rows 0-1. The joystick state machine
  0x5038E98C and its list 0x50390694 (same bytes as the 6600's: 0E/10/0F/11/A7) are still in the
  binary, but it reads `0x5038FA84(mask 0)` = 0, so it never sees a press [V] (dead code on this
  phone [I]).
- Line 5 (GPIO bit 19) has no key on this phone: every line-5 slot is 0 [V].
- The 6600 layout on the 3230 explains the run [V by table]: F1 = (row 0, line 0) = **Up arrow**
  (hour +1); F5 = (0,1) = Left; F2 = (1,0) = Down (should decrement; reported as nothing: [I] maybe not
  noticed, or the editor wrapped); Enter = 6600 joystick port = not read by this ekeyb.

## The table (0x503905F0, row * 6 + line) [V scan codes; names from the EStdKey values]

| row \ line | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| 0 | 0x10 Up | 0x0E Left | 0xA5 right soft | 0x12 edit | 0xB4 menu | - |
| 1 | 0x11 Down | 0x0F Right | 0xC5 end | 0x01 C (clear) | 0xA7 select | - |
| 2 | 0xA4 left soft | - | '1' | '6' | '8' | - |
| 3 | 0xC4 call | - | '5' | '7' | 0x7F # | - |
| 4 | - | 0xAC (Device8) | '9' | '0' | '3' | - |
| 5 | - | - | 0x2A * | '2' | '4' | - |

This is the N-Gage's layout 3 (ngage_keys[]) with edit and C swapped, a sixth (empty) line, and
0xAC at (4,1) [V compare]. 0xAC = EStdKeyDevice8: a side key [I: which one (voice / camera?) unknown].

## Other inputs in ekeyb (not keys of the matrix)

- GPIO 0x0C120C04 **bit 20** = a switch line [V]: ekeyb first writes 0x40 to 0x0C14F018 (ack [I]),
  then bit 20 clear -> state 1 -> event 0xAB (word 0x50390688), set -> state 2 -> 0xAD (0x5039068C),
  debounced (counter +0x1CC). Which level is idle is [I]; suggest reading it high (same convention as
  the 6600's switch line: high = not asserted). ekeyb also writes GPIO 0x0C120C0C/10/14 (config of
  bits 19/20 [I]); plain read-back is enough.
- Bit 28 (UEM over CBUS, reg 31 bit 1) with 0xA8/0xA9 (0x50390680/84) and 0xA6 (0x50390690): the
  power key path [I]; not modelled, as on the other boards.

## Proposed board change (ngage_keys.c)

Make the layout an enum and add the 3230 table; for rm51 map the GPIO page (sense line 5 and the
bit-20 switch) but not the joystick page (ekeyb never reads it; mapping it is harmless).

```c
/* Nokia 3230 [V positions: ekeyb 0x503905F0, row * 6 + line; names from the scan codes].
 * Joystick in the matrix; line 5 empty; no 0x0C15020A port. docs/research/rm51-keys.md */
static const NgageKey rm51_keys[] = {
    { KEY_UP, "up", 0, 0 },          { KEY_LEFT, "left", 0, 1 },
    { KEY_F2, "right soft", 0, 2 },  { KEY_LEFTSHIFT, "edit", 0, 3 },
    { KEY_F5, "menu", 0, 4 },
    { KEY_DOWN, "down", 1, 0 },      { KEY_RIGHT, "right", 1, 1 },
    { KEY_F4, "end", 1, 2 },         { KEY_BACKSPACE, "clear", 1, 3 },
    { KEY_ENTER, "select", 1, 4 },
    { KEY_F1, "left soft", 2, 0 },   { KEY_1, "1", 2, 2 }, { KEY_6, "6", 2, 3 }, { KEY_8, "8", 2, 4 },
    { KEY_F3, "call", 3, 0 },        { KEY_5, "5", 3, 2 }, { KEY_7, "7", 3, 3 }, { KEY_F10, "#", 3, 4 },
    { KEY_F6, "side 0xAC", 4, 1 },   { KEY_9, "9", 4, 2 }, { KEY_0, "0", 4, 3 }, { KEY_3, "3", 4, 4 },
    { KEY_KPASTERISK, "*", 5, 2 },   { KEY_2, "2", 5, 3 }, { KEY_4, "4", 5, 4 },
    { KEY_F12, "power", -1, -1 },
};
```

- Header: `typedef enum { NGAGE_KPD_NGAGE, NGAGE_KPD_NHL10, NGAGE_KPD_RM51 } NgageKpdLayout;`,
  `void ngage_keys_init(MemoryRegion *sys, qemu_irq irq, NgageKpdLayout layout);`; ngage.c line ~1818
  passes `NGAGE_KPD_RM51` for `ngage_rm51`, `NGAGE_KPD_NHL10` for nhl10/nhl2, else NGAGE.
- `kpd.nhl10` keeps meaning "6600-style GPIO line 5" (true for rm51 too: the "line 5 always driven"
  term in kpd_rows is harmless, no key sits there). Add `kpd.rm51`; map `gpio_mr` for both, `joy_mr`
  only for nhl10.
- gpio_read for rm51: also `v |= 1u << 20` at +04 (switch idle [I]).
- Regression test idea: a host-side check that every rm51_keys position's scan code at
  ROM 0x503905F0 + (row*6+col)*4 matches the key name (reads ROM.bin if present, skips otherwise).
