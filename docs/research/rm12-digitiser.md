# Nokia 7710 (RM-12): touch-screen digitiser (exyin.dll)

Static reading of `eka1/rm12/core/ROM.bin` (VA = 0x50000000 + offset), no runs.
[V] = read in the code, [I] = inferred. Scope: enough to model the digitiser on `-M nokia7710`.

## Result

`exyin.dll` (0x500C78C0, code 0x500C7924..0x500C8CD0) drives an **ADS7846-class touch controller
on the OMAP1510 Microwire, chip select 0**, through EuWire.dll's bus object. Pen-down is
**MPUIO 4** ("Armio_Bit4"), edge-triggered, the edge flipped by the driver to catch pen-up.
Imports are only EKern and EUser (no Espi, no OMAP ADC). No heavy emulation needed: a
synchronous uWire slave plus one MPUIO line.

## 1. Bus and commands

- Bus object: `Kern` global 0x80000B44 (getter 0x50016F70, imported by exyin at 0x500C8998),
  set only by **euwire.dll** (setter 0x50016F60, imported at 0x500852A4). [V]
- Transfer = bus vtable+48 (euwire 0x500850E4): `(this, cs, data16, nbRd, nbWr, releaseCs,
  setupCs, u16 *dst)`. [V] It writes, all 16-bit at 0xFFFB3000 + off (helpers 0x50016F0C /
  0x50016F20):
  - CSR (+0x04) = 0x1000 (CS_CMD, CS asserted); TDR (+0x00) = data16;
  - CSR = `nbRd | nbWr<<5 | cs<<10 | 0x1000 | 0x2000 (START)`; [V]
  - "poll" of +0x00 bit 15 is a no-op (`(v & 0x8000) == 1` is never true), so **RDR must hold the
    answer as soon as CSR START is written** (QEMU's uWire is synchronous: OK). [V]
  - result = 16-bit read of RDR (+0x00), stored **unmasked** into the sample; [V]
  - releaseCs != 0: CSR = 0 afterwards. [V]
- exyin always uses **cs = 0, nbWr = 8, nbRd = 12**; the command byte is the high byte of data16
  (QEMU passes `txbuf >> 8` = the byte to the slave). [V]

| data16 | ADS7846 decode [I] | use |
|---|---|---|
| 0xD100 | S, A=101 (X), 12-bit, diff, PD=01 | X: 1 discarded + 5 kept (this+60..69) [V] |
| 0x9100 | S, A=001 (Y), 12-bit, diff, PD=01 | Y: 1 discarded + 3 kept (this+70..75) [V] |
| 0xB900 | S, A=011 (Z1), 8-bit mode, PD=01 | Z1: 2 reads, last kept (this+76) [V] |
| 0xC900 | S, A=100 (Z2), 8-bit mode, PD=01 | Z2: 2 reads, last kept (this+78) [V] |
| 0x9400 | S, A=001, 12-bit, SER, PD=00 (PENIRQ on) | final command of each burst, reply ignored, CS released [V] |

- Reply words: X, Y = 0..4095 (12 bits) [V use; I range]. Z1/Z2 are 8-bit conversions read as 12
  bits, i.e. `z8 << 4` (pen-up signature Z1 = 0x000, Z2 = 0x0FF0) [V for the constants].
- A transfer returning an error stores 0 in that sample. [V]

## 2. Pen interrupt (MPUIO 4)

- Bound by name **"Armio_Bit4"** (string 0x500C8BE8; kernel name table at 0x5001A8F0 maps
  Armio_BitN to the MPUIO GPIO-interrupt demux). [V] OMAP level: the MPUIO interrupt, already used by
  the 9500 board for MPUIO 2/3. [I]
- Construction (0x500C81D0): MPUIO IO_CNTL (0xFFFB5008) bit 4 **set** (input);
  GPIO_INT_EDGE (0xFFFB501C) bit 4 **cleared** (falling edge). [V]
- ISR 0x500C7A20: cancels the sample timer, reads GPIO_INT_EDGE bit 4:
  - 0 (was falling) = **pen down**: set edge bit 4 (now rising), pen = 1, queue sample DFC. [V]
  - 1 (was rising) = **pen up**: clear edge bit 4, pen = 0; if a Button1Down was sent, queue the
    EButton1Up DFC. [V]
- So the line is **active low** (/PENIRQ): low while touched, back high on release. [V for the edges,
  I for the level]. MPUIO INPUT_LATCH (0xFFFB5000) bit 4 = 1 means "pen up" (0x500C83A0). [V]
- Ack: the kernel's ARMIO demux (GPIO_INT read, which clears in QEMU). exyin also disables /
  clears / re-enables its interrupt object around each sample burst (0x500C89D4 / E0 / EC). [I names]
- Pen-up is also caught by sampling: Z1 == 0 and Z2 == 0x0FF0 -> recheck routine 0x500C8830 (two
  Z1 reads; Z1 != 0 -> still down, re-arm rising edge and resample; Z1 == 0 -> stop sampling and wait
  for the edge). [V]

## 3. Sampling and filtering

- Pen-down edge -> sample DFC at once. Each burst: X x6, Y x4, Z1 x2, Z2 x2, then 0x9400. [V]
- X: median of 5 (bubble sort, element 2); Y: median of 3 (element 1). [V]
- Pressure (0x500C7BBC): if Z1 != 0: `Rt = X * 100 * (Z2/Z1 - 1) / 4096` (floats; 100.0f at
  0x500C7CC8, 2^-12 at 0x500C7CCC); **Rt > 200 -> sample rejected**. Z1 == 0 -> rejected (pen-up check
  if Z2 == 0x0FF0). [V]
- Then raw -> screen (section 4), and in **screen pixels**: [V]
  - first good sample after the edge: kept, immediate resample; the second within |dx|,|dy| <= 99 of
    it -> **EButton1Down (10)**; otherwise keep waiting.
  - after the down: one-sample delay line; a move is sent as **EPointerMove (1)** only if
    |dx| > 2 or |dy| > 2, and |dx| <= 429, |dy| <= 199 (bigger jumps dropped).
  - pen up (edge) -> **EButton1Up (11)** at the last reported point.
  - Constant samples are fine: they just send no moves. [V]
- Timer (this+40): created with 10 (0x500C7978 -> EKern 0x50025B04), re-queued after each burst
  while the pen is down. [V for the 10; I: 10 ms]
- Events: `TRawEvent::Set(type, x, y)` (EUser 1073) after clamping x to 0..639 and y to 0..319, then
  the kernel add-event (EKern 0x50029A38). [V]

## 4. Raw -> screen (calibration)

- 0x500C8690: with six words at 0x40000800 + 0x194..0x1A8 (kernel super-page area) [V]:
  `sx = (R11*x + R21*y + Tx) >> 16`, `sy = (R12*x + R22*y + Ty) >> 16`.
- Defaults written by the deferred start-up routine 0x500C828C (after configuring CS 0 through bus
  vtable+32/+40) [V]: R11 = 11463 (0x2CC7), R12 = -2, R21 = -25, R22 = 6159 (0x180F),
  Tx = -2310033 (0xFFDCBC6F), Ty = -2175762 (0xFFDECCEE).
- With these: **raw X ~ 206 -> sx 0, ~ 3860 -> sx 639; raw Y ~ 354 -> sy 0, ~ 3748 -> sy 319**
  (no axis swap or flip; the small cross terms barely matter). Inverse for the board:
  `x = (sx*65536 + 2310033 + 25*y) / 11463`, `y = (sy*65536 + 2175762 + 2*x) / 6159`.
- A user calibration (HAL XY-input calibration, stored by the shell on C:) may overwrite these words
  later. [I] Any raw 0..4095 is accepted; out-of-screen results are clamped. [V]
- Start-up self-test (0x500C83E0, two Z1 then two Z2 with 1000-iteration delays): must read
  **Z1 == 0 and Z2 == 0x0FF0**, else -2 (exyin fails). [V] So the pen must be up at boot.

## 5. Board model (sketch)

```c
/* ADS7846-like touch controller on uWire CS 0 + /PENIRQ on MPUIO 4 (nokia7710 only;
 * on the 9500 CS 0 is the cover display). */
typedef struct {
    qemu_irq penirq;       /* omap_mpuio_in_get(mpu->mpuio)[4], high = pen up */
    uint8_t cmd;
    bool down;
    int sx, sy;            /* screen pixels */
} Rm12Touch;

static void rm12ts_send(void *o, uint16_t data, int len)   /* len = 8: the command byte */
{
    ((Rm12Touch *)o)->cmd = data & 0xff;
}

static uint16_t rm12ts_raw_x(Rm12Touch *s) { return (s->sx * 65536 + 2310033 + 25 * 2000) / 11463; }
static uint16_t rm12ts_raw_y(Rm12Touch *s) { return (s->sy * 65536 + 2175762 + 2 * 2000) / 6159; }

static uint16_t rm12ts_receive(void *o, int len)           /* len = 12 */
{
    Rm12Touch *s = o;
    switch ((s->cmd >> 4) & 7) {
    case 5: return s->down ? rm12ts_raw_x(s) : 0;          /* X */
    case 1: return s->down ? rm12ts_raw_y(s) : 0;          /* Y (also the 0x94 power-down) */
    case 3: return s->down ? 0x800 : 0x000;                /* Z1 (8-bit << 4) */
    case 4: return s->down ? 0x800 : 0xff0;                /* Z2: Z2 == Z1 -> Rt = 0 */
    }
    return 0;
}

static void rm12ts_event(Rm12Touch *s, int sx, int sy, bool down)  /* from the UI */
{
    s->sx = MIN(MAX(sx, 0), 639);
    s->sy = MIN(MAX(sy, 0), 319);
    if (down != s->down) {
        s->down = down;
        qemu_set_irq(s->penirq, !down);    /* falling = down, rising = up */
    }
}
/* init: s->down = false; qemu_irq_raise(s->penirq);  // line idles high
 *       omap_uwire_attach(mpu->microwire, 0, rm12ts_send, rm12ts_receive, s); */
```

Notes: a press must last at least two sample bursts (two good samples) for EButton1Down; a
drag moves by > 2 px per timer tick to be seen. If the user calibration differs, map through the
super-page words instead of the defaults (or read them from guest memory). [I]
Side note (out of scope): euwire.dll itself names "Armio_Bit7" (0x50083868). [V string only]
