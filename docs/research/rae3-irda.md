# Nokia 9210 IrDA: the SIR UART behind Euart2.pdd

Firmware RAE-3 V 4.13. What the IrDA model (`src/qemu-ngage/hw/arm/nokia9210_ir.h`, board glue in `nokia9210.c`,
hack H-rae3-ir) is built on. **[V]** read in the firmware or seen in a run, **[I]** inferred. Addresses: ROM virtual
(0x5xxxxxxx), physical (0x05xxxxxx; the kernel's window is VA 0x58000000 = PA + 0x53000000).

## Result (2026-11-27)

`-M nokia9210` with `irda=` (or QEMU's 2nd serial port) beams both ways with `eka1/tools/ngage_irsend.py` [V run]:

- receive: Desk, Menu (F9), Tools, **Receive via infrared**; `ngage_irsend.py FILE` finds "EPOC", the link goes
  to 115200 baud, OBEX PUT; the Desk shows "You have: Remote device: irtest.txt".
- send: Calendar (F12), an entry, Menu, File, Send, **Via infrared**; `ngage_irsend.py --out DIR` gets `cal.vcs`
  (vCalendar 1.0 with the entry).

9210 to an emulated 6600 does not complete yet (see "Phone to phone" below).

## The stack

- `irda.esk` [V]: `irmux` / `tinytp` from `irda.prt`; `irPhysicalComPort= 1`, `irBaud= 115200`,
  `irSupportedBaudRates= 319`, `irPod= jeteye7401`.
- COMM::1 is **Euart2.pdd** ("Comm.Linda2", 3092 bytes at 0x50097080) [V]. Euart1.pdd ("Comm.Linda", ArtRx/ArtTx)
  and Euart1dce.pdd are the other UART (the serial port).
- SIR: IrDA.prt does the framing (BOF/EOF, escapes, FCS) and IrLAP in software [V: the frames on the link]. The
  UART only moves bytes, so the link carries the raw SIR stream, as on the 6600 (docs/research/6600-irda.md).

## Registers

The PDD calls the hardware only through EKern exports (import stubs 0x50097844..0x50097A60 -> EKern):

| EKern | Does [V] | PDD use [V] |
|---|---|---|
| 0x500029D4 | W 0x0500C004 | Configure: the divisor from the table at 0x50097AC8 (by TBps): 13 MHz / baud (9600 0x54A, 19200 0x2A5, 38400 0x153, 57600 0xE2, 115200 0x71) |
| 0x500029B4 | W 0x0500C008 = a \| b \| c \| 1 | Configure: data bits (0x200..0x380), stop bits (0 / 8), parity (0, 0x10..0x40) |
| 0x50002A4C | R 0x0500C00C, clears it if non-zero | RX ISR, when 0x05003000 bit 21 is set: bits 1/3/2 -> EComm's 0x40000000 / 0x10000000 / 0x20000000 flags. The PDD also writes 0 to it directly (0x500973B4 / 0x500977EC / 0x50097498) |
| 0x500029E8 | W 0x0500C010 | the TX byte |
| 0x50002A10 | R 0x0500C014 | polled to 0 after each TX byte |
| 0x50002A38 | R 0x0500C018 | polled to 0 before turning the line round and before reconfiguring |
| 0x500029FC | R 0x0500C01C | the RX byte (bits 7-0 kept) |
| 0x50002A24 | R 0x0500C020 | the RX ISR loops while it is non-zero |
| 0x50002870(n) | R 0x05003000 bit n (n < 32) | RX ISR: n = 21 (error with this byte); TX ISR: n = 22 (stop sending while it is set) |
| 0x50003448 / 0x50003434 | W 0x05008010 / 0x0500800C | 0x80 at Configure (0x50097548) / at Stop (0x500973CC): the IR UART's on / off bit [I]. Euart1/Euart1dce use the same pair |
| 0x500028FC(v) / 0x5000283C(a, b) / 0x50002944(code) | W 0x05002028 (clear) then 0x05002024 (set) | bit 16 clear while transmitting, set while receiving; 0xBB/0xCC/0xAA/0xDD set/clear bits 18/19, and bits 14-15, 17 cleared at Configure: the transceiver's mode pins [I]. Left to the catch-all |

The loop condition shared by both ISRs (EKern 0x5001D4C4) reads the kernel word 0x80000374, not hardware.

## Interrupts

The PDD binds `"IrqIrRx"` (0x50097AB8, the RX ISR 0x500976AC) and `"IrqIrTx"` (0x50097AC0, the TX ISR 0x50097784) by
name [V]. The kernel's FIQ name table (FiqTimer0 at 0x5002750C) has FiqIrRx / FiqIrTx at **13 / 14** (KeyPress = 39
and LidOpen = 10 agree with the board) [V]. The Irq... set is the same order on the IRQ controller (0x05005000).

- RX: a level while the FIFO holds data [I].
- TX: StartTx (0x50097464) disables RX, clears 0x05002000 bit 16, writes what EComm has until bit 22 or the end,
  waits for +0x18, enables TX. The TX ISR sends more; with nothing left it disables TX, waits for +0x18, sets bit 16
  and enables RX again [V]. So TX is "the transmitter takes bytes", a level [I]. The model keeps it high always: the PDD
  only enables it while it has something to send.

## Model choices

- TX is instant (+0x14 / +0x18 / bit 22 never busy). The link (`ngage_irlink.c`) collects the bytes and sends one
  datagram per burst. One-byte datagrams overflowed a peer's socket queue: a 2 KB frame is ~2000 datagrams, and
  only ~280 one-byte datagrams fit in Linux's default receive buffer.
- RX is paced at the phone's baud: 16 bytes, then their char time. Without pacing, one ISR would pass a whole frame to
  EComm.
- RX FIFO 64 bytes [I]. Bytes that arrive while the port is off are dropped.
- Port on/off = 0x05008010 / 0x0500800C bit 7 (a small register pair in the board; reads return what was written,
  as the catch-all did).
- The baud is 13 MHz / divisor, rounded to a standard rate. It is logged ("nokia9210-ir: 115200 baud") and set on a
  host tty link.

## Phone to phone (open)

9210 (`irda=udp:127.0.0.1:7480@127.0.0.1:7481`) and 6600 (`--irda a`) through a logging relay
(`build/evidence/rae3-ir-9210-6600-relay.log`) [V]:

- 9210 sends (Calendar, Send, Via infrared) while the 6600 has Infrared on: the 9210's 1-slot discovery reaches
  the 6600. The 6600 sends no XID reply. It sends an SNRM to the 9210's address 66 ms after the final XID frame.
  The 9210 answers UA, so the link is up at 115200 with the 6600 as primary. The 6600 asks IAS for "mRouter"
  IrDA:TinyTP:LsapSel and "IrDA:IrCOMM" Parameters (its PC Suite probe), gets "no such class", and disconnects
  (DISC/UA). The 9210's OBEX push never starts ("Connecting…"), then "No other infrared device found".
- When the 6600 discovers, the 9210 answers its XID within 2 ms, so the 9210 side is responsive.
- Likely cause [I]: the emulated 6600 is too slow for a 1-slot discovery (ngage_irsend.py uses 250 ms slots for it).
  It connects instead of replying, and the 9210 does not push over a link that the other side opened.
- Not tried yet: the 6600 sending to the 9210 (the 6600 is primary then, as it wants to be).
