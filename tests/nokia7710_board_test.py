"""Checks on the Nokia 7710 board (-M nokia7710: src/qemu-ngage/hw/arm/nokia9500.c's ra2_phone_7710,
nokia7710_input.c) that need no emulator run. Each regression test is named after the stall it guards
(docs/devices/nokia-7710-rm12/bring-up.md).

Run: python3 tests/nokia7710_board_test.py
The ROM checks are skipped when the (git-ignored) core ROM is not extracted.
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9500.c'
ISI = REPO / 'src/qemu-ngage/isi-replies-rm12.txt'
PATCH_SYSST = REPO / 'patches/qemu-ngage/0019-omap-sysst-power-on-reset.patch'
ROM = REPO / 'eka1/rm12/core/ROM.bin'
ROM9500 = REPO / 'eka1/ra2/core/ROM.bin'
B = 0x50000000


def word(rom, va):
    return struct.unpack_from('<I', rom, va - B)[0]


def phone(name):
    text = BOARD.read_text()
    return text.split('static const Ra2Phone %s = {' % name, 1)[1].split('\n};\n', 1)[0]


class Isi(unittest.TestCase):
    def test_rm12_checkcrc_6b(self):
        """Stall 1: IsaApi panic 1 "checksum not correct": the 7710's CheckCrc compares data[1] with 0x6B
        (the 9500's table had 0xA7)."""
        line = next(l for l in ISI.read_text().splitlines() if l.startswith('reply 00 01 02'))
        self.assertEqual(line.split()[4:6], ['00', '6b'])

    def test_rm12_indications_to_its_own_objects(self):
        """The 9500's indications went to its objects 0x69/0x52; the 7710's Custom.ldd is 0x65, the DSY 0x51."""
        inds = [l.split()[1:3] for l in ISI.read_text().splitlines() if l.startswith('ind ')]
        self.assertTrue(inds)
        for unit, obj in inds:
            self.assertEqual(int(unit), int(obj, 16))
            self.assertIn(obj.lower(), ('65', '51'))

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_checkcrc_compare_in_rom(self):
        self.assertEqual(word(ROM.read_bytes(), 0x50142F54), 0xE353006B)    # cmp r3, #0x6B


class Storage(unittest.TestCase):
    def test_rm12_c_drive_full_size(self):
        """Stall 2: Wserv's key-click plug-in left with KErrPathNotFound and Wserv jumped to 0: partition 0
        was sized from the record (240 sectors) while its FAT16 boot sector says 174048, so EFile's
        writes past the record failed. The partition must carry the boot sector's size."""
        body = phone('ra2_phone_7710')
        self.assertIn('ra2_parts_7710', body)
        parts = BOARD.read_text().split('static Ra2Part ra2_parts_7710[] = {', 1)[1].split('};', 1)[0]
        first = parts.strip().splitlines()[0]
        self.assertIn('"r05-", 174048', first)

    def test_rofs_only_partition_rounds_to_sectors(self):
        """The 7710 has no ROFx and its ROFS record (0xAFC9F0) is not a sector multiple: part 1 holds one
        directory entry and the ROFS rounded up (else MedDoc's last read runs out of range)."""
        body = BOARD.read_text().split('static void ra2_part1(Ra2Part *p)', 1)[1].split('\n}\n', 1)[0]
        self.assertIn('ROUND_UP(lr, 512)', body)
        self.assertIn('rofx ? 2 : 1', body)
        self.assertIsNone(re.search(r'rofx_file\s*=', phone('ra2_phone_7710')))


class Reset(unittest.TestCase):
    def test_rm12_power_on_is_cold_start(self):
        """Stall 3: EikSrvs data abort in EKern (0x5002860C): QEMU's power-on ARM_SYSST (0x3A) had bit 1
        (GLOB_SWRST) set, the bootstrap reported a warm restart (start-up type 4), the kernel built its
        security object from the (empty) super-page blob and got NULL. Power-on must report POR only."""
        text = PATCH_SYSST.read_text()
        self.assertIn('-    s->clkm.cold_start = 0x3a;', text)
        self.assertIn('+    s->clkm.cold_start = 0x20;', text)
        self.assertEqual(0x20 & 2, 0)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_bootstrap_reads_glob_swrst(self):
        """The bootstrap tests ARM_SYSST bit 1 (0x50000330..0x5000033C) and maps reset reason 2 to type 4."""
        rom = ROM.read_bytes()
        self.assertEqual(word(rom, 0x50000864), 0xFFFECE18)
        self.assertEqual(word(rom, 0x50000338), 0xE2022002)     # and r2, r2, #2
        self.assertEqual(word(rom, 0x500012A4), 0xE3A00004)     # reason 2 -> 4 (warm)


@unittest.skipUnless(ROM.exists() and ROM9500.exists(), 'core ROMs not extracted')
class Relocated(unittest.TestCase):
    """The DSP-bridge patches are the 9500's at relocated addresses: check the deltas against both ROMs."""

    def deltas(self):
        body = phone('ra2_phone_7710')
        d = {}
        for k in ('ehwa', 'bal', 'dms'):
            a, b = re.search(r'\.%s_delta = (0x[0-9a-f]+) - (0x[0-9a-f]+)' % k, body).groups()
            d[k] = (int(a, 16) - int(b, 16)) & 0xffffffff
        return d

    def test_patched_words_match(self):
        r7, r9 = ROM.read_bytes(), ROM9500.read_bytes()
        d = self.deltas()
        for k, va in (('ehwa', 0x507F60D4), ('dms', 0x50935E68), ('dms', 0x50935324), ('dms', 0x5093731C),
                      ('bal', 0x5092AD6C), ('bal', 0x5092A5D0), ('bal', 0x50925ECC)):
            self.assertEqual(word(r7, (va + d[k]) & 0xffffffff), word(r9, va), '%s %#x' % (k, va))

    def test_doc_export_thunks(self):
        """DiskOnChip.dll's five export thunks hold the same words as the 9500's."""
        r7, r9 = ROM.read_bytes(), ROM9500.read_bytes()
        for a7, a9 in ((0x500AFD38, 0x50065F20), (0x500AFD40, 0x50065F28), (0x500AFD3C, 0x50065F24),
                       (0x500AFD54, 0x50065F3C), (0x500AFD44, 0x50065F2C)):
            self.assertEqual(word(r7, a7), word(r9, a9))


class Panel(unittest.TestCase):
    INPUT = REPO / 'src/qemu-ngage/hw/arm/nokia7710_input.c'

    def test_rm12_kept_c_panel_status(self):
        """Owner's report 2026-11-18: a phone started from its kept C: never showed anything (and a fresh
        one showed its logo late): VideoDriver powers the panel up through its controller on Microwire
        CS 3 and in state 4 (0x5009A834) wants register 0x0A to read 0x40; nothing answered on CS 3 (0),
        the driver failed to state 8 and left the LCD controller off."""
        text = self.INPUT.read_text()
        self.assertIn('omap_uwire_attach(mpu->microwire, 3, rm12_panel_send, rm12_panel_receive, s);', text)
        self.assertIn('static const uint16_t rm12_panel_status = 0x40;', text)
        self.assertIn('rm12_panel_cmd == 0x0a ? rm12_panel_status : 0', text)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_state4_compares_0x40(self):
        rom = ROM.read_bytes()
        self.assertEqual(word(rom, 0x5009A840), 0xE3510040)     # cmp r1, #0x40
        self.assertEqual(word(rom, 0x5009A7EC), 0xE3510020)     # state 3: cmp r1, #0x20


class Keys(unittest.TestCase):
    INPUT = REPO / 'src/qemu-ngage/hw/arm/nokia7710_input.c'
    # what each host key must produce (EStdKey scan codes) [V: ekeyp table, docs/research/rm12-keys.md]
    WANT = {'UP': 0x10, 'DOWN': 0x11, 'LEFT': 0x0E, 'RIGHT': 0x0F, 'F1': 0x94, 'F2': 0xB4, 'F3': 0xC4,
            'F4': 0xC5, 'ESC': 0x04, 'ENTER': 0xAB}

    def table(self):
        return {k: (int(r), int(c)) for k, r, c in
                re.findall(r'\{ KEY_(\w+), (\d+), (\d+) \}', self.INPUT.read_text())}

    def test_positions_in_matrix(self):
        for k, (r, c) in self.table().items():
            self.assertTrue(0 <= r < 5 and 0 <= c < 6, k)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_scan_codes_match_rom(self):
        """ekeyp's 5x6 table at 0x500C77F0 (bytes, table[row * 6 + col]); column 0 is real (the 9500's
        matrix started at column 1)."""
        rom = ROM.read_bytes()
        t = self.table()
        for k, code in self.WANT.items():
            r, c = t[k]
            self.assertEqual(rom[0x500C77F0 - B + r * 6 + c], code, k)


if __name__ == '__main__':
    unittest.main()
