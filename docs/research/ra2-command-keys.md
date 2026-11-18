# Nokia 9500 (RA-2): command buttons and the cover keypad

Static reading of `eka1/ra2/core/ROM.bin` (no emulator run). [V] = read in the code or data,
[I] = inferred. Scope: enough to press the keys from the board (`-M nokia9500`).

## Result

The hypothesis is confirmed. The four command buttons beside the inner display and the whole cover
keypad are **not** on the I2C keyboard controller. They are a 5 x 5 matrix on the OMAP1510 MPUIO
keypad (KBR_LATCH / KBC), scanned by **`EKeyp.dll`** (0x5009D8B0, 2840 bytes; strings "KEYPAD
POWER", "KEYPAD INT", "Bind keypad"). The command buttons report scan codes 0xA4..0xA7
(EStdKeyDevice0..3). The EKeyb indices tried so far (84/85/93/94 = Device6..9) are arrow keys in the
phone's keymap, which is why they moved the form focus.

## 1. EKeyp.dll: the matrix driver

- Registers, through kernel helpers on base VA 0x580B5000 (= MPUIO 0xFFFB5000) [V]:
  - rows: 16-bit read of base+0x10 (KBR_LATCH), `& 0x1F` (0x5009DEC4 -> kernel 0x5000F6B4). A row
    bit is **0 while a key on it is pressed** (0x5009DC3C: bit 0 -> key down).
  - columns: 16-bit write of base+0x14 (KBC), value `& 0x3E` (0x5009DEDC -> 0x5000F6CC).
  - base+0x20 (KBD_INT) is read in the interrupt routine (0x5009E08C): non-zero queues the scan
    DFC, zero re-enables the interrupt. QEMU returns 1 here while a key is down, which fits. [V]
  - base+0x28 (KBD_MASKIT) bit 0 is cleared at start (0x5009E028 -> 0x5000F704). [V]
- Idle: KBC = 0 (all columns driven low), interrupt enabled (0x5009DAE8). [V]
- Scan (DFC 0x5009DB3C, then one timer tick per step, 0x5009DBB0): for step 0..4 write the column
  word from the table at 0x5009E376 = `3D 3B 37 2F 1F` (after the 0x3E mask: 3C 3A 36 2E 1E), so
  **step n drives KBC bit n+1 low**; KBC bit 0 is low in every step. Read the 5 row bits, compare
  with the previous value of that step, and for each changed row bit r send
  `key = table[r*5 + step]` (table at 0x5009E35C). [V]
- Events: `TRawEvent::Set(3 = EKeyDown | 4 = EKeyUp, scancode)` then `Kern::AddEvent` (imports
  0x5009E2A4, 0x5009E1FC). [V for 3/4 and the calls; I for the names]
- After 5 steps with all rows released the driver goes back to idle; otherwise it keeps scanning. [V]
- Start-up check (vtable+8, 0x5009DF60): KBC = 0, rows must read 0x1F, else error -44. No key may
  be down at boot. [V]
- Interrupt: a kernel interrupt object bound with the name "Bind keypad"; the number was not read.
  [I: OMAP1510 level-1 IRQ 1, the keyboard interrupt, which QEMU's `omap_mpuio` already drives]

### Scan-code table at 0x5009E35C [V]

| row (KBR bit) | step 0 (KBC1) | step 1 (KBC2) | step 2 (KBC3) | step 3 (KBC4) | step 4 (KBC5) |
|---|---|---|---|---|---|
| 0 | D6 | DA | DC | **A4** | **A5** |
| 1 | DB | D8 | D9 | **A6** | **A7** |
| 2 | D2 | DE | DF | E0 | E8 |
| 3 | D0 | E1 | E2 | E3 | DD |
| 4 | D4 | E4 | E5 | E6 | E7 |

### Special handling in the scan routine

- **A4..A7 (the command buttons) are delivered only while `this+0x18` is non-zero** (0x5009DD40).
  EKeyb sets it through the keypad object's vtable+12 (0x5009DAB0) with the lid state when it
  reads 0xE0 / 0xE1 (0x5009ECA4..0x5009ECF4; the object pointer is kept by the kernel at
  0x80000B4C, set by 0x5000F714, read by 0x5000F724). So the command buttons work only after the
  lid-open byte 0xE0 has been read. [V] This is the "observer" left open in
  `ra2-cover-display.md` section 2.
- `this+0x1C` (set through vtable+16, 0x5009DAC8, by EKeyb 0x5009F174 when its mode word
  `this+0x80` becomes 1, and at 0x5009E910): the next command-button press is reported with scan
  code 0 and so is its release. One press is swallowed. [V for the code; I: a wake-up press after
  the keyboard was idle or dimmed]. If a first press does nothing, press again.
- D0, D2, D4, D6: the key-down is held back and a timer (100 ticks) is started; a second one of
  these, or DB, cancels it. The down event is sent when the timer ends (0x5009DEEC). [V]
  [I: the four directions of the cover scroll key, DB = its centre press]

### Which physical key is which

- A4..A7 = EStdKeyDevice0..3 = the four command buttons, top to bottom. [V for the codes and
  that they are gated by the lid; I for the order top to bottom]
- D0..D8 / D9..E8 are EStdKeyDevice17..1F / EStdKeyApplication10..1F in later e32keys.h. [I for
  7.0s]. They are outside every block of the keymap (EKdata's last blocks are A4-B3 and B4-BB
  [V]), so they reach the cover UI as scan codes only.
- Cover key identities were not traced in the cover UI code (CuiWs / CuiAppFramework). Best guess
  from the table layout [I]: DE DF E0 / E1 E2 E3 / E4 E5 E6 = digits 1..9; E7, E8, DD = `*`, `0`,
  `#` in some order; D0 D2 D4 D6 = scroll directions, DB = select; D8, D9 = the two cover
  softkeys; DA, DC = send and end. Cheapest way to settle it: with the lid closed press each
  (row, col) once and look at the cover screen.

## 2. What S80 takes as the command buttons

- Keymap `EKdata.dll` (0x500A0220), unmodified table: scan-code block A4..B3 maps to
  `F842 F843 F844 F845 F846 F847 F807 F808 F809 F80A 000D F84D..F851` (array at 0x500A0B96, blocks
  at 0x500A036C). So A4..A7 -> EKeyDevice0..3 (0xF842..0xF845). [V]
  The same block explains the earlier tries: AA..AD (Device6..9, EKeyb indices 84/85/93/94) ->
  0xF807..0xF80A (the arrow key codes) and AE (DeviceA, indices 64/103) -> 0x000D (Enter). [V]
  The locale variants (Ekdata.NN.dll) were not compared. [I: the same]
- `EikCoctl.dll`, the button group's key handler at 0x50405A88 (Thumb): for a key code in
  0xF842..0xF845 (literals at 0x50405B30/34) the button index is `code - 0xF842` (0xFFFF07BE at
  0x50405B38); it then takes that button's command and calls the command observer. [V]
  So command button 1 (the "OK" at the top right) = EKeyDevice0 = scan code 0xA4, button 2 = 0xA5,
  button 3 = 0xA6, button 4 = 0xA7. [V for the index; I that the top label is index 0]

## 3. What the board must do

QEMU's model (`hw/arm/omap1.c`): `omap_mpuio_key(s, row, col, down)` sets `buttons[row]` bit
`col`; a row reads pressed when `buttons[row] & ~KBC`; the keyboard interrupt is raised while any
row is pressed, KBD_MASKIT is 0 and the MPUIO clock runs. [V] This matches EKeyp as it is: no
QEMU change is needed, only calls from the board.

**QEMU row = table row, QEMU col = step + 1.** Do not use col 0 (it is low in every step, so the
key would be seen in all five columns).

| key | scan code | `omap_mpuio_key(mpu->mpuio, row, col, down)` |
|---|---|---|
| command button 1 (OK in the form) | A4 | (0, 4) |
| command button 2 | A5 | (0, 5) |
| command button 3 | A6 | (1, 4) |
| command button 4 | A7 | (1, 5) |
| D6 / DA / DC | | (0, 1) / (0, 2) / (0, 3) |
| DB / D8 / D9 | | (1, 1) / (1, 2) / (1, 3) |
| D2 / DE / DF / E0 / E8 | | (2, 1) / (2, 2) / (2, 3) / (2, 4) / (2, 5) |
| D0 / E1 / E2 / E3 / DD | | (3, 1) / (3, 2) / (3, 3) / (3, 4) / (3, 5) |
| D4 / E4 / E5 / E6 / E7 | | (4, 1) / (4, 2) / (4, 3) / (4, 4) / (4, 5) |

Proposed change in `src/qemu-ngage/hw/arm/nokia9500_kbd.c` (sketch):

```c
/* EKeyp.dll matrix (table at 0x5009E35C): host key -> MPUIO keypad (row, col = step + 1) */
static const struct { unsigned key; uint8_t row, col; } RA2_MATRIX[] = {
    { KEY_F9, 0, 4 }, { KEY_F10, 0, 5 }, { KEY_F11, 1, 4 }, { KEY_F12, 1, 5 },   /* A4..A7 */
};
/* in kbc_key_event(), before the I2C table: */
    omap_mpuio_key(mpu->mpuio, row, col, evt->key.down);
```

- Remove F9..F12 from RA2_KEYS (indices 84/85/93/94 are arrows there); keep the mpuio pointer in
  the device state.
- Add a machine property for scripted presses, for example `keypad-key` = `row << 4 | col`, bit 8 =
  down, next to `kbd-byte`.
- Hold a key for at least one full scan (5 timer ticks plus the interrupt; 100 ms is safe) before
  releasing it, or the scan can miss it. [I]
- Conditions: the lid-open byte 0xE0 must have been read first (command buttons are dropped with
  the lid closed) [V]; no matrix key down at boot [V]; if the interrupt never fires, check that
  the MPUIO clock is on in QEMU (`s->clk`) [I].

## Not done

- Cover key identities (which of D0..E8 is which digit or softkey): guessed from the layout only.
- The keypad interrupt number and the scan timer period in milliseconds.
- Who consumes D0..E8 in the cover UI.
