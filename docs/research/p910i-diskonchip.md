# P910i DiskOnChip (udiskonchip.dll): what a TrueFFS API stand-in needs

Static reading of `eka1/p910i/core/ROM.bin` (VA = 0x50000000 + file offset), 2026-11-18, for a
stand-in like the Nokia 9500's H-ra2-doc. [V] = read from code or data, [I] = inferred.
Every address below is a ROM VA; all code in udiskonchip.dll is Thumb unless noted.

## Verdict

udiskonchip.dll holds a C++ media driver **and** the TrueFFS library, joined by **one internal
dispatcher at 0x500A27C4**: `TInt dispatch(void *ctx, TInt func, IOreq *req)` [V]. It has the same
function codes and IOreq layout as the 9500's exported `flCall` (only the arguments are shifted by
the `ctx` in r0). Every TrueFFS call the media driver makes goes through it (17 call sites in
0x500AF260..0x500AFDEC) [V], and the only path to the hardware init (0x500A2F50) starts there [V].
**Replace the dispatcher alone** (plus two small neutralisations, below). The media driver, the FAT
file system and DocSupportDrv.ldd stay unchanged.

## 1. The media driver class

- Export ord 1 (0x500AF261) = `new(0x8018)` then ctor 0x500AF278: vtable 0x500B0454, base ctor = EUser
  ord 1359, fields +0x8010 = +0x8014 = -1 [V]. It returns the object itself, so it is the media
  driver (a PDD whose factory and driver are one object, or the 7.0 local-media model) [I].
- Object fields [V]: +4 TrueFFS context (`ctx`, allocated in DoCreate), +8 "mounted", +12 media size in
  bytes, +0x10..+0x800F a 32 KB bounce buffer, +0x8010/+0x8014 first sector / count cached in it.
- vtable 0x500B0454 (GCC 2.9 layout, two zero words first) [V for addresses, I for names]:

| off | addr | role |
|---|---|---|
| +08 | 0x500AF2C5 | destructor (dismount, flExit-like 0x500A31D8, close docdrv channel, delete ctx) |
| +0C | 0x500AFC61 | DoCreate: allocate ctx (ctor only zeroes fields, no hardware), mount (0x500AF85C), open the "docdrv" LDD channel, IRQ test (below) |
| +10 | 0x500AF399 | Read(TInt64& aPos (r1), TInt aLength (r2), const TAny* aTrg (r3), stack: arg5, arg6) |
| +14 | 0x500AF559 | Write(same shape; read-modify-write of partial sectors via read 0x500AF8B8) |
| +18 | 0x500AF75D | Format(TInt64& aPos, TInt aLength): pos 0 -> full format 0x2CA then remount; else delete sectors 0x331 |
| +1C | 0x500AF341 | size: returns +12 |
| +20 | 0x500AF345 | Caps(TDes8&): 40-byte caps block, see 3 |
| +24 | 0x500AF7D5 | -> 0x500AFB20 (function 0x32F, "defragment": in/out word at IOreq+16) |
| +28 | 0x500AF7E1 | re-opens the docdrv channel when ctx+0x698 (IRQ mode) is set; nothing otherwise |

- Data to/from the client goes through the 32 KB buffer: Read fills +0x10 with the internal read
  (0x500AF8B8, at most 64 sectors per call), builds a TPtrC8 (EUser ord 1386) and copies with EUser
  ord 1223 (target 0x5005726C; args (&arg6 copy, arg5, des, aTrg + done)) [V call, I meaning: a
  thread write]. Write uses EUser ord 880 (target 0x50057224) the other way [I].
- Internal helpers [V]: read 0x500AF8B8(this, buf, sector, count, [cache flag]), write
  0x500AF988(this, buf, sector, count), delete 0x500AFAAC(this, sector, count), defrag 0x500AFB20,
  dismount 0x500AFBCC(this, handle), format 0x500AFBF4, 0x2C2 call 0x500AFC40(this, flags),
  mount + info 0x500AF85C.

## 2. The dispatcher and the function codes

`0x500A27C4 dispatch(ctx r0, func r1, req r2)` [V]: handle = req[0]; socket = bits 0..3 (must be
< byte ctx+0xA80), partition = bits 4..7 (<= 3); otherwise returns 20. On first use it runs the
hardware init 0x500A2F50 (ctx+0xA70 == 0) [V]; jump table over func - 0x2C2 (0..198) [V].
Spans 0x500A27C4..0x500A2F4E (1930 bytes) [V].

IOreq (24 bytes on the caller's stack) [V]: +0 irHandle, +4 irFlags, +8 (path, unused), +12 irData,
+16 irSectorNo / irLength, +20 irSectorCount. The media driver always passes **handle 0** [V].

| func | used by | IOreq in | what must come back |
|---|---|---|---|
| 0x32A mount | 0x500AF85C, retries | handle | 0; 26 or 23 = "unformatted" (driver then uses size 0x03E90000) [V] |
| 0x336 volume info | 0x500AF85C after mount | irData = 56-byte buffer | 0, word 0 = sectors (size = sectors << 9) [V]; other words unread [V] |
| 0x332 read sectors | 0x500AF8B8 | +12 buffer, +16 sector, +20 count | 0 [V] |
| 0x330 write sectors | 0x500AF988 | same | 0 [V] |
| 0x331 delete sectors | 0x500AFAAC | +16 sector, +20 count | 0 [V] |
| 0x32F defragment | 0x500AFB20 | +16 in/out | 0 (driver copies +16 back); 63 -> -4 [V] |
| 0x32D dismount | 0x500AFBCC | handle | ignored [V] |
| 0x2CA format | 0x500AFBF4 | +4 flags 8, +12 params (28+4 bytes from 0x500B03F4) | ignored, driver remounts [V] |
| 0x2C2 (after every op, irFlags 1) | 0x500AFC40 | +4 flags | ignored [V] |

Result handling in the driver [V]: 55 = not mounted -> mount 0x32A and retry once; 85 -> dismount and
return -33; any other non-zero -> -20 on read/write, -2 on delete. A stand-in returning 0 or 20
behaves like the 9500 one.

## 3. Drives and sizes

- One volume: handle 0 = C: [V: every IOreq the driver builds]. No second (binary) partition is used
  by this driver [V]; ROM is XIP in NOR, so the DiskOnChip holds only C: [I].
- The FAT image has **no MBR**: sector 0 is the FAT16 boot sector ("EPOC", 512 B/sector, 2 sectors per
  cluster, 1 reserved, 1 FAT of 245 sectors, 512 root entries, hidden 0, 125184 sectors) and the file
  is 64,094,208 bytes = 125184 sectors [V]. So info 0x336 must return **125184** in word 0.
- Caps (0x500AF345) [V]: size (TInt64 lo = +12, hi 0), type 6 (flash), battery 0, drive att 0x11
  (local | internal), media att 4 (formattable), +28 = 1, +32 = 0; copied min(40, MaxLength).
  Nothing in it comes from TrueFFS beyond the size.

## 4. Init the stand-in must satisfy

- With the dispatcher replaced, nothing in udiskonchip touches the DiskOnChip registers: no chip-id,
  power or ready wait remains (all were inside 0x500A2F50 behind the dispatcher) [V for the single
  call site; I that no other path reaches the hardware].
- Two other entry points bypass the dispatcher and must be neutralised:
  - 0x500A4868 (TrueFFS FAT filter, called directly by write 0x500AF9A6 before 0x330; result
    ignored) [V]. It reads ctx fields that only the real init fills and may run its own init
    (0x500A42FC). Make it `movs r0,#0; bx lr`.
  - 0x500A31D8 (flExit-like, called only by the destructor) [V]: also `movs r0,#0; bx lr` (only
    reached on shutdown).
- DocSupportDrv.ldd ("docdrv", ARM code) [V]: binds and enables interrupt 45 and has a request that
  completes on it; it reads the byte at VA 0x5C001020 (window + 0x1020) and wants bits 7..6 = 11,
  otherwise it faults (0x500B1728, code 1) in the request path 0x500B18EC [V read, I when it runs].
  DoCreate (0x500AFC61) opens it by name, queues one request, reads sector 0 through our read and
  checks the status: completed -> IRQ mode (ctx+0x698 = 1), still pending -> cancel + wait + close
  [V]. With no interrupt the cancel path is taken [I: the LDD's cancel completes the request]. So
  the board only needs **window + 0x1020 to read 0xC0** and no IRQ 45.

## 5. Proposed stand-in (hack "H-p910-doc")

Board (p910i.c), on the NOR ROM region before reset (the ROM region is a QEMU ROM loaded by
`p910_load`, so patch its RAM pointer / `address_space_write_rom` at the flash base + (VA - 0x50000000)):

| VA | old halfwords [V] | new |
|---|---|---|
| 0x500A27C4 (dispatcher) | b5f0 4657 464e 4645 b4e0 b087 4680 9105 | the stub (Thumb `bx pc; nop`, then ARM), at most 1930 bytes |
| 0x500A4868 (FAT filter) | b5f0 4657 464e 4645 | 2000 4770 (`movs r0,#0; bx lr`) |
| 0x500A31D8 (flExit-like) | b5f0 4657 464e 4645 | 2000 4770 |

Block device: put it inside the DiskOnChip window, already mapped by the bootstrap at physical
0xB0000000 = VA 0x5C000000 (8 KB, docs/research/p910i-soc-map.md) [V mapping], e.g. registers at
window + 0x1800 (VA 0x5C001800): +00 W command (1 read, 2 write, 3 info, 4 delete, 5 format),
+04 partition, +08 sector, +0C count, +10 buffer (VA; the board translates through the MMU as the
9500's `ra2_blk_*` does), +14 R result, +18 R sectors. The rest of the window reads 0, except +0x1020
which reads 0xC0 (DocSupportDrv). Backing: the FAT image, 125184 sectors, writes kept in memory
(as H-ra2-doc).

Stub outline (follows doc_standin_ra2.S; the 9500 `doc_flcall` body can be reused after the shift):

```
        .equ    BLK, 0x5c001800
        .text
        .thumb
_start:                                 @ 0x500A27C4, entered by Thumb BL
        bx      pc                      @ 4-aligned: continues at 0x500A27C8 in ARM
        nop
        .arm
doc_dispatch:                           @ r0 = ctx (ignored), r1 = function, r2 = IOreq
        mov     r0, r1
        mov     r1, r2
        stmfd   sp!, {r4-r6, lr}
        ldr     r4, =BLK
        ldr     r3, [r1]                @ handle: only 0 (socket 0, partition 0)
        cmp     r3, #0
        movne   r0, #20
        bne     9f
        ldr     r2, =0x336 ; cmp r0, r2 ; beq info      @ clear 0x30 bytes at irData, word 0 = [BLK+0x18]
        ldr     r2, =0x332 ; cmp r0, r2 ; moveq r6, #1 ; beq xfer
        ldr     r2, =0x330 ; cmp r0, r2 ; moveq r6, #2 ; beq xfer
        @ 0x32A mount, 0x32D, 0x331 (optionally cmd 4), 0x32F (leave +16), 0x2CA, 0x2C2: return 0
        mov     r0, #0
9:      ldmfd   sp!, {r4-r6, lr}
        bx      lr                      @ back to Thumb caller
xfer:   @ +04 = 0, +08 = [r1,#16], +0C = [r1,#20], +10 = [r1,#12], +00 = r6; r0 = [BLK+0x14] ? 20 : 0
info:   ...
        .ltorg
```

Checks: refuse to patch unless the old halfwords above match (as `ra2_doc_standin` does with its
words); log "p910i: H-p910-doc". Test: a host-side test like `eka1/test_ra2_doc_standin.py`
(stub assembles, fits in 1930 bytes, first halfword 0x4778 at a 4-aligned VA, old words checked).

Open [I]: the exact meaning of EUser ord 1223/880 (thread copy) and of 0x2C2 do not matter for the
stub; whether DocSupportDrv's 0x5C001020 check runs at boot is unverified (cheap to satisfy anyway).
