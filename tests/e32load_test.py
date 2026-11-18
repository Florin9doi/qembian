#!/usr/bin/env python3
"""tools/e32load.py: import-ordinal naming (plain unittest): tests/e32load_test.py

Regression for the 2026-11-15 audit finding: six RSurfaceManager imports of the 808's gcerenderstage.dll
were hand-named from thunk *address order*, which is not ordinal order (the import block lists ordinals
12,14,13,2,3,4). The loader itself was silent about it because it defaulted to the S60 3rd FP1 SDK, which
has no surfacemanager.dso at all - so every one of them stayed `ord<N>` and looked "unnamed but harmless".
"""
import os, re, struct, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import e32load
import dso_ordinals

DEV = 'nokia-808-rm807'
DEV_DIR = os.path.join(ROOT, 'build', 'roms', DEV, 'RM-807_113.010.1508_04.01+C00.01')
E32 = os.path.join(DEV_DIR, 'ghidra', 'gcerenderstage.dll.e32')


def _imports(image):
    """[(dll, ordinal, code offset)] of an E32 image, in import-block order"""
    h, img = e32load.parse(image)
    out = []
    p = h['importoff'] + 4
    for _ in range(h['dllrefcount']):
        noff, n = struct.unpack_from('<Ii', img, p)
        nm = img[h['importoff'] + noff:img.index(b'\0', h['importoff'] + noff)].decode()
        dll = re.sub(r'[\[{][0-9a-fA-F]+[\]}]', '', nm)   # 'gce{000a0000}[1028372b].dll' -> 'gce.dll'

        for j in range(n):
            coff = struct.unpack_from('<I', img, p + 8 + 4 * j)[0]
            w = struct.unpack_from('<I', img, h['codeoff'] + coff)[0]
            out.append((dll, w & 0xffff, coff))
        p += 8 + 4 * n
    return out


class _DeviceEnv:
    """Pin the device and drop the env override, so sdk_lib_default() answers from the profile."""

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


class SdkLibDefaultTest(_DeviceEnv, unittest.TestCase):
    def test_default_is_the_device_sdk_not_s60_fp1(self):
        """The 808 profile names ordinals from the Symbian^3 SDK; S60 3rd FP1 has no surfacemanager.dso."""
        got = e32load.sdk_lib_default()
        self.assertNotIn('S60_3rd_FP1', got, got)
        self.assertTrue(got.endswith(os.path.join('epoc32', 'release', 'armv5', 'lib')), got)

    def test_env_override_still_wins(self):
        os.environ['S60_SDK_LIB'] = '/nonexistent/sdk/lib'
        self.assertEqual(e32load.sdk_lib_default(), '/nonexistent/sdk/lib')


@unittest.skipUnless(os.path.exists(E32), 'needs the extracted 808 gcerenderstage.dll.e32')
class ImportOrdinalTest(_DeviceEnv, unittest.TestCase):
    def setUp(self):
        super().setUp()
        if not os.path.isdir(e32load.sdk_lib_default()):
            self.skipTest('device SDK lib dir not present: ' + e32load.sdk_lib_default())

    def test_surfacemanager_thunks_are_not_in_ordinal_order(self):
        """0x8160 Open(12), 0x8178 ctor(14), 0x8190 Close(13), 0x81A8 OpenSurface(2), 0x81B0 SurfaceInfo(3),
        0x81B8 CloseSurface(4). Sorting the six thunks by address and pairing them with 2,3,4,12,13,14 gives
        five wrong names - the exact mistake an earlier analysis made."""
        got = [(o, c) for dll, o, c in _imports(E32) if os.path.splitext(dll.lower())[0] == 'surfacemanager']
        self.assertEqual(got, [(12, 0x164), (14, 0x17c), (13, 0x194),
                               (2, 0x1ac), (3, 0x1b4), (4, 0x1bc)])

    def test_device_sdk_names_all_six(self):
        names = dso_ordinals.load(os.path.join(e32load.sdk_lib_default(), 'surfacemanager.dso'))
        self.assertEqual([names[o][0] for o in (12, 14, 13, 2, 3, 4)], [
            '_ZN15RSurfaceManager4OpenEv',
            '_ZN15RSurfaceManagerC1Ev',
            '_ZN15RSurfaceManager5CloseEv',
            '_ZN15RSurfaceManager11OpenSurfaceERK10TSurfaceId',
            '_ZN15RSurfaceManager11SurfaceInfoERK10TSurfaceIdR8TPckgBufINS_15TSurfaceInfoV01EE',
            '_ZN15RSurfaceManager12CloseSurfaceERK10TSurfaceId'])

    def test_every_unnamed_ordinal_is_reported(self):
        """gce.dll ord 1 and wsgraphicdrawer ords 15/50/83-100 have no .dso: the loader must say so."""
        h, img = e32load.parse(E32)
        unnamed = {}
        sdk_dir = e32load.sdk_lib_default()
        sdk = {n.lower(): n for n in os.listdir(sdk_dir)} if os.path.isdir(sdk_dir) else {}
        for dll, ordn, _ in _imports(E32):
            k = os.path.splitext(dll.lower())[0] + '.dso'
            names = dso_ordinals.load(os.path.join(sdk_dir, sdk[k])) if k in sdk else {}
            if ordn not in names:
                unnamed.setdefault(dll.lower(), set()).add(ordn)
        self.assertEqual(sorted(unnamed['gce.dll']), [1])
        self.assertIn(83, unnamed['wsgraphicdrawer.dll'])
        self.assertIn(100, unnamed['wsgraphicdrawer.dll'])


if __name__ == '__main__':
    unittest.main()
