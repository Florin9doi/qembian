"""Tests for tools/xzip_core.py (Nokia 6630 compressed core) on synthetic data.

Run from the repo root: python3 eka1/test_xzip_core.py
"""
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
import xzip_core as x  # noqa: E402


def rom(size=0x10000):
    r = bytearray(size)
    struct.pack_into('<I', r, 0, 0xEA000086)                              # reset vector branch
    struct.pack_into('<8I', r, 0x8C, 0x50000000, size, 0x50000100, 0x80000000, 0x80400000,
                     0x50000200, 0x50000300, 0)
    return bytes(r)


def partition(img, tail=b''):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    stream = c.compress(bytes(x.ROM_SKIP) + img) + c.flush()
    part = x.HDR_MAGIC + bytes(x.STREAM_START - 4) + stream + tail
    return part[:x.HDR_SIZE], part[x.HDR_SIZE:]


class Unpack(unittest.TestCase):
    def test_rm1_stream_starts_inside_record(self):
        """rm1_stream_in_hdr: the raw deflate stream starts at partition offset 0x3D0, 0x30 bytes before the
        .bin: a scan of the .bin alone found no stream (the core looked scrambled)."""
        img = rom()
        hdr, body = partition(img)
        out, h, _, _ = x.unpack(hdr, body)
        self.assertEqual(out[:len(img)], img)
        self.assertEqual(h['rom_base'], 0x50000000)

    def test_padded_to_rom_size(self):
        img = rom(0x20000)[:0x18000]                    # the stream holds less than rom_size
        hdr, body = partition(img)
        out, _, _, _ = x.unpack(hdr, body)
        self.assertEqual(len(out), 0x20000)

    def test_rejects_bad_record(self):
        hdr, body = partition(rom())
        with self.assertRaises(x.XzipError):
            x.unpack(b'\0' * 4 + hdr[4:], body)
        with self.assertRaises(x.XzipError):
            x.unpack(hdr[:-1], body)

    def test_rejects_non_rom(self):
        hdr, body = partition(bytes(0x10000))
        with self.assertRaises(x.XzipError):
            x.unpack(hdr, body)


if __name__ == '__main__':
    unittest.main()
