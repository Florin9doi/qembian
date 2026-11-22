#!/usr/bin/env python3
"""isiprobe.exe (src/n95apps/isiprobe): its IscApi.dll import library (iscapi.def) must name the RIscApi
functions at the ordinals the N95's IscApi.dll really exports them (n95-isc-isi-protocol.md section 2).
A wrong ordinal would call another function on the real phone. Needs the N95 symbol index (tools/syb_sym.py)."""
import os, re, subprocess, sys, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF = os.path.join(ROOT, 'src', 'n95apps', 'isiprobe', 'iscapi.def')
# export address of each function in the N95's IscApi.dll [verified: n95-isc-isi-protocol.md, ctor 0x8084bb58]
EXPECTED = {'IscCtor': 0x8084bb58, 'IscOpen': 0x8084bd78, 'IscSend': 0x8084bede, 'IscReceive': 0x8084bfda,
            'IscClose': 0x8084bb94}


def def_ordinals():
    out = {}
    for line in open(DEF):
        m = re.match(r'\s*(\w+)\s*@\s*(\d+)', line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


class IscApiDef(unittest.TestCase):
    def test_def_is_complete(self):
        o = def_ordinals()
        self.assertEqual(sorted(o.values()), list(range(1, 33)))     # IscApi.dll has 32 exports

    @unittest.skipUnless(os.path.exists(os.path.join(ROOT, 'build', 'roms', 'nokia-n95-8gb-rm320')), 'no N95 ROM')
    def test_ordinals_match_the_rom(self):
        o = def_ordinals()
        for name, addr in EXPECTED.items():
            r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'syb_sym.py'), 'sym',
                                f'IscApi_ord{o[name]}$'], capture_output=True, text=True)
            self.assertIn(f'{addr:08x}', r.stdout, f'{name} @ {o[name]}: {r.stdout.strip()}')


if __name__ == '__main__':
    unittest.main()
