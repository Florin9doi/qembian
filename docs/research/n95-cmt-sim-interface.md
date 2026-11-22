# N95 modem (CMT) SIM card interface: just enough to mock it

Static reading of MCUSW (`mcusw_f0100000.bin`, Ghidra MCP) plus three probe runs
(`tools/cmt/probe.py --secs 8 --gdb 12399 [--gdb-from-start] --gdb-cmds FILE`, values marked "runtime").
Goal: (1) the start-up SIM self-tests pass, (2) with no card the SIM stack says "no SIM" quickly,
(3) later a card model (ATR + T=0) can be plugged in.
[verified] = read in code (function given) or seen at runtime; [inferred] = guess.

Ghidra note: the interrupt handlers run from internal RAM at 0xfe000000. The scatter-load record at
0xf04e85d4 ({0xf04e8640, 0xfe000000, 0x1f2c}) copies image 0xf04e8640..+0x1f2c there [verified: the handler
bytes match]. A Ghidra block `iram_fe000000` with that copy (Thumb) now exists, so `FUN_fe00xxxx` decompile.
Image offset of a 0xfe00xxxx address = addr - 0xfe000000 + 0x3e8640.

## 0. Start-up self-tests st_sim_clk_loop_test / st_sim_io_ctrl_loop_test (MTC state 08 cause)

hal_st.c. Wrappers FUN_f0164b5e (clk) and FUN_f0164b92 (io) map the body's return to a result byte:
0 -> 0 (pass), 2 -> 6, 3 -> 5, 4 -> 4, else 1 [verified]. Bodies FUN_f0165c18 (clk), FUN_f0165cb6 (io).

Pin table at 0xf04b0df4 (hal_st.c "HW_ST_CONN_TABLE", 9 pins; three sub-tables of {pointer, mask}) [verified]:

| pin | enable (FUN_f0165950) | set / read (+0x48) | clear (+0x90) |
|-----|------------------------|--------------------|---------------|
| 3 (input) | - | **read 0x20031dc8 bit 17 (0x20000)** | - |
| 4 (output) | 0x20031ca0 bit 2 (0x4) | write 0x20031cd4 = 0x20000 (set) | write 0x20031cd8 = 0x20000 (clear) |
| 5 (output) | 0x20031ca0 bit 3 (0x8) | 0x20031cd4 bit 18 (0x40000) | 0x20031cd8 bit 18 |

(physical addresses; MCUSW uses 0xffd31cxx/0xffd31dxx.) 0x20031cd4 / 0x20031cd8 behave as GPIO
"set" / "clear" registers (write-one) [inferred from the table layout].

Loop helper FUN_f01659bc(out=4, in=3) [verified]: set pin 4 (cd4 |= 0x20000), wait 50, r = (dc8 bit 17 == 1);
clear pin 4 (cd8 = 0x20000), wait 50, if dc8 bit 17 still 1 then r = 0. Returns 1 = "the input follows the output".

clk test FUN_f0165c18 [verified]:
1. Retu reg 0x1c bit 3 (`0x1c0008`) := 0 (old value kept, restored at the end).
2. Enable pins 4 and 5 (0x20031ca0 |= 0x4, |= 0x8).
3. 0x20031cd4 = 0x40000 (`0x1c0008 << 15` truncated: pin 5 high).
4. Retu reg 0x0e bit 13 (`0xe2000`) := 1. Loop(4,3) must return 1, else result 2 (byte 6: FAIL).
5. Retu reg 0x0e bit 13 := 0. Loop(4,3) must NOT return 1, else result 3 (byte 5).
6. Restore pins, Retu 0x1c; result 0 = pass.

io test FUN_f0165cb6 [verified]:
1-3 as above; 4. Retu 0x0e bit 13 := 1, Loop(4,3) must return 1 (else 2);
5. 0x20031cd8 = 0x40000 (pin 5 low); Loop(4,3) must NOT return 1 (else 2). 6. Retu 0x0e bit 13 := 0, restore.

Meaning [inferred]: Retu reg 0x0e bit 13 closes a test loop in the Retu SIM level shifter; pin 4 drives
the SIM clock/data source, pin 5 enables the I/O path; pin 3 reads the line back.

**Model that passes both** [inferred, derived from the two sequences]:

    dc8 bit17 (read 0x20031dc8) = out(pin4) && out(pin5) && (retu[0x0e] & 0x2000)
    out latch: write 0x20031cd4 = M -> out |= M ; write 0x20031cd8 = M -> out &= ~M

Check: clk step 4 follows (all three set), step 5 does not (Retu bit 0); io step 4 follows, step 5 does
not (pin 5 low). Keep the other bits of 0x20031dc8 as today (Retu IRQ etc.: section 1).
Today 0x20031dc8 reads 0 (runtime access log, pc f01659f1 / f0165a0d), so both tests fail with 2 -> byte 6.

## 1. Hardware

### SIM controller block: 0x20034100 (virtual 0xffd34100) [verified: literal in f01be758, fe001aa8, fe001da0]

| off | use | evidence |
|-----|-----|----------|
| +0x04 | config: 0x0020aba3 written at power-up (clock divider / ETU) [value verified, meaning inferred]; bit 0 = clock-stop / clock on (f01be340 sets, f01be3de / f01be362 clear) [inferred]; bit 2 tested in deactivate (f01be362) | f01be56a, f01be4a8, runtime pc f01be583 |
| +0x08 | interrupt status, write-1-to-clear: 0x5f = clear all at power-up; bit 0 = TX busy (TX refill handler spins while set); bit 1 (2) = RX data (ack in RX handler); bit 2 (4) = ack in intc 0xa8 handler; bit 3 (8) = ack in intc 0xa4 handler (spins until it reads 0) | fe00199e, fe001a18, fe001b4a, fe001b98, f01be56a |
| +0x0c | control bits via FUN_f01be09a(set?,mask): 0x1 (power/enable), 0x2000 and 0x1000 cleared at power-up, 0x20 set when TS = 0x3F (inverse convention), 0x400 cleared after the TX of a command (switch to receive) [bits verified, names inferred] | f01be09a, fe001914, fe001a18 |
| +0x10 | write 4 = start of TX; bit 1 polled until set after the first TX byte [inferred: TX started] | fe001a18 |
| +0x14 | byte: RX interrupt threshold = expected bytes - 1 (1 at power-up) [inferred] | fe00199e, fe001a18, f01be56a |
| +0x20 | TX data (byte), up to 16 per refill (FIFO) | fe001a18 |
| +0x24 | RX data (byte), read while the expected count > 0 | fe00199e |
| +0x28 | 16-bit error/position counter compared in the 0xa4 handler [inferred: parity error index] | fe001b98 |

### Interrupts (intc lines; installed in FUN_f01be3ea; handlers in iram) [verified install, meanings inferred]

| intc | handler | does | meaning [inferred] |
|------|---------|------|--------------------|
| 0xa6 | fe00199e | +8 = 2; drains +0x24 into the buffer; parses ATR (fe001914) or ends the response: posts code 0 (OK) | **RX data** |
| 0xa7 | fe001a18 | refills +0x20 with up to 16 bytes; at the end: Retu timer, +0x10 = 4, enable 0xa6/0xa8, +0xc clear 0x400 | **TX FIFO empty** |
| 0xa8 | fe001b4a | +8 = 4 | TX done / guard |
| 0xa4 | fe001b98 | +8 = 8; same error position twice -> post 0xf (error) | parity error (retransmit) |
| 0xa3 | fe001bf0 | disable all; post 6 if state 0x40 (waiting ATR) else 0xf | ATR / card fault |
| 0xa5 | fe001b68 | disable all; post 0xf | overrun / fatal |

### Retu (CBUS device 1) involvement [verified: hw_retu.c FUN_f010ddc0 write(reg<<16|mask, value), FUN_f010de5a read]

- Reg 0x16 bit 13 (0x2000): **card present switch** read by FUN_f01be05c: set -> 0x12 (card), clear -> 0x11 (no card).
  [read verified; switch meaning inferred]. Today 0x0500 -> no card.
- Reg 0x1c (SIM control, low byte): 0x03 power, then bit 3 -> 0x0b (vcc / level shifter on) at power-up (runtime
  CBUS log); 0x8b / 0x8d / 0x88 start a **SIM timer** (fe0016e0, fe0017dc, fe001c92); bit 2 read in the timer
  handler (set = more reloads pending) [values verified, meaning inferred].
- Reg 0x1d: SIM timer count: 0x5dc0 at power-up, 0xfffa per long reload, else work-waiting-time computed in
  fe0017dc [verified].
- Reg 0x0e bit 13: self-test loop (section 0).
- **Retu "events" 4/5/6 are Retu interrupt bits, not GenIO pins** (FUN_f010df3e is hw_retu.c): event N =
  bit (0x8000 >> N) of reg 0x01 (IDR, write 1 = ack: FUN_f010dea8) and reg 0x02 (IMR, 1 = masked: f010dec8 /
  f010deb8) [verified]. So event 4 = 0x0800, 5 = 0x0400, 6 = 0x0200. Runtime: IMR ffd4 -> f1d4 after power-up.
  - event 4 (fe001c2c): **card detect change** -> FUN_f01bdb18(obj,1) debounce state machine.
  - event 5 (fe001d3a): **SIM timer expired**: Retu 0x1c bit 2 = 0 -> fe001c92 (timeout: in state 0x40 posts
    code 6 status 2 = no ATR); bit 2 = 1 -> fe001cea (reload 0x1d, after the last reload post 0x11).
  - event 6 (fe001c74): post 0xf (error) [inferred: SIM supply fault].
- The Retu interrupt reaches the MCU on intc 0xfa (handler FUN_f010def4: reads IDR & ~IMR, calls the first
  set bit's handler, then writes 0x400 to 0x20031dc0+0x20 = GenIO pin 10 edge ack) [verified]. One event per IRQ.

## 2. Driver flow and "no card"

Object 0xd818eac0 (pointer at 0xd801a440); +0x38 = state (0x40 = activating/waiting ATR, 2 = off),
+0x78 = card-detect state, function slots +0xcc.. (slot 0x35 = f01be05c, presence) [runtime].
FUN_f01bdb18 (card detect, arg 0 = initial check) [verified]: state 0 and presence 0x11 -> post **0x13**,
state 3; 0x12 -> state 1 -> (debounce timer) state 2 -> 0x12 posts 0x14 (inserted), 0x11 posts 0xd (removed);
state 3 + 0x12 posts 0xc. Activation FUN_f01be3ea posts 0x13 at once when the detect state is not 1.
Posts go through FUN_f01bd9a4 -> message {01, 00, 0000, code, ..} to task 0x29 **APDU_SERVER_TASK** [verified].

**Runtime today** (gdb, --gdb-from-start): FUN_f01bdb18(arg 0) -> post 0x13; activate -> post 0x13 again;
APDU_SERVER_TASK receives both (FUN_f01c077e, msg 01 00 00 00 13 00 01 00) [verified]. APDU's FUN_f01c28e0
routes codes 0xd/0xc/0x13/0x14 to FUN_f01c0a7c (card status) [verified]. **So the SIM interface already says
"no card" in ~ms; the 0xFB "not ready" comes from later (APDU -> SIMSON, or the self-test fail = MTC state 08
holding the servers)** [inferred]. Next to read: FUN_f01c0a7c and SIMSON_SERVER_TASK (entry 0xf01e379f),
after the self-tests pass.

## 3. What a card needs (for a later card model)

- Presence: Retu 0x16 bit 13 = 1 and raise Retu event 4 (IDR 0x0800) on insertion; driver debounces (state 1->2)
  and posts 0x14 [verified path; timing inferred].
- Power-up FUN_f01be56a / activation FUN_f01be4a8 [verified]: +4 = 0x20aba3, Retu 0x1d = 0x5dc0 (timeout),
  Retu 0x1c = 0x03 then 0x0b, +0xc clear 0x2000/0x1000, +0x14 = 1, +8 = 0x5f, ack + unmask Retu events
  4/5/6, enable intc 0xa3/0xa4/0xa6.
- ATR: each byte via +0x24 with intc 0xa6 while +0x14 (count-1) bytes are pending; fe001914 reads TS
  (0x3B direct, 0x3F inverse), T0 low nibble = historical bytes, TA..TD presence bits; it sets the next
  expected count; when complete posts code 0 and stores in the object buffer [verified parse; exact counts inferred].
  Minimal ATR: 3B 00 (no interface bytes) or a common SIM ATR such as 3B 9E 95 80 1F C3 80 31 E0 73 FE 21 1B 66 D0 01 7B 13 0D 00 6E.
- T=0 command [inferred, standard ISO 7816-3 with this hardware]: driver writes the 5-byte header via +0x20
  (intc 0xa7 for refills), clears +0xc bit 0x400 (receive), arms the Retu WWT timer; card answers procedure
  byte(s) (INS = send rest / receive Le bytes, 0x60 = wait, 61xx/6Cxx/SW1SW2) on +0x24 with intc 0xa6.
- Model per byte: put the byte in an RX FIFO, set +8 bit 1, raise intc 0xa6 (level until +8 bit 1 cleared and
  FIFO empty). TX: on write +0x10 = 4 / +0x20 accept bytes, set intc 0xa7 when FIFO empty, +8 bit 0 = 0.

## 4. Proposed QEMU model

```
/* self-test loop (section 0): GenIO 0x20031c00 page */
w 0x20031cd4: gpio_out |= v;   w 0x20031cd8: gpio_out &= ~v
r 0x20031dc8: (other bits) | ((gpio_out & 0x60000) == 0x60000 && (retu[0x0e] & 0x2000) ? 0x20000 : 0)

/* Retu interrupts (needed for card and for timeouts) */
retu_irq_level = retu[0x01] & ~retu[0x02];  -> intc line 0xfa (level); write reg 0x01 = clear those bits

/* no card (step 1): nothing else needed: Retu 0x16 bit 13 = 0 already gives post 0x13 to APDU server.
   Safety net: when Retu 0x1c is written with 0x8b/0x8d/0x0b and no card, after ~10 ms set retu[0x01] |= 0x0400
   (event 5, timer) with retu[0x1c] bit 2 = 0 -> driver posts 6 (no ATR). */

/* card (step 2) */
on insert: retu[0x16] |= 0x2000; retu[0x01] |= 0x0800 (event 4)
on retu[0x1c] write with bit 3 set and block +0x0c power: after 1 ms queue ATR bytes into RX FIFO
r +0x24: pop FIFO; r +0x08: bit1 = fifo nonempty, bit0 = 0; w +0x08: clear bits
intc 0xa6 = (+0x08 bit1) ; intc 0xa7 = tx_fifo_empty && tx_armed
w +0x20: tx_fifo push; when the 5-byte header is complete -> T=0 card logic answers procedure byte/data/SW
SIM timer: retu[0x1c] low byte 0x8x start -> after retu[0x1d] ETU-ish ticks set event 5 unless an RX byte came
```

Open [inferred]: exact +0x04 / +0x0c bit meanings, ATR byte counting, how the RX threshold (+0x14) gates
intc 0xa6, the Retu timer's tick unit, and what APDU/SIMSON do after code 0x13.
