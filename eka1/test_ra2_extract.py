"""Tests for tools/ra2_extract.py (Nokia 9500 RA-2 flash container) on synthetic data.

Run from eka1/: python3 -m unittest -v test_ra2_extract
"""
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))
import ra2_extract as x  # noqa: E402


def rec(payload, kind=1):
    return bytes([0x28, 0, 0, 0, 0, 0x12, 0x34, kind]) + struct.pack('>I', len(payload)) + bytes(3) + b'\x99' + payload


def sub(body, size, comp, load, tag):
    stored = len(body)
    return (struct.pack('<I', x.SUB) + struct.pack('<8I', 1, 2, size, comp, 3, stored, load, load)
            + tag.encode().ljust(20, b'\0') + body)   # the tag field is 20 bytes (+36..+55)


def container(*records):
    return b'\xa2' + struct.pack('>I', 4) + bytes(4) + b''.join(records)


def deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


class Records(unittest.TestCase):
    def test_walk_and_descriptor(self):
        d = container(rec(b'', kind=2), rec(b'ROFS' + bytes(60)))
        r = x.records(d)
        self.assertEqual([len(p) for _, _, p in r], [0, 64])
        self.assertEqual(r[1][0], 9 + 16)
        self.assertEqual(x.kind(r[1][2]), 'ROFS')

    def test_length_is_big_endian(self):
        d = container(rec(bytes(0x123)), rec(b'x'))
        self.assertEqual(x.records(d)[1][0], 9 + 16 + 0x123)

    def test_errors(self):
        with self.assertRaises(x.FormatError):
            x.records(b'\xa3' + bytes(8))
        with self.assertRaises(x.FormatError):
            x.records(container(rec(b'abc'))[:-1])          # runs past the end
        with self.assertRaises(x.FormatError):
            x.records(container() + b'\x14' + bytes(15))     # unknown record type
        with self.assertRaises(x.FormatError):
            x.records(container() + b'\x28' + bytes(5))      # truncated header


class SubHeader(unittest.TestCase):
    def test_deflate_core(self):
        img = bytes(range(256)) * 64
        p = sub(deflate(img), len(img), 1, 0x10600000, '001C24070606.27(00)')
        sh = x.sub_header(p)
        self.assertEqual((sh['tag'], sh['load'], sh['compression']), ('001C24070606.27(00)', 0x10600000, 1))
        self.assertEqual(x.unpack(p, sh), img)

    def test_stored(self):
        p = sub(b'hello world', 5, 0, 0, '001D')
        self.assertEqual(x.unpack(p, x.sub_header(p)), b'hello')

    def test_short_tag_data_at_56(self):
        # rm12_short_tag_field: the 7710's tags ("Tetra v7") are shorter than the 9500's 19 characters;
        # the data starts at +56 regardless (reading from the tag's NUL gave "invalid stored block lengths")
        img = b'7710' * 100
        p = sub(deflate(img), len(img), 1, 0x13B00000, 'Tetra v7')
        sh = x.sub_header(p)
        self.assertEqual((sh['tag'], sh['data_offset']), ('Tetra v7', 56))
        self.assertEqual(x.unpack(p, sh), img)

    def test_size_mismatch(self):
        img = b'abc' * 10
        p = sub(deflate(img), len(img) + 1, 1, 0, 'T')
        with self.assertRaises(x.FormatError):
            x.unpack(p, x.sub_header(p))

    def test_not_a_sub_header(self):
        self.assertIsNone(x.sub_header(b'ROFS' + bytes(60)))
        self.assertIsNone(x.sub_header(b'\x56\x34\x12\x99'))  # too short

    def test_fat_and_text_kinds(self):
        self.assertEqual(x.kind(b'\xe9\x00\x90EPOC\0\0\0\0' + bytes(8)), 'FAT')
        self.assertEqual(x.kind(struct.pack('<I', x.TEXT) + b'001T CRC'), 'text')


if __name__ == '__main__':
    unittest.main()
