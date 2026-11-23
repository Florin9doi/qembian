"""Checks on the Nokia 9300 board (src/qemu-ngage/hw/arm/nokia9500*.c) that need no emulator run.

Run: python3 tests/nokia9300_board_test.py
The ROM checks are skipped when the (git-ignored) core ROM is not extracted.
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9500.c'
KBD = REPO / 'src/qemu-ngage/hw/arm/nokia9500_kbd.c'
ROM = REPO / 'eka1/rae6/core/ROM.bin'


def func(path, name):
    text = path.read_text()
    return text.split(name, 1)[1].split('\n}\n', 1)[0]


def phone9300():
    """The Ra2Phone initialiser of the 9300 in nokia9500.c."""
    return func(BOARD, 'static const Ra2Phone ra2_phone_9300 = {')


def delta(field):
    """`.<field> = A - B` of the 9300's Ra2Phone: the 9300's address minus the 9500's, as a signed number."""
    m = re.search(r'\.%s = (0x[0-9a-f]+) - (0x[0-9a-f]+)' % field, phone9300())
    return int(m.group(1), 16) - int(m.group(2), 16)


class Kbc(unittest.TestCase):
    def test_nothing_sent_at_power_on(self):
        """The controller queued no lid state at reset."""
        body = func(KBD, 'static void kbc_reset(DeviceState *dev)')
        self.assertNotIn('fifo[', body)
        self.assertNotIn('timer_mod', body)
        self.assertIn('cover_open = false', body)


class KeyMap(unittest.TestCase):
    """nokia9500_kbd.c RA2_KEYS: host key -> index into EKeyb's scan-code table ([0x5009D930])."""

    def keys(self):
        return {k: int(i) for k, i in re.findall(r'\{ KEY_(\w+), (\d+) \}', KBD.read_text())}

    def test_no_index_twice(self):
        idx = [i for k, i in self.keys().items() if k not in ('MENU',)]   # COMPOSE and MENU share 16
        self.assertEqual(len(idx), len(set(idx)))

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_letters_and_digits_match_the_rom_table(self):
        rom = ROM.read_bytes()
        table = struct.unpack_from('<I', rom, 0x9D930)[0] - 0x50000000
        for key, i in self.keys().items():
            if len(key) == 1 and key.isalnum():
                scan = struct.unpack_from('<H', rom, table + 6 * i)[0]
                self.assertEqual(scan, ord(key), 'KEY_%s -> index %d = scan code %#x' % (key, i, scan))

    def test_release_sets_bit_0(self):
        """byte = index << 1, | 1 on release (EKeyb's key-up path)."""
        self.assertIn('RA2_KEYS[i].index << 1 | !down', KBD.read_text())


class DspBoot(unittest.TestCase):
    def test_model_has_dsp_sync(self):
        """Ehwaomap.pdd's DSP boot wait: the 9500's 0x507f60d4 moved to 0x507ead64."""
        self.assertEqual(0x507f60d4 + delta('ehwa_delta'), 0x507ead64)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rom_has_the_dsp_boot_wait(self):
        """The patched word is the backward branch of the DSP boot wait (bne 0x507ead58)."""
        w = struct.unpack_from('<I', ROM.read_bytes(), 0x7ead64)[0]
        self.assertEqual(w, 0x1afffffb)
        target = 0x507ead64 + 8 + ((w & 0xffffff) - 0x1000000) * 4
        self.assertEqual(target, 0x507ead58)


class BalStandin(unittest.TestCase):
    def slots(self):
        """The 9500's slot table moved by the 9300's bal_delta; the 9300's export directory has one entry fewer, so
        the 9500's export n is the 9300's export n - 1."""
        text = BOARD.read_text()
        body = func(BOARD, 'static void ra2_bal_standin(void)').split('slots[] = {', 1)[1].split('};', 1)[0]
        bd = delta('bal_delta')
        return [(int(o) - 1, int(old, 16) + bd, sym) for o, old, sym in
                re.findall(r'\{ (\d+), (0x[0-9a-f]+), NGAGE_RA2_BAL_NOS_SYM_(\w+) \}', body)]

    def test_deltas_agree(self):
        """Bal.dll / Dasfgwc.dll and DeviceManagementServer moved by the same amount; the stub starts at Bal's first slot."""
        self.assertEqual(delta('bal_delta'), delta('dms_delta'))
        self.assertEqual(0x50925ecc + delta('bal_delta'), 0x5091ab2c)
        self.assertEqual(0x50926bc0 + delta('bal_delta') + 4, 0x5091b824)     # the export directory
        self.assertEqual(len(self.slots()), 20)

    def test_blob_does_not_cover_kept_exports(self):
        """The blob overwrites code at 0x5091ab2c: no export left real may start inside it."""
        hdr = (REPO / 'src/qemu-ngage/hw/arm/ngage_rae6_bal_stub.h').read_text()
        size = int(re.search(r'ngage_rae6_bal_nos_stub\[(\d+)\]', hdr).group(1))
        patched = {o for o, _, _ in self.slots()}
        if ROM.exists():
            rom = ROM.read_bytes()
            for o in range(1, 28):
                a = struct.unpack_from('<I', rom, 0x91b824 + 4 * (o - 1))[0]
                if o not in patched:
                    self.assertFalse(0x5091ab2c <= a < 0x5091ab2c + size, 'export %d (%#x) inside [%#x, %#x)' %
                                     (o, a, 0x5091ab2c, 0x5091ab2c + size))

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_old_slot_values_match_rom(self):
        rom = ROM.read_bytes()
        for o, old, _ in self.slots():
            self.assertEqual(struct.unpack_from('<I', rom, 0x91b824 + 4 * (o - 1))[0], old, 'export %d' % o)


class Partitions(unittest.TestCase):
    def test_c_drive_is_partition_0(self):
        text = BOARD.read_text()
        table = text.split('static Ra2Part ra2_parts_9300[] = {', 1)[1].split('};', 1)[0]
        rows = [r for r in table.splitlines() if r.strip().startswith('{')]
        self.assertIn('RAE6n6270.u18', rows[0])


if __name__ == '__main__':
    unittest.main()
