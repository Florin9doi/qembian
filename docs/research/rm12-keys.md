# Nokia 7710 (RM-12): hardware keys

Static reading of `eka1/rm12/core/ROM.bin` (no emulator run), starting from the 9500 notes in
`ra2-command-keys.md`. [V] = read in the code or data, [I] = inferred. Scope: enough to press the
keys from the board (`-M nokia7710`, `omap_mpuio_key(mpuio, row, col, down)`).

## Result

Every key the ROM knows about is on the OMAP1510 MPUIO keypad, scanned by **`ekeyp.dll`**
(0x500C6E10, code 0x500C6E74..0x500C78A0) [V]. Same design as the 9500's EKeyp, with three
differences: **6 columns instead of 5** (column 0 is a real column), a **5 x 6 table** at
0x500C77F0, and none of the 9500's special cases (no lid gating, no swallowed press, no held-back
direction keys) except one for scan code 0xB4 [V]. The 5-way key reports plain
EStdKeyUp/Down/Left/Right (0x10/0x11/0x0E/0x0F) [V]. No other kernel module posts key events
(section 3).

## 1. The matrix driver (ekeyp.dll)

- Registers, through the same kernel helpers as the 9500 (EKern 0x500166B4..0x5001670C), base VA
  0x580B5000 = MPUIO 0xFFFB5000 [V]:
  - rows: 16-bit read of base+0x10 (KBR_LATCH), `& 0x1F` (0x500C6F50). Bit **0 = pressed**
    (0x500C73A0: bit clear -> event type 3 = EKeyDown, set -> 4 = EKeyUp). [V]
  - columns: 16-bit write of base+0x14 (KBC), value **`& 0x3F`** (0x500C6F68; the 9500 had 0x3E). [V]
  - base+0x20 (KBD_INT) read by the interrupt routine 0x500C7248: non-zero starts a scan, zero
    re-enables the interrupt. [V]
  - base+0x28 (KBD_MASKIT) bit 0 touched at construction through EKern 0x50016704, the same helper
    the 9500 doc read as "cleared". [V call, I direction]
- Interrupt bound as **"Lv2_Keyboard"** ("Bind Keyboard" on failure), i.e. the level-2 handler's
  keyboard line, which QEMU's `omap_mpuio` already raises. [V name, I number]
- Idle: KBC = 0 (all columns low), interrupt enabled (0x500C721C). [V]
- Scan (start 0x500C7284, timer callback 0x500C72F8 -> DFC -> 0x500C7324): column words at
  0x500C780E = `3E 3D 3B 37 2F 1F` (6 x 16-bit), so **step n (0..5) drives KBC bit n low**, the
  others high. Each step reads the 5 row bits, compares with the stored value for that step
  (`this+0x50+step`), and for each changed row r sends `table[r*6 + step]`. [V]
- Timer: started with 1000 (units not checked; [I] µs or ticks) per step. [V value]
- After step 5 it wraps to 0; if all 6 steps saw rows = 0x1F it stops and goes idle, otherwise it
  keeps scanning. [V]
- Start-up check (vtable+16, 0x500C6F78): KBC = 0, rows must read 0x1F, else -44. No key may be
  down at boot. [V]
- Power-down (power handler "DXpRockerPowerHandler", vtable 0x500C7844 -> 0x500C7494): sends
  EKeyUp for the codes of the current column and resets the stored rows. Power-up restarts the
  scan (0x500C718C). [V]
- Events: `TRawEvent::Set(type, scancode)` (EUser 0x50063F14 via 0x500C76B4) then
  `Kern::AddEvent` (0x500C7648). **Scan code 0xB4 only** goes through the 3-argument
  `Set(type, 0xB4, 0)` (EUser 0x50063F34 via 0x500C76A8): same scan code, second word zeroed
  (0x500C73A8/0x500C73CC). [V calls; I that the effect is "repeat count 0"]

### Scan-code table at 0x500C77F0 (5 rows x 6 steps) [V]

| row (KBR bit) | col 0 | col 1 | col 2 | col 3 | col 4 | col 5 |
|---|---|---|---|---|---|---|
| 0 | -- | **94** Menu | -- | **C5** No | **10** Up | **0F** Right |
| 1 | **B4** App0 | -- | **AA** Device6 | -- | **0E** Left | **11** Down |
| 2 | **C4** Yes | -- | -- | **AB** Device7 | -- | -- |
| 3 | -- | -- | -- | -- | -- | -- |
| 4 | **A8** Device4 | **A9** Device5 | **04** Escape | -- | -- | -- |

`--` = 0x00 (no key). 12 keys in all.

### Key codes (Ekdata.dll, 0x50087660) [V]

The unmodified table (key codes from 0x50088046; scan-code blocks 00-11, 2A, 30-39, 41-5A, 60-9E,
A4-AB, C4, C5, B4 at 0x500877AC) maps them 1:1, nothing remapped as on the 9500:
0x94 -> 0xF836 EKeyMenu, A4..AB -> 0xF842..0xF849 EKeyDevice0..7, B4 -> 0xF852 EKeyApplication0,
C4 -> 0xF862 EKeyYes, C5 -> 0xF863 EKeyNo, 0x04 -> 0x1B, arrows -> 0xF807..0xF80A. Note 0x03 (Enter)
is not in the matrix.

## 2. Which physical key is which

| scan code | name | physical key | |
|---|---|---|---|
| 10 / 11 / 0E / 0F | EStdKeyUp/Down/Left/RightArrow | 5-way key directions | [V code, I key] |
| AB | EStdKeyDevice7 | 5-way key centre (select) | [I] (best guess: no Enter in the matrix) |
| 94 | EStdKeyMenu | Menu key | [I] high confidence |
| B4 | EStdKeyApplication0 | Desk key (the only special-cased code) | [I] |
| C4 / C5 | EStdKeyYes / EStdKeyNo | Call (green) / End (red) | [I] |
| 04 | EStdKeyEscape | Back / Cancel key | [I] |
| AA | EStdKeyDevice6 | other hardware key (centre select if AB is not; or voice key) | [I] |
| A8 / A9 | EStdKeyDevice4 / 5 | a key pair, probably volume up / down or zoom | [I] |

Cheapest way to settle the [I] rows: on the running phone press each (row, col) once and watch
the screen (or log the key code the window server reports).

## 3. Keys outside the matrix

- **epower.dll**: no `TRawEvent::Set` import at all; its only interrupt is "Lv2_RTCAlarm". [V]
- **exyin.dll**: uses only the 3-argument `Set` (pointer events) and binds "Armio_Bit4" (the
  digitiser pen interrupt). No key. [V for the imports/strings, I that it posts pointer events only]
- **ECust.dll**: one function posts raw events, 0x500419C8 (a variant virtual, vtable entry
  0x50046AC8): `Set(4, 0x18, 0)` and `Set(4, 4, 0)` then AddEvent each, i.e. two releases posted
  by software, not read from a line. [V code, I meaning]. Its "Gpio_Bit11" interrupt
  (0x50043AAC) only stores a 0/1 state (`this+0x54`, 0x50043D00); no key event. [V] [I: card
  or accessory detect]
- **Power key**: not in any of these. [I] Like the N-Gage/9500 (DCT4 UEM, CBUS reg 0x1F bit 1),
  it sits on the modem side and reaches Symbian as an ISA message, not a scan code. Model it
  later through the fake modem if needed.

## 4. What the board must do

QEMU's `omap_mpuio_key(s, row, col, down)` sets `buttons[row]` bit `col`; a row reads pressed when
`buttons[row] & ~KBC`. **QEMU row = table row, QEMU col = step (0..5).** Unlike the 9500, column
0 is valid. No QEMU change is needed. Hold each key at least one full scan (6 steps; 100 ms is
safe) [I]; no key down at boot [V].

Proposed host keys for the phone window:

| key | scan | (row, col) | host key |
|---|---|---|---|
| 5-way up | 10 | (0, 4) | Up |
| 5-way down | 11 | (1, 5) | Down |
| 5-way left | 0E | (1, 4) | Left |
| 5-way right | 0F | (0, 5) | Right |
| 5-way select [I] | AB | (2, 3) | Enter |
| Menu | 94 | (0, 1) | F1 |
| Desk [I] | B4 | (1, 0) | F2 (and Home) |
| Call / Yes | C4 | (2, 0) | F3 |
| End / No | C5 | (0, 3) | F4 |
| Back / Escape | 04 | (4, 2) | Esc (and Backspace) |
| Device6 [I] | AA | (1, 2) | F5 |
| Device4 [I] (vol+?) | A8 | (4, 0) | F6 (or +) |
| Device5 [I] (vol-?) | A9 | (4, 1) | F7 (or -) |

```c
/* ekeyp.dll matrix (table at 0x500C77F0, 5 rows x 6 columns): host key -> MPUIO (row, col) */
static const struct { unsigned key; uint8_t row, col; } RM12_KEYS[] = {
    { KEY_UP, 0, 4 }, { KEY_DOWN, 1, 5 }, { KEY_LEFT, 1, 4 }, { KEY_RIGHT, 0, 5 },
    { KEY_ENTER, 2, 3 },                                   /* AB, 5-way select [I] */
    { KEY_F1, 0, 1 }, { KEY_F2, 1, 0 }, { KEY_HOME, 1, 0 }, /* Menu 94, Desk B4 */
    { KEY_F3, 2, 0 }, { KEY_F4, 0, 3 },                     /* Yes C4, No C5 */
    { KEY_ESC, 4, 2 }, { KEY_BACKSPACE, 4, 2 },             /* Escape 04 */
    { KEY_F5, 1, 2 }, { KEY_F6, 4, 0 }, { KEY_F7, 4, 1 },   /* AA, A8, A9 */
};
```

## Not done

- Physical identity of AA / AB / A8 / A9 / B4 (guessed from the key set of the phone).
- Interrupt number, timer period units, the power key's modem path.
