#!/usr/bin/env python3
"""Nokia 9210 Communicator (RAE-3) flash files -> the EKA1 core ROM (untrusted input: run with python3 -I).

  rae3_extract.py MAIN.130 OUT/ROM.bin [--chip8 OUT.bin LANG.13x]
  rae3_extract.py MAIN.000 OUT/ROM.bin --rae5 [LANG.00A]       Nokia 9210i (RAE-5, firmware 6.00): see rae5_image()

Container (the DCT3-era Phoenix "LINDA ALGORITHM" format, 0xA2 header as eka1_extract.py) [V]:
  header  0xA2, u32 BE length, then tokens (eka1_extract.parse_container's header code)
  0x19    a 14-byte record: type, u24 BE 512-byte sector, u16 BE device (1 or 8), u24 BE word address, data
          checksum, u24 BE size, header checksum; then `size` bytes. Device data is two 8-bit lanes: even bytes
          = chip A, odd bytes = chip B; the word address is the chip offset.
  0x14    a 10-byte record: type, u32 BE bus address, data checksum, u24 BE size, header checksum; plain bytes.
The core ROM (EKA1, base 0x50000000, size at header +0x90) [V: its directory walks, every executable's
header valid]:
  ROM 0x000000..0x3FFFFF = device 1, chip A (offsets 0..)
  ROM 0x400000..0x7FFFFF = device 1, chip B
  ROM 0x800000..       = device 8, chip A from offset 0x24DE00 (the main file's device-8 records start there),
                          then the 0x14 records at bus 0x02000000 + X = ROM 0x800000 + X (from 0x021D0100)
On the phone the upper part is at physical 0x02000000 + X (the kernel maps VA 0x50BCF000 to 0x023CF000).
The language file (.13x) fills device 8 below 0x24DE00 (both chips) and bus 0x08xxxxxx: a language area
not needed for the core ROM (--chip8 writes it for study).
"""
import argparse
import struct
import sys

DEV8_ROM_BASE = 0x24DE00            # device-8 chip-A offset of ROM 0x800000 [V Calicovw.app's header]


def records(data):
    """Yield (kind, address, payload): ('dev1'|'dev8', chip offset, raw 2-lane bytes) or ('bus', addr, bytes)."""
    if not data or data[0] != 0xA2:
        raise ValueError('not a 0xA2 Phoenix container')
    be = lambda o, n: int.from_bytes(data[o:o + n], 'big')
    pos = 5 + be(1, 4)
    while pos < len(data):
        kind = data[pos]
        if kind == 0x19:
            if pos + 14 > len(data):
                raise ValueError('truncated 0x19 record at 0x%x' % pos)
            size = be(pos + 10, 3)
            yield 'dev%d' % be(pos + 4, 2), be(pos + 6, 3), data[pos + 14:pos + 14 + size]
            pos += 14 + size
        elif kind == 0x14:
            size = be(pos + 6, 3)
            yield 'bus', be(pos + 1, 4), data[pos + 10:pos + 10 + size]
            pos += 10 + size
        else:
            raise ValueError('unknown record type 0x%02x at 0x%x' % (kind, pos))


def core_rom(main):
    a, b, up = bytearray(b'\xff' * 0x400000), bytearray(b'\xff' * 0x400000), bytearray(b'\xff' * 0x800000)
    for kind, addr, payload in records(main):
        if kind == 'dev1':
            if addr + len(payload) // 2 > 0x400000:
                raise ValueError('device 1 record past 4 MB at 0x%x' % addr)
            a[addr:addr + len(payload) // 2] = payload[0::2]
            b[addr:addr + len(payload) // 2] = payload[1::2]
        elif kind == 'dev8':
            off = addr - DEV8_ROM_BASE
            if off < 0:
                raise ValueError('device 8 record below the ROM part at 0x%x' % addr)
            up[off:off + len(payload) // 2] = payload[0::2]
        elif kind == 'bus' and 0x02000000 <= addr < 0x02800000:
            off = addr - 0x02000000
            up[off:off + len(payload)] = payload
    rom = bytes(a + b + up)
    if struct.unpack_from('<I', rom, 0x8c)[0] != 0x50000000:
        raise ValueError('no EKA1 ROM header (base 0x50000000) at +0x8C')
    size = struct.unpack_from('<I', rom, 0x90)[0]
    if not 0x800000 < size <= len(rom):
        raise ValueError('ROM size 0x%x out of range' % size)
    return rom[:size]


def rae5_image(main, lang=None):
    """Nokia 9210i (RAE-5, RAE53006.000 + a language file RAE53006.00x): the flash image, bank 0 from offset 0 [V layout
    from the records; board unverified].
      device 1, chip A (0..0x7D0000)  = ROM 0..0x7D0000        (header +0x8C base 0x50000000, size +0x90 = 0xFD0000)
      bus 0x007D0000..0x00800000      = ROM 0x7D0000..0x800000  (plain bytes)
      device 1, chip B (0..0x7D0000)  = ROM 0x800000..0xFD0000  (ROM size = 0x7D0000 + 0x30000 + 0x7D0000)
      bus 0x013A0000..0x01590000      = ROM 0xBA0000..0xD90000  the language area (bank 1 = 0x01000000 + ROM - 0x800000);
                                        the main file already holds the English one and the .00A / .00U files carry the
                                        same bytes, the other languages replace it [V compared]
      bus 0x017D0000..                = the PPM (product profile "MPPV 06.00", language/keyboard data) right behind the
                                        ROM (ROM 0xFD0000..): the image is the flash as bank 0 + bank 1 hold it."""
    img = bytearray(b'\xff' * 0x1000000)
    for data in (main, lang):
        if data is None:
            continue
        a, b = bytearray(b'\xff' * 0x7D0000), bytearray(b'\xff' * 0x7D0000)
        have_dev1 = False
        for kind, addr, payload in records(data):
            if kind == 'dev1':
                if addr + len(payload) // 2 > 0x7D0000:
                    raise ValueError('device 1 record past 0x7D0000 at 0x%x' % addr)
                a[addr:addr + len(payload) // 2] = payload[0::2]
                b[addr:addr + len(payload) // 2] = payload[1::2]
                have_dev1 = True
            elif kind == 'bus' and addr < 0x01000000:
                img[addr:addr + len(payload)] = payload
            elif kind == 'bus' and addr < 0x02000000:
                off = 0x800000 + addr - 0x01000000
                img[off:off + len(payload)] = payload
            else:
                raise ValueError('record kind %s at 0x%x not in the RAE-5 map' % (kind, addr))
        if have_dev1:
            img[0:0x7D0000] = a
            img[0x800000:0x800000 + 0x7D0000] = b
    if struct.unpack_from('<I', img, 0x8c)[0] != 0x50000000:
        raise ValueError('no EKA1 ROM header (base 0x50000000) at +0x8C')
    size = struct.unpack_from('<I', img, 0x90)[0]
    if not 0x800000 < size <= 0xFD0000 + 0x10000:
        raise ValueError('ROM size 0x%x out of range' % size)
    end = 0x800000 + 0x7D0000 + 0x10800          # the PPM's end (bus 0x017E0800 in the main file)
    return bytes(img[:end])


def chip8(lang):
    """The language file's device-8 data: chip A then chip B (8 MB each), bus 0x08xxxxxx into chip A."""
    a, b = bytearray(b'\xff' * 0x800000), bytearray(b'\xff' * 0x800000)
    for kind, addr, payload in records(lang):
        if kind == 'dev8':
            a[addr:addr + len(payload) // 2] = payload[0::2]
            b[addr:addr + len(payload) // 2] = payload[1::2]
        elif kind == 'bus' and 0x08000000 <= addr < 0x08800000:
            a[addr - 0x08000000:addr - 0x08000000 + len(payload)] = payload
    return bytes(a + b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('main')
    ap.add_argument('out')
    ap.add_argument('--chip8', nargs=2, metavar=('OUT', 'LANG'))
    ap.add_argument('--rae5', nargs='?', const='', metavar='LANG', help='the 9210i (RAE-5): flash image, optional language file')
    a = ap.parse_args()
    if a.rae5 is not None:
        img = rae5_image(open(a.main, 'rb').read(), open(a.rae5, 'rb').read() if a.rae5 else None)
        open(a.out, 'wb').write(img)
        print('%s: RAE-5 flash image 0x%x bytes, ROM size 0x%x, root 0x%08x' % (
            a.out, len(img), struct.unpack_from('<I', img, 0x90)[0], struct.unpack_from('<I', img, 0x94)[0]))
        return
    rom = core_rom(open(a.main, 'rb').read())
    open(a.out, 'wb').write(rom)
    print('%s: core ROM 0x%x bytes, root 0x%08x' % (a.out, len(rom), struct.unpack_from('<I', rom, 0x94)[0]))
    if a.chip8:
        open(a.chip8[0], 'wb').write(chip8(open(a.chip8[1], 'rb').read()))
        print('%s: language area (device 8, chips A and B)' % a.chip8[0])


if __name__ == '__main__':
    try:
        main()
    except ValueError as e:
        sys.exit('rae3_extract: %s' % e)
