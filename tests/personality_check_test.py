#!/usr/bin/env python3
"""bug 2026-11-17: tools/personality/check.py died "cannot access local variable 'tomllib'" for every personality without
a personality.toml (a function-local import shadowed the module one). Runs it on the N95 profile (sym92)."""
import os, subprocess, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class PersonalityCheck(unittest.TestCase):
    def test_runs_without_traceback(self):
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/personality/check.py'), '--device',
                            'nokia-n95-8gb-rm320'], capture_output=True, text=True, timeout=600)
        self.assertNotIn('Traceback', r.stdout + r.stderr)
        self.assertNotIn("local variable 'tomllib'", r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
