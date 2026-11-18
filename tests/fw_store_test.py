#!/usr/bin/env python3
"""tools/fw/store.py import (plain unittest).

Bug 2026-11-17 (Nokia E6-00 rm-609.rar): one archive held two firmware versions sharing package files (a data block,
card-reference XMLs, a type label); the first record moved them out of the expanded archive and the second died with
FileNotFoundError. Only the last record may move shared files."""
import os, shutil, sys, tempfile, unittest, zipfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
import store


class TwoFirmwaresOneArchive(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.old = store.STORE
        store.STORE = os.path.join(self.tmp, 'roms')
        self.zip = os.path.join(self.tmp, 'rm-609.zip')
        with zipfile.ZipFile(self.zip, 'w') as z:
            for n in ('a.core.fpsx', 'b.core.fpsx', 'shared.pcx'):
                z.writestr('rm-609/' + n, n)

        def fake_identify(dirs):
            def rec(ver, core):
                return {'type_code': 'RM-609', 'sw_version': ver, 'date': '', 'confidence': 'test', 'products': [],
                        'files': [{'name': core, 'role': 'core'}, {'name': 'shared.pcx', 'role': 'other'}]}
            return [rec('1.0', 'a.core.fpsx'), rec('2.0', 'b.core.fpsx')]
        self.orig = store.idmod.identify
        store.idmod.identify = fake_identify
        self.orig_index = store.write_index
        store.write_index = lambda: None

    def tearDown(self):
        store.idmod.identify = self.orig
        store.write_index = self.orig_index
        store.STORE = self.old
        shutil.rmtree(self.tmp)

    def test_shared_file_reaches_both(self):
        done = store.import_paths([self.zip])
        self.assertEqual(len(done), 2)
        for d in done:
            self.assertTrue(os.path.exists(os.path.join(d, 'shared.pcx')), d)
        self.assertTrue(os.path.exists(self.zip))       # the archive itself is never touched


if __name__ == '__main__':
    unittest.main()
