#!/usr/bin/env python3
"""Static checks of the table stub framework (src/standins/_shared/tablestub.h) and its SE P1i users.

Bug 2026-11-17: the framework kept no pending requests, so DoCancel completed nothing; the P1i start-up
animation player (DoCancel(0..4) by request number on MPEG4DECDRV) waited forever in CActive::Cancel and APS
never saw it exit. And DSP_CONTROL_DRV controls 0/1 returned no module buffer, so mmmgmtserver read the DSP
codec module into address 0 (-38) and the window server failed to initialise.

  python3 tests/tablestub_test.py
"""
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H = open(os.path.join(ROOT, 'src/standins/_shared/tablestub.h')).read()


def src(p):
    return open(os.path.join(ROOT, 'src/standins', p)).read()


class TableStub(unittest.TestCase):
    def test_vendor_neutral_name(self):
        self.assertFalse(os.path.exists(os.path.join(ROOT, 'src/standins/_shared/nokiastub.h')))
        for d in os.listdir(os.path.join(ROOT, 'src/standins')):
            for f in os.listdir(os.path.join(ROOT, 'src/standins', d)):
                if f.endswith(('.cpp', '.h', '.mmp', '.inl')):
                    self.assertNotRegex(src(os.path.join(d, f)), r'nokiastub|NokiaStub|NOKIA_STUB', f)

    def test_pending_requests_kept_and_cancelled(self):
        self.assertIn('TRequestStatus* iPending[KStubMaxPending]', H)
        self.assertRegex(H, r'iPending\[~aFn\] = s;')
        self.assertRegex(H, r'Kern::RequestComplete\(iPending\[i\], KErrCancel\)')
        self.assertRegex(H, r'iCancelByNumber \? \(TUint\)i == v : \(v >> i\) & 1')

    def test_mpeg4dec_cancels_by_number(self):
        self.assertIn('TABLE_STUB_LDD_HOOKS_CANCELNUM("MPEG4DECDRV"', src('mpeg4dec/mpeg4dec.cpp'))

    def test_dspcontrol_hands_out_module_memory(self):
        s = src('dspcontrolldd/dspcontrolldd.cpp')
        self.assertRegex(s, r'aFn == 0 \|\| aFn == 1')
        self.assertIn('Kern::ChunkUserBase', s)
        self.assertIn('Kern::ThreadRawWrite(aClient, a1, &user', s)

    def test_ext_sound_play_buffer(self):
        """bug 2026-11-17: EXT_SOUND_DRV control 3 wrote no play buffer; ZebraMMFSwCodecWrapper built a CMMFPtrBuffer
        over address 0 and SecurityApp died KERN-EXEC 3 (memmove in ZebraMMFDevSoundPlugin) before its No SIM screen"""
        s = src('extsoundldd/extsoundldd.cpp')
        self.assertIn('aFn == 3 && a1', s)
        self.assertIn('Kern::ChunkUserBase', s)
        self.assertRegex(s, r'out\[2\] = \{ \(TUint32\)user, BufferSize \}')
        self.assertIn('epocallowdlldata', src('extsoundldd/extsoundldd.mmp'))


if __name__ == '__main__':
    unittest.main()
