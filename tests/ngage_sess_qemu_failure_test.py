"""ngage_sess: a QEMU that exits at start-up is reported with QEMU's own message.

Regression (qemu_exit_hidden): `ngage_phone.py --model p910i --mmc CARD` failed with the daemon's
"'Daemon' object has no attribute 'rsp'" traceback after a 10 s wait; the real cause, in qemu.out, was
"Property 'p910i-machine.mmc' not found" (the P910i board has no card slot).
Run: python3 -m unittest -v tests/ngage_sess_qemu_failure_test.py
"""
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('ngage_sess', REPO / 'eka1/tools/ngage_sess.py')
sys.path.insert(0, str(REPO / 'eka1/tools'))
ns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ns)


class QemuFailure(unittest.TestCase):
    def test_qemu_exit_hidden(self):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            (d / 'qemu.out').write_text("qemu-system-arm: info: p910i: H-p910-doc: stand-in\n"
                                        "qemu-system-arm: Property 'p910i-machine.mmc' not found\n")
            msg = ns.qemu_failure(d)
        self.assertIn("Property 'p910i-machine.mmc' not found", msg)
        self.assertNotIn('info:', msg)

    def test_no_output(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertIn('(no output)', ns.qemu_failure(Path(t)))


if __name__ == '__main__':
    unittest.main()
