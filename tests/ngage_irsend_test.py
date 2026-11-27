"""eka1/tools/ngage_irsend.py: the IrDA peer script, driven against a scripted fake phone (no QEMU).

Run: python3 tests/ngage_irsend_test.py
"""
import struct
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'eka1/tools'))
import ngage_irsend as I  # noqa: E402


class FakeLink:
    """Hands the script the phone's frames, one batch per read; records what the script writes."""

    def __init__(self, batches=()):
        self.batches, self.sent = list(batches), []

    def write(self, data):
        self.sent += I.SirReader().feed(data)

    def read(self, timeout):
        if self.batches:
            return b''.join(I.sir_wrap(f, 0) for f in self.batches.pop(0))
        time.sleep(min(timeout, 0.01))
        return b''


def connected_lap(link, ca=0x5e):
    lap = I.IrLap(link, ftimer=0.05, retries=1)
    lap.daddr, lap.ca = b'PHON', ca
    lap._apply_qos(I.qos_params(bytes.fromhex('01012082010183013f840101850180860104080110')))  # the 6600's UA
    lap._link_up(True)
    return lap


class SendToPhoneFreeze(unittest.TestCase):
    """send_to_phone_freezes_6600: from the capture of script -> 6600. The script advertised the Computer hint
    (84 20); the phone, Infrared on, took it for a PC Suite host and its mRouter probed us during the push, then
    dropped the link without answering OBEX DISCONNECT, sent SNRMs that the script ignored, and froze."""

    def test_discovery_hints_have_no_computer_bit(self):
        info = I.discovery_info(b'qembian')
        self.assertFalse(info[0] & 0x04, 'Computer hint bit set')
        self.assertTrue(info[0] & 0x80 and info[1] & 0x20, 'OBEX hint missing')
        self.assertEqual(info[2:], b'\x00qembian')

    def test_hints_can_be_overridden(self):
        self.assertEqual(I.discovery_info(b'x', bytes([0x92, 0x25])), b'\x92\x25\x00x')

    def test_xid_answer_in_listen_uses_the_hints(self):
        peer, ours = b'\x49\x5f\xad\x60', None
        xid = bytes([0xFF, I.XID_CMD | I.PF, 0x01]) + peer + b'\xff' * 4 + bytes([0x00, 0x00, 0x00])
        link = FakeLink([[xid]])
        lap = I.IrLap(link)
        lap.listen(time.monotonic() + 0.1)
        rsp = [f for f in link.sent if f[1] == I.XID_RSP | I.PF]
        self.assertEqual(len(rsp), 1)
        self.assertEqual(rsp[0][14:16], I.DISCOVERY_HINTS)

    def test_snrm_from_phone_while_primary_is_refused_with_dm(self):
        link = FakeLink()
        lap = connected_lap(link)
        snrm = bytes([0xFF, I.SNRM | I.PF]) + b'PHON' + lap.saddr + bytes([0x34]) + bytes.fromhex('01013e')
        link.batches = [[snrm]]
        with self.assertRaises(I.LinkLost) as cm:
            lap.exchange([])
        self.assertIn('SNRM', str(cm.exception))
        self.assertFalse(lap.connected)
        self.assertEqual(link.sent[-1], bytes([0xFE, I.DM | I.PF]))

    def test_snrm_for_another_station_is_ignored(self):
        link = FakeLink()
        lap = connected_lap(link)
        snrm = bytes([0xFF, I.SNRM | I.PF]) + b'PHON' + b'ELSE' + bytes([0x34])
        rr = bytes([lap.ca << 1, I.PF | I.RR])
        link.batches = [[snrm, rr]]
        self.assertEqual(lap.exchange([]), [])
        self.assertTrue(lap.connected)
        self.assertNotIn(bytes([0xFE, I.DM | I.PF]), link.sent)

    def test_no_frames_sent_after_the_link_dropped(self):
        link = FakeLink()
        lap = connected_lap(link)
        lap.connected = False
        with self.assertRaises(I.LinkLost):
            lap.exchange([b'\x09\x11\x01data'])
        self.assertEqual(link.sent, [])


class Framing(unittest.TestCase):
    def test_sir_round_trip_with_escapes(self):
        f = bytes([0x5e << 1 | 1, 0x10, I.BOF, I.EOF, I.CE, 0x00])
        self.assertEqual(I.SirReader().feed(I.sir_wrap(f, 3)), [f])

    def test_bad_fcs_dropped(self):
        w = bytearray(I.sir_wrap(b'\xbc\x11abc', 0))
        w[3] ^= 1
        self.assertEqual(I.SirReader().feed(bytes(w)), [])


if __name__ == '__main__':
    unittest.main()
