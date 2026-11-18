# Sony Ericsson P910i (PNX4000, EKA1): SoC map for a QEMU board

Static reading of `eka1/p910i/core/ROM.bin` (VA = 0x50000000 + file offset). No emulator runs.
[V] = read from code, [I] = inferred / best guess. Addresses in code listings are ROM VAs.

## 1. Boot path

- Reset: `0x50000000: b 0x50000220`. All code up to the jump to stage 2 is position independent;
  absolute literals are physical flash addresses `0x9000xxxx` (e.g. SRAM copy source
  `0x900005c0`, stage-2 entry `0x90001398`). So with the MMU off the bootstrap runs from NOR flash
  at **physical 0x90000000** [V]. Whether the SoC aliases flash at 0 is unknown; simplest board:
  flash at 0x90000000 and initial PC = 0x90000000 (or also alias it at 0) [I].
- Entry registers [V, 0x220]: `r11 = ~r11; if (r11 != r12) r12 = 0`. r12 is a boot-flags word,
  valid only if r12 == ~r11 on entry. Bit 28 set = skip the cold-boot HW init (0x470);
  bit 31/30 = "already relocated" paths. **Cold boot: r11 = r12 = 0** (gives r12 = 0).
- CPU set-up (0x349c) [V]: reads MIDR part; if 0x926 SCTLR = 0x0005E27A else 0x0000627A
  (MMU off, V = high vectors 0xFFFF0000, RR); switches to SVC, IRQ/FIQ masked. Use **arm926** [I].
- Table selection [V]: `0x460` returns the table list at 0x50000314, `0x468` returns id 1 hard-coded,
  `0x2cc` walks `[count][id][count x 16-byte entries]` until id matches (count 0 = hang). Only one
  table (id 1) exists. Entry = `[phys, size, attr, virt]`.
- attr low 5 bits = type [V from 0x2a34/0x2a9c]: 1 = ROM, 2 = RAM, 3 = I/O, 9 = special (first entry,
  D0000000/0x41000, probably bootstrap-reserved RAM: page tables/superpage) [I]. Bits 5-6 (0x60) are
  filled by a RAM bus-width probe (0x2b38) unless bit 31 is set; bits 29/31 are set as "done" flags.
  Top byte (0x90/0x92/0x94/0x95/0x52) = cache/buffer/AP encoding [I].
- RAM [V]: first type-2 entry = **phys 0xD0000000, 32 MB**. Size probe 0x184c is an address-alias
  test (writes ~x at power-of-two offsets from 16 KB up to the declared size and checks offset 0),
  result stored in the entry. Give exactly 32 MB plain RAM at 0xD0000000 [I: P910i RAM size].
  Controller init for it is the 0x78000000 block (below).
- 0x2a34: if PC lies inside a RAM-type entry, the bootstrap thinks it was loaded into RAM and skips
  HW init; from flash it does not [V].
- Stage 2: sp = RAM phys + 0xff0, copies the table into RAM, then `0x2f8` sets sp/lr and jumps to
  phys **0x90001398** (C code: page tables, superpage) [V]. MMU-on at 0x3418/0x34e8: TTBR = r0,
  DACR = 0x55555557, temporary 1 MB identity section for the current PC, SCTLR |= M (0x627B /
  0x5E27B), then `pc = lr + offset` into virtual space, identity section removed, TLB flushed, and
  the continuation called [V]. Kernel entry at 0x12c8: `ldm r0, {r0, r1}` then `sp = r1; pc = r0`
  (0x30c) [V]; the entry point is the EKern entry from the ROM header [I].
- Virtual layout [V for explicit virts, I for the allocation rule]: entries with virt = 0 that are
  I/O get **consecutive 4 KB pages from 0x58000000 in table order** (verified by cross-reference:
  bootstrap phys 0x50000010 = kernel 0x58000010; bootstrap's polled phys 0x58000700 bit 4 = kernel's
  0x58001700; UART bank code `0x58001000 + n*0x20`). ROM 0x90000000/32 MB (virt 0) -> 0x50000000
  (ROM header rom_base) [I]. Kernel data 0x80000000..0x80500000 (header) [V], superpage at
  virt 0x40000000 (kernel reads `[0x400000EC]`) [V].

| phys | size | attr | virt |
|---|---|---|---|
| 0xD0000000 | 0x41000 | 0x00002009 (type 9) | 0 (?) |
| 0xD0000000 | 32 MB | 0x52000022 (RAM) | 0xFE000000 |
| 0x50000000 | 4K | I/O | 0x58000000 |
| 0x58000000 | 4K | I/O | 0x58001000 |
| 0x70000000 | 4K | I/O | 0x58002000 |
| 0x78000000 | 4K | I/O | 0x58003000 |
| 0x80000000 | 4K | I/O | 0x58004000 |
| 0x10000000 | 4K | I/O | 0x58005000 |
| 0x18000000 | 4K | I/O | 0x58006000 |
| 0x18200000 | 4K | I/O | 0x58007000 |
| 0x20000000 | 4K | I/O | 0x58008000 |
| 0x22000000 | 4K | I/O | 0x58009000 |
| 0x28000000 | 4K | I/O | 0x5800A000 |
| 0x30000000 | 4K | I/O | 0x5800B000 |
| 0x10000000 | 4K | 0x94000023 | 0x5BFF0000 |
| 0xB0000000 | 32 MB | 0x95000023 | 0x5E000000 |
| 0xB0000000 | 8K | 0x94000023 | 0x5C000000 |
| 0x90000000 | 128K | 0x94000023 | 0x5CF00000 |
| 0x90000000 | 32 MB | 0x92000021 (ROM) | 0 -> 0x50000000 [I] |
| 0x68000000 | 64K | 0x94000023 | 0x5CF40000 |

## 2. Registers touched by the bootstrap before the kernel (cold path 0x470..0x8a8) [V]

| # | phys | access | value / wait | meaning [I] |
|---|---|---|---|---|
| 1 | 0x50000008 | strb | 0x8B | CGU PLL config |
| 2 | 0x50000010 | ldr/str | save r3; write r3 \| 0x14000 | CGU clock enables |
| 3 | 0x30000000 | strb | 0x80 | ? (display/companion ctrl) |
| 4 | 0x30000004 | str | 0x00010230 | |
| 5 | 0x30000008 | str | 0x00002224 | |
| 6 | 0x58000700 | ldrh/strh | \|= 0x100 | serial bridge enable |
| 7 | 0x58000700 | ldrh poll | **wait bit 4 (0x10) set** (before and after each access below) | bridge ready -> return 1 |
| 8 | 0x18000000 | strb | 0x28 (repeated up to 47x) | command to external chip |
| 9 | 0x18200000 | ldrb | value & 0x1F; 0/1/2 -> r12 low bits 1/2/3; loop until two equal reads in a row | boot-mode / HW id; return 0 |
| 10 | 0x18200000 | strb, then ldrb | 2 | |
| 11 | 0x50000010 | str | restore saved r3 | |
| 12 | 0x68000020.. | copy | code 0x900005c0..0x90000608 copied to SRAM and run | needs **64 KB SRAM at 0x68000000** |
| 13 | 0x90000000 | strh/ldrh | 0x98 (CFI query), read +2, 0x50, 0xFF, read | flash ID; stored to SRAM 0x68000000 (id), 0x68000004 (~id) |
| 14 | 0x80000000.. | str | +0=5, +0x10=1, +4=0x103, +8=0x922, +0x14=0x922, +0x18=0x901, +0xC = 0x82 if id==0x8813 else 0x86, +0x1C=0 | static memory controller (flash CS) |
| 15 | 0x78000000.. | str | +0=0x2411, +4=0, +8=600, +0xC=0x700, +0x10=0x01111242, +0x14=0x906, +0x18=0x22, +0x1C=0, +0x20=1, +0x24=3, +0x30: 1, then 3 x8, then 8 | SDRAM controller init (NOP/precharge/8 refresh/mode) |
| 16 | 0x80000000.. | str | +4=0x10B, +8=0x945, +0x14=0x128, +0x18=0x104, +0xC=0xA2/0xA6, +0x1C=0 | |
| 17 | 0x50000000 | ldr poll | **wait bit 2 (0x4) set** | PLL lock -> return 4 |
| 18 | 0x5000000C | str | 8 | |
| 19 | 0x50000010 | str | \|= 0x841 | |
| 20 | 0x80000000 (from SRAM) | str | +0xC=0xB2/0xB6, +4=0x105/0x106, +0=9 | burst mode |
| 21 | 0x90003986 / 0x90004986 | strh | 0x60, 0x03, then 0xFF, read | Intel flash "set Read Configuration Register" (address = RCR value) |
| 22 | CP15 SCTLR | mcr | \|= 0x40001000 | I-cache (+ ARM920 nF bit) |
| 23 | 0x50000028 | str | 0x2643 | |
| 24 | 0x580000CC | ldr/str | \|= 0x180 | |

Board needs: reads of 0x58000700 with bit 4 set, 0x50000000 with bit 2 set, 0x18200000 = 0
(or 1/2); everything else is write-and-forget. The flash must accept 0x98/0x50/0xFF/0x60-0x03 commands
(QEMU pflash_cfi01 rejects 0x60 followed by 0x03; use a ROM region with a tiny command shim, or
ignore writes) [I]. Flash id 0x8813 picks variant A, anything else variant B; both fine [I].

## 3. Interrupt controllers [V]

Two 32-bit controllers in the phys 0x50000000 page; numbers 0..31 on IC0, 32..63 on IC1.

| block | phys | virt |
|---|---|---|
| IC0 (IRQ 0..31) | 0x50000200 | 0x58000200 |
| IC1 (IRQ 32..63) | 0x50000300 | 0x58000300 |

Register layout (same for both):
- +0x00 enable mask (1 = enabled; Enable = orr, Disable = bic) [V 0x150e0..0x15230]
- +0x04 clear, write-1-to-clear latched (edge) pending [V: timer ISR writes 2 / 4, init writes 0xE]
- +0x08 pending status (read) [V]
- +0x0C, +0x10 read-modify-write config fields (polarity / edge-vs-level) [V access, I meaning]
- +0x14 FIQ select (1 = FIQ, 0 = IRQ) [V]
- Cascade: IC1 IRQ-routed output = IC0 bit **19 (IrqPicIrq)**, IC1 FIQ-routed output = IC0 bit
  **20 (IrqPicFiq)**; enabling any IRQ >= 32 also sets IC0 enable bit 19 or 20 [V 0x1514c].
- IRQ dispatcher [V]: handler 0x50014524 loops `n = 0x500153ac(); if n != -1 -> 0x5001eb94(n)`.
  0x500153ac: `p = IC0[0x208] & ~IC0[0x214]`, lowest set bit; if it is 19, `p = IC1[0x308] &
  ~IC1[0x314]`, 32 + lowest bit. It does **not** AND with the enable mask, so the model should
  return `pending & enable` at +0x08 (other code computes `+0x08 & +0x00 & ~+0x14`, which stays
  correct) [I]. FIQ path 0x50014548: for n in 0..63 (skip 19, 20) if `pending & enable & fiqsel`.
- CPU IRQ line = any `(pending & enable & ~fiqsel)` on IC0 (with bit 19 derived from IC1);
  FIQ line likewise with fiqsel [I].

IRQ number -> name (table of TLitC pointers at 0x500147ec, copied to 0x8000031c, 64 entries) [V]:

| n | name | n | name | n | name | n | name |
|---|---|---|---|---|---|---|---|
| 0 | IrqDma | 16 | IrqPcm | 32 | IrqUartRx1Pin | 48 | IrqMmcSense |
| 1 | IrqHsTimer0 | 17 | IrqI2C | 33 | IrqUartRx2Pin | 49 | IrqPcmData0 |
| 2 | IrqHsTimer1 | 18 | IrqRtc | 34 | IrqUartRx3Pin | 50 | IrqPcmData1 |
| 3 | IrqHsTimer2 | 19 | IrqPicIrq (IC1 cascade) | 35 | IrqUartRx4Pin | 51 | IrqPcmSync |
| 4 | IrqWatchDog | 20 | IrqPicFiq (IC1 cascade) | 36 | IrqUartRx5Pin | 52 | IrqPcmClk |
| 5 | IrqUart1 | 21 | IrqDac | 37 | IrqUartIrRx6Pin | 53 | IrqCamera |
| 6 | IrqUart2 | 22 | IrqPen | 38 | IrqUartTx2Pin | 54 | IrqIOPort2 |
| 7 | IrqUart3 | 23 | IrqTouchscreen | 39 | IrqUartTx4Pin | 55 | IrqIOPort3 |
| 8 | IrqUart4 | 24 | IrqTouchscreenAux | 40 | IrqUartTx5Pin | 56..63 | literals at 0x50039c84..0x50039cf4 (not decoded) |
| 9 | IrqUart5 | 25 | IrqKeyDown | 41 | IrqIOPort0 | | |
| 10 | IrqUart6 | 26 | IrqKeypad | 42 | IrqIOPort1 | | |
| 11 | IrqDispFifo | 27 | IrqKeyAux | 43 | IrqFlip | | |
| 12 | IrqCamSync | 28 | IrqSWInt | 44 | IrqService | | |
| 13 | IrqCamErr | 29 | IrqJtagTx | 45 | IrqSTS | | |
| 14 | IrqMmc0 | 30 | IrqJtagRx | 46 | IrqDispWait | | |
| 15 | IrqMmc1 | 31 | IrqMemoryStick | 47 | IrqDispSync | | |

## 4. Tick timer (HsTimer) [V unless marked]

phys **0x50000400**, virt 0x58000400. One free-running counter with three match channels =
IrqHsTimer0/1/2 (IRQ 1/2/3).
- +0x00 byte: match status, write-1-to-clear: bit0 = match0, bit1 = match1, bit2 = match2 (init writes 7).
- +0x04 byte: control; init writes 2, 8, 9 with read-backs (reset / enable) [I meaning]; run the
  counter once bit 0 is set, or always [I].
- +0x08: counter (read, 32-bit up-counter).
- +0x0C halfword: written 0 at init (prescaler?) [I].
- +0x14 halfword: interrupt enables, bit0 = match0, bit3 = match1, bit6 = match2.
- +0x18 / +0x1C / +0x20: match0 / match1 / match2 (interrupt when counter == match).
- Init 0x50014d20: +0x0C = 0, +0x00 = 7, IC0 clear (+0x04) = 0x0E, +0x04 = 2, 8, 9.
- Tick ISR 0x50013fdc: `match0 += 0x200` (re-based to counter + 0x200 if already passed),
  +0x00 = 1 (clear match0), IC0 +0x04 = 2 (clear IRQ 1). Match1 (idle/one-shot, 0x141c8):
  match1 = counter + n (n >= 6), +0x00 = 2, IC0 +0x04 = 4.
- Clock: ms -> counts is `ms << 15 / 1000` (0x50014264), so **32768 Hz**; tick 512 counts =
  **64 Hz (15.625 ms)** [V arithmetic, I clock source].
- Model: match status bit latches when counter reaches match; IC line raised while
  (status & enable bit); IC latch cleared by IC +0x04 [I].

## 5. Debug UART [V]

Bank of 16550-style UARTs (byte registers, 4-byte stride, 0x20 per unit) at phys **0x58000000 +
n*0x20**, virt 0x58001000 + n*0x20 (helpers 0x50015cb8 write +0, 0x50015cd0 read +0,
0x50015ce8 read +8, 0x50015d00 read +0x14). Kernel debug putc 0x50016cc8: unit = 5 if
superpage `[0x400000EC] == 6` else **4**; waits for `LSR (+0x14) bit 5` (THRE), writes THR (+0x00).
So the default debug port is **phys 0x58000080** (QEMU `serial_mm_init(..., regshift 2)`).
Six units (IrqUart1..6 = IRQ 5..10, unit n -> IRQ 5+n) [I]. edldrv.pdd uses units 0..4 [V].
Euart1.pdd touches 0x58000010 (clock enables) and 0x58000100 (pin mux / GPIO) [V access].

## 6. Best-guess physical map

| device | phys | virt | evidence |
|---|---|---|---|
| CGU / PLL / clock enables | 0x50000000 (0x00..0xFF) | 0x58000000 | boot #1,2,17-19,23; [I] name |
| GPIO / pin mux | 0x50000100 | 0x58000100 | used by Euart, ekeypad, Exyin, ampctrl [I] |
| IC0 / IC1 | 0x50000200 / 0x50000300 | 0x58000200 / 0x300 | sec. 3 [V] |
| HsTimer | 0x50000400 | 0x58000400 | sec. 4 [V] |
| 0x500 block (RTC/watchdog?) | 0x50000500 | 0x58000500 | pwrnotif, EKern 0x14fb8 [I] |
| 0x700 block | 0x50000700 | 0x58000700 | EKern 0x14c2c..0x14d10 [I] |
| touchscreen ADC | 0x50000800 | 0x58000800 | Exyin, TouchScreenCtrl [I] |
| keypad | 0x50000900 | 0x58000900 | ekeypad.dll [I] |
| UART bank 0..5 | 0x58000000 + n*0x20 | 0x58001000 + n*0x20 | sec. 5 [V] |
| serial bridge to external chip (status 0x5FC bit0 busy, regs 0x600..0x618; 0x700 bit4 ready) | 0x580005FC..0x58000710 | 0x580015FC.. | EKern 0x15b60.., boot #6,7 [V access, I meaning] |
| DMA (8 ch x 0x20 at +0x100) | 0x70000000 | 0x58002000 | EKern table 0x50039d04 [I] |
| SDRAM controller | 0x78000000 | 0x58003000 | boot #15, EPower [I] |
| static memory (flash) controller | 0x80000000 | 0x58004000 | boot #14,16,20 [I] |
| misc peripherals, Memory Stick at +0x300 | 0x10000000 | 0x58005000, 0x5BFF0000 | EKern 0x389e0.., MEDMS.PDD, prodriver [I] |
| external chip (cmd / data) | 0x18000000 / 0x18200000 | 0x58006000 / 0x58007000 | boot #8-10, videodriver_ext [I] |
| ? | 0x20000000, 0x22000000 | 0x58008000, 0x58009000 | videodriver_ext [I] |
| audio / PCM | 0x28000000 | 0x5800A000 | Esdrv.pdd (12 refs) [I] |
| display (?) | 0x30000000 | 0x5800B000 | boot #3-5, videodriver_ext [I] |
| internal SRAM 64 KB | 0x68000000 | 0x5CF40000 | boot #12 [V] |
| NOR flash 32 MB (Intel CFI, RCR) | 0x90000000 | 0x50000000, 0x5CF00000 (128K) | header, boot #13,21 [V] |
| DiskOnChip (8K window) / 32 MB alias | 0xB0000000 | 0x5C000000 / 0x5E000000 | DocSupportDrv uses 0x5C001000 [I] |
| SDRAM 32 MB | 0xD0000000 | 0xFE000000 (+ kernel 0x80000000..) | table type 2 [V], size [I] |

Open: meaning of the type-9 entry; IRQ names 56..63; the 0x18000000 external chip protocol beyond
the boot handshake; whether a boot ROM at 0 exists.
