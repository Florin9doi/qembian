#!/usr/bin/env python3
"""bug 2026-11-17 (Nokia 6720 classic): its Hal.dll declares 2 screens (EDisplayNumberOfScreens); the window server
built a second (TV-out) screen with 0 display modes and died KERN-EXEC 3 picking a mode in it, the phone stayed
black. models/nokia-6720-rm424/code-patches.txt makes it 1 screen (H-6720-one-screen). Checks the patch resolves in
this firmware, replaces a 2 with a 1, and that the E52 (same platform, boots) says 1 (skipped without firmware)."""
import json, os, sys, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
import patches  # noqa: E402

FW = os.path.join(ROOT, 'build', 'firmware', 'RM-424', '032.001', '02.01+C08.01')
E52 = os.path.join(ROOT, 'build', 'firmware', 'RM-469', '022.009', '05.01+C00.01')


def hal_word(fw, off):
    j = json.load(open(os.path.join(fw, 'core.json')))
    img = open(os.path.join(fw, 'core-flat.img'), 'rb').read()
    for e in j['files']:
        if e['path'].lower().endswith('/hal.dll'):
            a = e['image']['code'] + off - j['base']
            return img[a:a + 4].hex()


@unittest.skipUnless(os.path.exists(os.path.join(FW, 'core.json')), '6720 firmware not extracted')
class OneScreen(unittest.TestCase):
    def test_patch_turns_two_screens_into_one(self):
        lines = [l for l in open(os.path.join(ROOT, 'models', 'nokia-6720-rm424', 'code-patches.txt')).read().splitlines()
                 if 'H-6720-one-screen' in l and not l.startswith('#')]
        self.assertEqual(len(lines), 1)
        mods = patches.modules(os.path.join(FW, 'core.json'))
        addr, orig, new = patches.resolve_lines(lines, mods)[0].split()[:3]
        self.assertEqual((orig, new), ('02000000', '01000000'))
        self.assertEqual(hal_word(FW, 0xdec), orig)

    @unittest.skipUnless(os.path.exists(os.path.join(E52, 'core.json')), 'E52 firmware not extracted')
    def test_e52_says_one(self):
        self.assertEqual(hal_word(E52, 0xdec), '01000000')


if __name__ == '__main__':
    unittest.main()
