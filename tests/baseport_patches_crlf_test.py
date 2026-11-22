#!/usr/bin/env python3
"""Regression test: qemu-baseport patches 0019-0028 never applied to a clean checkout (2026-11-11 .. 11-22).

The Syborg baseport sources are CR LF. syborg_fb.cpp and syborg_mednvmemory.cpp in the long-lived dev
epocroot had been rewritten with LF, so patches diffed from them came out LF (0019, 0021, 0022, 0024,
0027, 0028) and 0020 became a whole-file rewrite that turned the file LF. A fresh
scripts/setup-epocroot.sh stopped at 0019 ("different line endings"); the same had happened to 0003/0013
before (d48ffba). 0020's context also still said "syborg-pg", which the rename had changed in 0006.
tools/patch_eol.py now lints and fixes the patches and the epocroot from setup-epocroot.sh.

  python3 tests/baseport_patches_crlf_test.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import patch_eol as pe  # noqa: E402

HAVE_PATCH = shutil.which('patch') is not None
HAVE_CHECKOUTS = all(os.path.isdir(os.path.join(ps.checkout, '.git')) for ps in pe.patch_sets() if ps.patches)

CR = b'\r\n'
ORIG = b''.join(b'line %d\r\n' % i for i in range(1, 41))              # a CR LF source, 40 lines


def lf_patch_one_change():
    """What a diff from an LF copy of ORIG looks like: line 20 changed, LF everywhere."""
    return (b'--- a/src/a.cpp\n+++ b/src/a.cpp\n@@ -18,5 +18,5 @@\n'
            b' line 18\n line 19\n-line 20\n+line twenty\n line 21\n line 22\n')


def whole_file_lf_rewrite():
    """The old 0020's shape: every CR LF line removed, the file added back LF with one change."""
    old = [b'-' + l for l in pe.split_lines(ORIG)]
    new = [b'+' + l.rstrip(CR) + b'\n' for l in pe.split_lines(ORIG.replace(b'line 20\r', b'line twenty\r'))]
    return b'--- a/src/a.cpp\n+++ b/src/a.cpp\n@@ -1,40 +1,40 @@\n' + b''.join(old + new)


class Checkout:
    """A throw-away pinned checkout holding src/a.cpp = ORIG, and a patch set over it."""
    def __init__(self, patches):
        self.dir = tempfile.mkdtemp(prefix='patch_eol_test.')
        repo = os.path.join(self.dir, 'repo')
        os.makedirs(os.path.join(repo, 'src'))
        pe.write_bytes(os.path.join(repo, 'src/a.cpp'), ORIG)
        git = lambda *a: subprocess.run(['git', '-C', repo, '-c', 'user.name=t', '-c', 'user.email=t@t',
                                         '-c', 'core.autocrlf=false'] + list(a), check=True,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        git('init', '-q'); git('add', '.'); git('commit', '-q', '-m', 'pinned')
        files = []
        for i, data in enumerate(patches):
            f = os.path.join(self.dir, '%04d-test.patch' % (i + 1))
            pe.write_bytes(f, data)
            files.append(f)
        self.set = pe.PatchSet('pkg', repo, 'HEAD', files)

    def applied(self):
        """src/a.cpp after the set's patches on a clean tree."""
        t = os.path.join(self.dir, 'tree')
        shutil.rmtree(t, ignore_errors=True)
        pe.clean_tree(self.set, t)
        for p in self.set.patches:
            ok, out = pe.run_patch(t, pe.read_bytes(p))
            assert ok, out
        return pe.read_bytes(os.path.join(t, 'src/a.cpp'))

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


class RepoPatches(unittest.TestCase):
    def test_every_patch_has_its_targets_line_endings(self):
        if not HAVE_CHECKOUTS:
            self.skipTest('pinned checkouts missing (scripts/fetch.sh)')
        found = [str(f) for ps in pe.patch_sets() for p in ps.patches for f in pe.lint_patch(p, pe.styles_for(ps))]
        self.assertEqual(found, [])

    @unittest.skipUnless(HAVE_PATCH, 'no GNU patch')
    def test_every_patch_set_applies_to_clean_pinned_sources(self):
        if not HAVE_CHECKOUTS:
            self.skipTest('pinned checkouts missing (scripts/fetch.sh)')
        self.assertEqual([os.path.basename(p) + ': ' + out for p, out in pe.apply_all(pe.patch_sets())], [])

    def test_qemu_baseport_patch_set_is_found(self):
        sets = {ps.pkg: ps for ps in pe.patch_sets()}
        names = [os.path.basename(p) for p in sets['sf/adapt/qemu'].patches]
        self.assertIn('0019-fb-secure-buffer-own-address.patch', names)
        self.assertEqual(names, sorted(names))
        self.assertTrue(any('personalities' in p for p in sets['sf/os/kernelhwsrv'].patches))

    def test_normalise_keeps_committed_patches_byte_for_byte(self):
        # a multi-file patch's "diff -ru" line between files was once dropped by the parser
        if not HAVE_CHECKOUTS:
            self.skipTest('pinned checkouts missing (scripts/fetch.sh)')
        for ps in pe.patch_sets():
            st = pe.styles_for(ps)
            for p in ps.patches:
                data = pe.read_bytes(p)
                self.assertEqual(pe.normalise(data, st), data, p)

    def test_no_old_project_name_in_patches(self):
        # 0020's context said "syborg-pg" after 0006 had been renamed: it no longer applied
        for ps in pe.patch_sets():
            for p in ps.patches:
                self.assertNotIn(b'syborg-pg', pe.read_bytes(p), p)

    def test_setup_runs_the_lint_and_the_epocroot_check(self):
        s = pe.read_text(pe.SETUP)
        self.assertIn('patch_eol.py" lint --fix', s)
        self.assertIn('patch_eol.py" epocroot --fix', s)
        self.assertLess(s.index('lint --fix'), s.index('in-container.sh" /work/scripts/setup-epocroot.sh'))


@unittest.skipUnless(HAVE_PATCH, 'no GNU patch')
class Synthetic(unittest.TestCase):
    def test_lf_patch_on_crlf_source_is_flagged_and_fixed(self):
        c = Checkout([lf_patch_one_change()])
        try:
            p = c.set.patches[0]
            f = pe.lint_patch(p, pe.styles_for(c.set))
            self.assertEqual([(x.path, x.bad, x.eol_only) for x in f], [('src/a.cpp', 6, 0)])
            self.assertFalse(pe.apply_all([c.set]) == [], 'GNU patch should refuse the LF patch')
            changed, fails = pe.fix_set(c.set)
            self.assertEqual((changed, fails), ([p], []))
            self.assertEqual(pe.lint_patch(p, pe.styles_for(c.set)), [])
            self.assertEqual(c.applied(), ORIG.replace(b'line 20\r', b'line twenty\r'))
        finally:
            c.close()

    def test_whole_file_line_ending_rewrite_is_rediffed_to_the_real_change(self):
        c = Checkout([whole_file_lf_rewrite()])
        try:
            p = c.set.patches[0]
            f = pe.lint_patch(p, pe.styles_for(c.set))
            self.assertEqual(len(f), 1)
            self.assertEqual(f[0].eol_only, 39)
            changed, fails = pe.fix_set(c.set)
            self.assertEqual(fails, [])
            fixed = pe.read_bytes(p)
            self.assertEqual(fixed.count(b'\n@@'), 1)
            self.assertIn(b'@@ -17,7 +17,7 @@', fixed)
            self.assertIn(b'-line 20\r\n+line twenty\r\n', fixed)
            self.assertEqual(c.applied(), ORIG.replace(b'line 20\r', b'line twenty\r'))
        finally:
            c.close()

    def test_fix_leaves_a_correct_patch_alone(self):
        good = lf_patch_one_change().replace(b'\n', CR).replace(b'a.cpp\r\n', b'a.cpp\n').replace(b'@@\r\n', b'@@\n')
        c = Checkout([good])
        try:
            self.assertEqual(pe.lint_patch(c.set.patches[0], pe.styles_for(c.set)), [])
            self.assertEqual(pe.fix_set(c.set), ([], []))
        finally:
            c.close()

    def test_epocroot_file_rewritten_with_lf_is_found_and_converted(self):
        c = Checkout([lf_patch_one_change()])
        try:
            e = os.path.join(c.dir, 'epocroot')
            f = os.path.join(e, 'pkg', 'src', 'a.cpp')
            os.makedirs(os.path.dirname(f))
            pe.write_bytes(f, ORIG.replace(CR, b'\n'))
            self.assertEqual(pe.epocroot_drift(c.set, e), [('src/a.cpp', CR)])
            pe.epocroot_drift(c.set, e, fix=True)
            self.assertEqual(pe.read_bytes(f), ORIG)
            self.assertEqual(pe.epocroot_drift(c.set, e), [])
        finally:
            c.close()


if __name__ == '__main__':
    unittest.main()
