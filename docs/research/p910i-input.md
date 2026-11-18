# P910i input: touch screen, flip keypad / QWERTY, side keys, jog dial, flip

Static reading of the core ROM (`eka1/p910i/core/ROM.bin`, VA = 0x50000000 + offset), no runs.
[V] = read in the code, [I] = inferred. Addresses are kernel virtual (0x58000xxx = phys
0x50000xxx, docs/research/p910i-soc-map.md).

Modules read: Exyin.dll (code 0x500B3624, digitiser + flip), ekeypad.dll (code 0x500D3E44,
flip keypad panel + IOPort2 key), critajogdial.dll (code 0x500D3B04), TouchScreenCtrl.ldd
(code 0x500B4284), EKern register helpers 0x50016068..0x500162b0, EKern
`Interrupt::SetMode`-like helper 0x50015260.

## 0. Shared pieces

### Interrupt mode helper 0x50015260(irq, mode) [V]
Writes the IC config registers for the line's bit b:
| mode | +0x0C bit | +0x10 bit (edge select) |
|---|---|---|
| 0 | 0 | 0 |
| 1 | 1 | 0 |
| 2 | 0 | 1 |
| 3 | 1 | 1 |

All input drivers use modes 0/1 only, i.e. **level** interrupts, and +0x0C is the **polarity**:
1 = active high, 0 = active low [I, strong: the GPIO handlers re-arm with the opposite polarity
of the level they just read, sec. 3-5]. The internal ADC lines (23, 26) use mode 1 (active high).
Our IC model (src/qemu-ngage/hw/arm/p910i_soc.c) ignores +0x0C today; it must apply it at least
to the GPIO-pin lines 41, 42, 43, 54, 55: `asserted = pol ? pin : !pin` (level, re-evaluated
whenever the pin or +0x0C changes).

### Driver interrupt objects [V]
`0x500225ac` binds an interrupt object by IRQ *name* (strings in each DLL), `0x50022578` = enable,
`0x50022588` = disable, `0x50038698`/`0x50038708` = DFC init / add. Raw events go to the window
server through `0x50027adc` (Kern add-event) with `TRawEvent::Set(type, a, b)` (EUser 0x50055910).
TRawEvent types used: 1 EPointerMove, 2 EPointerSwitchOn, 3 EKeyDown, 4 EKeyUp, 10 EButton1Down,
11 EButton1Up [V numbers, I enum names = Symbian 7 TRawEvent::TType].

### GPIO input register 0x58000100 (phys 0x50000100) [V reads]
| bit | meaning | read by |
|---|---|---|
| 0 | jog dial phase B | critajogdial DFC |
| 1? | jog dial phase A = the pin behind IrqIOPort1 (42) [I: bit not read, guessed by analogy with bit 2] | (interrupt only) |
| 2 | side key, 0 = pressed (pin behind IrqIOPort2, 54) | ekeypad |
| 18 | flip: 1 = closed, 0 = open (pin behind IrqFlip, 43) | Exyin, ekeypad |

The same word is also the pin-mux register written by Euart (soc map), so the model returns
`(stored & ~inmask) | inputs` [I]. **Idle value matters**: with polarity implemented, a 0 in bit 2
at boot is a held side key (ekeypad arms IRQ 54 active-low and would see it pressed), so idle =
bit 2 = 1, bit 1 (jog A) = 1, bit 18 = flip state.

## 1. Touch screen (block 0x58000800, IRQ 23 "IrqTouchscreen")

### Registers (Exyin init 0x500B36CC, EKern helpers 0x50016078..0x50016148) [V access, I meaning]
| off | width | init write | meaning |
|---|---|---|---|
| +0x00 | byte | (helper 0x50016078 sets bits 1:0; not called by Exyin) | mode [I] |
| +0x08 | half | `|= 8`, then bit 0 = 1 | bit 0 = enable/run (power-on sets, power-off clears, also TouchScreenCtrl ctl 0/1); bit 3 = auto-scan / IRQ enable [I] |
| +0x10 | word, read | | **sample**: bits 9:0 = raw **Y**, bits 25:16 = raw **X**, bit 31 = 1 when the pen is **up** (helper returns `!bit31` as "down") |
| +0x14 | word | 0x42 | timing [I] |
| +0x18 | half | 0x84 | timing [I] |
| +0x1C | word | 0x3E | timing [I] |
| +0x24 / +0x28 | half | 0 / 0x3FF | window min/max channel 1 [I] |
| +0x2C / +0x30 | half | 0 / 0x3FF | window min/max channel 2 [I] |
Also `0x58000024 bits 1:0 = 1` (helper 0x5001614C, CGU: ADC clock [I]).
No pressure value. IrqPen (22) and IrqTouchscreenAux (24) are bound by no driver [V: names only
in EKern's table].

### Interrupt sequence [V code, I hardware]
- IRQ 23: mode 1 (level, active high), enabled at init.
- ISR 0x500B3D48 reads +0x10 three times (X, Y, bit 31) into the sampler, then queues the DFC
  once; it does **not** disable the interrupt, so **reading +0x10 must drop IRQ 23** (data-ready
  cleared on read; the latched sample must stay the same for all three reads) [I, forced].
- DFC 0x500B3830 (state machine):
  - pen-down sample, previous sample up: if `debounce` flag (set by the last up-sample) is set,
    the sample is dropped and the flag cleared; else **EButton1Down(x, y)** (and a call into
    ampctrl.dll 0x500D5248, key click [I]).
  - pen-down, previous down: 4-sample filter (0x500B3A5C/0x500B3AEC: the first 4 samples only
    fill the buffer, later ones give an outlier-rejected average); **EPointerMove(x, y)** when the
    position changed.
  - pen-up sample after a down: **EButton1Up** at the previous position; sets `debounce`.
  - first down after "switch on" (vtable 0x500B37D0 sets a flag): EPointerSwitchOn instead.
- So the hardware must deliver a **sample stream while the pen is down** (one IRQ per sample) and
  **one pen-up sample** (bit 31 = 1) on release, then go quiet [I].
  Needed: >= 2 down samples for EButton1Down (debounce), >= 4 more before moves are reported.

### Calibration [V]
Exyin writes the default calibration into the superpage at init (0x40000800 + 0x194..0x1A8),
R11 R12 R21 R22 Tx Ty as 16.16 fixed point:
`0x0000442B, 0, 0, 0xFFFF9EBD, 0xFFDECA56, 0x015CEE6F`. Transform 0x500B3C78 with p = (rawX, rawY):
```
X32 = R11*rawX + R21*rawY + Tx      screen_x = X32 >> 16 (arithmetic), clamp 0..207
Y32 = R12*rawX + R22*rawY + Ty      screen_y = Y32 >> 16, clamp 0..319
screen_x = (17451*rawX - 2176426) >> 16
screen_y = (22867567 - 24899*rawY) >> 16
```
Inverse (pixel centre, what the model should produce for pixel (px, py)):
```
rawX = round(((px + 0.5)*65536 + 2176426) / 17451)    px 0 -> 127, 207 -> 904
rawY = round((22867567 - (py + 0.5)*65536) / 24899)   py 0 -> 917, 319 -> 78
```
X grows with raw X, Y grows as raw Y falls [V]. The values live in the superpage and the HAL
calibration call can overwrite them (a saved user calibration restored from C:) [I]; if touches
land off after that, read the 6 words back from 0x40000994 with gdb.

## 2. Flip keypad and QWERTY: a second resistive panel (block 0x58000900, IRQ 26 "IrqKeypad")

The keypad is **not** a row/column matrix: it is an ADC block with the same layout as the touch
block [V: helpers 0x500161FC..0x500162B0 mirror 0x50016094..0x50016148]; ekeypad maps the (x, y)
point into key rectangles.

### Registers [V access]
Same as sec. 1 at +0x900: init (0x500D40A0) `+0x08 |= 8`, `+0x24=0 +0x28=0x3FF +0x2C=0
+0x30=0x3FF`, `+0x14=0x42 +0x18=0x84 +0x1C=0x3E`, `+0x08 bit0 = 1`. `+0x10`: bits 25:16 = x,
bits 9:0 = y, bit 31 = 1 when nothing is pressed.

### Interrupt / events [V]
IRQ 26 mode 1 (level, active high). ISR disables the line and queues the DFC; DFC 0x500D4134 reads
x, y, then bit 31, emits events, re-enables IRQ 26 at the end. One key at a time:
- no key held, bit 31 = 0: look up (x, y); if found, **EKeyDown(scan)**, remember it.
- key held, bit 31 = 1: **EKeyUp(held scan)**; samples with bit 31 = 0 while held are ignored.
Model: one sample + IRQ on press, one bit-31 sample + IRQ on release; clear the IRQ on reading
+0x10 [I] (otherwise the re-enable at the end of the DFC re-enters at once).

### Key tables (built once in 0x500D4200) [V]
Key = `TRect(cx-h, cy-h, cx+h, cy+h)`, hit test `TRect::Contains(x, y)` (right/bottom exclusive).
Which table depends on the flip (GPIO bit 18) [V]:
- **bit 18 = 1 (closed): table A** (flip front, 19 keys, h = 89), at 0x80002824.
- **bit 18 = 0 (open): table B** (h = 66), at 0x800029A4: all 36 entries if the word 0x80002820 is
  non-zero, else only the first 3 (the corner keys). 0x80002820 is set/cleared by
  TouchScreenCtrl.ldd control 2/3 (user side; who calls it not traced) [V write, I caller].

Table A (x, y -> scan code; `x` = bits 25:16, `y` = bits 9:0):
| x \ y | 238 | 416 | 594 | 772 |
|---|---|---|---|---|
| 238 | 0xAE DeviceA | '1' 0x31 | '2' 0x32 | '3' 0x33 |
| 416 | 0xAD Device9 | '7' 0x37 | '5' 0x35 | '9' 0x39 |
| 594 | 0xA7 Device3 | '*' 0x85 NkpAsterisk | '8' 0x38 | '6' 0x36 |
| 772 | 0xB3 DeviceF | '4' 0x34 | '0' 0x30 | '#' 0x7F Hash |
Corner keys (in both tables): (0, 1023) 0xB1 DeviceD, (1023, 1023) 0xAC Device8, (0, 0) 0xB2
DeviceE. The y = 238 row is the 4 non-digit flip keys (call/end/C/menu-type keys) [I names]; the
corners are probably the side keys wired as shorts across the panel (Device8 = UIQ Confirm =
jog press, DeviceD/E = back / internet key) [I names].

Table B (flip open, QWERTY), centres x in {177, 309, 441, 574, 707, 839}, y in {177, 309, 441, 574,
707, 839}:
| y \ x | 839 | 707 | 574 | 441 | 309 | 177 |
|---|---|---|---|---|---|---|
| 839 | O | P | L | Backspace 0x01 | FullStop 0x7A | Enter 0x03 |
| 707 | I | K | M | RightFunc 0x19 | Space 0x05 | N |
| 574 | J | U | B | H | Y | T |
| 441 | G | V | C | F | R | E |
| 309 | D | X | LeftShift 0x12 | Z | S | W |
| 177 | Q | A | Comma 0x79 | - | - | - |
Letters are the upper-case ASCII scan codes (0x41..0x5A) [V values]. Scan -> key code goes
through Ekdata*.dll (not decoded; standard EKData layout) [I].

## 3. Side key on IrqIOPort2 (54) [V]
ekeypad 0x500D3E64 / DFC 0x500D3F0C: GPIO bit 2; bit = 0 -> **EKeyDown(0xB5 = EStdKeyApplication1)**
and re-arm IRQ 54 mode 1 (fire when it goes high); bit = 1 -> **EKeyUp(0xB5)**, mode 0. Init arms
mode 0 (waits for the press). So the key is active low. Which physical key: unknown (internet /
camera-type application key) [I].

## 4. Jog dial (critajogdial.dll, IrqIOPort1 = 42) [V code, I pins]
Quadrature decoder in the DFC 0x500D3C44, state s (0 idle):
- idle: IRQ 42 mode 0 (fires when phase A goes low).
- A low (s = 0): s = (GPIO bit 0 ? 2 : 1), re-arm mode 1 (wait for A high). No event.
- A high, s = 1: if bit 0 = 1 -> **0xA5 (EStdKeyDevice1)** KeyDown+KeyUp; else nothing. s = 0.
- A high, s = 2: if bit 0 = 0 -> **0xA6 (EStdKeyDevice2)** KeyDown+KeyUp; else nothing. s = 0.
One detent = one key pair. Direction "Device1": A falls with B = 0, B rises, A rises (B = 1).
Direction "Device2": A falls with B = 1, B falls, A rises (B = 0). UIQ: Device1 = TwoWayUp,
Device2 = TwoWayDown [I, quartzkeys names from memory]. The ISR disables IRQ 42; the DFC re-enables
it after re-arming, so the line is a plain level of pin A through the polarity bit.
Jog **press** is not in this driver: best candidate is the 0xAC (Device8 = Confirm) corner of the
keypad panel [I].

## 5. Flip (Exyin, IrqFlip = 43) [V]
- GPIO bit 18: 1 = closed, 0 = open [I from the keypad tables, V the bit].
- Init 0x500B3DEC: reads bit 18, arms IRQ 43 with the opposite polarity (bit 1 -> mode 0, bit 0
  -> mode 1), enables it, then sends the initial state.
- ISR 0x500B3DD0 disables IRQ 43, DFC 0x500B3F50 re-reads bit 18, re-arms with the opposite
  polarity, re-enables, and **always** sends a raw key pair: **EKeyDown + EKeyUp of 0x77
  (EStdKeyF24) when closed, 0x76 (EStdKeyF23) when open** (0x500B3EEC).
- No HAL value or superpage word written for the flip [V for Exyin]. The UI side therefore learns
  the flip state from the F23/F24 key events (window server / UIQ flip handling; Flipkeypad.app
  only names a semaphore "Flipkeypad_sem" and its MBM; cppVFlip.ctl = "Virtual flip" control panel
  for a phone without its flip) [I, not traced].
- Kernel effects: the keypad lookup table switch (sec. 2). Touch is not disabled by the kernel
  on flip close; TouchScreenCtrl.ldd ctl 0/1 (enable/disable IRQ 23 + block bit 0) is the user
  side's switch [V ctl, I caller].
- Today our GPIO reads 0, so the phone has booted "flip open" (F23 sent, QWERTY table B) [I].

## 6. Power key
Not in any of these drivers (no GPIO/IRQ for it here); probably behind the power chip
(pwrnotif.ldd / the serial bridge at 0x580005FC). Out of scope [I].

## 7. Proposed board model (minimal, faithful)

```
/* p910i_input.c, inside the 0x50000000 sys page model */

/* GPIO input word at +0x100 */
#define GPIO_JOG_B   (1u << 0)
#define GPIO_JOG_A   (1u << 1)    /* [I] pin of IrqIOPort1 */
#define GPIO_KEY_APP (1u << 2)    /* 0 = pressed, IrqIOPort2 */
#define GPIO_FLIP    (1u << 18)   /* 1 = closed, IrqFlip */
#define GPIO_INMASK  (GPIO_JOG_B | GPIO_JOG_A | GPIO_KEY_APP | GPIO_FLIP)
gpio_in = GPIO_JOG_A | GPIO_KEY_APP | (flip_closed ? GPIO_FLIP : 0);   /* idle */
read +0x100  -> (reg[0x100] & ~GPIO_INMASK) | gpio_in

/* pin -> IRQ line levels (IC applies +0x0C polarity, level, mode 0/1) */
pin(41)=JOG_B? (unused) pin(42)=JOG_A pin(43)=FLIP pin(54)=KEY_APP
asserted(n) = (IC.cfg0C bit n) ? pin(n) : !pin(n)

/* ADC block, two instances: base 0x800 (IRQ 23) and 0x900 (IRQ 26) */
struct adc { u16 ctl08; u32 sample; bool irq; bool down; int x, y; timer t; };
write +0x08: ctl08 = v (bit 0 run, bit 3 irq enable)
read  +0x10: v = sample; irq = 0; update IC; return v
other offsets: plain storage
press(x, y): down = 1; push_sample(x, y, up=0); if touch: start t (10 ms period [I])
move(x, y) : x,y = ...                       (touch: next timer sample carries it)
t expiry   : if down: push_sample(x, y, 0); rearm
release()  : down = 0; stop t; push_sample(x, y, up=1)
push_sample: if (ctl08 & 1): sample = (up << 31) | (x << 16) | y; irq = 1; IC level(n) = irq
```
- Touch: host pointer (px, py) on the 208x320 panel -> (rawX, rawY) by the sec. 1 inverse.
- Keypad: a host key -> the centre of its rectangle in table A (flip closed) or B (open);
  press = one sample, release = one bit-31 sample. Keep one key down at a time.
- Jog: per detent, drive the GPIO sequence of sec. 4 with ~2 ms between steps (each A edge must be
  seen by ISR + DFC before the next: wait for the IC enable bit of 42 to come back, or use >= 5 ms).
- Flip: toggle GPIO bit 18; IRQ 43 asserts through the polarity bit; nothing else.
- Side key 0xB5: clear GPIO bit 2 on press, set it on release.
- sydb/window mapping suggestion: digits/*/# and the 4 flip keys -> table A when closed;
  letters -> table B (needs 0x80002820 != 0, i.e. the UI enabled the keyboard); arrows up/down ->
  jog Device1/Device2; Enter -> 0xAC corner (Confirm) [I]; F-key -> flip toggle.

## Open
- Physical names of 0xAE/0xAD/0xA7/0xB3 (flip front top row), 0xB1/0xB2/0xAC corners, 0xB5.
- Which GPIO bit is jog phase A (bit 1 guessed).
- Who calls TouchScreenCtrl ctl 2/3 (QWERTY enable) and ctl 0/1 (touch off), and how UIQ consumes
  F23/F24 (Ekdata / window server / QikFlip).
- Sample period and the meaning of +0x14/+0x18/+0x1C.
