#!/usr/bin/env python3
"""bug 2026-11-17 (SE P1i): c32's BServer died in RHeap::Free on the first CCPU->app call (ICBSimLockStatus): its reply
builder ReAllocs the 16-byte reply buffer and writes through the stale TPtr. models/se-p1i/code-patches.txt makes the
reply length 0 (H-p1i-bserver-callback-reply). This checks the patch lines resolve in this firmware and the bytes they
replace are the reply builder's two `ldr r0,[r5]` (skipped without the extracted firmware)."""
import json, os, sys, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
import patches  # noqa: E402

FW = os.path.join(ROOT, 'build', 'firmware', 'SE-P1i', 'R6E30', 'CDA162024-70')
PATCHES = os.path.join(ROOT, 'models', 'se-p1i', 'code-patches.txt')


@unittest.skipUnless(os.path.exists(os.path.join(FW, 'core.json')), 'SE P1i firmware not extracted')
class BServerCallbackReplyPatch(unittest.TestCase):
    def test_lines_resolve_to_the_reply_builder_loads(self):
        mods = patches.modules(os.path.join(FW, 'core.json'))
        lines = [l for l in open(PATCHES).read().splitlines() if 'H-p1i-bserver-callback-reply' in l and not l.startswith('#')]
        self.assertEqual(len(lines), 2)
        img = open(os.path.join(FW, 'core-flat.img'), 'rb').read()
        base = json.load(open(os.path.join(FW, 'core.json')))['base']
        for ln in patches.resolve_lines(lines, mods):
            addr, orig, new = ln.split()[:3]
            a = int(addr, 16)
            self.assertTrue(0xf81a5e76 < a < 0xf81a5f2c, hex(a))      # inside the reply builder
            self.assertEqual(img[a - base:a - base + 2].hex(), orig)  # ldr r0,[r5]
            self.assertEqual(new, '0020')                            # movs r0,#0


if __name__ == '__main__':
    unittest.main()
