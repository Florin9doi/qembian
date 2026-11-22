# P800 display: why popcorn's DoRequest 0 never completes, and what the board must do

Static reading of the P800's `videodriver.dll` (kernel extension, VA 0x500AD0D0, 10572 bytes) and
EKern's DMA helpers (`eka1/p800/core/ROM.bin`). [V] = read in the code, [I] = inferred, [V run] =
seen in a run. Virtual -> physical as in `p910i-display.md` (virt 0x58001700 -> phys 0x58000700
LCD FIFO, 0x58000010 -> phys 0x50000010 CGU, 0x58002000 -> phys
0x70000000 DMA, 0x58006000/0x58007000 -> phys 0x18000000/0x18200000 chip, 0x5800B000 -> phys
0x30000000).

Same SoC, same blocks as the P910i, older driver: one object with a timer-driven power-on state
machine, and the pixel transfer is a PL080 memory-to-peripheral DMA **flow-controlled by the LCD
FIFO block** (word count in the FIFO block, not in the DMA Control word).

## 1. What completes DoRequest 0

Channel vtable 0x500AF9D0: DoCancel 0x500AD1A4, DoRequest 0x500AD1B8, DoCreate 0x500AD164,
DoControl 0x500AD28C (22 functions, jump table) [V]. Driver singleton at kernel data 0x80002784 [V].

DoRequest 0 (0x500AD1B8 -> 0x500ADD78 -> 0x500ADDE4) [V]: stores the completion callback
(0x500AD260 = `Complete(req, KErrNone)`) at drv+0x68, then:
- if the driver state `drv+4` is already **5** (panel on) it completes at once;
- else (state 10 = off, set at construction) it powers the panel up: CGU |= 0x1C000 (fault "Lcd FIFO
  not enabled" if bit 14 does not read back), FIFO block set-up, a 1-unit priming DMA on channel 2,
  chip writes **02=01**, **38=82**, target `drv+0x64` = 5, and runs the state machine (0x500AE104).

State machine 0x500AE104 (state at drv+4) [V]; timers are one-shot objects created with periods 10,
20, 30, 40, 80 (drv+0x70..0x80; unit ms or ticks [I]) whose callback queues the state DFC (drv+0x84):

| state | does | next step triggered by |
|---|---|---|
| 0 | power resource on (0x100) | timer drv+0x74 (20) -> 1 |
| 1 | WriteReg(0x1C, 0, 0x20) -> **1C=20** | timer drv+0x80 (80) -> 2 |
| 2 | WriteReg(0x01, 0, 0x20) -> **01=20**; state=3; **full-frame DMA** (0x8200 words from FB offset 0); state=4 | **DMA completion, result 1, seen while state == 4** |
| 4 | WriteReg(0x00, mask 0x20, 0x40) -> **reg 0 bit 6 = display on** | timer drv+0x80 -> 5 |
| 5 | **calls drv+0x68 = completes DoRequest 0**; if target is 10, goes on to power-off (6..9) | - |

Matches the run exactly: 38=02 (bootstrap), 02=01, 38=82, 1C=20, 01=20, two ch2 DMAs (the priming
one and the full frame), then nothing: **stuck in state 4** [V code + V run].

How state 4 is left [V]: the panel object (0x500AE5A0, a kernel DMA-client class on channel 2,
callback vtable at panel+0x34 = 0x500AF93C, slot +8 = **0x500AE980 `DmaComplete(result)`**):
- result 1 and `drv state == 4` -> queue DFC panel+0xA90 (0x500AEA18) -> state machine 4 -> 5.
- any result: then, unless the camera object's flag is set, enable IrqDispFifo (only in states 3..5).

The result comes from EKern's per-channel DMA ISR 0x50019594 (bound to **IrqDma, IRQ 0**) [V]:
`0x5000F820(ch)`: IntStatus(ch) (virt 0x58002000) == 0 -> ignore; IntTCStatus(ch) set ->
`table[0x80000224][ch] <= 31 ? 1 : 3`; IntErrorStatus -> 2. Result 1: clear TC/Err
(IntTCClear +0x08, IntErrClr +0x10), then `DmaComplete(1)` **from the ISR** [V].

**Why it hangs [I, strong]:** the state handler (running in a DFC, interrupts enabled) starts the
DMA and only *afterwards* writes state = 4 (0x500AE1A4..0x500AE1BC). Our PL080 completes the
channel inside the Config write, IRQ 0 is taken at once, `DmaComplete(1)` sees state **3**, queues
nothing, and no other event ever moves state 4. On the phone the 133 KB transfer takes milliseconds.
(The P910i's driver tolerates instant completion; this one does not.)

**What the board must do:** complete the DMA *later* (a timer, ~1-7 ms of virtual time), i.e.
TC + IRQ 0 after the guest has returned from the DFC. Nothing else is needed for DoRequest 0:
DispSync and DispFifo are not on this path. Quick check before coding [run]: a breakpoint at
0x500AE980 should show r1 = 1 and `[[r0+0x3C]+4]` = 3.

`table[0x80000224][2]` (EKern's per-channel "remaining" [I]) is written by EKern's generic transfer
path (0x5000F344.. etc.); the videodriver programs the channel directly, so for its transfers it
keeps the value from the 1-unit priming transfer (<= 31) -> result 1 [I].

## 2. LCD FIFO block (phys 0x58000700, virt 0x58001700) as the P800 driver uses it

| off | access | driver does [V] | board must |
|---|---|---|---|
| +0x00 | ldrh/strh | power-up: if CGU bit 14 was on, wait **bit 4**; writes **0x1FCA**; power-off clears bit 6 then waits bit 4 | read bit 4 set (as now) |
| +0x04 | ldrb | FIFO level, spins while **> 0x78** before every chip access | read 0 (as now) |
| +0x08 | str (EKern 0x50011ABC) | **transfer length in words** (0x8200 = full frame) for the flow-controlled DMA | keep; may size the DMA delay |
| +0x10 | bit set (EKern 0x50011ABC) | **bit 0 = start** (`|= 1` right after +0x08) | keep; clear bit 0 on DMA completion [I] |
| +0x14 | str | 0xD0000000 (watched window start) | keep |
| +0x18 | str | 0xD0020800 (end) | keep |
| +0x1C | strh | 0x10 | keep |
| +0x20..+0x5F | str / ldr | init writes 0xFFFFFFFF to all 16 words (W1C clear); DispFifo DFC scans it, 260 blocks of 512 bytes, W1C | read 0 (as now) |

"Lcd FIFO not enabled" (Kern::Fault -2) = CGU phys 0x50000010 bit 14 does not read back after
`|= 0x1C000` [V]; the sys model keeps written values, so it holds.
Bus timing at phys 0x30000000: strb 0x80 to +0, 0x00122447 [I: literal read as a pool word] to +4,
0x2E8D to +8: write-only, catch-all is fine.

Interrupts [V]: **IrqDispFifo (11)** config 1 at panel+0x40 (ISR 0x500AE480: disable itself, queue the
dirty-scan DFC); **IrqDispSync (47)** config 3 at panel+0x58 (ISR 0x500AE534: writes 0x8000 to
virt 0x58000304 (IC ack of line 47 [I]), sets the camera object's flag). IrqCamErr is the camera
object's (out of scope). DispFifo = "something in the watched window is dirty" [I]: with the dirty
bitmap reading 0 the board must **not** raise IRQ 11 (each DFC would probe 512 blocks, find none
and re-enable it: a busy loop). DispSync at 60 Hz as now is harmless [I].

## 3. Frame buffer and display on

- Phys **0xD0000000** [V run: DMA source; V: FIFO +0x14/+0x18 window 0xD0000000..0xD0020800], linear
  address from a kernel chunk (drv+0x5C, Epoc::LinearToPhysical at 0x50014B60) [V].
- **208 x 320, 16 bpp, stride 416 bytes** (drv+0x0C=208, +0x10=320, +0x24=16, +0x30=416; twips
  3328 x 5120) [V]. Full frame = 0x8200 words = 0x20800 bytes [V]. RGB565 [I] (as the P910i).
- **Display on = chip register 0 bit 6** (state 4: `WriteReg(0, mask 0x20, val 0x40)`) [V], the
  same bit as the P910i, so the existing console logic works once state 4 is reached.
  WriteReg = 0x500AEA88: shadow[reg] (kernel data 0x80002740) = (shadow & ~mask) | val; if the DMA
  channel is enabled it halts it (Config bit 18) and waits for Active (bit 17) to clear, writes
  index/value/0x0B to the chip, then un-halts [V]. The bootstrap/init also *reads* chip reg 0x38
  (ldrb at phys 0x18200000) and ignores the value [V].
- 1C=20, 01=20, 02=01, 38=82: panel power/booster/mode steps [I]; not needed for the picture.

## 4. Proposed change to p910i_lcd.c (P800 only; the P910i keeps instant completion)

```c
/* P910Lcd: */
    QEMUTimer *dma_timer;
    uint8_t dma_pending;            /* channels whose TC is still to come */
    int64_t dma_delay_ns;           /* 0 = finish inside the Config write (P910i) */

static void dma_finish(P910Lcd *l, int n)
{
    l->ch[n][3] &= ~0xfffu;
    l->ch[n][4] &= ~1u;
    l->tc_raw |= 1u << n;
    l->fifo[0x10 >> 2] &= ~1u;      /* LCD FIFO transfer done [I] */
    dma_update(l);
}

static void dma_timer_cb(void *opaque)
{
    P910Lcd *l = opaque;
    uint8_t p = l->dma_pending;

    l->dma_pending = 0;
    for (int n = 0; n < 8; n++) {
        if (p & (1u << n)) {
            dma_finish(l, n);
        }
    }
}

/* dma_run(): after the copy / LLI walk, instead of the tail that clears and sets tc_raw: */
    if (!l->dma_delay_ns) {
        dma_finish(l, n);
        return;
    }
    /* H-p800-dma: the P800 videodriver writes its state only after starting the transfer and
     * drops a TC that arrives first (docs/research/p800-display.md 1) */
    uint32_t words = l->fifo[0x08 >> 2] ? l->fifo[0x08 >> 2] : 1;   /* flow 5: FIFO counts */
    l->dma_pending |= 1u << n;
    timer_mod(l->dma_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + l->dma_delay_ns
              + (int64_t)words * 4 * 50);                            /* ~20 MB/s [I] */

/* dma_write(), Config: don't restart a channel that is only being un-halted */
        if ((val & 1) && !(val & (1 << 18)) && !(l->dma_pending & (1u << n))) {
            dma_run(l, n);
        }

/* lcd_reset(): */ l->dma_pending = 0; timer_del(l->dma_timer);
/* init: */ void p910_lcd_init(MemoryRegion *sys, bool p800)
    l->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dma_timer_cb, l);
    l->dma_delay_ns = p800 ? 1000000 : 0;    /* 1 ms + size */
```

Keep Config bit 17 (Active) reading 0 (it does: the stored value never has it), or WriteReg would spin
while a transfer is pending. Call site: `p910_lcd_init(sys, !strcmp(phone->machine, "p800"))`.
Regression test idea: a host-side check that DoRequest 0 completes (popcorn passes 0x50AD472E) and
chip reg 0 bit 6 is set on `-M p800`.

Open [I]: unit of the state timers; whether the FIFO block also raises DispFifo at transfer end on
the phone (not needed); meaning of chip regs 01/02/1C/38.
