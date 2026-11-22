#!/usr/bin/env python3
"""xzip_core.py: turn a BB5 EKA1 phone's compressed core (FIASCO partition SOS*CORE, read by the
SOS_XZIP stage) into the plain EKA1 XIP ROM image (and, optionally, the plain ROFS behind it).

First and only phone checked: Nokia 6630 (RM-1, S60 2nd FP2, Symbian 8.0a), RM1_6.050340_B4.C0R
split by tools/fiasco.py into SOS*CORE@00070000.hdr (the 1 KB partition record) and
SOS*CORE@00070400.bin (the rest). Run with python3 -I: the input is untrusted, nothing in it runs.

Format ([V] = read in SOS_XZIP's code or checked on the 6630 file, [I] = inferred):
  partition = hdr (0x400 bytes, at flash 0x70000) + bin; offsets below are into hdr+bin.
  - SOS_XZIP's driver (Thumb, file offset 0x7e8, XZIP runs at 0x10090000) reads the partition from
    its start, memcpy()s the first 0x3D0 bytes to the destination unchanged, then calls
    inflateInit2_(strm, -15, "1.2.1", 56) on the rest: a single RAW deflate stream (no zlib or
    gzip wrapper, 32 KB window) starting at partition offset 0x3D0, output going to dest+0x3D0.
    It loops inflate(Z_SYNC_FLUSH) on chunks until Z_STREAM_END, then inflateEnd. [V]
  - No scrambling, XOR or cipher: the bytes are inflated as they are in flash. [V]
  - The stream's first 0x30 output bytes are zero (they fill dest up to 0x400); the EKA1 ROM
    starts at inflated offset 0x30, i.e. dest+0x400: reset vector 'b' at 0, TRomHeader at 0x8C
    (6630: base 0x50000000, size 0x00C00000). [V]
  - The stream inflates to less than rom_size (6630: 0xBF1000 of 0xC00000); the tail is padded
    with zeros here. [I]
  - After the stream: 0xFF fill, then a plain ROFS image ('ROFS' magic) on a 64 KB boundary, its
    size at ROFS+0x24 (6630: partition offset 0x630000, 0x99CAC0 bytes, ending exactly at the end
    of the data). No checksum over the deflate data is checked by XZIP. [V]
Refuses: a hdr that is not 0x400 bytes or lacks the record magic A3 95 97 80, a stream that does
not inflate to its end, a non-zero 0x30 prefix, or no valid EKA1 TRomHeader at ROM+0x8C.
"""
import argparse
import struct
import sys
import zlib

HDR_SIZE = 0x400
HDR_MAGIC = bytes.fromhex('a3959780')
STREAM_START = 0x3D0        # memcpy length / inflate start in SOS_XZIP [V]
ROM_SKIP = 0x30             # zero bytes the stream emits before the ROM (dest 0x3D0..0x400) [V]


class XzipError(Exception):
    pass


def eka1_header(d):
    """EKA1 TRomHeader at 0x8C (same checks as tools/fw/semc.py eka1_header), or None"""
    if len(d) < 0x100:
        return None
    base, size, root, kdata, klimit, prim, sec, csum = struct.unpack_from('<8I', d, 0x8C)
    if base & 0xFFF or not 0x1000 <= size <= 0x8000000 or not base <= root < base + size:
        return None
    if not base <= prim < base + size:
        return None
    return dict(rom_base=base, rom_size=size, root=root, kern_data=kdata, kern_limit=klimit,
                primary=prim, secondary=sec, checksum=csum)


def unpack(hdr, body):
    """-> (rom bytes padded to rom_size, header dict, stream end offset, rofs bytes or None)"""
    if len(hdr) != HDR_SIZE or hdr[:4] != HDR_MAGIC:
        raise XzipError('not a 0x400-byte FIASCO partition record (magic A3 95 97 80)')
    part = hdr + body
    z = zlib.decompressobj(-15)
    try:
        out = z.decompress(part[STREAM_START:])
    except zlib.error as e:
        raise XzipError('no raw deflate stream at partition offset 0x%X: %s' % (STREAM_START, e))
    if not z.eof:
        raise XzipError('deflate stream does not end (truncated input?)')
    end = len(part) - len(z.unused_data)
    if len(out) < ROM_SKIP + 0x100 or any(out[:ROM_SKIP]):
        raise XzipError('inflated data does not start with 0x%X zero bytes' % ROM_SKIP)
    rom = out[ROM_SKIP:]
    h = eka1_header(rom)
    if h is None:
        raise XzipError('no EKA1 TRomHeader at ROM+0x8C')
    if struct.unpack_from('<I', rom, 0)[0] >> 24 != 0xEA:
        raise XzipError('ROM does not start with a branch (reset vector)')
    if len(rom) > h['rom_size']:
        raise XzipError('inflated ROM 0x%X is larger than rom_size 0x%X' % (len(rom), h['rom_size']))
    rom = rom + bytes(h['rom_size'] - len(rom))
    rofs = None
    off = (end + 0xFFFF) & ~0xFFFF
    while off + 0x30 <= len(part):
        if part[off:off + 4] == b'ROFS':
            size = struct.unpack_from('<I', part, off + 0x24)[0]
            if off + size <= len(part):
                rofs = part[off:off + size]
            break
        if part[off:off + 0x10000].strip(b'\xff'):
            break               # something else, not a ROFS on this boundary
        off += 0x10000
    return rom, h, end, rofs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('hdr', help='SOS*CORE@<addr>.hdr (the 1 KB partition record from tools/fiasco.py)')
    ap.add_argument('bin', help='SOS*CORE@<addr+0x400>.bin')
    ap.add_argument('out', help='output ROM.bin (plain EKA1 XIP ROM, rom_size bytes)')
    ap.add_argument('--rofs', help='also write the plain ROFS image that follows the stream')
    a = ap.parse_args(argv)
    with open(a.hdr, 'rb') as f:
        hdr = f.read()
    with open(a.bin, 'rb') as f:
        body = f.read()
    try:
        rom, h, end, rofs = unpack(hdr, body)
    except XzipError as e:
        sys.exit('xzip_core: refused: %s' % e)
    with open(a.out, 'wb') as f:
        f.write(rom)
    print('ROM %s: base 0x%08X size 0x%X root 0x%08X kdata 0x%08X klimit 0x%08X primary 0x%08X '
          'secondary 0x%08X checksum 0x%08X; stream ended at partition offset 0x%X'
          % (a.out, h['rom_base'], h['rom_size'], h['root'], h['kern_data'], h['kern_limit'],
             h['primary'], h['secondary'], h['checksum'], end))
    if a.rofs:
        if rofs is None:
            sys.exit('xzip_core: no ROFS image found after the stream')
        with open(a.rofs, 'wb') as f:
            f.write(rofs)
        print('ROFS %s: 0x%X bytes' % (a.rofs, len(rofs)))


if __name__ == '__main__':
    main()
