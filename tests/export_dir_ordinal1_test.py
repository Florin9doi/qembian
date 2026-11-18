#!/usr/bin/env python3
"""Regression test: N81 8GB / E71 (personality sym92l91) never booted (2026-11-10 .. 11-17).

Their loader (9.1 F32 layout) allocates iExportDirCount words for E32Loader::ReadExportDir and reads
buf[ordinal - 1]; the multiple memory model wrote count + 1 words from ordinal 0: a loader-heap overrun per
RAM-loaded DLL (USER 45, then a fatal kernel fault in the loader) and every import bound one ordinal off.
Checks the kernel patch keeps both pieces: the personality switches the convention on, and the multiple
model's ReadExportDir honours it (the moving model already did, for sym91).

  python3 tests/export_dir_ordinal1_test.py
"""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(ROOT, 'personalities/sym92l91/patches/0027-sym92l91-export-dir-from-ordinal1.patch')
KERNEL = os.path.join(ROOT, 'build/epocroot/sf/os/kernelhwsrv/kernel/eka')


class ExportDirFromOrdinal1(unittest.TestCase):
    def test_patch_sets_flag_for_sym92l91(self):
        p = open(PATCH).read()
        self.assertRegex(p, r'\+\s*ExecExportDirFromOrdinal1 = 1;')
        self.assertIn('ExecLoaderProcessTail = _FOFF(TProcessCreateInfo, iDebugAttributes)', p)   # the sym92l91 branch's context

    def test_patch_multiple_model_skips_ordinal0(self):
        p = open(PATCH).read()
        self.assertIn('memmodel/epoc/multiple/mcodeseg.cpp', p)
        self.assertRegex(p, r'\+.*skip = ExecExportDirFromOrdinal1 \? sizeof\(TLinAddr\) : 0')

    @unittest.skipUnless(os.path.isdir(KERNEL), 'no epocroot (scripts/setup-epocroot.sh)')
    def test_epocroot_has_it(self):
        s = open(os.path.join(KERNEL, 'kernel/sexecpers.cpp')).read()
        block = s[s.index('pers == KPersonalitySym92L91)'):]
        block = block[:block.index('else if')]
        self.assertIn('ExecExportDirFromOrdinal1 = 1', block)
        m = open(os.path.join(KERNEL, 'memmodel/epoc/multiple/mcodeseg.cpp')).read()
        self.assertTrue(re.search(r'ExecExportDirFromOrdinal1', m))


if __name__ == '__main__':
    unittest.main()
