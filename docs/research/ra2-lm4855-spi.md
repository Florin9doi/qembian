# Nokia 9500 (RA-2): the LM4855 amplifier / Espi SPI hang

Static reading only (core ROM `eka1/ra2/core/ROM.bin`, VA = 0x50000000 + offset, objdump ARM); nothing
was run. [V] = read in the code at the given address, [I] = inferred.

## Modules [V]

| module | header | code | notes |
|---|---|---|---|
| Lm4855.ldd | 0x5005d760 | 0x5005d7c4, 0xc88 | 1 export (0x5005ddd0, factory) |
| Espi.dll | 0x5005e460 | 0x5005e4c4, 0xa84 | kernel extension, 0 exports; entry 0x5005e4c8 |

All register accesses go through EKern's McBSP accessors (export slots 0x50038xxx -> 0x500100xx), with
base 0x580B1000 (McBSP2, phys 0xFFFB1000) [V].

## 1. Espi interface as used by Lm4855

- **How Lm4855 gets it** [V]: Espi's entry (0x5005e4c8) builds a 0x1c-byte object (vtable 0x5005ef2c),
  initialises McBSP2 (0x5005e6a4) and calls EKern 0x50010298 = `*(0x80000b44) = obj` (slot 0x50039010).
  Lm4855 reads it back with EKern 0x500102a8 (`return *(0x80000b44)`, slot 0x50039014, thunk
  0x5005e090) at 0x5005d8c0 and keeps it at `this+0x40`.
- **Espi vtable 0x5005ef2c** (GCC 2.9x: two zero words, then) [V for addresses, I for names]:
  +8 `Configure(const TSpiConfig*)` 0x5005e884; +0xc `GetConfig(TSpiConfig*)` 0x5005ea50;
  +0x10 `Write(const TDesC8&)` 0x5005ea70; +0x14 `Read` 0x5005ec2c (returns -5); +0x18 dtor 0x5005e650.
- **Configure** [V]: arg = 5 words {speed kHz, word mode 0/1/2 (8/16/32 bit), ?, ?, ?}. Lm4855 passes
  {1000, 0, 1, 0, 0} (0x5005d8c8..0x5005d8f4) and calls LeaveIfError on the result (0x5005d8f8 ->
  EUser 0x500e1ab4). Only register writes (SRGR1 divisor, XCR/RCR word length, SPCR1 bit 11/3);
  no wait. It returns -5 if `speed*1000 > clock*1000` from the clock read at 0x5005ec60
  (0x580ECF00 / 0x580ECE00 DPLL / CKCTL) [V]; so a clock read of 0 would give a Leave(-5), not a hang [I].
- **Write(const TDesC8& aBuf)** 0x5005ea70 [V]: synchronous, polled; no interrupt, DFC, semaphore or
  callback anywhere in Espi (no such import) [V]. Steps: length 0 -> -6; mark busy, power handler
  on (EKern 0x5002a590); SPCR2 |= GRST 0x40, FRST 0x80, XRST 0x01 (3 read-modify-writes, 0x5005eaa4..
  0x5005ead0); per word: **poll SPCR2 (0x580B1008) bit 1 XRDY until set** (loop 0x5005ebb8..0x5005ebc4),
  write DXR2 (+4) then DXR1 (+6); at the end **poll SPCR2 until XRDY = 1 and bit 2 (XEMPTY) = 0**
  (loop 0x5005ebe8..0x5005ec08); clear busy, power handler standby (0x5005ec1c); return 0.
- **Lm4855's register write** 0x5005dc80(this, value) [V]: bit-reverses the byte, builds a TBuf8<1>,
  requests power (own handler `this+0x3c`, EKern 0x5002a6a0 with 0x400), calls `iSpi->Write(buf)`
  (0x5005dd5c, vtable +0x10), releases power (0x5005dd6c), returns Write's result (or -42 / the power
  error). Callers: 0x5005d910 (init, value 0), 0x5005db94, 0x5005dc4c, 0x5005dc7c (tail call).
- **What DoCreate waits for** [V model side, I that this is the spinning loop]: the init virtual
  0x5005d894 (vtable entry 0x5005e3d4) -> 0x5005dc80(this, 0) -> Espi Write -> the final loop
  0x5005ebe8..0x5005ec08. QEMU's `omap_mcbsp_writeh` SPCR2 case sets XEMPTY (bit 2) := XRST and nothing
  ever clears it (build/qemu-ngage/qemu-11.1.3/hw/arm/omap1.c, SPCR2 write) [V], so after the
  XRST write the loop never ends: a kernel-side busy spin, not a blocked wait. The per-word XRDY loop
  passes: Espi sets SRGR2 = 0x2000 (CLKSM, 0x5005e7c8), PCR = 0x0A01 (CLKXM, SCLKME = 0, 0x5005e788),
  so with GRST+XRST QEMU computes a non-zero tx_rate and its sink tick raises XRDY with
  tx_req = rate << bps, far more than a few words [V for values, I for the run-time effect].

## 2. Guest stand-in (preferred: no dependence on the McBSP model)

**H-ra2-lm4855 (2 words)**: Espi `Write` returns KErrNone at once [V for addresses and old words]:

| VA | old | new |
|---|---|---|
| 0x5005ea70 | 0xe92d41f0 (`push {r4-r8,lr}`) | 0xe3a00000 (`mov r0, #0`) |
| 0x5005ea74 | 0xe1a08000 (`mov r8, r0`) | 0xe1a0f00e (`mov pc, lr`) |

Safe because Write's only side effects are McBSP2 writes and its own busy flag / power handler,
which it sets and clears itself; Lm4855 ignores the data path. Espi's only caller is Lm4855 (the
global 0x80000b44 is read by Lm4855 only in this pair; other users not checked [I]).

Smallest alternative (1 word): only the final XEMPTY test, 0x5005ec04 `tst r0, #4` 0xe3100004 ->
`tst r0, #0` 0xe3100000 (Z set, the `bne` at 0x5005ec08 never loops). Keeps the per-word XRDY poll,
which relies on QEMU's sink tick [I], so the 2-word stub is the safer seam.

Alternative in Lm4855 (2 words): 0x5005dc80 old 0xe92d4070 / 0x5005dc84 old 0xe24dd01c ->
0xe3a00000 / 0xe1a0f00e (register write = `return 0`); also skips Lm4855's power request.

Board code: same form as `ra2_dsp_boot_patch` in `src/qemu-ngage/hw/arm/nokia9500.c` (read, compare
old, `ra2_put32`).

## 3. QEMU McBSP alternative

Espi only writes and polls SPCR2 (offset 8): bit 1 XRDY must read 1, and bit 2 XEMPTY must read 0
after the data words [V]. A fix in `omap_mcbsp_writeh` (shared omap1.c, gate on SPI mode = SPCR1
CLKSTP bits 12:11 != 0; Espi writes SPCR1 = 0x1800 at 0x5005e77c [V]): in the DXR1 case
`if (s->spcr[0] & 0x1800) { s->spcr[1] = (s->spcr[1] | 2) & ~4; return; }` before the tx_req path.
DRR is never read [V]. It is a patch to the shared QEMU tree (patches/qemu-ngage), larger than the
2-word guest patch, and the guest patch needs no McBSP clock state at all; prefer the guest patch
unless real SPI traffic is wanted later.
