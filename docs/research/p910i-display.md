# P910i display: just enough to show the screen

Static reading of `videodriver_ext.dll` (0x500B4BA0), `ECust.dll` (0x500B2A10) and EKern's DMA
helpers (0x500158A0..0x50015AB0). [V] = read in the code, [I] = inferred. Virtual -> physical:
0x58000xxx -> 0x50000xxx, 0x58001xxx -> 0x58000xxx, 0x58002xxx -> 0x70000xxx, 0x58006000 ->
0x18000000, 0x58007000 -> 0x18200000, 0x5800B000 -> 0x30000000 (docs/research/p910i-soc-map.md).

## 1. Frame buffer

- **Fixed physical address 0xD0000000** (start of SDRAM). ECust's variant returns 0xD0000000
  (vtable slot at 0x500B34E4 -> 0x500B2C78) and a linear address **0x58010000** (slot 0x500B34E8 ->
  0x500B2C80) [V values, I that these are VideoRamPhys / VideoRamLinear]. The bootstrap's type-9
  mapping (phys 0xD0000000, 0x41000 bytes) is this video RAM [I].
- ECust screen size: **208 x 320** (0x500B2C64 writes 208, 320 into a TSize) [V].
- **16 bits per pixel, stride 416 bytes, no header**: one frame is 0x20800 bytes = 208*320*2 [V];
  the driver writes 0xD0000000 and 0xD0020800 into the LCD block (+0x14/+0x18, below) [V]; the
  full-frame transfer is 0x8200 words from offset 0 (0x500B61C0) [V]; line offset = line * 416
  (0x500B63D0: `n*13*32`) [V]. Pixel format **RGB565** [I] (EColor64K; check by eye, 444 if the
  colours are wrong).
- The video RAM is 0x41000 = two frames. Only the first (0xD0000000..0xD00207FF) is watched and
  sent to the panel [V for the watched window]; the second half is probably a back buffer for
  video/yuvtransfer [I]. Show the first.

## 2. How pixels reach the panel

The panel has its own controller and memory, an indexed-register chip on the external bus
(command/index port phys **0x18000000**, data port phys **0x18200000**, byte writes). Writes to it
go through an "LCD FIFO" block at phys **0x58000700**. No LCD controller scans RAM.

Register write to the chip (0x500B66A4 `WriteReg(reg, mask, val)`) [V]:
1. shadow[reg] = (shadow[reg] & ~mask) | val (shadow table in kernel data 0x80002740);
2. if the DMA channel is busy (`0x500B766C(chan) & 1`), queue it (max 16; **the 17th queued write
   calls Kern::Fault "ScreenDriver FAULT"** via 0x500B5EB0) [V code, I name of 0x500B766C];
3. else: wait `byte[0x58000704] <= 0x78`; strb reg -> 0x18000000; wait; strb shadow[reg] ->
   0x18200000; wait; strb **0x0B** -> 0x18000000 (re-select the pixel-data register) [V].

Init writes seen [V]: reg 1 val 0x40 (mask 0), 0x3B bits 1/4, 0x3C=2, 0x3D=0, 0x3E=100, 0x3F=0,
0x38=0x80, reg 0 bit 0x20 clear then bit 0x40 set (display on), 0x30/0x31 = 16-bit value lo/hi.
Reads from the chip: none in the driver (only the bootstrap reads 0x18200000, already modelled) [V].

Pixel transfer (0x500B6430) [V]: wait FIFO level <= 0x78, strb 0x0B -> 0x18000000, then a kernel
DMA channel: source = frame-buffer physical + offset, count in words, start. Destination = the data
port 0x18200000 [I]. Partial updates: the block at 0x58000700 keeps a **dirty bitmap of 512-byte
blocks** of the watched window at +0x20..+0x5F (260 blocks; write-1-to-clear) [V access, I meaning];
the driver scans it (0x500B627C) and DMAs the dirty runs. If no bit is set it gives up after 512
probes and calls back "nothing to do" (no hang) [V].

### LCD FIFO block, phys 0x58000700 (virt 0x58001700)

| off | access | driver does | board must |
|---|---|---|---|
| +0x00 | strh/ldrh | writes 0x1FC2; init waits for **bit 4** if CGU bit 14 was already on [V] | read bit 4 set (already in `p910_ready`) |
| +0x04 | ldrb | spins while **> 0x78** (FIFO level) before every chip access [V] | read 0 |
| +0x14 | str | 0xD0000000 (watched window start) [V] | keep (it is the frame-buffer phys) |
| +0x18 | str | 0xD0020800 (end) [V] | keep |
| +0x1C | strh | 0x10 [V] | keep |
| +0x20..0x5F | ldr / str | dirty bitmap, W1C [I] | read 0 (the board reads RAM itself) |

CGU phys 0x50000010: the driver ORs 0x1C000 and **faults "Lcd FIFO not enabled" if bit 14 does not
read back** [V]; the sys model keeps written values, so this holds.
Phys 0x30000004 = 0x00110889, 0x30000008 = 0x2443 (bus timing for the chip's chip-select [I]):
write-only.
Phys 0x580005FC (bit 0 busy, polled until 0) and 0x5800060C, 0x20000000 belong to the camera path
(near "IrqCamErr") [V access, I meaning], not the display.

## 3. Interrupts and the power-on state machine

Bound by name [V]: **IrqDispFifo (11)**, config 1, and **IrqDispSync (47)**, config 3. IrqDispWait (46)
is not used by this driver [V: no string]. IrqDma (0) is EKern's DMA ISR [V name table].

State machine at `[obj+8]` (0x500B5B50) [V]:
- 0: chip init, reg 0 &= ~0x20, state 1, **full-frame DMA** (0x20800 bytes).
- 1: (after the transfer completes [I]) reg 0 |= 0x40, state 2.
- 2..6: DispSync enabled. DispSync ISR (0x500B5F3C) disables itself, sets [0x80]=1 and queues a DFC
  (0x500B604C) that scans the dirty bitmap and DMAs dirty blocks; the interrupt is re-enabled when the
  transfer is done or nothing was dirty (0x500B5F08) [V code, I order].

What must happen to avoid a hang or fault:
- **DMA transfers must complete** (TC status + IrqDma). Otherwise the channel stays busy, state stays 1,
  and later register writes (contrast, backlight, flip) pile up until the 17th faults the kernel [V
  fault path, I trigger].
- DispSync (the panel's tearing/frame signal): raise it at ~60 Hz while IC1 bit 15 is enabled. With
  the dirty bitmap reading 0 the DFC finds nothing and re-arms; never raising it is also safe for the
  picture (the board reads RAM) but leaves the driver's update path unexercised [I].
- DispFifo: not needed while the FIFO level reads 0 [I].

## 4. Backlight / panel power

Nothing found that waits on pwmdrv/ampctrl in this path. Not investigated further.

## 5. DMA controller, phys 0x70000000: an ARM PL080 [I, strong]

EKern's helpers match the PL080 layout exactly [V accesses]: +0x00 IntStatus, +0x04 IntTCStatus,
+0x08 IntTCClear (write channel bit), +0x0C IntErrorStatus, +0x10 IntErrClr, +0x14 RawIntTCStatus,
+0x18 RawIntErrorStatus, +0x1C EnbldChns (controller is switched off when 0), +0x30 Configuration
(bit 0 enable). Channels: 8 x 0x20 at +0x100 (table 0x50039D04): +0 Src, +4 Dest, +8 LLI,
+0xC Control (size mask 0xFFF read back at 0x500159E8), +0x10 Config (bit 0 enable, bit 18 = 0x40000
halt, 0x50015A6C).

QEMU's own `pl080` model refuses peripheral flow control without request lines (the M2P transfer to
the LCD FIFO would never run). Cheapest: a small board-local PL080 that, when a channel's Config
bit 0 is set, finishes at once: clear Config bit 0 and the EnbldChns bit, set Raw/IntTCStatus bit n
(when Control bit 31 and Config bit 15 allow), raise IRQ 0 while `IntTCStatus | IntErrorStatus`
!= 0; IntTCClear clears. Copying is optional: if Dest is 0x18200000 the board may feed the bytes to
the panel model, otherwise skip it (for memory-to-memory channels, do the copy: other drivers may
rely on it).

## 6. Proposed board model

1. Graphic console 208x320. On each refresh read 0x20800 bytes at phys **0xD0000000** (or at the
   value last written to 0x58000714, default 0xD0000000), RGB565 little-endian, stride 416. Mark
   dirty only when the bytes changed (hash or `memory_region_snapshot_and_clear_dirty` on the RAM).
2. Phys 0x58000700 block (small MMIO, replaces the catch-all there): +0x00 keep, read |= 0x10;
   +0x04 read 0; +0x14/+0x18/+0x1C keep; +0x20..+0x5F read 0, writes ignored.
3. Phys 0x18000000/0x18200000: accept byte writes (log the register index/value; optionally keep the
   chip registers so "display off" (reg 0 bit 6 clear) blanks the console).
4. PL080 at 0x70000000 with instant completion -> IRQ 0 (section 5).
5. DispSync: QEMU timer at 60 Hz pulsing IC1 bit 15 (IRQ 47) when enabled.
6. Phys 0x30000000 page: keep values (catch-all is fine).

If the picture appears but never updates, it is still the RAM path (1): no driver cooperation is
needed for it. If boot faults with "ScreenDriver FAULT", look at the DMA completion first.

Open [I]: exact meaning of the chip registers (0x30/0x31 are probably a start line/address), the
DispFifo ISR, whether 0x58000700 +0x00 bits other than 4 are polled elsewhere (EKern 0x14C2C..).
