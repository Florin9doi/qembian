#!/usr/bin/env python3
"""tools/cmt/isirecord.py: pairing of the real modem's answers with the phone's requests in an n95-cmt access log,
and the comparison with the fake modem (the N95's table mode). Synthetic logs, no QEMU."""
import os, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, 'tools', 'cmt', 'isirecord.py')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'cmt'))
import isirecord  # noqa: E402


def isi(direction, res, tid, mid, robj, sobj, data, queued=False):
    b = [0x1b, 0x00 if direction == 'APE->CMT' else 0x6c, 0x6c if direction == 'APE->CMT' else 0x00, res, 0, 0,
         robj, sobj, tid, mid] + list(data)
    b[4] = len(b) - 6
    return 'ISI %s %sres=%02x tid=%02x id=%02x robj=%02x sobj=%02x len=%d: %s\n' % (
        direction, 'queued ' if queued else '', res, tid, mid, robj, sobj, len(b), ' '.join('%02x' % x for x in b))


class Pairing(unittest.TestCase):
    def log(self, *lines):
        f = tempfile.NamedTemporaryFile('w', suffix='.log', delete=False)
        f.write('CBUS W dev=1 reg=0x17 v=0020 pc=f010ce2d\n' + ''.join(lines))
        f.close()
        return f.name

    def test_answer_paired_by_resource_tid_and_channel(self):
        p = self.log(isi('APE->CMT', 0x15, 0x07, 0x02, 0x00, 0x07, [0, 0], queued=True),   # queued copy: skipped
                     isi('APE->CMT', 0x15, 0x07, 0x02, 0x00, 0x07, [0, 0]),
                     isi('APE->CMT', 0x15, 0x07, 0x02, 0x00, 0x18, [0, 0]),                # same tid, channel 24
                     isi('CMT->APE', 0x15, 0x07, 0x65, 0x18, 0x30, [0x0a, 0x0a]),
                     isi('CMT->APE', 0x15, 0x07, 0x65, 0x07, 0x30, [0x01, 0x01]),
                     isi('CMT->APE', 0x15, 0x00, 0xc0, 0x00, 0x30, [0x0a, 0x04]))          # indication (object 0)
        reqs, inds = isirecord.parse([p])
        self.assertEqual(len(reqs), 2)
        self.assertEqual(reqs[0][3], [(0x65, bytes([1, 1]))])          # channel 7 got its own answer
        self.assertEqual(reqs[1][3], [(0x65, bytes([0x0a, 0x0a]))])
        self.assertEqual(inds, [(0x15, 0xc0, bytes([0x0a, 0x04]))])

    def test_several_answers_kept_in_order(self):
        p = self.log(isi('APE->CMT', 0x77, 0x00, 0x01, 0x00, 0x24, [0, 2]),
                     isi('CMT->APE', 0x77, 0x00, 0x02, 0x24, 0x3b, [0, 0]),
                     isi('CMT->APE', 0x77, 0x00, 0x09, 0x24, 0x3b, [0, 0]))
        reqs, _ = isirecord.parse([p])
        self.assertEqual([m for m, _ in reqs[0][3]], [0x02, 0x09])

    def test_report_classifies_against_the_fake_modem(self):
        p = self.log(isi('APE->CMT', 0x77, 0x00, 0x07, 0x00, 0x24, [0, 0]),        # table: 09 {00 00} = same as 2nd
                     isi('CMT->APE', 0x77, 0x00, 0x09, 0x24, 0x3b, [0, 0]),
                     isi('APE->CMT', 0x01, 0x05, 0xee, 0x00, 0x07, [1, 2]),        # nothing answers 01 EE
                     isi('CMT->APE', 0x01, 0x05, 0xef, 0x07, 0x2f, [1, 0]),
                     isi('APE->CMT', 0x1b, 0x06, 0x00, 0x00, 0x0b, [0x41]),        # serial number: private
                     isi('CMT->APE', 0x1b, 0x06, 0x01, 0x0b, 0x2b, [0, 1, 0x41, 0x14, 0, 3, 0x31, 0x32, 0x33]))
        md = p + '.md'
        r = subprocess.run([sys.executable, TOOL, p, '--device', 'nokia-n95-8gb-rm320', '--md', md, '--emit'],
                           capture_output=True, text=True, check=True)
        text = open(md).read()
        self.assertIn('## same (1)', text)
        self.assertIn('## missing (1)', text)
        self.assertIn('| 01 EE ', text)
        self.assertNotIn('31 32 33', text)                              # the serial number is not written
        self.assertIn('reply 01 EE EF 01 00', r.stdout)                 # emitted: constant answer, table lacks it
        self.assertNotIn('reply 1B 00', r.stdout)


if __name__ == '__main__':
    unittest.main()
