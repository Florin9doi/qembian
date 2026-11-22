# N95 modem (CMT) MCU-DSP interface (MDI): just enough for a fake DSP

Static reading of MCUSW (`mcusw_f0100000.bin`, Ghidra MCP; hal_mdi.c / hw_mdi_rap3g.c 0xf0121f14..0xf0123390,
mdi_mm.c 0xf0121a00..0xf0121e90, router pn_router 0xf011bbe6/0xf011c75e/0xf011ccf4) plus one probe run
(`tools/cmt/probe.py --secs 6 --gdb`, values marked "runtime").
Goal: a QEMU-side fake DSP that the MCU accepts as booted, that registers PhoNet device 0x7c, and that
moves ISI messages both ways. [verified] = read in code (function given) or seen at runtime; [inferred] = guess.

Address mapping as in the SSI doc: virtual 0xffdXXXXX = physical 0x200XXXXX [verified at runtime with
`monitor gva2gpa 0xffd40280` -> 0x20040280].

## 1. Hardware

### Blocks and memory

| what | virtual | physical | notes |
|------|---------|----------|-------|
| MDI interrupt/doorbell block | 0xffd40280 | 0x20040280 | +4 MCU->DSP doorbell, +8 DSP->MCU interrupt clear (below) [verified f010da88, f010da94] |
| MDI misc | 0xffd40300 | 0x20040300 | +4 = 1 written just before the DSP is started (f0122d1c, pc f0122dfb, runtime) [verified write; meaning inferred: MDI/DSP-window enable] |
| DSP reset control | 0xffd32d00 | 0x20032d00 | **+0x18 = 1 starts (releases) the DSP** (f0122e7e, pc f0122e83 runtime). Same block +0x14 = 2 is the MCU's own reset (f010c352) [verified write; meaning inferred] |
| pdram0 (hw_conf record 5, size >= 0x4000) | 0xff000000 | **0x30000000** | DSP RAM shared with the MCU; the mailboxes live at +0x3800/+0x3c00 [verified f0122d1c; runtime gva2gpa]. Our map has it as `quiet 0x30000000 0x00100000` |
| mdi0 (hw_conf record 0x10, size >= 256K) | 0xe0800000 | **0x08140000**, size 0x80000 | SDRAM buffer pool for DSP->MCU messages [verified f012223e; runtime] |

Address space the DSP uses in mailbox entries ("DSP address"): **SDRAM offset = physical - 0x08000000**.
[verified f012223e: S+0x20 = virt(mdi0) - phys(mdi0) + 0x8000000 (runtime 0xe06c0000), MCU virt = DSP addr + S+0x20;
S+0x24 does the same for the MCU's message heap (runtime 0xd7e00000); S+0x30/0x34 = mdi0 in DSP addresses
= 0x140000..0x1c0000 (runtime)]. S = hal_mdi state 0xd800e0c4.

### Interrupt lines (intc numbers) and the doorbell/ack registers

The six MDI lines intc 0x12..0x17 map to bits 0..5 of the block's registers: intc 0x12+n <-> bit (1<<n)
[verified: f0122e86 clears 1,2,4,8,0x10,0x20; each handler clears its own bit].

| intc | bit | direction | meaning | MCU handler |
|------|-----|-----------|---------|-------------|
| 0x13 | 0x02 | DSP->MCU | D2M mailbox has entries | f0121f94: clear bit 2, drain D2M ring [verified] |
| 0x15 | 0x08 | DSP->MCU | DSP consumed M2D entries (space free) [inferred] | f0122172 (Thumb label): callback 5(0) -> task 0x23 resends the pending TX message; clear bit 8 [verified] |
| 0x17 | 0x20 | DSP->MCU | boot handshake / control | first f012221a, then f0121f54 (callback 7) [verified] |
| 0x12, 0x14, 0x16 | 1, 4, 0x10 | - | masked, no handler installed by hal_mdi [verified f0122d1c/f0122196] |

- **0x20040288 (+8) = DSP->MCU interrupt clear**: the MCU writes the bit of the line it handled
  (f010da94). The model: pending &= ~value; line n deasserts when its bit is clear [verified writes; semantics inferred].
- **0x20040284 (+4) = MCU->DSP doorbell** (f010da88): value 1 = new M2D entry (f01231de), 4 = a D2M entry was
  consumed (f0123226), 0x10 = boot handshake step (f012221a, f0122a34) [verified writes; meaning inferred from context].

## 2. Boot handshake

Task 0x23 (mdi_mm.c main loop FUN_f0121b28) on its start message 0xb1 calls FUN_f012223e [verified]:
1. Check the DSP image (secure call; "Illegal DSP code record" otherwise -> reset 0x18).
2. FUN_f0122d1c: M2D mailbox = pdram0+0x3800, D2M mailbox = pdram0+0x3c00 (pointers at 0xd800e3ec/0xd800e3f0);
   zero them; mask intc 0x12..0x17; write 0x20040304 = 1; copy the first 0x100 bytes of DSP image+0x400,
   16-bit halves of each word swapped, to pdram0+0x3f00 (boot header; +0x3f08 gets flags: |2 always, |1 if the
   start-up reason is 9/10/11/0x18, |8 if FUN_f01a3a9c(-2)==9; runtime 0x0002000e) [verified].
3. FUN_f0122e86: unmask 0x17, clear all six bits, **write 0x20032d18 = 1 (start DSP)** [verified].
4. Register callback 5 = f0121afb, callback 7 = f0121acb [verified; runtime 0xd800dbb0].

Then the MCU waits for intc 0x17, three times [verified chain]:
- **0x17 #1** -> f012221a: install f0121f54 as the 0x17 handler, doorbell 0x10, clear 0x20.
- **0x17 #2** -> f0121f54 -> callback 7 (f0121acc): flag 0xd8001738 = 1, unmask 0x17, mask 0x13, message to
  task 0x23 -> FUN_f0122a34 = doorbell 0x10.
- **0x17 #3** -> callback 7 again (flag set) -> task 0x23 -> FUN_f0122196: re-zero the mailboxes, install the
  0x15/0x13/0x17 handlers, **send the first M2D entry (config)**, clear 0x20, unmask 0x13 and 0x17 (0x15 is
  unmasked right after its install).
  Config entry [verified f0122196]: w0 = mdi0 size (0x80000), w1 = 0x01020304 (byte-order marker),
  w2 = 0x18010000, w3 = mdi0 DSP address (0x140000).

No timeout in hal_mdi itself [verified: no timer in these functions]. Failure today: nothing raises 0x17, the
mailbox is never initialised (runtime: 0xd8001738 = 0, both mailbox words 0), no message from device 0x7c ever
arrives, and MTC resets with **reason 0x18** after its DevSyncTimer (1000 ms, f0176736) [verified at runtime:
final r4 = 0x18, r7 = 0xf04b1959 = MTC device list, pc f010c352].
A second watchdog [inferred]: the MDI RX task (FUN_f0121d2a) resets with reason 9 when it gets one of its
timer signals (ids 0xc0..0x1a7) while 0xd8001739 is 0; that flag is set by the first DSP message with media 0x12
and resource 0xDA. Runtime showed 0xd8001739 = 1 already, so something else may set it; send the 0xDA message anyway.

## 3. Mailboxes and message transport

### Ring layout (both in pdram0) [verified f01231de, f0123190, f0123226, f0123208, f01231c2, fatal dump f0122eba]

```
0x30003800  M word: bits 0..15  = M2D write index (MCU)    bits 16..31 = D2M read index (MCU)
0x30003804  M2D slots: 16 x 16 bytes, slot = index & 15
0x30003c00  D word: bits 0..15  = D2M write index (DSP)    bits 16..31 = M2D read index (DSP)
0x30003c04  D2M slots: 16 x 16 bytes
```
Indices count 0..31 and wrap to 0 (pending = (write - read) mod 32, at most 16). The MCU spins until it
reads the same D word twice (f0123190), so the DSP must update the slot before the index.

### Entry (4 little-endian words w0..w3), flags = w2 >> 24

PhoNet header bytes b0..b9 (b0 media, b1 receiver dev, b2 sender dev, b3 resource, b4/b5 length, b6 receiver
obj, b7 sender obj, b8 utid, b9 msg id). Length L = bytes after b5.
- w0 = b0 | b1<<8 | b2<<16 | b3<<24
- w1 = (L >> 8) | (L & 0xff) << 8 | b6<<16 | b7<<24   (i.e. the length in big-endian byte order)
- w2 = flags<<24 | sub<<16 | (b8 | b9<<8 for M2D)
- w3 = DSP address of the whole message (with its big-endian length) in SDRAM

| flags | sub | direction | meaning |
|-------|-----|-----------|---------|
| 0x06 | - | both | **long PhoNet message at w3**, total L+6 bytes [verified TX f0122888 (w2 = 0x06000000+b8b9, w3 = buffer - S+0x24), RX f0121f94 path bit2+bit1] |
| 0x04 | - | both | short message inline (only if L <= *0xd8001748; RX copies 6 bytes to a short-msg buffer and dispatches on L) [verified dispatch; contents inferred]. A fake DSP need not send these |
| 0x09 | 0x02 | M2D | MCU returns up to 3 used mdi0 RX buffers (w0, w1, w3 = their DSP addresses), queued by f0122500, sent by f0122720 when > 2 are pending [verified] |
| 0x08 + bits 0..2 | 0x02 | D2M | DSP returns MCU TX buffers: bit0 -> w0, bit1 -> w1, bit2 -> w3 are DSP addresses of buffers to free (MCU adds S+0x24 and frees them, f0121c4a mode 2) [verified]. Send 0x0f/0x02 with all three or fewer bits |
| 0x14 | - | D2M | "mode selected" (switches to a 4-byte tagged buffer format, short threshold 8). **Don't send**: after it a 0x06 entry is a fatal error ("hal_mdi_handle_mode_selected") [verified f0121f94] |
| 0x18 | 0x01 | M2D | config at start (section 2) [verified] |

RX path [verified]: 0x13 handler -> callback 6 (f0121c4a, registered by FUN_f0121d2a via f01224f2) mode 0:
checks w3..w3+L+6 lies in mdi0 (DSP 0x140000..0x1c0000, else assert), copies to an OS message, queues the
buffer for return (f0122500; > 64 unreturned -> reset 0xd), sends it to task 0x22 (FUN_f0121d2a), which turns
the length into host order and hands it to the router FUN_f011bbe6.

### How device 0x7c gets registered [verified f011bbe6 -> f011c75e -> f011ccf4]

The router (table 0xd800d280: [0] own device 0x00, [1] ready flag = 1 at runtime, [2 + dev>>2] = media)
learns every unknown sender device < 0x80 from any incoming message whose resource is not 0xD0:
table[2 + b2>>2] = b0. So **the first D2M message with b2 = 0x7c registers it**; b0 must be the MDI media id,
**0x12** [inferred: FUN_f0121d2a's alive check wants b0 == 0x12; the MDI TX path treats media-control 0xd7
messages for media 0x12 itself (f0121b28)].

## 4. What the DSP must answer at boot

- One D2M message from device 0x7c, media 0x12, resource **0xDA** (any message id, e.g. an indication):
  registers 0x7c and sets the "DSP alive" flag (FUN_f010fbb2(0xd1)) [verified f0121d2a/f011c75e; id inferred].
  Example: `12 00 7c da 00 06 00 00 00 01 00 00` (receiver 0x00 = MCU) [inferred].
- MTC after the sync (f0176736): sends the template at 0xf04e45a2 = `00 7c 00 e6 00 06 00 30 00 48 00 18`
  (note: stored with the length little-endian, `06 00`; resource 0xE6, receiver obj 0x00, sender obj 0x30,
  msg id 0x48, payload 00 18), then calls the RF component search FUN_f0176626(1, 0x7c00, 0xE6) and sends an
  indication (0x15F0 ...) with the result; **no reply is awaited there** [verified f0176736/f017657e]. The template
  continues with a second message (`00 7c 00 f1 ...`, resource 0xF1). Replies: unknown [inferred: the
  usual ISI `msg id + 1` response with a status byte 0 would be the cheapest try].

## 5. Fake DSP (QEMU device) design

```
state: pending (6 bits), dsp_started, hs_step, m2d_rd, d2m_wr, rx_alloc (in mdi0)
PDRAM = 0x30000000 (RAM), MDI = 0x20040280, RST = 0x20032d00, SDRAM = 0x08000000

raise(bit): pending |= bit; set intc line 0x12+log2(bit)
write MDI+8 (clear): pending &= ~v; lower lines whose bit is clear
write RST+0x18 = 1: dsp_started = 1; hs_step = 0; m2d_rd = d2m_wr = 0; after ~100 us raise(0x20)
write MDI+4 (doorbell) v:
  v & 0x10: if hs_step < 2: hs_step++; raise(0x20)          # 0x17 #2 and #3
  v & 0x01: drain_m2d()
  v & 0x04: nothing (MCU consumed a D2M slot)

drain_m2d():
  while m2d_rd != (rd32(PDRAM+0x3800) & 0xffff):
     e = PDRAM+0x3804 + (m2d_rd & 15)*16 ; w0..w3
     flags = w2>>24
     if flags == 0x18: config seen -> d2m_send(alive msg: 12 00 7c da ...)
     if flags == 0x06: msg = SDRAM + w3, len = swap16(w1 & 0xffff) + 6
                       handle_isi(msg)             # e.g. answer res 0xE6/0xF1, or forward to a host table
                       d2m_entry(w0=w3, w1=0, w2=0x09020000, w3=0)   # give the buffer back (flags 8|bit0, sub 2)
     if flags == 0x09: (RX buffers returned) free them in rx_alloc
     m2d_rd = (m2d_rd + 1) % 32
     wr32(PDRAM+0x3c00, (m2d_rd << 16) | d2m_wr)
  raise(0x08)                                       # intc 0x15: M2D space

d2m_send(bytes):                                    # bytes with big-endian length at [4..5]
  buf = rx_alloc(len)   (DSP addr in 0x140000..0x1c0000, phys = 0x08000000 + addr)
  copy bytes to phys
  d2m_entry(w0=b0|b1<<8|b2<<16|b3<<24, w1=b4|b5<<8|b6<<16|b7<<24, w2=0x06000000, w3=buf)

d2m_entry(w0..w3):
  if (d2m_wr - (rd32(PDRAM+0x3800) >> 16)) mod 32 >= 16: queue for later (retry on doorbell 4)
  write slot PDRAM+0x3c04 + (d2m_wr & 15)*16 ; d2m_wr = (d2m_wr+1) % 32
  wr32(PDRAM+0x3c00, (m2d_rd << 16) | d2m_wr) ; raise(0x02)   # intc 0x13
```

Notes: w1 as written above (`b4 | b5<<8`) is the same as `(L>>8) | (L&0xff)<<8` because b4/b5 are the big-endian
length. Keep the mailbox and the slots in guest RAM (pdram0 is plain RAM to the MCU). The DSP image,
the boot header at 0x30003f00 and 0x20040304 can be ignored.
