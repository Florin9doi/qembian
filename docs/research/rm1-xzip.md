# Nokia 6630 (RM-1): SOS_XZIP and the compressed core

Tool: `eka1/tools/xzip_core.py` (static, `python3 -I`; nothing from the firmware runs).

```
python3 -I eka1/tools/xzip_core.py 'eka1/rm1/fiasco/core/SOS*CORE@00070000.hdr' \
    'eka1/rm1/fiasco/core/SOS*CORE@00070400.bin' eka1/rm1/output/rom/ROM.bin \
    --rofs eka1/rm1/output/rom/rofs.img
```

## Format

- [V] SOS_XZIP (29 696 bytes, flash 0x50400) is position-independent ARM/Thumb (RVCT scatter-load
  stub at 0x0/0xE0, r9 static base) linked to run at 0x10090000 (literal 0x10090BE9 = Thumb
  function at file 0xBE8). It holds zlib 1.2.1 inflate, adler32 (0x1BC) and crc32 (0x280).
- [V] Driver at file offset 0x7E8 (Thumb): allocates `chunk*20` bytes, reads the partition from
  its start through a callback (global[0] = read function, global[1] = chunk size, set by the
  init function at 0x8DE), `memcpy(dest, buf, 0x3D0)`, then
  `inflateInit2_(strm, -15, "1.2.1", 56)` with `next_in = buf+0x3D0`, `next_out = dest+0x3D0`,
  and loops read/inflate(flush=2) until Z_STREAM_END (then inflateEnd at 0x1BFE, returns 1).
- [V] So the core is one raw deflate stream (32 KB window, no zlib/gzip wrapper) starting at
  partition offset 0x3D0, i.e. 0x30 bytes before the end of the 1 KB record header
  (`SOS*CORE@00070000.hdr`) - this is why no stream was found at the start of the .bin.
  No XOR, scrambling or cipher.
- [V] The stream inflates to 0xBF1030 bytes: 0x30 zero bytes (dest 0x3D0..0x400), then the EKA1
  ROM: reset vector `b` (0xEA000086) at 0, TRomHeader at 0x8C: base 0x50000000, size 0x00C00000,
  root 0x50003C60, kernel data 0x80000000, limit 0x80400000, primary 0x50004408, secondary
  0x50004810, checksum 0x7DEC64FC. `\EKern.EXE` is in the ROM directory (UTF-16, ROM+0x353D2).
- [I] The ROM is padded with zeros from 0xBF1000 to rom_size (as the 3230/6600 ROM.bin files are
  full rom_size). The header checksum's algorithm was not checked (a plain word sum does not match).
- [V] The stream ends at partition offset 0x629A60; then 0xFF, then at 0x630000 (flash 0x6A0000)
  a plain `ROFS` image (`ROFS` 30 FF 00 02, then dir tree 0x30/0x27E0, file entries
  0x2810/0x106EC, image size 0x99CAC0 at +0x24, max 0x9B0000) running exactly to the end of the
  data. The tool writes it with `--rofs` (not compressed, needs nothing).
- [I] The 1 KB record's first 0x30 bytes are the FIASCO record header; the 0x3D0 bytes copied raw
  land in RAM before the ROM and are not part of it.
