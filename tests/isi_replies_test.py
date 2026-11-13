#!/usr/bin/env python3
"""Regression tests of the fake modem's reply tables (platform/isi/replies-bb5.txt + device layers).

  python3 tests/isi_replies_test.py
"""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import isi            # noqa: E402
import device as devmodel   # noqa: E402  (tools/device.py)

BB5 = [d for d in devmodel.device_ids() if devmodel.get(devmodel.load(d), 'family', '') == 'bb5-s60']


def reply_of(dev, res, req):
    _, _, replies = isi.load(isi.dbs_of(devmodel.load(dev)))
    for r, q, p, n, data, _ in replies:
        if (r, q) == (res, req):
            return p, n, data
    return None


class SlowUnlockEmergencyCheck(unittest.TestCase):
    """Bug 2026-11-12: unlocking the N95's keypad (LSK, then RSK) took 10-20 s. While the keypad is
    locked, every key goes through the TSY's emergency number check (PN_CALL 0x01,
    CALL_EMERGENCY_NBR_CHECK_REQ 0x28 on ISC channel 7); the fake modem never answered and the TSY
    waited ~10 s per key for its timeout."""

    def test_bb5_family_found(self):
        self.assertIn('nokia-n95-8gb-rm320', BB5)

    def test_emergency_check_answered_not_emergency(self):
        for dev in BB5:
            with self.subTest(dev=dev):
                rep = reply_of(dev, 0x01, 0x28)
                self.assertIsNotNone(rep, 'CALL_EMERGENCY_NBR_CHECK_REQ unanswered')
                p, n, data = rep
                self.assertEqual(p, 0x29)                     # CALL_EMERGENCY_NBR_CHECK_RESP
                self.assertGreaterEqual(n, 2)                 # status + a pad byte
                self.assertEqual(data[0], 0x00)               # not an emergency number: LSK/RSK must not dial


class SettingsPhoneHangNfcSecureElement(unittest.TestCase):
    """Bug 2026-11-13: Settings -> Phone on the 808 hung for good. gsapp starts NfcServer and waits for its
    rendezvous; nfcserver never finished starting because the TSY's UICC power-control request
    PN_UICC 0x8C msg 0x21 {F7} (does the SIM have an NFC secure element) was never answered. The TSY's parser
    (701 nokiatsy 0x80f7c6ac) wants msg 0x22, the service echoed, status 0, nsb >= 1 and sub-block 002A whose
    [sb+7] is 0 or 1; status 0 without that sub-block, or another flag, panics the TSY inside c32exe."""

    def test_uicc_power_control_answered_no_secure_element(self):
        rep = reply_of('nokia-808-rm807', 0x8C, 0x21)
        self.assertIsNotNone(rep, 'UICC 0x21 unanswered: nfcserver never starts, Settings -> Phone hangs')
        p, n, data = rep
        self.assertEqual(p, 0x22)                             # the response the TSY subscribes to (not F0)
        self.assertEqual(data[0], 0xF7)                       # [10] service echoed
        self.assertEqual(data[1], 0x00)                       # [11] status OK
        self.assertGreaterEqual(data[5], 1)                   # [15] number of sub-blocks
        self.assertEqual((data[6] << 8) | data[7], 0x002A)    # sub-block id at [16]
        self.assertEqual((data[8] << 8) | data[9], 8)         # its length
        self.assertIn(data[6 + 7], (0, 1))                    # [sb+7] the flag: anything else panics the TSY
        self.assertEqual(data[6 + 7], 0)                      # no secure element: nfcserver's short path


if __name__ == '__main__':
    unittest.main()
