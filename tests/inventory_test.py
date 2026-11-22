#!/usr/bin/env python3
"""tools/fw/inventory.py: the kernel-drivers.txt tags (which firmware .ldd/.pdd is a stand-in, our EPL build, a
declared gap, or an undeclared omission). 2026-11-19: ecomm/elocd/pipelib were listed as missing although our
builds of them are in the ROM; after the 808 profile dropped them from missing_drivers they showed as undeclared."""
import os, sys, tempfile, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
import inventory as I


class DriverTags(unittest.TestCase):
    def test_tags(self):
        have, missing = {'custom', 'd_display'}, {'bc02.ldd', 'medsd.pdd'}
        ours = {'elocd.ldd', 'ekern.exe'}
        self.assertEqual(I.driver_tag('custom.ldd', have, missing, ours), 'stand-in')
        self.assertEqual(I.driver_tag('elocd.ldd', have, missing, ours), 'ours (EPL build)')
        self.assertEqual(I.driver_tag('BC02.LDD', have, missing, ours), 'missing (declared)')
        self.assertEqual(I.driver_tag('NfcDrv.LDD', have, missing, ours), '')       # undeclared: visible as blank

    def test_manifest_ours_rows(self):
        d = tempfile.mkdtemp()
        p = os.path.join(d, 'hybrid-manifest.txt')
        open(p, 'w').write('804b2b80      6448 81 N95  Sys/Bin/EDISP.DLL\n'
                           '8203f4d0    104348 81 OURS sys/bin/ELOCD.LDD\n'
                           'garbage line\n')
        self.assertEqual(I.manifest_ours(p), {'elocd.ldd'})
        self.assertEqual(I.manifest_ours(os.path.join(d, 'none.txt')), set())


if __name__ == '__main__':
    unittest.main()
