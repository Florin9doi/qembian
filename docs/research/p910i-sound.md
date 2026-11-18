# P910i sound path: why POPCORN aborts in Esdrv.pdd, and what the board must answer

Static reading only (ROM `eka1/p910i/core/ROM.bin`, VA = 0x50000000 + offset; no emulator runs).
[V] = read in the code, [I] = inferred.

## 1. The abort

`0x500CDFEC` is slot +0x0C of vtable `0x500D133C` [V]: that vtable belongs to a 20-byte **state object
"A"** (word layout: +0 vtbl, +4 second vtbl, +8 state number, +12 **owner** (back-pointer), +16 flag),
not to the ESound LDD channel [V]. The function does
`r3 = [this+12]` (owner) -> `[owner+0xDC]` -> `[..+8]` (the state of sibling state object "B").

The ESound.ldd field +0xDC (`str r5,[r4,#220]` at 0x500CBAE8) is an unrelated field with the same
offset: the LDD's "pending write request when the buffer is full" [V]. It is not what the PDD reads.

Esdrv.pdd has one hardware singleton ("hw", 0x1AC bytes, pointer in kernel data `0x800038A8`, refcount
`0x800038A4`, created at 0x500CF218 on the first channel) [V]. Its construction (0x500CF5D4 ->
0x500CF6FC) [V]:

| hw field | what | set at |
|---|---|---|
| +0x18 | status: 0 = not yet initialised, 1 = ready, bit 2 = codec transfer busy, bits 3/4/5 = deferred A/B/C start | 0 at 0x500CF708 |
| +0xD0 | 28-byte helper | 0x500CF848 |
| +0xD4 | I2C write queue (16 entries x 76 bytes; head +0x4D4, tail +0x4D8) | 0x500CF744 |
| +0xD8 / +0xDC / +0xE0 | state objects A / B / C (playback / record / ... [I]), **owner (+12) left 0** | 0x500CF78C / 7D0 / 814 |

The owners are filled in by only one routine, **0x500CF8C0** (`hw+0x18 = 1`; for A, B, C:
`+12 = hw, +8 = 1, +16 = 0`) [V]. It is called only from 0x500D0558, which is the I2C-completion
callback of the hw object (secondary interface at hw+20, thunk 0x500D10D4 -> 0x500D0558) and only
runs it when `hw+0x18 == 0` [V]. The ctor ends with the codec init (0x500CFCA0): a list of codec
register writes (I2C device byte 0x34, then reg, value; e.g. 31=0, 18=1, 12=0x7F, 10=8, 8=16, 7=..|0x80,
1=0x80, 14=0x72, 12=0xFF) sent through 0x500D0568 -> queue 0x500CED7C -> i2ccontroller.dll, with
hw+20 as the completion callback [V].

So **A's owner stays NULL until the first codec I2C transfer list completes** [V]. Then:

- POPCORN writes sound: ESound.ldd 0x500CBA2C copies the data into its buffer and, if not yet playing
  (`ldd+0x13C == 0`), calls `[ldd+0x5C]->vtbl+0x4C` [V] = hw vtable 0x500D1294 + 0x4C = **0x500D01BC**
  ("start play") [V].
- 0x500D01BC: if `hw+0x18 & 4` it only sets the deferred bit 8; otherwise it calls `A->vtbl+0x0C` =
  0x500CDFEC directly [V].
- `hw+0x18` is still 0 (bit 2 clear), so A->Start runs with `A+12 == 0` and loads `[0 + 0xDC]`:
  virtual 0 is unmapped -> data abort [I: fault address should be 0xDC, instruction 0x500CE004;
  "0x500CE00C" is then LR_abt = fault PC + 8. Check the DFAR: 0xDC confirms this; 0x8 would mean
  hw+0xDC itself was NULL, which only an allocation failure could do].

**Cause: a missing hardware answer, not a software race** [V for the chain, I for "never answered"]:
i2ccontroller.dll never sees an I2C transfer complete, so the codec-init callback never fires and the
state objects never get their owner. Note 0x500D0558 ignores the result code [V]: any completion,
even an error (NACK -> -2, after the PDD's one retry -> -29) is enough to initialise the objects.

## 2. The I2C controller (i2ccontroller.dll 0x508A0570, kernel extension)

Registers at virt 0x58001800 = **phys 0x58000800** (I/O page order: virt 0x58001000 = phys
0x58000000 UART bank page) [I for the phys, V for the virt]. A PNX-style I2C block:

| phys | access | driver use | model |
|---|---|---|---|
| 0x58000800 TX | strh | `addr|0x100` (START), `reg`, `val|0x200` (STOP): one 3-byte write per transfer [V] (0x508A0854..88C) | on a write with bit 9 (STOP): transfer done -> set STS bit 0 |
| 0x58000804 STS | ldrh / strh | ISR reads `& 5`: ==1 -> success, else error (bit 2 = NAI, no ack) [V]; writes 1 to clear bit 0 [V] (0x508A0B58..74; also before each transfer 0x508A0868) | bit 0 TDI (w1c), bit 2 NAI (w1c). Reset 0 |
| 0x58000808 CTL | ldrh / strh | `|= 5` before the bytes (interrupt enables TDI, NAI) [V]; ISR `&= ~5` [V]; `= 0x100` at reset/start (soft reset [I]) | store; 0x100 = reset (clear STS/FIFO), read back without bit 8 |
| 0x5800080C | strh 0x184 | clock high/low divider [I] | store only |
| 0x58000810 | strb 0 (device 0x34) / 31 (others) | clock / mode [I] | store only |
| 0x50000010 (sys page, virt 0x58000010) | ldr/str | bit 9 = I2C clock enable, set before / cleared after [V] | already in the sys page |

Interrupt: **IRQ 17 (IrqI2C)** bound with `Interrupt::Bind(17, ...)` at 0x508A0B00 [V]. Level:
`irq = (STS & CTL & 5) != 0` [I]. The ISR (0x508A0B4C) clears STS, masks CTL, queues a DFC
(0x508A08F8) that turns the clock off and calls the client back with 0 / -2 [V].

Order per transfer [V]: CTL |= 5; STS = 1; TX = 0x134; TX = reg; TX = val|0x200 -> wait for IRQ 17.
Codec init alone is about 10 such transfers back to back (the PDD queue sends one 3-byte entry per
transfer, 0x500CF0F4/0x500CF104) [V].

### Proposed board model (p910i_soc.c, ~40 lines)

```c
/* I2C (i2ccontroller.dll), phys 0x58000800, IRQ 17. Every transfer completes at once with ACK. */
typedef struct { MemoryRegion mr; qemu_irq irq; uint16_t sts, ctl; } P910I2c;
static void i2c_update(P910I2c *s) { qemu_set_irq(s->irq, (s->sts & s->ctl & 5) != 0); }
static uint64_t i2c_read(void *o, hwaddr a, unsigned sz) {
    P910I2c *s = o;
    switch (a) { case 4: return s->sts; case 8: return s->ctl; default: return 0; }
}
static void i2c_write(void *o, hwaddr a, uint64_t v, unsigned sz) {
    P910I2c *s = o;
    switch (a) {
    case 0: if (v & 0x200) s->sts |= 1; break;          /* STOP byte: transfer done, ACKed */
    case 4: s->sts &= ~(v & 5); break;                  /* w1c */
    case 8: if (v & 0x100) { s->sts = 0; v &= ~0x100; } s->ctl = v; break;
    default: break;                                     /* 0x0C/0x10 clock: ignore */
    }
    i2c_update(s);
}
/* map 0x20 bytes at 0x58000800 (priority above the catch-all), irq = p910_irq(17) */
```

A codec register file is not needed: nothing reads codec registers back (only writes are queued) [V
for Esdrv's list builder 0x500CF0A8, which only ever writes]. If ampctrl.dll also uses this I2C bus,
it gets the same instant ACK [I].

Regression test idea (tests/): a unit-level check of the model is cheap; the real test is
"POPCORN does not abort in Esdrv.pdd and `hw+0x18` becomes 1 after boot" (read 0x800038A8 -> +0x18).

## 3. Next layer (not read in depth)

After the owners are set, A->Start (0x500CDFEC) sends more codec writes (again via I2C, completion
handler 0x500CE360 via thunk 0x500D10E4) and then starts the PCM side through `[hw+4]->vtbl+0x3C`
(pcmdrv.pdd 0x500D20F0) [V for the call, I for its target]. That is where the PCM block (phys
0x28000000, IRQs 49..52) and the PL080 DMA come in; whether it polls or waits for an interrupt was not
read. If POPCORN then hangs instead of aborting, look there next: the LDD only blocks a writer when its
buffer is full (`ldd+0xDC` pending), so the PCM/DMA must eventually consume buffers (DMA completion
IRQ or PCM data IRQ) for the second and later writes to complete.

## 4. Cheapest stand-in without a model (patch, not recommended)

Make the ctor initialise the state objects instead of running the codec init:
ROM 0x500CF864 `eb00010d` (bl 0x500CFCA0, codec init) -> `eb000015` (bl 0x500CF8C0, reset) [V
encoding]. This removes the abort, but A->Start still queues I2C writes that never complete, so A
stays in its transition state and playback never starts; POPCORN may block once the LDD buffer fills.
The I2C model above is about as cheap and keeps the driver honest.
