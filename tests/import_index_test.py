#!/usr/bin/env python3
"""tools/import_index.py: the firmware-wide dll->ordinal->name / module->thunk index.

2026-11-15: written after an audit caught six RSurfaceManager imports named from thunk *address order*
(the import block lists ordinals 12,14,13,2,3,4) and a loader that silently used the wrong platform SDK.
These tests pin both properties, plus the rule that an ordinal nobody can name stays visible.
"""
import json, os, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import e32load
import import_index as I

DEV = 'nokia-808-rm807'
DEV_DIR = os.path.join(ROOT, 'build', 'roms', DEV, 'RM-807_113.010.1508_04.01+C00.01')
E32 = os.path.join(DEV_DIR, 'ghidra', 'gcerenderstage.dll.e32')
HAVE_INDEX = os.path.exists(os.path.join(DEV_DIR, 'imports.json'))

SM_NAMES = {
    12: '_ZN15RSurfaceManager4OpenEv',
    14: '_ZN15RSurfaceManagerC1Ev',
    13: '_ZN15RSurfaceManager5CloseEv',
    2: '_ZN15RSurfaceManager11OpenSurfaceERK10TSurfaceId',
    3: '_ZN15RSurfaceManager11SurfaceInfoERK10TSurfaceIdR8TPckgBufINS_15TSurfaceInfoV01EE',
    4: '_ZN15RSurfaceManager12CloseSurfaceERK10TSurfaceId',
}


class _Env:
    """pin the device, drop the SDK env override"""

    def setUp(self):
        self.env = os.environ.get('S60_SDK_LIB')
        self.dev = os.environ.get('SYBORG_DEVICE')
        os.environ.pop('S60_SDK_LIB', None)
        os.environ['SYBORG_DEVICE'] = DEV

    def tearDown(self):
        os.environ.pop('SYBORG_DEVICE', None)
        if self.dev is not None:
            os.environ['SYBORG_DEVICE'] = self.dev
        if self.env is not None:
            os.environ['S60_SDK_LIB'] = self.env


class LayoutTest(_Env, unittest.TestCase):
    def test_index_lives_in_the_device_build_dir(self):
        self.assertEqual(os.path.dirname(I.index_path()), DEV_DIR)


@unittest.skipUnless(os.path.exists(E32), 'needs the extracted 808 gcerenderstage.dll.e32')
class E32ImportsTest(_Env, unittest.TestCase):
    def test_ordinals_are_not_in_thunk_address_order(self):
        """0x160 Open(12), 0x178 ctor(14), 0x190 Close(13), 0x1A8 OpenSurface(2), 0x1B0 SurfaceInfo(3),
        0x1B8 CloseSurface(4). Pairing sorted thunks with 2,3,4,12,13,14 names five of them wrongly."""
        h, imps = I.e32_imports(open(E32, 'rb').read())
        sm = [(o, off) for dll, o, off, kind in imps
              if os.path.splitext(dll.lower())[0] == 'surfacemanager']
        self.assertEqual(sm, [(12, 0x160), (14, 0x178), (13, 0x190),
                              (2, 0x1a8), (3, 0x1b0), (4, 0x1b8)])
        self.assertTrue(all(kind == 'code' for dll, o, off, kind in imps if 'surfacemanager' in dll))

    def test_names_come_from_the_device_sdk(self):
        names = I.Names(e32load.sdk_lib_default())
        self.assertEqual({o: names.name('surfacemanager.dll', o) for o in SM_NAMES}, SM_NAMES)

    def test_unnamed_ordinals_are_reported_not_invented(self):
        """gce ord 1 and the wsgraphicdrawer interface ords have no .dso; they must stay None."""
        names = I.Names(e32load.sdk_lib_default())
        self.assertIsNone(names.name('gce.dll', 1))
        self.assertIsNone(names.name('wsgraphicdrawer.dll', 84))
        self.assertIsNotNone(names.name('euser.dll', 654))          # User::AllocZL


class CoverageTest(unittest.TestCase):
    """bug 2026-11-17 (registry revalidation): the index, and so the registry's binary list, missed 52 modules
    of the 808: XIP images without import thunks (ekern.exe itself, Palette.dll, the charset tables) and ROFS
    images whose extension was not in a fixed list (.drv, .pxy, .pxt, .agt, Elocl.NN)"""

    def test_e32_by_header_not_extension(self):
        hdr = bytearray(0x40)
        hdr[16:20] = b'EPOC'
        self.assertTrue(I.is_e32(bytes(hdr)))
        self.assertFalse(I.is_e32(b'\0' * 0x40))
        self.assertFalse(I.is_e32(b'EPOC'))                   # too short to be a header

    def test_shadowed_layers(self):
        """the same path in XIP and ROFS2 (808: sqlsrv.exe, sqldb.dll, ...): the higher layer is effective"""
        mods = {'Sys/Bin/sqlsrv.exe': {'origin': 'xip@0x80001000'},
                'sys/bin/sqlsrv.exe': {'origin': 'rofs2@0x400'},
                'sys/bin/only.dll': {'origin': 'rofs1@0x10', 'shadowed_by': 'stale'}}
        I.mark_shadowed(mods)
        self.assertEqual(mods['Sys/Bin/sqlsrv.exe'].get('shadowed_by'), 'rofs2@0x400')
        self.assertNotIn('shadowed_by', mods['sys/bin/sqlsrv.exe'])
        self.assertNotIn('shadowed_by', mods['sys/bin/only.dll'])   # a stale mark is cleared


@unittest.skipUnless(HAVE_INDEX, 'needs a built index: tools/import_index.py build')
class BuiltIndexTest(_Env, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.idx = I.load()
        if 'sys/bin/gcerenderstage.dll' not in self.idx['modules']:
            self.skipTest('index has no ROFS scope yet: run build without --scope xip')

    def test_rofs_module_imports(self):
        m = self.idx['modules']['sys/bin/gcerenderstage.dll']
        self.assertEqual([o for d, o, off, kind in m['imports'] if 'surfacemanager' in d],
                         [12, 14, 13, 2, 3, 4])

    def test_names_are_attached_to_the_ordinals(self):
        sm = self.idx['dlls']['surfacemanager.dll']
        self.assertEqual({int(k): v for k, v in sm.items() if int(k) in SM_NAMES}, SM_NAMES)
        self.assertIsNone(self.idx['dlls']['wsgraphicdrawer.dll']['84'])   # unnamed, but present

    def test_xip_modules_are_indexed(self):
        m = self.idx['modules']['Sys/Bin/WsGraphicDrawer.dll']
        self.assertTrue(any(os.path.splitext(d.lower())[0] == 'ecom' for d, o, off, kind in m['imports']))

    def test_modules_without_imports_and_odd_extensions(self):
        low = {k.lower() for k in self.idx['modules']}
        for p in ('sys/bin/ekern.exe', 'sys/bin/palette.dll', 'sys/bin/etherpkt.drv', 'sys/bin/elocl.01',
                  'sys/bin/vpnconnagt.agt'):
            self.assertIn(p, low)

    def test_xip_and_rofs_offsets_name_the_ldr(self):
        """bug 2026-11-17: XIP thunk offsets were 4 bytes before the ldr pc,[pc,#-4] (ROFS ones were right)"""
        import struct
        core = open(os.path.join(DEV_DIR, 'core-flat.img'), 'rb').read()
        base = json.load(open(os.path.join(DEV_DIR, 'core.json')))['base']
        m = self.idx['modules']['Sys/Bin/WsGraphicDrawer.dll']
        code = int(m['base'], 16)
        for dll, o, off, kind in m['imports'][:20]:
            self.assertEqual(struct.unpack_from('<I', core, code - base + off)[0], I.LDR_PC, (dll, o, hex(off)))
        r = self.idx['modules']['sys/bin/gcerenderstage.dll']
        self.assertEqual([off for d, o, off, k in r['imports'] if 'surfacemanager' in d][0], 0x160)

    def test_device_option_labels_and_places_the_index(self):
        """bug 2026-11-17: build --device X used $SYBORG_DEVICE's profile for the directory"""
        os.environ['SYBORG_DEVICE'] = 'nokia-n95-8gb-rm320'
        self.assertEqual(os.path.dirname(I.index_path(DEV)), DEV_DIR)
        self.assertEqual(self.idx['device'], DEV)

    def test_every_import_has_a_slot_in_the_dll_table(self):
        for path, m in self.idx['modules'].items():
            for dll, o, off, kind in m.get('imports', []):
                k = os.path.splitext(dll.lower())[0] + '.dll'
                self.assertIn(k, self.idx['dlls'], (path, dll))
                self.assertIn(str(o), self.idx['dlls'][k], (path, dll, o))


if __name__ == '__main__':
    unittest.main()
