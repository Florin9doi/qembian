# Nokia 9500 (RA-2): the XBUS modem link, enough to mock the peer

Static reading of the core ROM (`eka1/ra2/core/ROM.bin`, VA = 0x50000000 + offset); nothing was run.
[V] = read in the code or data, [I] = inferred, best guess.

Modules (files.json): XbusDma.pdd 0x500B37C0 (0x1E64), XbusProtocol.ldd 0x500B5690 (0x2200),
Isa_if_driver.ldd 0x500F1040, Isa_if_protocol_base.dll 0x500F3390, IsaApi.dll 0x501003B0,
`System/Isa_api_protocol.ini` 0x500F1000+ (UTF-16 text).

## 0. Who waits for what

- `Isa_api_protocol.ini` SECTION_XBUS: `xbusdma.pdd`, `xbusprotocol.ldd`, `isa_if_driver.ldd;4`:
  the `4` is `header_size`, "bytes appended in the beginning of the message ... only when using Xbus" [V].
  So every ISI message the ISA_IF driver hands down has 4 spare bytes in front for the XBUS header.
- IsaApi `RIsaApi::CheckSync` (0x501013EC) [V]: DoControl(5) on the isa_if channel = GetState into a
  4-byte buffer; state 0 = synced, done. Otherwise DoRequest(2) (state-change notify) plus a
  2 000 000 us timer (literal 0x001E8480 at 0x501014EC), User::WaitForRequest(both); if the timer wins:
  `Panic("IsaApi", 4)` "Timer expired, Sync not received!" [V]. Loops until GetState returns 0.
- The state reaches 0 when XbusProtocol's link state ([link+0x40]) goes 1 -> 2 and it calls its
  state-change routine 0x500B5988(link, 0) [V], which forwards to the protocol base (0x500B7494 ->
  Isa_if_protocol_base) [I: that is how isa_if's state becomes 0].

## 1. Hardware: what XbusDma.pdd drives

Strings in the PDD: `XLv2_UART2`, `Armio_Bit3` [V]. Kernel maps 0xFFF00000 -> 0x58000000, so:

| Block | VA literal | Phys | Use |
|---|---|---|---|
| UART2 (OMAP "UART2 COM") | 0x580B0800 | 0xFFFB0800 | the data link, 8N1 [I], DMA both ways [V] |
| MPU GPIO | 0x580CE000 | 0xFFFCE000 | GPIO 8 = wake-the-modem output (pulse) [V] |
| MPUIO | 0x580B5000 | 0xFFFB5000 | MPUIO 3 = modem-wants-to-talk input, edge IRQ ("Armio_Bit3") [V/I] |
| System DMA | via kernel accessors at 0xFFFED800 + ch*0x40 | 0xFFFED800 | one RX, one TX channel [V] |
| UART1 / UART3 | 0x580B0000 / 0x580B9800 | | only in a const base table (0x500B5524..), not used for XBUS [V] |

UART register access goes through EKern accessors (`base, value`) that index OMAP UART registers
(reg*4): e.g. 0x5000EB68 = read LSR, 0x5000EC5C = read MSR, 0x5000EDEC = write LCR,
0x5000ED5C = write FCR, 0x5000EDC8 = write MDR1 (0x20), 0x5000EEB4/0x5000EED8 = SCR (0x40),
0x5000EEA8 = reg 0x4C [V offsets, I names].

UART2 init (0x500B3FE8) [V]: wait LSR.THRE (bit5); LCR=0xBF (EFR access); EFR bit4 (enhanced) set;
LCR=0; MCR |= 0x40 (TCR/TLR access); TCR=0x2D, TLR=0; MCR &= ~0x40; LCR=0; EFR=0x57 [I: auto CTS/RTS +
flow]; LCR 0xBF, EFR bit4 cleared, 0x4C=0; LCR set (8 data bits, via 0x5000EE3C, divisor [I]); MDR1=0
(16x UART mode); SCR |= 0xC3 (DMA mode 2, trigger granularity) ; IER restored.

Modem lines [V]:
- `0x500B3F80` reads MSR and tests bit 4 = **CTS**: "modem ready/awake". Send (0x500B3A70) and the
  wake path test it.
- Wake (0x500B47B0 / 0x500B3C2C): if CTS is low, GPIO DATA_OUTPUT (0xFFFCE004) bit 8 is cleared,
  ~62 short delays, then set again (a low pulse), and the PDD waits for CTS / its timer.
- MPUIO: `GPIO_INT_EDGE`-ish register at MPUIO+0x1C gets bit 3 set (0x500B3DD8) [V]; MPUIO 3 is the
  modem's "I have data" line (edge IRQ) [I].
- LSR error bits (0x500B4240): bit4 break, bit1 overrun, bit2 parity, bit3 framing -> error codes 1..4
  [V].

DMA [V register writes, I names]: two kernel DMA channel objects, both with CSDP=0, CICR(+4) = 0x23 (RX,
timeout/drop/block IRQs) or 3 (TX), CFN(+0x12)=1, CSFI/CSEI = 0; RX CCR(+2) = **0x400F** = sync 15
(OMAP_DMA_UART2_RX), destination post-increment; TX CCR = 0x1000 (source post-increment), sync set
when started [I: 14 = UART2_TX]. The RX path also polls the channel's current-position register
(ch+0x18, 0x500B4C88) from a 50-tick timer [V] to notice partial frames.

What QEMU 11.1.3 gives (build/qemu-ngage/qemu-11.1.3) [V]:
- `omap_uart_init` is a plain `serial_mm_init(base, regshift 2)`: 16550 registers 0x00..0x1C only.
  **No EFR/TCR/TLR/MDR1/SCR**, and the `txdma/rxdma` arguments are **ignored** (no DMA requests).
- `hw/dma/omap_dma.c` implements OMAP 3.1 channels with hardware-synchronised requests (`drq`), so
  DMA would work if UART2 raised `s->drq[OMAP_DMA_UART2_TX/RX]`. The CPC/timeout behaviour the RX
  poller expects is unverified [I].
- MPUIO and GPIO models exist in omap1.c (lines can be driven with qemu_irq) [V].

Verdict: doing it at the register level needs a new UART2 model (16550 + OMAP extras + DMA requests +
CTS) on top of QEMU's DMA, and the DMA RX timeout/CPC semantics to be right. Medium-heavy and fragile.
See section 4 for the cheaper PDD stand-in.

## 2. XBUS framing and the sync handshake

All in XbusProtocol.ldd [V unless marked].

Header = 4 bytes, built by 0x500B5E9C: `b0, b1, b2, crc` where **crc = CRC-8, poly 0x07, init 0, no
reflection, over b0..b2** (table of 16-bit entries at 0x500B759C; routine 0x500B5E00). Checked on receive
(0x500B636C); a bad CRC restarts the receiver / counts an error.

Frame types (receive switch at 0x500B6408, on `b0 & ~0x08`; bit 3 is a sequence bit):

| b0 | meaning | b1 | b2 |
|---|---|---|---|
| 0x01 | line event: state-change callback with -2 [I: wake/idle marker] | - | - |
| 0x10 | **SYNC** (peer starts sync; phone: link state 0 -> 1) | 0 | 0 |
| 0x20 \| S<<3 | **DATA**, payload of `b1<<8 \| b2` bytes follows | len hi | len lo |
| 0x30 | **ACK**; b1 = what is acked: 0x10 sync, 0x20/0x28 data with seq bit, 0xF0 | see left | 0 |
| 0x31 | ignored | | |
| 0xF0 | peer request; phone answers ACK `30 F0 00 F5` [I: ping/reset] | | |

Ready-made frames (CRC computed):
- SYNC: `10 00 00 A2`
- SYNC ACK: `30 10 00 B6`
- DATA ACK seq 0: `30 20 00 4F`; seq 1: `30 28 00 crc8(30 28 00)`.
- ACK of F0: `30 F0 00 F5`.

The phone's receiver reads a 4-byte header, then (for DATA) the payload length (0x500B64AC stores the
length and S, then the next receive is `len` bytes) [V]. No payload CRC seen [I].

Sync handshake (link state at [link+0x40]: 0 idle, 1 syncing, 2 synced):
1. Phone boots with state 0 and only listens (state 0 -> 1 happens **only** on a received SYNC,
   0x500B649C) [V]. So **the peer speaks first**.
2. Peer -> phone: `10 00 00 A2`. Phone: state 1, queues its sync sender (DFC at link+0x1C ->
   0x500B6B0C) [V].
3. Phone -> peer: while in state 1 it sends one byte per send-complete, cycling through the table at
   0x500B77FC: `01 10 00 00 A2 01 10 00 00 A2 ...` [V bytes; I: byte-wise so a hunting receiver can
   lock on]. The peer should scan for `10 00 00 A2`.
4. Peer -> phone: `30 10 00 B6`. Phone (state 1 and ACK b1 == 0x10, 0x500B6560): state 2, resets its
   queues, calls the state-change routine with 0 -> isa_if GetState = 0 -> IsaApi CheckSync returns
   [V path]. Sending the ACK right after step 2 (without waiting for 3) should be accepted, since the
   handler only checks state == 1 [I].

Keep CTS asserted (MSR bit 4) throughout so the phone never waits on the wake pulse [I].

## 3. ISI after sync

- Phone -> peer DATA (0x500B692C) [V]: the ISA_IF buffer already has 4 spare bytes; the header is
  written over them: `b0 = 0x20 | (txseq<<3)`, `b1:b2 = descriptor length - 4` (big-endian), b3 = CRC.
  txseq (bit 3 of [tx+0x1C]) toggles after each frame. Bytes 4.. are the **ISI message exactly as the
  ISA_IF driver got it** (media byte first: same layout the N-Gage/6600 isa_chan tables decode) [I on the
  media byte value].
- The peer must ACK each phone DATA frame with `30 (0x20|seq<<3) 00 crc`; the phone compares the seq bit
  with its own ([rx+5]) before releasing the next frame (0x500B65F0) [V].
- Peer -> phone DATA: same header with the peer's own toggling seq bit, then the ISI message. The phone
  ACKs with `30 (0x20|S) 00 crc` (0x500B60B0 / 0x500B6278) [V]. Start the peer's seq at 0 after sync [I].
- One message per frame; maximum receive buffer is ~2000 bytes (PDD restarts RX with 2000,
  0x500B4224) [I].

So the board feeds bytes 4.. of each phone DATA frame to the existing reply-table engine
(`ngage_isi.c` concept) and wraps each reply in a DATA header.

## 4. Proposed board-side peer

### 4a. Register-level (if we keep the real XbusDma.pdd)

New device `nokia9500_xbus` replacing omap1's UART2 at 0xFFFB0800 (size 0x400):
- 16550 core + OMAP extras as plain storage: LCR 0xBF bank (EFR at 0x08), MCR bit6 TCR/TLR bank,
  MDR1 0x20, SCR 0x40, 0x44 SSR (bit0 TX FIFO full = 0), 0x4C, 0x50 SYSC, 0x58 SYSS (bit0 reset done = 1).
- LSR: THRE|TEMT always (0x60), DR when the RX queue is non-empty. MSR: CTS (bit 4) = 1 always.
- TX: each byte written to THR (by CPU or by the TX DMA channel) goes to the protocol parser; raise
  `drq[OMAP_DMA_UART2_TX]` while SCR has DMA enabled and the TX FIFO has room (always).
- RX: queue bytes from the peer; raise `drq[OMAP_DMA_UART2_RX]` while bytes are queued and SCR DMA is on;
  UART2 IRQ (`OMAP_INT_UART2`) for RX timeout when DMA is off or the frame is shorter than the DMA count.
- Before queueing a peer frame, pulse MPUIO 3 (qemu_irq into omap1's mpuio).
- Parser: hunt `10 00 00 A2` -> queue `30 10 00 B6`; at link-up time (first UART init seen + ~10 ms)
  queue `10 00 00 A2`; DATA header -> collect `len` bytes -> queue ACK -> hand ISI to the reply engine ->
  queue reply DATA frames.
Risk: the RX DMA's timeout IRQ / CPC polling semantics in QEMU's omap_dma (unverified).

### 4b. Cheaper: an XbusDma.pdd stand-in (like H-ra2-doc)

Replace the PDD's exported entry points with a stub that talks to a board mailbox; the board then does
the section 2/3 framing (or skips the byte level altogether). XbusDma exports (export dir 0x500B564C, 8
entries) [V addresses, I names from behaviour]:

| Ord | Addr | Call (this = the 0xFC-byte PDD object) | Behaviour to mimic |
|---|---|---|---|
| 1 | 0x500B481C | `CXbusDma* New()` | allocate 0xFC bytes, construct (0x500B3828); return pointer or 0 |
| 2 | 0x500B3A54 | `SetOwner(this, link)` | store link at +0x2C (panics on 0); link = XbusProtocol object for callbacks |
| 3 | 0x500B3A70 | `Send(this, const TDesC8& frame)` | queue frame bytes to the modem; later call link callback ord 8 |
| 4 | 0x500B3C70 | `Receive(this, TInt len)` | arm a receive of `len` bytes (4 = header, then payload); when done call ord 6 |
| 5 | 0x500B419C | `LineError(this, TInt code)` / restart RX | called on LSR errors; stub: no-op |
| 6 | 0x500B3FA0 | `Lock(this)` | nested interrupt disable (count at +0x64, saved CPSR at +0x68) |
| 7 | 0x500B3FCC | `Unlock(this)` | undo 6 |
| 8 | 0x500B467C | `PowerDown/Cancel(this)` | cancel timers; stub: return |

Callbacks the PDD makes into XbusProtocol.ldd exports (dir 0x500B78A0) [V addresses]:
- ord 6 0x500B57F8 `RxDone(link, ptr?, len?)`: (0,0) = error path; otherwise feeds the receive parser.
- ord 8 0x500B5860 `TxDone(link)`: advances the TX state machine (this is what clocks the sync bytes).
- ord 11 0x500B5988 `LineState(link, TInt state)`: -2 = modem line event; stored at +0x54.
- ord 12 0x500B59EC `IsIdle(link)` -> bool.
The exact argument layout of RxDone (pointer to the 4-byte header vs. a descriptor) needs one more read of
0x500B57F8..0x500B5B88 and of the PDD's RX completion (0x500B4D44) before writing the stub [I].

Cheapest of all [I]: since IsaApi only needs isa_if's GetState = 0 and then ISI buffers, a stand-in at the
protocol-base level (replacing XbusProtocol.ldd's `DIsaIfProtocolBase` virtuals) would skip framing
entirely, but that interface was not read here.
