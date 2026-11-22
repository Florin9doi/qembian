# SE P800: internal storage (for `-M p800`)

Static reading of the P800 core ROM (`eka1/p800/core/ROM.bin`, VA = 0x50000000 + offset) and the
flash files. [V] = read from code or bytes, [I] = inferred. Purpose: just enough to model the
storage on our board; not a full study of LFFS, ROLF or the Memory Stick.

## 1. EStart drive set-up (EStart.exe 0x500A55D0, Thumb)

Main mount routine at 0x500A5AE8 [V order, call targets named by their arguments]:

| step | call | what |
|---|---|---|
| 1 | 0x500A573C(`Z:\System\Libs\MEDRAM.PDD`) | load PDD; any error other than 0 / -11 -> "Loading driver failed" + **panic ESTART 0** [V] |
| 2 | 0x500A5A44(`Media.Lfs`) | find the `Media.Lfs` physical device; one failure path leaves/panics with -2 [I: probably frees an already loaded one so step 7 can load it] |
| 3 | AddFileSystem(`Z:\System\Libs\EFAT.FSY`) | error other than 0 / -11 -> 0x500A6608 (Leave/panic) [V branch, I name] |
| 4 | 0x500A6460(fs, `Z:\`) | session path [I] |
| 5 | 0x500A59EC(2), 0x500A59EC(0) | per-drive set-up for drives 2 (C) and 0 (A) [I: drive info/settings] |
| 6 | MountFileSystem(`Fat`, **2 = C:**) then a directory scan of `C:\` (0x500A5A98) | |
| 7 | load `Z:\System\Libs\MEDINT.PDD` (panic ESTART 0 on failure) | |
| 8 | MountFileSystem(`Lffs`, **4 = E:**) | |
| 9 | MountFileSystem(`Rolf`, **0 = A:**) | |

MountFileSystem wrapper 0x500A5708(name, drive): result 0, -11 (AlreadyExists), -20 (Corrupt),
-18 (NotReady) accepted; anything else -> **panic ESTART 2** [V]. So a missing/garbled medium does
not panic, only an unexpected error code does.

A second routine (0x500A5884) mounts drive 0 = A: with `Z:\System\Libs\EAROLF` + `Rolf` and drive
3 = D: with `MEDMS.PDD` + `MSFAT` + `Fat` (Memory Stick) [V]; a failure at 0x500A655C there gives
panic ESTART 5 [V]. The `Lffs` file system lives inside ELocal.fsy (string at 0x50097540) [V].

Drive letters, as passed to MountFileSystem [V]: A: ROLF, C: FAT on the RAM drive (medram),
D: Memory Stick FAT, E: LFFS on internal NOR (medint), Z: ROM.
[I, open] The LNG image (the LFFS volume) holds `\SYSTEM\APPS\DEMO\...`, GsChess, HomeRun and
references `C:\Documents\GsChess\`; whether the user-visible "internal memory" is the RAM C: or
the LFFS E: depends on F32's drive -> local-drive mapping, not read. For the board it does not
matter: both media must just work.

## 2. medint.pdd (`Media.Lfs`, 0x500A9D10, ARM) = Intel StrataFlash-class NOR driver

- Base: virt **0x5E000000** (= phys 0xB0000000 through the bootstrap's 32 MB mapping) stored in
  the hardware object at +0x38 and +0x3C [V, 0x500AA4B0]. Reads are a plain memcpy from
  base + offset in read-array mode [V, 0x500AA628]. 0x5C000000 is not used by medint [V].
- Bus: x16; commands are 16-bit stores (`strh`) at the block / base address [V].
- **Partition: offset 0, length 0x00C00000 (12 MB)**, constants in the caps code [V, 0x500AA2A8].
  Caps [V, 0x500AA338]: iType 6 (EMediaFlash), iDriveAtt 0x11 (local|internal), iMediaAtt 4,
  iFileSystemId 3 (LFFS) [I names], iEraseBlockSize = CFI block size.
- Init (0x500AA510, 0x500AA598) [V]: `0x98` (CFI query) at base, read bytes at base+2*index:
  - 0x27: device size = 2^n (only used for the unlock-all loop range)
  - 0x2A/0x2B: write buffer = 2^n bytes (the program unit)
  - 0x2D/0x2E: number of blocks - 1 (stored, not otherwise seen used)
  - 0x2F/0x30: erase block size = (lo | hi<<8) * 256
  - 0x36 bit 5 (instant individual block locking): if set, unlock every block at init
  then `0xFF`. No `QRY` check, no `0x90` ID read anywhere in medint [V].
  `0xB8` + `0x00` = STS pin configuration (level RY/BY#) [V].
- Program (0x500AA68C) [V]: clear IRQ, `0x50`; **one 32-bit store** `(count<<16) | 0xE8` at the
  block address (count = buf/2 - 1 halfwords, no wait for XSR); buffer copied with 32-bit stores
  (exactly 2^CFI[0x2A] bytes); `0xD0` at the start address. No polling.
- Erase (0x500AA730) [V]: `0x50`, clear IRQ, `0x20`, `0xD0` at the block. No polling.
- Suspend `0xB0` (0x500AA780), resume `0xD0` (0x500AA798) [V].
- Lock/unlock (0x500AA8DC) [V]: **32-bit store** `(0xD0|0x01)<<16 | 0x60` per block; with
  CFI 0x36 bit 5 clear it polls `0x70` status bit 7 instead of streaming them.
- **Completion is interrupt driven** [V]: "IrqSTS", interrupt **45** bound with (45, 3)
  (0x500AA57C), acknowledged by writing 0x2000 to virt 0x58000304 = phys 0x50000304 = IC1 +04
  (write-1-to-clear), bit 13 -> IRQ 32+13 = 45 [V regs, matches p910i_soc.c IC layout].
  ISR (0x500AA814): `ldrh` status at base, `0x50`, then: state program/erase -> `0xFF`, complete
  with -2 if status & 0x3A (lock/VPP/program/erase error) else 0; state suspend -> suspended if
  status bit 6 (ESS) set and no error bits [V].
- FTL: none in medint; the file system on top is LFFS (ELocal.fsy) working on erase blocks [V].

## LNG payload = the LFFS volume image [V]

`P800_Language_5_EN.LNG.bin`: header `[0xB0000000][0x00C00000]`, then 12 MB = **96 erase blocks of
0x20000**, each starting with an LFFS block header `[u32 n][u32 n+1][u32 n][0x20000][0x60 = block
count][0x00180004][0x090E0040][0x02000080]...` (n = 0..0x5F) [V]. Blocks 0..0x19 hold data, the rest
are ~99 % 0xFF. No FAT boot sector (no 0x55AA at any sector end) [V]. So: flash offset 0 = LNG
payload (LFFS, medint's partition), offset 0xC00000 = ROLF (0x3FFA00 bytes, A:). Both end at
0xFFFA00, so the chip is probably 16 MB (28F128-class) inside the 32 MB window [I].

## 3. medram.pdd (`Media.IRam`, 0x50B546B0, ARM)

The standard EKA1 internal RAM drive (kernel RAM-drive chunk); no board register access seen [I].
Needs nothing from the board; FAT is formatted/mounted by F32/EStart on it (empty after a cold boot,
`Corrupt` is tolerated by EStart) [I].

## 4. Board model

`pflash_cfi01` as is is **not** enough [V against QEMU 11.1.3 hw/block/pflash_cfi01.c]:
- it takes a 32-bit store as one cycle (`cmd = value & 0xFF`), so `0xE8`+count and `0x60`+`0xD0`
  issued as one `str` lose their second half (buffered program breaks: the first data word becomes
  the count);
- no interrupt output (medint never polls program/erase, it waits for IRQ 45 -> writes hang);
- no `0xB8`, `0xB0` (error path -> read array, mostly harmless).

Recommended: a small own device `p800_nor.c` (RAM-backed ROM device at phys 0xB0000000, 32 MB
window, x16):
- reads: array mode = direct RAM (romd); status mode = status in every halfword; CFI mode = table
  at offset 2*index;
- writes: split every 32-bit write into two 16-bit cycles (low halfword first);
- commands: FF, 70, 50, 98, 90 (optional), B8 xx (ignore), 20/D0, E8/count/data.../D0, 40/10,
  60/D0|01 (status ready), B0 (status 0xC0, ESS), D0 resume; status 0x80 when ready,
  0x80|0x10/0x20 on errors (never needed);
- after program / erase / suspend completes: pulse IRQ 45 (IC1 bit 13), best a few hundred us
  later on a QEMU timer so the ISR runs after the thread waits; the IC must latch line 45 until the
  W1C at IC1 +04 (the ISR does not mask it, so a level line held high would storm) [I];
- CFI [I, StrataFlash J3-like]: 0x10..0x12 "QRY", 0x13 = 1 (Intel), **0x27 = 0x18** (16 MB),
  **0x2A = 0x05** (32 B buffer), 0x2D/0x2E = 0x7F/0x00, **0x2F/0x30 = 0x00/0x02** (128 KB, must match
  the 0x20000 in the LFFS headers [V]), **0x36 = 0x00** (skip unlock-all); blocks power up
  unlocked;
- contents: LNG payload at 0, ROLF payload at 0xC00000, 0xFF elsewhere; per-phone backing file so
  the LFFS keeps its writes.
The MMU aliases at virt 0x5E000000 and 0x5C000000 need nothing from the board.
