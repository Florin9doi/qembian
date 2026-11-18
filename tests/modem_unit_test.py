#!/usr/bin/env python3
"""Host-only regression tests for the fake modem's packet and SMS helpers (no QEMU, no phone).

- The TCP MSS clamp of syborg_modem.c (data_clamp_mss and its checksum helpers, compiled from the source file as they
  are, with data_mtu replaced by a constant): MSS at an even and an odd option offset, checksum checked against an
  independent full recompute (review 2026-11-12 N4).
- tools/sms_provision.py: TPDU sizes, UDH port addressing and concatenation, payload reassembly, fresh references.
- The modem core's request-dependent ISI replies (the whole syborg_modem_core.c, built as the hosted board builds it,
  driven by tests/modem_core_harness.c with the N95's generated reply table): the realignment to the N95 modem
  firmware of 2026-11-18 (docs/research/n95-isi-encoders.md).

  python3 tests/modem_unit_test.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sms_provision  # noqa: E402

MODEM = os.path.join(ROOT, 'src/qemu-syborg/hw/arm/syborg_modem_core.c')   # the functions live in the core (split 2026-11-12)


def extract(src, name):
    """the text of one top-level C function (static ...name(...) { ... })"""
    m = re.search(r'^static [^\n;]*\b%s\(' % name, src, re.M)
    if not m:
        raise AssertionError('function %s not found' % name)
    i = src.index('{', m.end())
    depth = 0
    for j in range(i, len(src)):
        depth += {'{': 1, '}': -1}.get(src[j], 0)
        if depth == 0:
            return src[m.start():j + 1]
    raise AssertionError('unbalanced %s' % name)


def csum(data):
    if len(data) % 2:
        data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return s


def tcp_ok(pkt):
    ihl = (pkt[0] & 15) * 4
    tcp = pkt[ihl:]
    pseudo = pkt[12:20] + struct.pack('!BBH', 0, 6, len(tcp))
    return csum(pseudo + tcp) == 0xffff


def make_syn(options):
    tcp = bytearray(struct.pack('!HHIIBBHHH', 0x1234, 80, 1, 0, 0, 0x02, 0xffff, 0, 0)) + options
    while len(tcp) % 4:
        tcp += b'\x01'
    tcp[12] = (len(tcp) // 4) << 4
    ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(tcp), 0, 0x4000, 64, 6, 0,
                               bytes([10, 0, 2, 15]), bytes([54, 172, 45, 38])))
    pkt = ip + tcp
    pseudo = pkt[12:20] + struct.pack('!BBH', 0, 6, len(tcp))
    c = 0xffff - csum(pseudo + bytes(tcp))
    pkt[20 + 16:20 + 18] = struct.pack('!H', c)
    assert tcp_ok(pkt)
    return pkt


class MssClamp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(MODEM) as f:
            src = f.read()
        body = '\n'.join(extract(src, f) for f in ('csum_add', 'csum_fold', 'tcp_checksum', 'data_clamp_mss'))
        body = body.replace('SyborgModemState *s, ', '').replace('data_mtu(s)', '1400')
        prog = '#include <stdint.h>\n#include <stdio.h>\n#include <stdbool.h>\n' + body + r'''
int main(void) {
    uint8_t d[128]; int n = 0, c;
    while (n < 128 && (c = getchar()) != EOF) d[n++] = c;
    data_clamp_mss(d, n);
    fwrite(d, 1, n, stdout);
    return 0;
}'''
        cls.tmp = tempfile.mkdtemp()
        cfile, cls.exe = os.path.join(cls.tmp, 'mss.c'), os.path.join(cls.tmp, 'mss')
        with open(cfile, 'w') as f:
            f.write(prog)
        subprocess.run(['gcc', '-O1', '-Wall', '-o', cls.exe, cfile], check=True)

    def run_clamp(self, pkt):
        return subprocess.run([self.exe], input=bytes(pkt), capture_output=True, check=True).stdout

    def mss_of(self, pkt):
        tcp = pkt[20:]
        o = 20
        while o < (tcp[12] >> 4) * 4:
            if tcp[o] == 1:
                o += 1
                continue
            if tcp[o] == 2:
                return struct.unpack('!H', tcp[o + 2:o + 4])[0]
            o += tcp[o + 1]
        return None

    def test_even_offset(self):
        out = self.run_clamp(make_syn(b'\x02\x04\x05\xb4'))
        self.assertEqual(self.mss_of(out), 1360)
        self.assertTrue(tcp_ok(out))

    def test_odd_offset(self):
        out = self.run_clamp(make_syn(b'\x01\x02\x04\x05\xb4'))      # NOP, MSS: the value at an odd offset
        self.assertEqual(self.mss_of(out), 1360)
        self.assertTrue(tcp_ok(out))

    def test_small_mss_untouched(self):
        pkt = make_syn(b'\x02\x04\x02\x00')
        self.assertEqual(self.run_clamp(pkt), bytes(pkt))

    def test_not_syn_untouched(self):
        pkt = make_syn(b'\x02\x04\x05\xb4')
        pkt[20 + 13] = 0x10                                            # ACK only (checksum now wrong: untouched)
        self.assertEqual(self.run_clamp(pkt), bytes(pkt))


class SmsProvision(unittest.TestCase):
    def parse(self, tpdu):
        self.assertLessEqual(len(tpdu), 164)
        self.assertEqual(tpdu[0] & 0x43, 0x40)                        # SMS-DELIVER with UDHI
        oalen = tpdu[1]
        o = 3 + (oalen + 1) // 2
        o += 2 + 7                                                     # PID, DCS, SCTS
        udl = tpdu[o]
        ud = tpdu[o + 1:]
        self.assertEqual(len(ud), udl)
        self.assertLessEqual(udl, 140)
        udhl = ud[0]
        udh, payload = ud[1:1 + udhl], ud[1 + udhl:]
        ies, i = {}, 0
        while i < len(udh):
            ies[udh[i]] = udh[i + 2:i + 2 + udh[i + 1]]
            i += 2 + udh[i + 1]
        self.assertEqual(ies[0x05][:2], b'\x0b\x84')                   # destination port 2948
        return ies.get(0x00), payload

    def message(self, pin):
        body = sms_provision.wsp_push(sms_provision.provisioning_doc('internet', 'Internet'), pin)
        return body, sms_provision.sms_deliver_tpdus(body, '12345')

    def test_reassembly(self):
        for pin in (None, '1234'):
            body, tpdus = self.message(pin)
            parts = [self.parse(t) for t in tpdus]
            if len(tpdus) > 1:
                refs = {c[0] for c, _ in parts}
                self.assertEqual(len(refs), 1)
                self.assertEqual([c[2] for c, _ in parts], list(range(1, len(tpdus) + 1)))
            self.assertEqual(b''.join(p for _, p in parts), body)

    def test_reference(self):
        body = bytes(300)
        a = [self.parse(t)[0][0] for t in sms_provision.sms_deliver_tpdus(body, '1', ref=7)]
        self.assertEqual(set(a), {7})                                  # one reference for all parts of a message
        seen = {self.parse(sms_provision.sms_deliver_tpdus(body, '1')[0])[0][0] for _ in range(20)}
        self.assertGreater(len(seen), 1)                               # a fresh one per message by default

    def test_rejects(self):
        with self.assertRaises(ValueError):
            sms_provision.sms_deliver_tpdus(b'x', '12a')
        with self.assertRaises(ValueError):
            sms_provision.sms_deliver_tpdus(bytes(130 * 256), '1')
        with self.assertRaises(ValueError):
            sms_provision.istr('a\x00b')


HARNESS = os.path.join(ROOT, 'tests', 'modem_core_harness.c')
N95 = 'nokia-n95-8gb-rm320'


class ModemCoreReplies(unittest.TestCase):
    """2026-11-18: the canned and core-built ISI replies differed from what the N95 8GB's own modem firmware encodes
    (docs/research/n95-isi-encoders.md 10, 11.5). Each test pins one realigned reply, as the core sends it with the
    N95's layered table (platform/isi/replies-bb5.txt + models/nokia-n95-8gb-rm320/isi-replies.txt)."""

    @classmethod
    def setUpClass(cls):
        import isi
        import device as devmodel
        cls.tmp = tempfile.mkdtemp()
        cls.exe = os.path.join(cls.tmp, 'core')
        core = os.path.join(ROOT, 'src/qemu-syborg/hw/arm')
        subprocess.run(['gcc', '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-DSYBMODEM_HOSTED',
                        '-I', core, '-I', os.path.join(ROOT, 'src/hosted/include/board'), '-o', cls.exe, HARNESS,
                        os.path.join(core, 'syborg_ccpu.c')], check=True)
        cls.tab = os.path.join(cls.tmp, 'n95.tab')
        with open(cls.tab, 'w') as f:
            f.write(isi.table_text(devmodel.load(N95)))

    def ask(self, *reqs, config='', wall=-1):
        """[(res, id, tid, data bytes)] for every frame the core delivers"""
        out = subprocess.run([self.exe, config, self.tab, str(wall)] + list(reqs), capture_output=True, text=True,
                             check=True).stdout
        frames = []
        for ln in out.splitlines():
            b = [int(x, 16) for x in ln.split()]
            frames.append((b[0], b[1], b[2], b[3:]))
        return frames

    def one(self, req, **kw):
        f = self.ask(req, **kw)
        self.assertEqual(len(f), 1, f)
        return f[0]

    # 1. MTC_RF_STATUS_QUERY 15 16 -> 79 {x, rf}: rf = 1 in NORMAL (section 2)
    def test_mtc_rf_status_on_in_normal(self):
        self.assertEqual(self.one('15,16')[1:], (0x79, 0x42, [0x00, 0x01]))

    def test_mtc_rf_status_off_when_rf_inactive(self):
        f = self.ask('15,01,0A,00', '15,16')
        self.assertEqual(f[-1][3], [0x00, 0x00])                 # RF_INACTIVE (offline): rf 0

    # 2. 1B 2E -> 2F: the real layout (sb F000, len8 at +2) cut to the 16-byte sub-block the phone reads (18 data
    # bytes); the modem's own 138 bytes now get through our ISC stand-in too (KErrNoMemory + needed length, iscrx.h),
    # but no table reply exceeds SAServer's 128-byte buffer (2026-11-18 regression; n95-isi-encoders.md 6, 12)
    def test_product_profile_bits_fits_saserver_buffer(self):
        res, mid, _, d = self.one('1B,2E,00')
        self.assertEqual(mid, 0x2F)
        self.assertEqual(d, [0x00, 0x01, 0xF0, 0x00, 0x10, 0x00] + [0] * 12)
        self.assertLessEqual(10 + len(d), 128)

    # 3. EM 17 00 / 17 0A: one sub-block per set mask bit, in bit order (section 5)
    def test_em_mask_03_gives_two_subblocks(self):
        self.assertEqual(self.one('17,00,03,00')[3], [0x01, 0x02, 0x00, 0x04, 0x02, 0x00, 0x01, 0x04, 0x00, 0x00])

    def test_em_battery_mask_02_gives_bars_only(self):
        self.assertEqual(self.one('17,0A,02,00')[3], [0x01, 0x01, 0x16, 0x04, 0x07, 0x07])

    def test_em_charger_before_bars(self):
        self.assertEqual(self.one('17,0A,03,00')[3], [0x01, 0x02, 0x15, 0x04, 0x01, 0x00, 0x16, 0x04, 0x07, 0x07])

    def test_em_all_bits_default_blocks(self):
        d = self.one('17,0A,1F,00')[3]
        self.assertEqual(d[:2], [0x01, 0x05])
        self.assertEqual([d[2 + 4 * i] for i in range(5)], [0x15, 0x16, 0x17, 0x18, 0x25])
        self.assertEqual(d[-4:], [0x25, 0x04, 0xFF, 0xFF])

    def test_em_mask_zero_or_unknown_bit_fails(self):
        self.assertEqual(self.one('17,0A,00,00')[3], [0x02, 0x00])
        self.assertEqual(self.one('17,0A,20,00')[3], [0x02, 0x00])
        self.assertEqual(self.one('17,08,04,00')[3], [0x02, 0x00])

    def test_em_08_unchanged(self):
        self.assertEqual(self.one('17,08,01,00')[3], [0x01, 0x01, 0x13, 0x04, 0x01, 0x00])

    def test_em_without_table_line_unanswered(self):
        out = subprocess.run([self.exe, '', '-', '-1', '17,0A,03,00'], capture_output=True, text=True, check=True)
        self.assertEqual(out.stdout, '')

    # 4. SEC 08 11 -> 12: 6 bytes {05, 16, flag, 00 00 00} (section 3)
    def test_sec_state_resp_is_6_bytes(self):
        self.assertEqual(self.one('08,11,00,00')[1:], (0x12, 0x42, [0x05, 0x16, 0x00, 0x00, 0x00, 0x00]))

    # 5. NET_RAT 0A 36 -> 37: sub-block 2C is 8 bytes (section 4.1, 11.5 item 7)
    def test_rat_subblock_2c_is_8_bytes(self):
        gsm = [0x00, 0x01, 0x2C, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00]
        self.assertEqual(self.one('0A,36,00')[3], gsm)                                       # no SIM: table
        self.assertEqual(self.one('0A,36,00', config='sim.present=1;sim.rat=1')[3], gsm)     # SIM: core
        self.assertEqual(self.one('0A,36,00', config='sim.present=1;sim.rat=2')[3],
                         [0x00, 0x01, 0x2C, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00])

    def test_rat_on_pn_modem_network_unchanged(self):
        # the N8's PN_MODEM_NETWORK path (another modem, TSY-verified) keeps its 4-byte block
        self.assertEqual(self.one('C8,36,00', config='sim.present=1;sim.rat=1')[3], [0x00, 0x01, 0x2C, 0x04, 0x01, 0x00])

    # 6. NET_MODEM_REG_STATUS 0A 00 -> 01 (+ IND 02): sub-block 00 is 8 bytes, then sub-block 09 (24 bytes)
    def test_reg_status_common_subblock_is_8_bytes_no_sim(self):
        d = self.one('0A,00,00')[3]
        self.assertEqual(d[:10], [0x00, 0x02, 0x00, 0x08, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00])
        self.assertEqual(d[10:12], [0x09, 0x18])
        self.assertEqual(len(d), 34)

    def test_reg_status_common_subblock_is_8_bytes_with_sim(self):
        f = self.ask('0A,00,00', config='sim.present=1;sim.mcc=001;sim.mnc=01;sim.lac=258;sim.ci=1')
        self.assertEqual([x[1] for x in f], [0x01, 0x02])                # RESP, then the IND
        for _, _, _, d in f:
            self.assertEqual(d[:10], [0x00, 0x02, 0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00])
            sb = d[10:]
            self.assertEqual(sb[:4], [0x09, 0x18, 0x01, 0x02])           # LAC 0x0102 at +2
            self.assertEqual(sb[8:11], [0x00, 0xF1, 0x10])               # PLMN 001/01 at +8
            self.assertEqual(sb[0x0d:0x10], [1, 1, 1])                   # packet capability
            self.assertEqual(len(sb), 0x18)

    def test_reg_status_on_pn_modem_network_unchanged(self):
        d = self.one('C8,00,00', config='sim.present=1')[3]
        self.assertEqual(d[:6], [0x00, 0x02, 0x00, 0x04, 0x00, 0x02])
        self.assertEqual(len(d), 30)

    # 7. 0A 40: F0 not supported (section 4.1, 11.5 item 9)
    def test_net_0a40_not_supported_on_n95(self):
        self.assertEqual(self.one('0A,40,00')[1:], (0xF0, 0x42, [0x01, 0x40, 0x00, 0x00, 0x00, 0x00]))

    # 8. SIM read 09 19 -> 1A: no SIM {2F FB}; SIM {2F 24 00 00 00 00} (section 9.1)
    def test_sim_read_no_sim_is_fb(self):
        self.assertEqual(self.one('09,19,2F,00')[1:], (0x1A, 0x42, [0x2F, 0xFB]))

    def test_sim_read_with_sim_is_6_bytes(self):
        self.assertEqual(self.one('09,19,2F,00', config='sim.present=1')[3], [0x2F, 0x24, 0x00, 0x00, 0x00, 0x00])

    # 9. SIM_STATUS 09 C0 -> C1 (section 9.1)
    def test_sim_status_no_sim_is_6_bytes(self):
        self.assertEqual(self.one('09,C0,00,00')[3], [0x00, 0x06, 0x00, 0xFF, 0x00, 0x00])

    def test_sim_status_other_service_stays_2_bytes(self):
        self.assertEqual(self.one('09,C0,01,00', config='modem.simst=01:01')[3], [0x01, 0x01])
        self.assertEqual(self.one('09,C0,01,00')[3], [0x01, 0x06])

    def test_sim_status_ready_sim(self):
        self.assertEqual(self.one('09,C0,00,00', config='sim.present=1')[3], [0x00, 0x01, 0x01, 0x00, 0x00, 0x00])

    # 10. RTC 19 0A -> 0B: date/time sub-block 01 before sub-block 04 (section 11.1)
    def test_rtc_time_reply_has_datetime_subblock(self):
        d = self.one('19,0A,00,00', wall=951868747)[3]                   # 2000-02-29 23:59:07 UTC
        self.assertEqual(d[:2], [0x01, 0x02])
        self.assertEqual(d[2:6], [0x01, 0x0C, 0x01, 0x00])
        self.assertEqual(d[6:13], [0x07, 0xD0, 2, 29, 23, 59, 7])
        self.assertEqual(d[14:], [0x04, 0x04, 0x01, 0x00])
        self.assertEqual(len(d), 18)

    def test_rtc_time_dates(self):
        import datetime
        for t in (0, 1795003200, 4102444799, 1709251199):       # 1970, 2026-11-18, 2099-12-31 23:59:59, 2024-02-29
            with self.subTest(t=t):
                u = datetime.datetime.fromtimestamp(t, datetime.timezone.utc)
                d = self.one('19,0A,00,00', wall=t)[3]
                self.assertEqual(d[6:13], [u.year >> 8, u.year & 0xFF, u.month, u.day, u.hour, u.minute, u.second])

    def test_rtc_time_without_host_clock_is_the_table_date(self):
        d = self.one('19,0A,00,00', wall=-1)[3]
        self.assertEqual(d[6:13], [0x07, 0xEA, 11, 18, 12, 0, 0])


class GeneratedN95ReplyTable(unittest.TestCase):
    """the table QEMU and the hosted board load (tools/device.py qemu-args writes <build dir>/gen/isi-replies.tab)
    carries the realigned bytes (2026-11-18)"""

    def test_generated_tab_has_realigned_replies(self):
        env = dict(os.environ, SYBORG_DEVICE=N95)
        env.pop('SYBORG_INSTANCE', None)
        args = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'qemu-args'], env=env,
                              capture_output=True, text=True, check=True).stdout.split()
        tab = [a.split('=', 1)[1] for a in args if a.startswith('syborg-modem.replies=')][0]
        lines = {}
        with open(tab) as fh:
            text = fh.read()
        for ln in text.splitlines():
            f = ln.partition('#')[0].split()
            if f:
                lines[(f[0], f[1])] = ' '.join(f[2:])
        self.assertEqual(lines[('0A', '36')], '37 0A 00 01 2C 08 01 01 00 00 00 00')
        self.assertTrue(lines[('0A', '00')].startswith('01 22 00 02 00 08 06 02 00 00 00 00 09 18'))
        self.assertEqual(lines[('0A', '40')], 'F0 06 01 40 00 00 00 00')
        self.assertEqual(lines[('09', '19')], '1A 02 2F FB')
        self.assertEqual(lines[('1B', '2E')], '2F 12 00 01 F0 00 10 00')   # 18 bytes: sb F000 len 16, within SAServer's buffer (2026-11-18 regression)
        self.assertTrue(lines[('19', '0A')].startswith('0B 12 01 02 01 0C 01 00'))
        self.assertTrue(lines[('19', '0A')].endswith('04 04 01 00'))
        self.assertEqual(lines[('17', '0A')], '0B 0A 01 02 15 04 01 00 16 04 07 07')
        self.assertNotIn(('15', '16'), lines)            # answered by the core from the MTC state


if __name__ == '__main__':
    unittest.main()
