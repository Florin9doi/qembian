#!/usr/bin/env python3
"""Regression test: a ROFS1 file replaced with [[rom.rofs_replace]] must also be what a ROFx extension's directory
entry for it points at (bug 2026-11-17: the SE P1i's Z: lists ROFS1 files through its ROFx directory too, so the
replaced PVR2DIF.dll was still loaded from its old bytes and the window server kept panicking).

  python3 tests/rofs_replace_rofx_test.py      (needs the composed se-p1i build: tools/device.py build se-p1i)
"""
import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as D   # noqa: E402
import rofs          # noqa: E402


class RofxRedirect(unittest.TestCase):
    def test_p1i_pvr2dif_both_directories(self):
        prof = D.load('se-p1i')
        gen = os.path.join(D.build_dir(prof), 'gen')
        r1, r2 = (os.path.join(gen, 'rofs%d-patched.img' % n) for n in (1, 2))
        if not (os.path.exists(r1) and os.path.exists(r2)):
            self.skipTest('se-p1i not composed')
        d1, d2 = open(r1, 'rb').read(), open(r2, 'rb').read()
        e1 = rofs.entry(d1, 'Sys/Bin/PVR2DIF.dll')
        base, _ = rofs.files(d2)
        e2 = rofs.entry(d2, 'Sys/Bin/PVR2DIF.dll', base=base)
        self.assertIsNotNone(e1)
        self.assertIsNotNone(e2)
        size1, addr1 = struct.unpack_from('<II', d1, e1 + 20)
        size2, addr2 = struct.unpack_from('<II', d2, e2 + 20)
        self.assertEqual((size2, addr2), (size1, addr1))
        self.assertLess(size1, 1000)                 # our stub, not the 8008-byte original


if __name__ == '__main__':
    unittest.main()
