# Nokia 9210 (RAE-3) keys: what the board must model

Static research (Ghidra headless, no emulator). [V] = read in the code, [I] = inferred.
Decompiles: `build/evidence/ghidra-rae3/ekeyb.c(.lst)` (EKeyb.dll), `ecust.c(.lst)` (ECust.dll),
kernel accessors in `ekern.c.lst` 0x50003360..0x50003440 and 0x50002870..0x500028d4.

## Who scans

- **EKeyb.dll (0x508A0AF0, 3600 bytes) is the keyboard driver** [V]: it binds three interrupts by
  name, "IrqKeyPress", "IrqLidOpen", "IrqLidClosed" (strings at 0x508A17C0..), and holds the
  matrix -> scan-code table. It touches the hardware only through kernel accessors (EKern exports).
  Ekdata.dll is the scan-code -> character keymap (window server side), not needed for the board [I].
- "KeyDepress" (40) and "KeyCol9" (41) are not bound by EKeyb [V: only the three names above].
  The model need not drive them.
- The phone-side keypad on the cover is not in this matrix [V: EKeyb has no other source]. On the
  9210 the cover keypad belongs to the phone engine (separate DCT3 MCU) and reaches Symbian through
  the cellular link (cmt.ext) [I]. Not part of this keyboard model.

## Registers: keyboard block at PA 0x05013000 (VA 0x58013000) [V]

Kernel accessors (EKern), all 32-bit:

| off  | dir | accessor   | use in EKeyb |
|------|-----|------------|--------------|
| 0x04 | R   | 0x50003394 | row input; driver uses `~v & 0x3FF` -> **10 rows, active low** (a pressed key in a driven column reads 0) |
| 0x08 | W   | 0x50003380 | not used by EKeyb |
| 0x0C | W   | 0x5000336C | init: 0x3FF |
| 0x10 | W   | 0x500033A8 | init: 0 |
| 0x14 | W   | 0x500033D0 | init (last): 0x3FF |
| 0x18 | W   | 0x500033BC | init (last): 0x3FF |
| 0x1C | R   | 0x5000340C | not used by EKeyb |
| 0x20 | W   | 0x500033F8 | **column drive**: written with 0 (all driven), 0xFFFFFFFF (none), `~(1<<c)` (column c) |
| 0x24 | W   | 0x500033E4 | written with exactly the same values as 0x20, just before it [V]; probably direction/enable [I] |
| 0x28 | W   | 0x50003420 | init: 0 |

0x0C/0x10/0x14/0x18/0x28 are probably row interrupt/pull-up configuration [I]; a model can store
and ignore them.

Init (0x508A0ECC) [V]: +0x0C=0x3FF, +0x10=0, +0x24=0, +0x20=0, +0x28=0, enable the KeyPress,
LidOpen and LidClosed interrupts, +0x18=0x3FF, +0x14=0x3FF.

## Lid: GPIO input PA 0x05003000 bit 9 [V]

- 0x05003000 is a GPIO input word (kernel 0x50002870: pin n<32 -> bit n of 0x05003000, n>=32 ->
  0x05003004) [V].
- ECust.dll 0x50093EE4 = `(GPIO(0x05003000) & 0x200) != 0`, the variant's "lid closed" method
  (vtable+0x154) [V for the code; I for the slot, the walked vtable puts it at +0x14C from the first
  code pointer, i.e. +0x154 with the usual two leading words].
- EKeyb scan: a key whose table code is non-zero is **dropped while bit 9 is set** (0x508A11F0..
  0x508A1210) [V]. So **bit 9 must read 0 (lid open) for the inner keyboard and command buttons.**
- Lid interrupts: LidOpen (source 10) ISR 0x508A0DD0 / LidClosed (source 9) ISR 0x508A0EA8 disable
  their own line and queue a DFC -> 225 ms timer (0x36EE8 us) -> 0x508A0DFC reads the lid
  method: 0 -> TRawEvent 0x12 ECaseOpen and re-enables **LidClosed**; else 0x13 ECaseClose and
  re-enables **LidOpen**. The event is posted only when it differs from the last (initially 0) [V].
  The "disable mine, enable the other" pattern means level lines: LidOpen asserted while open,
  LidClosed asserted while closed [I]. Holding source 10 high at boot yields one ECaseOpen ~225 ms
  after EKeyb starts (harmless, likely wanted by the Series 80 shell) [I].

## Interrupt and scan sequence [V]

1. Idle: +0x24 = +0x20 = 0 (all columns driven), KeyPress interrupt (source 39, IRQ controller
   0x05005000) enabled.
2. KeyPress ISR (vtable 0x508A1864 -> 0x508A0B58 -> 0x508A0F40): **disables source 39** and queues
   a DFC; nothing is written to the keyboard block (no device ack) [V]. DFC 0x508A0D58 starts a
   periodic 10000-unit timer (10 ms [I]) whose callback runs the scan 0x508A0F74.
3. Scan: with all columns driven, read rows; if `~rows & 0x3FF == 0` -> no key: clear state,
   stop the timer, **re-enable source 39**, done. Else set +0x24/+0x20 = 0xFFFFFFFF (no column)
   and re-read up to 11 times until rows read 0x3FF (else abort this tick). Then for c = 0..9:
   +0x24/+0x20 = `~(1<<c)`, store `~rows & 0x3FF`. Then back to 0/0.
4. Ghost filter: if a column has 2+ bits and shares a row with another column, the whole
   snapshot is discarded [V]. Two keys in different columns of the same row are fine.
5. Diff against the previous snapshot; index `i = c*10 + r`; press -> TRawEvent EKeyDown (3),
   release -> EKeyUp (4), scan code `table[i]` (table at 0x508A17F1, 100 bytes, same table for both).
   Index 26 (Chr, code 0x18) held toggles an internal flag; arrows/Enter/Space have a pointer-
   cursor emulation mode (only when a kernel flag is set) that the board need not care about.

So for the model: **source 39 level = "any key pressed"** (or: any row low with the current
column drive); rows = AND over driven columns of the pressed-row masks, inverted; with no column
driven rows must read 0x3FF.

## Scan-code table (0x508A17F1), index = column*10 + row [V for the bytes]

Names from e32keys.h TStdScanCode [V for the values]; key identities are [I] unless obvious.

| col \ row | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | - | - | - | A5 Device1 | A7 Device3 | - | - | 05 Space | 9F Help | 7B ForwardSlash |
| 1 | - | - | - | A4 Device0 | A6 Device2 | - | 5A Z | 12 LeftShift | 43 C | 42 B |
| 2 | - | - | - | - | - | - | 18 LeftFunc (Chr) | 4E N | 4D M | 7A FullStop |
| 3 | - | - | - | - | - | 94 Menu | 10 Up | 0F Right | 11 Down | 0E Left |
| 4 | - | - | - | - | - | 3A ? | 2B ? | 01 Backspace | 79 Comma | 03 Enter |
| 5 | 04 Escape | B4 App0 | B5 App1 | B6 App2 | B7 App3 | B9 App5 | BA App6 | BB App7 | - | - |
| 6 | - | 16 LeftCtrl | 56 V | 58 X | 36 6 | B8 App4 | - | - | - | - |
| 7 | 31 1 | 32 2 | 33 3 | 34 4 | 35 5 | 37 7 | 38 8 | 39 9 | 30 0 | 50 P |
| 8 | 02 Tab | 51 Q | 57 W | 45 E | 52 R | 54 T | 59 Y | 55 U | 49 I | 4F O |
| 9 | 1A CapsLock | 41 A | 53 S | 44 D | 46 F | 47 G | 48 H | 4A J | 4B K | 4C L |

- Command buttons beside the display: A4..A7 = EStdKeyDevice0..3, top to bottom [I, by analogy
  with the 9500, docs/research/ra2-command-keys.md, where A4..A7 top to bottom is verified].
- B4..BB = EStdKeyApplication0..7: the 9210's eight application keys (Desk, Telephone,
  Messages, Internet, Contacts, Office, Calendar, Extras) [I order].
- 0x3A and 0x2B are not standard codes; Ekdata gives them meaning (probably punctuation / '+'
  keys) [I]. 0x1A at col 9 row 0 sits where the 9210 has a key left of A [I].

## Minimal C model proposal

```c
/* PA 0x05013000, 0x30 bytes. Interrupt: source 39 of the 0x05005000/0x05006000 controllers. */
typedef struct {
    uint16_t pressed[10];     /* per column: bit r set = key (c, r) held */
    uint32_t drive, dir;      /* +0x20, +0x24 */
    uint32_t cfg[16];         /* other written words, stored only */
    qemu_irq key_irq;         /* source 39 */
} Rae3Kbd;

static uint32_t rae3_kbd_rows(Rae3Kbd *s)
{
    uint32_t low = 0;
    for (int c = 0; c < 10; c++)
        if (!(s->drive & (1u << c)))    /* bit clear = column driven */
            low |= s->pressed[c];
    return ~low & 0x3FF;                /* active low */
}

static void rae3_kbd_update(Rae3Kbd *s)
{
    int any = 0;
    for (int c = 0; c < 10; c++) any |= s->pressed[c];
    qemu_set_irq(s->key_irq, any != 0); /* level; the driver masks it at the controller while scanning */
}

read  +0x04 -> rae3_kbd_rows(s); +0x1C -> 0; others -> stored value
write +0x20 -> s->drive = v;  +0x24 -> s->dir = v;  others -> store
host key (code) -> (c, r) from the table above -> pressed[c] |= / &= ~(1 << r); rae3_kbd_update()

Lid: GPIO in 0x05003000 bit 9 = 0 (open). Drive source 10 (LidOpen) high and source 9 (LidClosed)
low while open; swap on a "close lid" host key, set bit 9.
```

Keyboard focus test: press A (col 9, row 1) -> EKeyDown 0x41 expected within ~10 ms of the IRQ.
