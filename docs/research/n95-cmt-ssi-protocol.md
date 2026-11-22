# N95 modem (CMT) SSI link: just enough to be the APE end

Static reading of MCUSW (`mcusw_f0100000.bin`, Ghidra MCP; files ssi_t.c / ssi_drv.c / ssi_hw.c,
code 0xf012cd00..0xf012e600, plus hw_gdma.c 0xf010cf40..0xf010d9b4 and hw_io_event.c 0xf010e332..).
Goal: let a QEMU-side APE peer bring the link up and move ISI messages both ways.
Each claim: [verified] = read in code (function given), [inferred] = best guess.

Note for readers of the code: several ssi_drv.c functions were not functions in Ghidra (code between
inline string tables); they were created at f012d100, f012d168, f012d17a, f012d324, f012d42e.

Addresses: the driver uses virtual 0xffdXXXXX = physical 0x200XXXXX (SSI 0xffd24000 = 0x20024000
was seen at runtime; for the other blocks the same mapping is [inferred], confirmed for CAWake
0x20030804 and the SST/SSR set-up by the probe run's access log).

## Driver state block (RAM 0xd80104c8, "S")

Useful to read with gdb while bringing the peer up [verified, from the functions below]:

| S+ | meaning |
|----|---------|
| 0x02 | last control command sent and waiting for an answer (7 = OPEN_CONN) |
| 0x03 | tx_state: 0 idle, 1 OPEN_CONN sent, 3 wake test running |
| 0x04 | rx_state: 0 idle, 2 receiving data, 3 wake test running |
| 0x05 | BREAK/proxy state: 0 none, 1 BREAK seen before proxy registered, 2 proxy registered, 3 BREAK seen after |
| 0x06 | NAK counter (10 -> CMT reset) |
| 0x08 | wake-test (link) state 0..4 (3 = wake lines OK, 4 = test failed or timed out) |
| 0x0a | **APE alive**: 0 -> every control frame except 0xCAACCAAA is dropped ("APE is not alive") |
| 0x0b | BREAK handshake step: 1 -> 2 |
| 0x0f / 0x10 | "CMT-only reset" flags from the start-up reason (see Start-up, case B) |
| 0x13 | ACWake is high (driver's copy) |
| 0x2c | data channel = 3 |
| 0x3c | last OPEN_CONN word (resent after a NAK) |
| 0x44 | control word deferred until ACWake goes high |

## 1. Hardware the model needs

### SST (transmitter, CMT -> APE), 0x20024700
- +0x04 MODE (2 = awake, 0 = asleep), +0x08 FRAMESIZE 0x1f, +0x0c TXSTATE (written 0 at init, read only for the
  debug dump), +0x18 DIVISOR 2, +0x24 CHANNELS 4, +0x28 0, +0x2c 4 [verified f012de4a].
- +0x10 BUFSTATE: the driver busy-waits until it reads **0** before writing a control word [verified f012da78].
  The model returns 0 (the word goes out at once).
- **+0x80 = channel 0 tx buffer: each write is one 32-bit control frame to the APE** [verified f012da78].
- +0x8c = channel 3 tx buffer: GDMA destination for data (table at 0xf049fdb4, entry 3) [verified f010d0a2].
- +0x20 BREAK: writing 1 sends a BREAK to the APE [verified write in f012dae4; meaning from the debug label
  "SST, BREAK" in f012df36 = inferred].
- Sleep/wake: +0x04 = 0 on both SST and SSR when idle and ACWake is low (f012dc6a); = 2 on wake (f012dd12,
  f012e37e) [verified]. The model can ignore MODE.

### SSR (receiver, APE -> CMT), 0x20024600
- +0x04 MODE 2, +0x08 FRAMESIZE 0x1f, +0x0c RXSTATE, +0x28 CHANNELS 4, +0x34 TIMEOUT 200, +0x38 4 [verified f012de4a].
- **+0x80 = channel 0 rx buffer: one read = one control frame from the APE** [verified f012df2e, called first
  thing in the 0x3c handler f012d42e]. Reading presumably pops it and clears the interrupt [inferred: no other
  ack is written].
- +0x8c = channel 3 rx buffer: GDMA source for data [verified f010d0a2 table entry 3 = 0xffd2468c].
- +0x10 BUFSTATE (debug read only) [verified f012df36].
- +0x1c BREAK detected: written 0 to clear (f012dadc, called at the start of the BREAK handler) [verified].
- +0x20 ERROR: 1 = signalling error, 2 = timeout; acknowledged by writing the same value to +0x24
  [verified f012e1da]. Init: if +0x20 reads 1 it writes +0x24 = 1 ("Line ERROR happened before init") [verified f012de4a].
  The model keeps +0x20 = 0.

### Interrupts (intc numbers, handlers installed by f012d73c) [verified installation; event mapping partly inferred]
| intc | handler | event |
|------|---------|-------|
| 0x3c | f012d42e | control frame in SSR channel 0 (reads SSR+0x80) [verified handler reads it; "ch 0 rx full" inferred] |
| 0x6a | f012d100 | BREAK received from the APE (clears SSR+0x1c) [verified] |
| 0x6b | f012e1da | SSR error (reads SSR+0x20) [verified] |
| 0..3 | f010cffa.. (f010d99c(ch)) | GDMA channel 0..3 done; the handler only stops the channel and calls the client callback, it reads no status [verified f010d99c, table 0xf04a0004] |

0x6a is unmasked at init only in case A (S+0x0f = 0); f012dae4 unmasks it in case B [verified f012d73c, f012dae4].
0x3c is masked briefly around control writes in f012d8fc (no effect on a model).

### Wake lines
- **CAWake (CMT -> APE)**: block 0x20030800: +0x04 write 1 = set high, +0x08 write 1 = set low, +0x00 read = level
  [verified f012da58 (set/clear), f012df36 (read at 0xffd30800+0)]. Runtime: 0x20030804 = 1 at init [verified, probe].
- **ACWake (APE -> CMT)**: GenIO event pin 0x1d (= bit 29) of the block at 0x20031d40 [verified address use in
  f012d42e/f012dd12; physical inferred]:
  - +0x08 bit 29 = ACWake level (1 = high) [verified f012dd12, f012df36].
  - +0x10 = irq enable (write mask), +0x14 = irq disable, +0x20 = irq status, write 1 to ack [verified hw_io_event
    f010e48e / f010e4b2 / f010e46a; the driver writes 0x20000000 directly to +0x20].
  - Edge select: two set/clear pairs, (+0x28 set / +0x2c clear) and (+0x34 set / +0x38 clear) [verified f010e3fa].
    The driver writes +0x38 when ACWake is high and +0x34 when it is low, i.e. pair 2 = "detect rising" (1) /
    "detect falling" (0) [inferred]; pair 1 = edge mode [inferred]. Model: raise the pin's interrupt on every
    change of ACWake while enabled; that matches both readings.
  - Handler: f012dd12, registered with f010e332(0x1d, ...) in f012de4a. Which intc line carries GenIO events was
    not traced [inferred: the existing n95-cmt GenIO/intc model must route it].
- 0x20031d14 / 0x20031d18 written 1 at init, 0x20031d14 = 1 again when the wake test passes [verified f012de4a,
  f012dd12]; meaning unknown, the model can ignore it.
- ACCPLL lock: 0x20032d8c bit 4 must read 1, else the driver logs "ACCPLL LOCK DROPPED" and re-locks
  [verified f012da78, f012e1da]. The probe already reads 0xd490 there (bit 4 set).

### GDMA (hw_gdma.c), 4 channels, block 0x20040780 [verified f010d0a2 / f010cf40; physical inferred]
Channel `c` registers at base + 0x400*c + offset [verified]:
| off | written by f010d0a2 |
|-----|---------------------|
| 0x484 | 1 (enable) |
| 0x488 | 0 = stop (polled until it reads 0); finally 2 = **start** |
| 0x48c / 0x490 | partial word / residual bits (f010cf40 reads 0x490 >> 3 as residual bytes: model returns 0) |
| 0x494 | source physical address |
| 0x498 / 0x49c | source width code / width in bits (32) |
| 0x4a0 | destination physical address (also read back by f010cf40) |
| 0x4a4 / 0x4a8 | destination width code / bits |
| 0x4ac | element count - 1 (elements = 32-bit words for SSI) |
| 0x4b4 | 1 (from the peripheral table) |
Global: base+0x88+4*c = DMA request line: **0x42 = SSR channel 3 (rx)**, **0x4a = SST channel 3 (tx)**
[verified, table 0xf049fdb0 entry 3 bytes 9/10]; base+4 / base+8 = 1<<(c+8) (direction-dependent enable)
[verified write, meaning inferred]. The channel is the first free of 4; its done interrupt is intc `c`.

Model: when 0x488 is written 2 with request 0x42, copy `count` words from the APE peer's queue for channel 3
to guest physical `dst`, then raise intc `c`; with request 0x4a, read `count` words from guest physical `src`
and hand them to the peer as channel-3 data, then raise intc `c`.

## 2. Link protocol

### Control word (32 bits, on channel 0 both ways) [verified f012d42e, f012d8fc, f012d324]
```
bits 31..28  command
bits 27..24  channel (3 for data)
bits 23..8   length in 32-bit words           (OPEN_CONN only)
bits  7..0   checksum = byte3 ^ byte2 ^ byte1  (OPEN_CONN only; f012d9ea)
```
| cmd | word | direction | meaning |
|-----|------|-----------|---------|
| 0 | 0x00000000 | APE->CMT | link start / wake test request; CMT also sends it after a BREAK handshake [verified] |
| 1 | 0x10000fff | CMT->APE | link up, wake test passed (or BREAK handshake done) [verified words; meaning inferred] |
| 1 | 0x10000ffe | CMT->APE | link up, wake test failed/timed out (CMT ignores ACWake from then on) [verified f012d40c, f012dd12] |
| 7 | 0x73LLLLCC | both | OPEN_CONN on channel 3: LLLL = words, CC = 0x73 ^ (LLLL>>8) ^ (LLLL&0xff) [verified] |
| 8 | 0x83000000 | both | CONN_READY ch 3: receiver has armed its DMA, send the data [verified] |
| 0xc | 0xC3000000 | both | NAK ch 3 [verified] |
| - | 0xCAACCAAA | CMT->APE | BREAK handshake marker, the only word sent before "APE alive" [verified f012da78, f012dae4] |

There is no ACK command; a transfer is confirmed by DMA completion only [verified: no other cmd handled].
Any other command -> "UNKNOWN MESSAGE RECEIVED", answered with NAK [verified f012d42e].
Before APE alive (S+0x0a = 0) every received frame with cmd != 0 is ignored; while S+0x10 = 1 (case B, before
the BREAK handshake) every frame is ignored [verified f012d42e].

### Data (ISI messages) [verified unless marked]
- Data travels on **channel 3** by GDMA, 32-bit words (SSR/SST +0x8c) [verified f012d42e S+0x2c = 3, f010d0a2].
- CMT -> APE (f012ce0c -> f012d8fc): the payload is the whole ISI/PhoNet message, `len = msg[4..5] + 6` bytes
  (u16 at offset 4 read natively = little-endian on this CPU) [verified f01e5fca]. Words = (len + 3) >> 2;
  padding bytes are whatever follows in the buffer [inferred]. Max 0xffff bytes [verified f012ce0c].
  Byte order inside each 32-bit frame: memory word loaded LE by the DMA [inferred: the model sees
  b0 | b1<<8 | b2<<16 | b3<<24].
- Header layout seen in a CMT-built message (f01e6320): [0] media, [1] rdev, [2] sdev, [3] resource,
  [4..5] length, [6] robj, [7] sobj, [8..] tid, msg id, ... [verified field use; names inferred, PhoNet-style].
  Messages with resource 0xd7 stay in the CMT (task 0x2d) [verified f01e5be6].
- APE -> CMT: received words land in an 8000-byte ring (0xd80108a8), each message 32-byte aligned; the
  ssi task copies `words*4` bytes to a new buffer and gives it to the "Sos proxy" callback (S-4 = 0xd80104c4)
  [verified f012d42e, f012d3b2, f012cd86]. So the APE sends the same layout, padded to whole words.
  No checksum over the data [verified: only the control word has one].

### One transfer, sender -> receiver [verified f012d8fc, f012d42e, f012d324, f012d3b2]
```
sender:   OPEN_CONN 0x73LLLLCC           (sender raises its wake line first: CMT calls f012da58(1))
receiver: CONN_READY 0x83000000          (receiver idle, checksum OK, room in buffer) -> arms DMA for LLLL words
          or NAK 0xC3000000              (checksum bad / rx busy / buffer full)
sender:   LLLL words on channel 3
both:     DMA done -> idle; CMT drops CAWake when tx and rx are idle (f012d324/f012d3b2 -> f012da58(0) [inferred arg])
```
- CMT receiving (APE sends OPEN_CONN): accepted if rx_state = 0 and a free slot in its 10-entry ptr table
  [verified f012d42e]; replies 0x83000000 and starts DMA, rx_state = 2.
- CMT sending: waits for 0x83000000 with channel 3 while tx_state = 1 ("Illegal CONN_READY" otherwise,
  "Connected to illegal channel" for channel != 3) [verified]. A NAK for its OPEN_CONN: resend the saved word
  after timer 5000 (f012d168); 10 NAKs (any) -> "NAK limit reached" -> CMT reset [verified].
- CMT queues further tx messages (100-slot ring) while one is in flight; the next OPEN_CONN goes out from the
  tx-done callback f012d324 [verified].
- Control frames from the CMT are only written while it believes ACWake is high (S+0x13); otherwise one word is
  parked in S+0x44 and sent on the next ACWake rising edge [verified f012d8fc, f012dd12].
  **The peer should keep ACWake high** (or the link must be in wake-test state 4, where ACWake is ignored).

### Timers (f0109db6(cb, 0, ticks); unit not checked, microseconds [inferred])
- 1000: wake-test timeout -> f012d40c: link state 4, tx/rx idle, APE alive = 1, send 0x10000ffe [verified].
- 500000 (0x7a120): response timer, armed whenever tx or rx is busy (f012d17a). Each firing with the system still
  busy logs "System jammed" + SST/SSR dump; 30 in a row -> "Response time limit reached" -> CMT reset [verified].
- 5000: resend after NAK for OPEN_CONN [verified].

### Error/BREAK rules [verified]
- BREAK from the APE once it is alive -> "APE has reseted (BREAK), request CMT reset" (f012d100). Never send BREAK
  after the link is up.
- SSR error interrupt: timeout error -> CMT sends NAK 0xC3000000; signalling error while alive -> NAK or CMT reset
  (f012e1da). The model never raises 0x6b.
- "Request CMT reset" = f012cd70(1,0) -> Sos proxy callback; without a registered proxy just a log line.

### Start-up [verified unless marked]
f012d73c (task message 0xb1) initialises, raises CAWake (f012de4a -> f012da58(1)), sets "ACWake high" = 1, and
waits. **The CMT never speaks first** in case A: every control word except 0xCAACCAAA is refused until APE alive.

Start-up reason (f010c442 bitmask; case B if bit 7, 6 or 8 is set, or bit 4 without bit 9; bits 4/6/7/8 come from
reset-cause register (0xffd??+0x18 at DAT_f010c634) bits 1..4 and a RAM flag) [verified f012de4a];
"case B = the CMT alone was reset while the APE kept running" [inferred]. Cold power-on should be case A [inferred].

**Case A, cold boot (S+0x0f = 0): wake test** (f012d42e cmd 0, f012dd12, f012d40c)
1. APE -> 0x00000000. CMT: tx/rx_state = 3, link = 1, **drops CAWake**, starts the 1000 timer, enables the ACWake irq.
2. APE drops ACWake. CMT (link 1 -> 2): raises CAWake.
3. APE raises ACWake. CMT (link 2 -> 3): sends **0x10000fff**, APE alive = 1, tx/rx idle.
   - ACWake rising while still link 1 -> link 4, sends 0x10000ffe, APE alive.
   - No ACWake activity: timer -> link 4, 0x10000ffe, APE alive, ACWake irq left disabled for good.
   Simplest peer: hold ACWake high, send 0x00000000, wait for 0x10000ffe.

**BREAK handshake (case B; also possible in case A)** (f012cfba proxy registration, f012d100, f012dae4)
1. CMT, once its Sos proxy has registered and ACWake is high: sends **0xCAACCAAA** and a BREAK (SST+0x20 = 1)
   (case B: right after registration; case A: only after the APE has sent a BREAK).
2. APE sends a BREAK. CMT: APE alive = 1, sends **0x00000000** then **0x10000fff**.
In case B the CMT ignores all control frames until this is done (S+0x10 cleared in f012dae4).

"Sos proxy registered" = the CMT's own SOS proxy (sos_proxy_gen_func.c) called f012cfba at start-up; nothing the
APE does [verified that registration only sets S-4 / S+5; who calls it was not traced, inferred internal].
Above the link, the MTC start-up sync is an ISI-level exchange (not part of this document).

## 3. Proposed APE peer (QEMU side)

```
state LINK_DOWN; acwake = 1 (drive GenIO pin 29 high), no BREAK, SSR+0x20 = 0, SST+0x10 = 0

on CMT write SST+0x80 (word w):
  if w == 0xCAACCAAA:            pending_break = true                 # case B handshake
  elif w == 0x00000000:          pass                                 # follows a BREAK handshake
  elif w>>28 == 1:               state = LINK_UP                       # 0x10000fff / 0x10000ffe
  elif w>>28 == 7:               # CMT wants to send
      n = (w>>8) & 0xffff; assert (w&0xff) == 0x73 ^ (n>>8) ^ (n&0xff)
      expect_rx_words = n; send_ctrl(0x83000000)
  elif w>>28 == 8 and (w>>24 & 0xf) == 3:   # our OPEN_CONN accepted
      tx_go = true                           # data leaves when the CMT arms GDMA request 0x42
  elif w>>28 == 0xc:            retry our OPEN_CONN later (keep the queued message)
on CMT write SST+0x20 = 1 (BREAK) after 0xCAACCAAA: raise SSR BREAK (SSR+0x1c = 1, intc 0x6a)  # answer

boot (after MCUSW reaches the scheduler and SSI init ran, i.e. SST+0x2c was written 4):
  send_ctrl(0x00000000)            # case A; optional: pulse ACWake low then high for 0x10000fff
  (case B: wait for 0xCAACCAAA + BREAK, answer with one BREAK)

send_ctrl(w): SSR+0x80 <- w, raise intc 0x3c

send ISI msg m (bytes, length = u16 at m[4] (LE) + 6), only in LINK_UP and when idle:
  n = (len + 3) / 4; send_ctrl(0x73000000 | n<<8 | (0x73 ^ n>>8 ^ n&0xff))
  wait CONN_READY; then when GDMA channel c starts with request 0x42:
      write n LE words of m (padded) to guest phys dst, raise intc c
receive: GDMA start with request 0x4a, count n = expect_rx_words:
      read n words from guest phys src, raise intc c, deliver bytes[0 : u16(m[4])+6] as one ISI message
never: BREAK after LINK_UP, SSR errors, ACWake low while the CMT has something to send
```

## Open / not checked
- Which intc line carries the GenIO (ACWake) event, and the exact edge semantics of +0x28..+0x38 [inferred].
- Whether reading SSR+0x80 is the only ack for intc 0x3c [inferred].
- Timer tick unit; length byte order as the real APE sees it (big-endian ISI on the Symbian side vs the LE u16
  the CMT reads here: a byte swap may happen in the CMT's proxy or the APE driver) [inferred].
- f012da58's argument in the idle paths (decompiler lost it; read as 0 = CAWake low) [inferred].
- Who calls the proxy registration f012cfba (no direct xref; via a function pointer) [inferred internal].
