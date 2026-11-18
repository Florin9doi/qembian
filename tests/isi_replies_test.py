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
# the Symbian 9.1 members (N73, E61): their 2006 TSY parses NET_MODEM_REG_STATUS differently
BB5_91 = [d for d in BB5 if str(devmodel.get(devmodel.load(d), 'platform.os_version', '')) == '9.1']
BB5_NEW = [d for d in BB5 if d not in BB5_91]


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


class DeviceManagerInfoReadHang(unittest.TestCase):
    """Bug 2026-11-13: `*#0000#` (the "Device manager" software-version dialog, NSmlDMSync.exe) hung on
    the N8 line with an empty window. The app -> EInfoServer M:2; einfoserver.exe builds PN_PHONE_INFO
    (0x1B) message 0x0B and sends it with IscApi::SendSynchron + User::WaitForRequest and *no timeout*, so
    while the fake modem did not answer, the info server never completed and the dialog stayed blank.
    The reply is the product name ('N' sub-block, request data 00 02; 'M'/manufacturer answers 00 01)."""

    def test_n8_phone_info_name_answered(self):
        rep = reply_of('nokia-n8-rm596', 0x1B, 0x0B)
        self.assertIsNotNone(rep, 'PHONE_INFO 0x0B unanswered: *#0000# hangs (einfoserver has no timeout)')
        p, n, data = rep
        self.assertEqual(p, 0x0C)                             # INFO_…_RESP, the id the parser accepts
        self.assertEqual(data[0], 0x00)                       # cause OK
        self.assertEqual(data[2], 0x4E)                       # sub-block id 'N' = product name
        self.assertEqual(data[3:5], [0x0C, 0x00])             # sub-block length 12 as the parser expects it
        length = data[5]                                      # string length (frame[15]), string from frame[16]
        self.assertGreaterEqual(length, 2)
        name = bytes(data[6:6 + length]).decode('latin-1')
        self.assertEqual(len(name), length)
        self.assertTrue(name.startswith('RM-'), name)         # the type code, e.g. RM-596


class MessagingHangsNetRegStatus(unittest.TestCase):
    """Bug 2026-11-17: Messaging (Mce.exe) never opened on any S60 3rd/5th phone without SIM: its AppUi
    blocks in RConnectionMonitor::ConnectL(), and the Connection Monitor server waited forever for an ETel
    request the TSY turned into PN_NETWORK NET_MODEM_REG_STATUS_GET_REQ (0A 00), which the fake modem
    left unanswered. A reply with only NET_REG_INFO_COMMON panicked NOKIATSY ('IsiMessage OFFS -9'):
    the TSY also reads the NET_GSM_REG_INFO sub-block (0x09) up to +0x16."""

    def test_reg_status_answered_no_sim_with_gsm_block(self):
        for dev in BB5_NEW:
            with self.subTest(dev=dev):
                rep = reply_of(dev, 0x0A, 0x00)
                self.assertIsNotNone(rep, 'NET_MODEM_REG_STATUS_GET_REQ unanswered')
                p, n, data = rep
                self.assertEqual(p, 0x01)
                self.assertEqual(data[0], 0x00)               # cause OK
                self.assertEqual(data[1], 2)                  # two sub-blocks
                self.assertEqual(list(data[2:4]), [0x00, 0x04])
                self.assertEqual(data[4], 0x06)               # NOSERV_NOSIM
                self.assertEqual(data[6], 0x09)               # NET_GSM_REG_INFO
                self.assertGreaterEqual(data[7], 0x18)        # >= 24 bytes: the TSY reads up to +0x16
                self.assertGreaterEqual(n, 6 + data[7])

    def test_93_reg_status_e0_answered_like_0a00(self):
        # the 9.3 TSYs (E52, E72, E5-00) ask with NET_REG_STATUS_GET_REQ 0xE0 instead of 0x00
        for dev in BB5_NEW:
            with self.subTest(dev=dev):
                rep = reply_of(dev, 0x0A, 0xE0)
                self.assertIsNotNone(rep, 'NET_REG_STATUS_GET_REQ (E0) unanswered')
                self.assertEqual(rep[0], 0xE1)
                self.assertEqual(bytes(rep[2]), bytes(reply_of(dev, 0x0A, 0x00)[2]))



class EtelPanicsOnRegStatus91(unittest.TestCase):
    """Bug 2026-11-18: after the NET_MODEM_REG_STATUS reply went into the BB5 family table (2026-11-17), the
    N73 and the E61 lost ETel: their 9.1 NOKIATSY panicked CTelServer USER 21 in ISIMSG on that reply (its
    sub-block layout is the S60 3.1+ one). The reply lives in each newer member's own table; the 9.1 phones
    leave the request unanswered, as before."""

    def test_91_members_found(self):
        self.assertIn('nokia-n73-rm132', BB5_91)
        self.assertIn('nokia-e61-rm89', BB5_91)

    def test_not_in_family_table(self):
        _, _, replies = isi.load([os.path.join(ROOT, 'platform', 'isi', 'replies-bb5.txt')])
        self.assertFalse([r for r in replies if (r[0], r[1]) == (0x0A, 0x00)])

    def test_91_members_leave_it_unanswered(self):
        for dev in BB5_91:
            with self.subTest(dev=dev):
                self.assertIsNone(reply_of(dev, 0x0A, 0x00))


class E61StartupStalls(unittest.TestCase):
    """E61 bring-up 2026-11-18, two start-up stalls in the E61's own table (models/nokia-e61-rm89/isi-replies.txt).
    1. res 0x54 msg 0x0D answered with the shared status 01: npaprivlistener.exe gave up and exited, Starter took
       that as a dead critical process and went to state 252. The E61 answers status 00.
    2. WlmServerExe asked PN_PHONE_INFO msg 0x23 (the WLAN MAC address) and never got an answer: the WLAN server
       accepted no connection, ConnMon waited on it, start-up stayed at 211 (the Nokia logo for ever)."""

    def test_npaprivlistener_status_ok(self):
        p, n, data = reply_of('nokia-e61-rm89', 0x54, 0x0D)
        self.assertEqual(p, 0x0E)
        self.assertEqual(data[0], 0x00)

    def test_wlan_mac_answered(self):
        rep = reply_of('nokia-e61-rm89', 0x1B, 0x23)
        self.assertIsNotNone(rep, 'WLAN MAC read (PN_PHONE_INFO 0x23) unanswered')
        p, n, data = rep
        self.assertEqual(p, 0x24)
        self.assertEqual(data[0], 0x00)          # byte 10 of the message: status, must be 0
        self.assertGreaterEqual(len(data), 7)    # bytes 11..16: the MAC, reversed
        self.assertNotEqual(bytes(data[1:7]), bytes(6))



class LintCrashedOnSonyEricsson(unittest.TestCase):
    """Bug 2026-11-18: `tools/isi.py lint` died on the Sony Ericsson profiles (no ISI modem, no reply table).
    Devices with another modem kind are skipped."""

    def test_lint_runs_over_every_device(self):
        import contextlib
        import io
        self.assertTrue([d for d in devmodel.device_ids()
                         if devmodel.get(devmodel.load(d), 'modem.kind') != 'isi-fake-peer'])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(isi.cmd_lint(), 0)


if __name__ == '__main__':
    unittest.main()
