"""Tests of eka1/tools/rae3_extract.py (Nokia 9210 flash files -> core ROM). Synthetic records; the real-ROM
checks run only when the (git-ignored) firmware is there. Run: python3 eka1/test_rae3_extract.py"""
import pathlib
import struct
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'tools'))
import rae3_extract as x  # noqa: E402

MAIN = HERE / 'rae3_rom/RAE33004.130'
ROM = HERE / 'rae3/core/ROM.bin'


def container(*recs):
    body = b''.join(recs)
    return bytes([0xA2]) + struct.pack('>I', 4) + struct.pack('>I', 0) + body


def rec19(device, word, payload):
    return (bytes([0x19]) + (0).to_bytes(3, 'big') + device.to_bytes(2, 'big') + word.to_bytes(3, 'big') +
            b'\0' + len(payload).to_bytes(3, 'big') + b'\0' + payload)


def rec14(addr, payload):
    return bytes([0x14]) + addr.to_bytes(4, 'big') + b'\0' + len(payload).to_bytes(3, 'big') + b'\0' + payload


class Records(unittest.TestCase):
    def test_parse_both_kinds(self):
        data = container(rec19(1, 0x10, b'abcd'), rec14(0x02000004, b'xy'))
        self.assertEqual(list(x.records(data)), [('dev1', 0x10, b'abcd'), ('bus', 0x02000004, b'xy')])

    def test_unknown_record_refused(self):
        with self.assertRaises(ValueError):
            list(x.records(container(b'\x33\0\0')))


class Rae3TwoLanes(unittest.TestCase):
    """Regression rae3_lanes: device data is two 8-bit lanes (even bytes chip A = ROM 0..4 MB, odd bytes chip B =
    ROM 4..8 MB); read as one stream the ROM had no header and its UTF-16 names were interleaved."""

    def rom_with(self, *extra):
        hdr = bytearray(0x100)
        struct.pack_into('<3I', hdr, 0x8c, 0x50000000, 0x800100, 0x50000000)
        lanes = bytearray(len(hdr) * 2)
        lanes[0::2] = hdr                       # chip A
        lanes[1::2] = b'B' * len(hdr)           # chip B
        return container(rec19(1, 0, bytes(lanes)), *extra)

    def test_chip_a_is_rom_start_chip_b_is_4mb(self):
        rom = x.core_rom(self.rom_with())
        self.assertEqual(struct.unpack_from('<I', rom, 0x8c)[0], 0x50000000)
        self.assertEqual(rom[0x400000:0x400004], b'BBBB')

    def test_upper_part_from_device8_and_bus(self):
        """Regression rae3_rom_gap: ROM 0x800000 is device-8 chip-A offset 0x24DE00 and the bus records continue at
        0x02000000 + offset; mapping chip 8 from 0 left 26 files blank."""
        d8 = bytearray(4)
        d8[0::2] = b'P3'
        rom = x.core_rom(self.rom_with(rec19(8, x.DEV8_ROM_BASE, bytes(d8)), rec14(0x02000080, b'Z')))
        self.assertEqual(rom[0x800000:0x800002], b'P3')
        self.assertEqual(rom[0x800080:0x800081], b'Z')

    def test_device8_below_the_rom_part_refused(self):
        with self.assertRaises(ValueError):
            x.core_rom(self.rom_with(rec19(8, 0x100, b'ab')))


@unittest.skipUnless(MAIN.exists(), 'RAE-3 firmware not there')
class RealFirmware(unittest.TestCase):
    def test_core_rom(self):
        rom = x.core_rom(MAIN.read_bytes())
        self.assertEqual(len(rom), 0xbd0000)
        self.assertEqual(struct.unpack_from('<I', rom, 0x94)[0], 0x5089aaac)     # root directory
        self.assertEqual(struct.unpack_from('<I', rom, 0x5080aa90 - 0x50000000)[0], 0x10000079)  # Calicovw.app


if __name__ == '__main__':
    unittest.main()
