# Nokia 9500 IrDA: OMAP UART3 in hardware SIR mode behind Euart3.pdd (analysis, not implemented)

Firmware RA-2 V 06.27 (`eka1/ra2/core/ROM.bin`). Static reading only (Ghidra headless on Euart3.pdd, the EKern helpers it
imports). **[V]** read in the firmware, **[I]** inferred. Addresses: ROM virtual; the kernel maps the OMAP peripherals at
VA 0x58000000 + (PA - 0xFFF00000) (UART3: PA 0xFFFB9800 = VA 0x580B9800).

## Summary

- COMM::3 (`IrDa.esk`: `irPhysicalComPort= 3`, `irBaud= 115200`, `irSupportedBauds= 319`) is **Euart3.pdd**
  ("Comm.HelenSir", 7208 bytes at 0x5011B3F0) [V]. Euart1/Euart2 ("Comm.Helen_1/2") are the other UARTs; Bc02_uart1.pdd
  is Bluetooth; XbusDma.pdd is the modem link on UART2 (already modelled: `Ra2Uart` in nokia9500.c).
- Unlike the 9210 (Euart2.pdd) and the 6600 (euart1.pdd), the 9500's UART does the **SIR framing in hardware**. Euart3
  puts OMAP UART3 in IrDA SIR mode, so the chip adds the BOFs, escapes, FCS and EOF. Whole frames move by **system DMA**
  both ways, and the PDD gets one interrupt per frame [V]. IrDA.prt hands the PDD bare IrLAP frames, the first
  byte being the extra-BOF count [V TX path].
- The link outside is still the raw SIR byte stream, so a model can reuse `ngage_irlink.c` and talk to
  `ngage_irsend.py` and the other phones. But the board needs a **SIR codec** (wrap / unwrap, CRC-16 FCS) in C, plus
  the UART3 IrDA registers, two DMA requests and frame-level interrupts.
- Today QEMU's UART3 is a bare 16550 (`serial_mm`, registers 0x00..0x1C only). The first IR use would hang the phone:
  the TX path waits `while (!(LSR & 0x80))`, and on a 16550 bit 7 (FIFO error) stays 0 [V loop at FUN_5011BAAC; I hang].

## Register access

The PDD writes UART3 through EKern helpers (0x5000EB18..0x5000EF38). Each turns a register index into an offset:
index x 4 for UART3, x 1 for a byte-packed UART at VA 0x580CE800 [V the `cmp r0, #0x580CE800` in every helper]. Two
generic ones take the offset directly: write8(base, off, v) 0x5000EF2C, read8(base, off) 0x5000EF38.

| Offset | OMAP name [I: OMAP1510 UART/IrDA register map] | Euart3 use [V] |
|---|---|---|
| 0x00 / 0x04 | RHR-THR / IER (DLL/DLH with LCR 0xBF) | RHR drained by the RX handler while LSR bit 0 is 0; IER = 0x20 (TX status) at start; RX arm sets IER \|= 0x0C, RX stop clears it (& 0xF3) |
| 0x08 | IIR (read) / FCR / EFR (LCR 0xBF) | the ISR (0x5011B5F0) reads IIR: bit 2 = last RX byte (a frame is in), bit 5 = TX status (frame sent), bit 3 = RX overrun (reset FIFO, re-arm RX DMA). FCR = 0x57 before TX, 0x0F on RX reset. EFR enhanced bit 4 set |
| 0x0C | LCR | 0xBF to reach EFR/DLL/DLH, then restored |
| 0x10 | MCR | bit 6 around the TCR/TLR writes |
| 0x14 | LSR | in IrDA mode: bit 0 = RX FIFO **empty**, bits 2-4 = CRC / abort / frame too long (any -> frame dropped), bit 7 = TX FIFO empty (polled before every frame) |
| 0x18 / 0x1C | TCR / TLR | 0x2D / 0 at configure |
| 0x20 | MDR1 | 7 (off) at configure, **0x21** at the end: mode 1 = SIR, bit 5 [I: SIP_MODE] |
| 0x24 | MDR2 | 0 |
| 0x28 / 0x2C | write TXFLL / TXFLH, read SFLSR / RESUME | TX: the frame length - 1 (13 bits) before each frame; both read after it |
| 0x30 / 0x34 | write RXFLL / RXFLH, read SFREGL / SFREGH | configure: 5 / 8 = max RX frame 0x805; RX: SFREGH:SFREGL = the received frame's length (12 bits, FCS included) |
| 0x38 | BLR | 0 |
| 0x3C | ACREG | 0xC0 at power-up, 0 at power-down, a per-baud value at configure (table 0x5011CF10 [I pulse width]); **\|= 4 (SCTX_EN) starts each TX frame** |
| 0x40 | SCR | 1 (DMA control) at idle, \|= 0xC3 for a frame (DMA mode) |
| 0x48 | EBLR | the number of extra BOFs + 1, from the first byte of each TX buffer when it changes (0x5011C25C) |
| 0x4C | [I OSC_12M_SEL] | 1 |
| 0x580B9000 | another block | polled through 0x5000EF78 (a wait loop, up to 100 tries) while a TX is still busy at power-down [I] |

Baud: the PDD's tables at 0x5011CE9C.. (by TBps) give the divisor / ACREG per rate [V tables; values not decoded].

## Frame flow [V the functions, I the hardware side]

- **TX** (StartTx 0x5011BAAC, next frame 0x5011BB08): EComm's vtable +0x30 fills the buffer. Byte 0 = XBOF count
  (it goes to EBLR when it changes), the rest is the IrLAP frame. Then TXFLL/H = length - 1, the TX DMA channel is
  programmed with the length (0x5011C42C), FCR = 0x57 (reset), the PDD waits for LSR bit 7, sets SCR \|= 0xC3 and
  ACREG \|= 4. The UART pulls the frame by DMA, wraps it in SIR and raises IIR bit 5. The ISR then sends the next frame.
  With no more frames, the PDD switches back to RX (0x5011BD40: RX FIFO reset, RX DMA re-armed for 0x80F bytes).
- **RX** (0x5011BE60): the RX DMA channel runs with 0x80F elements into a hardware chunk. At a frame's end the UART
  raises IIR bit 2. If LSR & 0x1C is clear, the PDD reads SFREGH:SFREGL (the length), the DMA's count (channel
  register 0x18) and the bytes still in the FIFO (RHR while LSR bit 0 = 0). It strips the 2 FCS bytes and passes the
  frame to EComm (vtable +0x2C), then re-arms the RX DMA and IER \|= 0x0C.
- **DMA**: the PDD programs OMAP system DMA channels through EKern (channel register = ch << 6 \| reg: +2 CCR, bit 7
  EN, cleared before reprogramming; +0x10 element count; +0x18 the address counter). It binds the request lines
  through EKern's DMA API (0x5001CDE4). In QEMU's omap.h these are OMAP_DMA_UART3_TX 18 / OMAP_DMA_UART3_RX 19 [I].
- **Interrupt**: OMAP_INT_UART3 = 15 (level 1) [I: QEMU's omap.h; the bind call goes through 0x5002A590].

## What a model needs (estimate)

The 9210's model plus about three times the code:

1. An overlay over QEMU's UART3 at 0xFFFB9800 (the `Ra2Uart` pattern in nokia9500.c: own register file, DMA request
   lines `drq_tx` / `drq_rx`, the IRQ). It needs the IrDA registers above, mode switching by MDR1, IIR/LSR with
   IrDA semantics while MDR1 = SIR, and the 16550 behaviour otherwise (nothing else uses UART3 [I]).
2. A SIR codec in C: wrap (EBLR - 1 extra 0xFF BOFs [I], 0xC0, escaped frame + FCS-16, 0xC1) and unwrap (resync on
   0xC0, unescape, check the FCS). Then a frame queue and a status FIFO (length, CRC/abort flags). `ngage_irsend.py`'s
   `sir_wrap` / `SirReader` are the reference to port; the FCS is the IrDA CRC-CCITT (0x8408, init 0xFFFF, inverted).
3. TX: on ACREG bit 2, run the TX DMA request until TXFL + 1 bytes are in, encode, `ngage_irlink_tx` the burst, raise
   IIR bit 5 after the frame's char time.
4. RX: decoded frames go into the RX FIFO, which feeds DMA requests until the frame (with its FCS) is in memory. Then
   push the length to SFREG, raise IIR bit 2, and keep LSR bit 0 = 1 (empty) so the PDD's drain loop ends.
5. `irda=` / the 2nd serial port as on the other boards (`ngage_irlink.c`). The 9500's launcher already passes
   `--irda` through `ngage_sess.py`; the board ignores it today.

Prerequisite: the 9500 must reach a UI that can turn infrared on (bring-up stall 31 / the wizard). Until then the
IR path is not reachable to test.

Open [I]: the exact ACREG / divisor tables per baud, the 0x580B9000 block, the request numbers the PDD actually binds
(the board's DMA log would show them), and whether the PDD relies on DMA end-of-block interrupts as well as the UART's.
