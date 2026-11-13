#!/usr/bin/env python3
"""tools/hosted/profsym.py: reading the hosted profiler's reports from a serial log (phone mode prints one per
Ctrl+P, each with its (pc, lr) pairs) and the RAM code segments."""
import os, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools/hosted'))
import profsym

LOG = """SYBCODE: Z:\\sys\\bin\\AknIcon.dll code=70100000+2000 data=00000000+0 bss=00000000+0 ep=70100000
PROF lr d0000100 d0000200 5
PROF region kernel 10 samples (50%)
PROF region rom 10 samples (50%)
PROF pc d0000100 7
PROF total 20 samples
noise PROF pc 12
PROF lr d0000300 80001001 3
PROF thread dd001000 7
PROF thread 00000000 3
PROF tpc dd001000 80001000 6
THREADMAP dd001000 EwSrv.exe[10003b20]0001::alfredserver
PROF region kernel 4 samples (40%)
PROF region rom 6 samples (60%)
PROF pc 80001000 6
PROF pc d0000300 4
PROF total 10 samples
PROF region kernel 1 samples (100%)
"""


class ProfsymTest(unittest.TestCase):
    def test_two_complete_reports_each_with_its_own_callers(self):
        r = profsym.reports(LOG)
        self.assertEqual(len(r), 2)                 # the third has no total: incomplete, dropped
        self.assertEqual(r[0][2], 20)
        self.assertEqual(r[0][1], [(0xd0000100, 7)])
        self.assertEqual(r[0][3], [(0xd0000100, 0xd0000200, 5)])
        self.assertEqual(r[1][0], {'kernel': 4, 'rom': 6})
        self.assertEqual(r[1][3], [(0xd0000300, 0x80001001, 3)])

    def test_threads_and_their_pcs_with_names(self):
        r = profsym.reports(LOG)[1]
        self.assertEqual(r[4], [(0xdd001000, 7), (0, 3)])
        self.assertEqual(r[5], [(0xdd001000, 0x80001000, 6)])
        self.assertEqual(profsym.threadmap(LOG), {0xdd001000: 'EwSrv.exe[10003b20]0001::alfredserver'})

    def test_ramcode_segments(self):
        self.assertEqual(profsym.ramcode(LOG), [(0x70100000, 0x2000, 'AknIcon.dll')])

    def test_no_report(self):
        self.assertEqual(profsym.reports('PROF region kernel 1 samples\n'), [])


if __name__ == '__main__':
    unittest.main()
