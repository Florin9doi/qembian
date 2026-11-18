#!/usr/bin/env python3
"""tools/ecom_index.py: the ECom plugin index (SPI files -> plugin DLL, interface, implementation).

2026-11-17: written for the registry's ecom-plugin edges (docs/registry/README.md listed them as missing). The
decoder pieces are tested on hand-made bytes; the 808 checks run when its firmware is extracted."""
import os, struct, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import ecom_index as E

DEV = 'nokia-808-rm807'
DEV_DIR = os.path.join(ROOT, 'build', 'roms', DEV, 'RM-807_113.010.1508_04.01+C00.01')


def u16s(s):
    return [ord(c) for c in s]


class Scsu(unittest.TestCase):
    def test_ascii_is_itself(self):
        self.assertEqual(E.scsu_decode(b'BT GPS PSY'), u16s('BT GPS PSY'))

    def test_dynamic_window_and_quote(self):
        # UTS #6 example 2 shape: SD0 window 0x0400 (index 0x08), then Cyrillic bytes 0x80+
        self.assertEqual(E.scsu_decode(bytes([0x18, 0x08, 0x9F, 0xB0])), [0x041F, 0x0430])
        # SQU quotes one UTF-16 unit; SCU enters Unicode mode, UC0 leaves it
        self.assertEqual(E.scsu_decode(bytes([0x0E, 0x20, 0xAC, 0x41])), [0x20AC, 0x41])
        self.assertEqual(E.scsu_decode(bytes([0x0F, 0x4E, 0x2D, 0xE0, 0x41])), [0x4E2D, 0x41 - 0x80 + 0x80])

    def test_window_offsets(self):
        self.assertEqual(E._window_offset(0x08), 0x0400)
        self.assertEqual(E._window_offset(0xF9), 0x00C0)
        with self.assertRaises(ValueError):
            E._window_offset(0x00)


def rsc(resource_runs, compressed=True):
    """a uid-0x101F4A6B resource file with one resource"""
    hdr = struct.pack('<4I', 0x101F4A6B, 0, 0, 0) + bytes([0]) + struct.pack('<H', 200)
    body = bytes([1 if compressed else 0])           # the bit array: resource 0 holds compressed Unicode
    start = len(hdr) + len(body)
    data = hdr + body + resource_runs
    index = struct.pack('<2H', start, len(data))
    return data + index[:2] + struct.pack('<H', len(data))


class ResourceAndRegistryInfo(unittest.TestCase):
    def test_v1_registry_info_through_runs(self):
        """v1 (no format UID): dll, 1 interface, 1 implementation with display name 'Ab' (compressed run),
        default 'x', opaque ''. The run layout and the 0xab alignment byte follow BaRscImpl.cpp."""
        plain1 = struct.pack('<IHIHIB', 0x101FE99A, 1, 0x101F7A7C, 1, 0x101FE999, 60) + bytes([2])
        runs = bytes([0, len(plain1)]) + plain1 + bytes([2]) + b'Ab' + bytes([3]) + bytes([1]) + b'x' + bytes([0])
        res = E.rsc_resources(rsc(runs))
        info = E.registry_info(res[0])
        self.assertEqual(info['format'], 1)
        self.assertEqual(info['dll_uid'], '0x101fe99a')
        im = info['interfaces'][0]['implementations'][0]
        self.assertEqual((info['interfaces'][0]['uid'], im['uid'], im['version'], im['name'], im['default_data']),
                         ('0x101f7a7c', '0x101fe999', 60, 'Ab', 'x'))

    def test_v2_has_rom_only(self):
        body = struct.pack('<IIHIHIB', E.V2, 0x1028373B, 1, 0x10281D4A, 1, 0x1028373C, 1) + bytes([0, 0, 0, 1])
        info = E.registry_info(E.rsc_resources(rsc(bytes([0, len(body)]) + body))[0])
        self.assertEqual((info['format'], info['dll_uid']), (2, '0x1028373b'))
        self.assertTrue(info['interfaces'][0]['implementations'][0]['rom_only'])

    def test_other_resource_formats_are_refused(self):
        with self.assertRaises(ValueError):
            E.rsc_resources(struct.pack('<4I', 0x101F5010, 0, 0, 0) + bytes(10))


@unittest.skipUnless(os.path.exists(os.path.join(DEV_DIR, 'ecom.json')), 'needs ecom_index.py build (808)')
class Built808(unittest.TestCase):
    def test_gce_render_stage_and_no_errors(self):
        idx = E.load(DEV)
        self.assertEqual(idx['errors'], [])
        impls = {(n, iu, im['uid']) for n, iu, im in E.implementations(idx)}
        self.assertIn(('gcerenderstage', '0x10285a29', '0x20031cba'), impls)   # docs/research/gcerenderstage-808.md
        self.assertGreater(len(idx['plugins']), 1000)


if __name__ == '__main__':
    unittest.main()
