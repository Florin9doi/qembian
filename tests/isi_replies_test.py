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
                self.assertEqual(data[2], 0x00)               # NET_REG_INFO_COMMON
                c = data[3]                                   # 4 (as first written) or 8 (the N95 modem's, 2026-11-18)
                self.assertIn(c, (0x04, 0x08))
                self.assertEqual(data[4], 0x06)               # NOSERV_NOSIM
                self.assertEqual(data[2 + c], 0x09)           # NET_GSM_REG_INFO
                self.assertGreaterEqual(data[3 + c], 0x18)    # >= 24 bytes: the TSY reads up to +0x16
                self.assertGreaterEqual(n, 2 + c + data[3 + c])

    def test_93_reg_status_e0_answered_like_0a00(self):
        # the 9.3 TSYs (E52, E72, E5-00) ask with NET_REG_STATUS_GET_REQ 0xE0 instead of 0x00
        for dev in BB5_NEW:
            with self.subTest(dev=dev):
                rep = reply_of(dev, 0x0A, 0xE0)
                self.assertIsNotNone(rep, 'NET_REG_STATUS_GET_REQ (E0) unanswered')
                self.assertEqual(rep[0], 0xE1)
                if str(devmodel.get(devmodel.load(dev), 'platform.os_version', '')) == '9.2':
                    continue    # S60 3.1 (N95 line) never sends E0; its 0A 00 is the N95 modem's 8-byte form (2026-11-18)
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



class N95ModemRealignment(unittest.TestCase):
    """2026-11-18: the canned replies differed from what the N95 8GB's own modem firmware encodes
    (docs/research/n95-isi-encoders.md 10, 11.5). Table side; the core-built replies are in tests/modem_unit_test.py
    (ModemCoreReplies)."""

    N95 = 'nokia-n95-8gb-rm320'

    def test_reg_status_common_subblock_is_8_bytes(self):
        p, n, data = reply_of(self.N95, 0x0A, 0x00)
        self.assertEqual(data[:10], [0x00, 0x02, 0x00, 0x08, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00])
        self.assertEqual(data[10:12], [0x09, 0x18])
        self.assertEqual(n, 34)

    def test_rat_subblock_2c_is_8_bytes(self):
        self.assertEqual(reply_of(self.N95, 0x0A, 0x36), (0x37, 10, [0x00, 0x01, 0x2C, 0x08, 0x01, 0x01, 0, 0, 0, 0]))

    def test_sim_read_no_sim_is_fb(self):
        self.assertEqual(reply_of(self.N95, 0x09, 0x19), (0x1A, 2, [0x2F, 0xFB]))

    def test_rtc_time_reply_has_datetime_subblock(self):
        p, n, data = reply_of(self.N95, 0x19, 0x0A)
        self.assertEqual((p, n), (0x0B, 18))
        self.assertEqual(data[:4], [0x01, 0x02, 0x01, 0x0C])     # OK, 2 sub-blocks, sb 01 of 12 bytes first
        self.assertEqual(data[14:], [0x04, 0x04, 0x01, 0x00])    # then sb 04 (RTC state valid)

    def test_em_lines_in_mask_bit_order(self):
        self.assertEqual(reply_of(self.N95, 0x17, 0x00)[2], [0x01, 0x02, 0x00, 0x04, 0x02, 0x00, 0x01, 0x04, 0x00, 0x00])
        self.assertEqual(reply_of(self.N95, 0x17, 0x0A)[2], [0x01, 0x02, 0x15, 0x04, 0x01, 0x00, 0x16, 0x04, 0x07, 0x07])

    def test_mtc_rf_status_left_to_the_core(self):
        self.assertIsNone(reply_of(self.N95, 0x15, 0x16))

    def test_net_0a40_not_supported_on_n95(self):
        self.assertEqual(reply_of(self.N95, 0x0A, 0x40), (0xF0, 6, [0x01, 0x40, 0x00, 0x00, 0x00, 0x00]))

    def test_net_0a40_kept_41_on_s60_32_and_5th(self):
        # the 2026-11-17 Messaging fix on the 9.3/9.4 phones answered 41; their modems are not the N95's
        for dev in BB5:
            if str(devmodel.get(devmodel.load(dev), 'platform.os_version', '')) not in ('9.3', '9.4'):
                continue
            with self.subTest(dev=dev):
                self.assertEqual(reply_of(dev, 0x0A, 0x40), (0x41, 3, [0x01, 0x00, 0x00]))



class SAServerSpinsOnLongProductProfile(unittest.TestCase):
    """2026-11-18 regression: the realignment made 1B 2E -> 2F the 138 data bytes the N95 modem's encoder sends.
    SAServer receives on ISC channel 35 with a 128-byte buffer (10-byte header + 118 data bytes): our ISC stand-in
    completed the 148-byte message with KErrOverflow (-9), SAServer re-posted the same buffer at once (1.5 million
    times by 156 s) and start-up starved: N95 8GB logo late and no home screen, 5230/5800/E52 black screen.
    Closed: the real NokiaISCDriver answers KErrNoMemory (-4) with the needed length and SAServer grows its buffer
    (tests/iscpeer_overflow_test.py, iscrx.h); the phone's parser (SAFeatureInfoService FUN_8092313e) reads only
    status, nsb, sub-block F000 (ISIMESSAGE type 3: id16, len8 at +2), flags +4 and two words +8/+12, so the table
    carries the real layout cut to that whole 16-byte sub-block (18 data bytes). The 118-byte cap stays: a short
    receive buffer must never be the thing a table line relies on the driver to fix."""

    SASERVER_MAX_DATA = 128 - 10

    def test_product_profile_is_real_layout_cut_to_what_the_phone_reads(self):
        for dev in BB5:
            with self.subTest(dev=dev):
                rep = reply_of(dev, 0x1B, 0x2E)
                if rep is None:
                    continue
                p, n, data = rep
                self.assertEqual((p, n), (0x2F, 18))
                d = data + [0] * (n - len(data))
                self.assertEqual(d[:4], [0x00, 0x01, 0xF0, 0x00])      # status OK, 1 sub-block, id16 F000
                self.assertEqual(d[4], n - 2)                          # len8 = the whole sub-block (old form: 0, length in the filler)
                self.assertEqual(d[5], 0x00)                           # filler (the real modem: 88 00)
                self.assertGreaterEqual(n, 2 + 16)                     # flags +4 and words +8/+12 present

    def test_no_reply_overflows_128_byte_receive_buffer(self):
        for dev in BB5:
            text = isi.table_text(devmodel.load(dev))
            for ln in text.splitlines():
                ln = ln.split('#')[0].split()
                if len(ln) < 4:
                    continue
                with self.subTest(dev=dev, msg=' '.join(ln[:2])):
                    self.assertLessEqual(int(ln[3], 16), self.SASERVER_MAX_DATA)


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


class EmptyOkPacketFrozeHapticsPlugin(unittest.TestCase):
    """Bug (N8, H-haptics-fail; fixed for the 808 on 2026-11-19, task vibra-qemu-device step 1): an "OK with an
    empty packet" reply to the haptics request ISI 1C 9E froze the UI. hapticsadaptation.dll accepts a reply with
    [0x0A] == 0, [0x0B] != 0, [0x11] == 0 and hands the packet at [0x16..] (length u16 BE at [0x14..0x15]) to
    hwrmhapticsvibepacketizerplugin, which switches on the packet's u16 LE opcode and reads the status at
    packet[2]; an empty packet makes it leave -2 and EwSrv's FeedbackPlayerThread waits for good
    (docs/research/haptics-isi-808.md 3). The 808's table now answers success with a decodable packet
    (OpenDevice OK, handle 1, verified 2026-11-19: taps, Calendar open/close, no stall); the N8 and 701 keep
    the family's failure status until verified there."""

    def fields(self, data, n):
        # table data bytes start at ISI offset 0x0A; pad to the declared length as the modem core does
        isi = [0] * 0x0A + list(data) + [0] * (n - len(data))
        return isi

    def test_808_haptics_reply_is_success_with_decodable_packet(self):
        rep = reply_of('nokia-808-rm807', 0x1C, 0x9E)
        self.assertIsNotNone(rep, 'haptics 1C 9E unanswered: every touch feedback waits 3 s')
        p, n, data = rep
        self.assertEqual(p, 0x9F)
        self.assertGreaterEqual(n, len(data))
        isi = self.fields(data, n)
        self.assertEqual(isi[0x0A], 0, 'status must be OK (else the point of the override is lost)')
        self.assertNotEqual(isi[0x0B], 0, 'sub-block count must be non-zero (adaptation returns -2 otherwise)')
        self.assertEqual(isi[0x11], 0)
        plen = (isi[0x14] << 8) | isi[0x15]
        self.assertGreaterEqual(plen, 3, 'the packet needs an opcode and a status: an empty packet froze the plugin')
        self.assertLessEqual(0x16 + plen, len(isi), 'packet length field must not exceed the reply')
        packet = isi[0x16:0x16 + plen]
        opcode = packet[0] | (packet[1] << 8)
        self.assertIn(opcode, (0x04, 0x05, 0x17, 0x1C, 0x1D, 0x12, 0x13, 0x1B, 0x11, 0x09, 0x07, 0x08, 0x10,
                               0x16, 0x18, 0x19, 0x1A, 0x1E, 0x1F, 0x20), 'opcode the decoder handles')
        self.assertEqual(packet[2], 0, 'VibeTonz status 0 = success')
        if opcode in (0x04, 0x05, 0x17, 0x1C, 0x1D, 0x12):
            self.assertEqual(plen, 7, 'these opcodes carry a 4-byte handle after the status')
            self.assertNotEqual(packet[3:7], [0xFF] * 4, 'handle -1 means "closed"')

    def test_808_haptics_reply_sub_block_length_matches(self):
        p, n, data = reply_of('nokia-808-rm807', 0x1C, 0x9E)
        isi = self.fields(data, n)
        total = len(isi)
        self.assertEqual(total % 4, 0, 'ISI messages are 4-byte aligned')
        self.assertEqual((isi[0x0E] << 8) | isi[0x0F], total - 0x0C, 'sub-block length = total - 12')

    def test_n8_and_701_keep_the_failure_status_until_verified(self):
        for dev in ('nokia-n8-rm596', 'nokia-701-rm774'):
            if dev not in BB5:
                continue
            with self.subTest(dev=dev):
                p, n, data = reply_of(dev, 0x1C, 0x9E)
                self.assertEqual(p, 0x9F)
                self.assertEqual(data[0], 1, 'failure status: the adaptation completes -2 at once, no packet decoded')


if __name__ == '__main__':
    unittest.main()
