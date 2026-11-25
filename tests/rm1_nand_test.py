#!/usr/bin/env python3
"""eka1/tools/rm1_nand.py: the ROFx rebase (H-rm36-rofx-rebase) on synthetic ROFS/ROFx images, plus a check
of the 6680's real images when they are present (skipped otherwise).

Bug (2026-11-26, Nokia 6680): the variant ROFx's references into the core ROFS fell 4..288 bytes short of the
core's files (built against another core); "Dll Preload List" was read from 12 bytes before its start, EFile's
loader panicked FSCLIENT 25, then EComServer USER 22 over and over and the boot stopped at the logo.
"""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'eka1/tools'))
import rm1_nand as N  # noqa: E402

MAX = 0x10000           # the synthetic core's iMaxImageSize: the ROFx starts here (ROFS-relative)
UIDS = bytes(range(16))


def entry(name, size, addr, uids=UIDS):
    nm = name.encode('utf-16-le')
    e = bytearray(struct.pack('<H', 0) + uids + bytes((0x1e // 2, 0x01)) + struct.pack('<II', size, addr)
                  + bytes((0xff, len(name))) + nm)
    struct.pack_into('<H', e, 0, len(e))
    return bytes(e) + b'\0' * (-len(e) % 4)


def image(magic, base, entries, size):
    """A ROFS (base 0) or ROFx (base MAX) whose file entries follow the 0x30-byte header."""
    ents = b''.join(entry(*e) for e in entries)
    h = bytearray(0x30)
    h[0:4] = magic
    struct.pack_into('<IIII', h, 0x08, base + 0x30, 0, base + 0x30, len(ents))
    struct.pack_into('<I', h, 0x2c, MAX)
    img = bytearray(bytes(h) + ents)
    return bytes(img + b'\0' * (size - len(img)))


def core(files):
    return image(b'ROFS', 0, files, 0x4000)


def rofx(files):
    return image(b'ROFx', MAX, files, 0x1000)


def addrs(img):
    return {(n, s): a for _, n, s, a, _ in N.rofs_entries(img, 0)}


class Entries(unittest.TestCase):
    def test_parses_core_and_rofx_offsets(self):
        c = core([('a.dll', 100, 0x1000), ('b.txt', 7, 0x2000)])
        self.assertEqual(addrs(c), {('a.dll', 100): 0x1000, ('b.txt', 7): 0x2000})
        x = rofx([('b.txt', 7, 0x2000), ('new.rsc', 9, MAX + 0x800)])
        self.assertEqual(addrs(x), {('b.txt', 7): 0x2000, ('new.rsc', 9): MAX + 0x800})


class RofxRebase(unittest.TestCase):
    def test_rofx_dll_preload_list_12_bytes_short_is_moved_to_core_file(self):
        # the 6680 case: the ROFx points 12 bytes before the core's "Dll Preload List"
        c = core([('Dll Preload List', 0x19c, 0x34a0)])
        x, moved = N.rebase_rofx(c, rofx([('Dll Preload List', 0x19c, 0x3494)]))
        self.assertEqual(moved, 1)
        self.assertEqual(addrs(x)[('Dll Preload List', 0x19c)], 0x34a0)

    def test_matching_rofx_is_left_unchanged(self):
        c = core([('a.dll', 100, 0x1000), ('b.txt', 7, 0x2000)])
        x0 = rofx([('a.dll', 100, 0x1000), ('b.txt', 7, 0x2000)])
        x, moved = N.rebase_rofx(c, x0)
        self.assertEqual((x, moved), (x0, 0))

    def test_rofx_own_data_and_empty_files_untouched(self):
        c = core([('a.dll', 100, 0x1000)])
        x, moved = N.rebase_rofx(c, rofx([('a.dll', 100, MAX + 0x40), ('empty', 0, 0x0ff0)]))
        self.assertEqual(moved, 0)
        self.assertEqual(addrs(x), {('a.dll', 100): MAX + 0x40, ('empty', 0): 0x0ff0})

    def test_same_name_picks_by_size(self):
        # pres.smil: four files of one name, told apart by size
        c = core([('pres.smil', 2392, 0x1000), ('pres.smil', 2096, 0x1958)])
        x, moved = N.rebase_rofx(c, rofx([('pres.smil', 2392, 0x0ff8), ('pres.smil', 2096, 0x1950)]))
        self.assertEqual(moved, 2)
        self.assertEqual(addrs(x), {('pres.smil', 2392): 0x1000, ('pres.smil', 2096): 0x1958})

    def test_same_key_picks_nearest_following_core_file(self):
        c = core([('x.rsc', 8, 0x1000), ('x.rsc', 8, 0x2000)])
        x, _ = N.rebase_rofx(c, rofx([('x.rsc', 8, 0x1ff4)]))
        self.assertEqual(addrs(x)[('x.rsc', 8)], 0x2000)

    def test_uids_must_match(self):
        c = core([('a.dll', 100, 0x1000, bytes(16))])
        with self.assertRaises(ValueError):
            N.rebase_rofx(c, rofx([('a.dll', 100, 0x0ffc)]))

    def test_no_core_file_ahead_is_an_error(self):
        c = core([('a.dll', 100, 0x1000)])
        with self.assertRaises(ValueError):
            N.rebase_rofx(c, rofx([('a.dll', 100, 0x1004)]))          # core file behind, not ahead
        far = core([('a.dll', 100, 0x3000)])
        with self.assertRaises(ValueError):
            N.rebase_rofx(far, rofx([('a.dll', 100, 0x3000 - N.REBASE_WINDOW - 4)]))   # beyond the window

    def test_directory_outside_the_image_lists_nothing(self):
        # eka1/test_rm1_nand.py's filler images: a 'ROFS' magic with no real header
        junk = bytearray(b'ROFS' + bytes(range(256)) * 4)
        self.assertEqual(N.rofs_entries(bytes(junk), 0), [])
        self.assertEqual(N.rebase_rofx(bytes(junk), b'ROFx' + b'\x55' * 64)[1], 0)

    def test_not_a_rofs_pair_is_left_alone(self):
        self.assertEqual(N.rebase_rofx(b'\0' * 0x100, b'ROFx' + b'\0' * 0x100)[1], 0)

    def test_layout_places_the_rebased_rofx(self):
        c = core([('Dll Preload List', 0x19c, 0x34a0)])
        parts, _ = N.layout(c, rofx([('Dll Preload List', 0x19c, 0x3494)]))
        at, data = [p for p in parts if p[0] == N.ROFS_START + MAX][0]
        self.assertEqual(addrs(data)[('Dll Preload List', 0x19c)], 0x34a0)


@unittest.skipUnless((ROOT / 'eka1/rm36/output/rom/rofs.img').exists()
                     and (ROOT / 'eka1/rm36/fiasco/v27/_pre@01060000.bin').exists(), 'no 6680 firmware')
class Nokia6680(unittest.TestCase):
    def test_6680_rofx_points_at_core_dll_preload_list(self):
        c = (ROOT / 'eka1/rm36/output/rom/rofs.img').read_bytes()
        x, moved = N.rebase_rofx(c, (ROOT / 'eka1/rm36/fiasco/v27/_pre@01060000.bin').read_bytes())
        self.assertEqual(moved, 1023)
        self.assertEqual(addrs(x)[('Dll Preload List', 0x19c)], 0x3244a0)
        self.assertEqual(c[0x3244a0:0x3244a4], 'Z'.encode('utf-16-le').join([b'\xff\xfe', b'']))


if __name__ == '__main__':
    unittest.main()
