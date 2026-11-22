"""Checks on the Nokia 9500 board (src/qemu-ngage/hw/arm/nokia9500*.c) that need no emulator run.

Run: python3 tests/nokia9500_board_test.py
The ROM checks are skipped when the (git-ignored) core ROM is not extracted.
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9500.c'
KBD = REPO / 'src/qemu-ngage/hw/arm/nokia9500_kbd.c'
ROM = REPO / 'eka1/ra2/core/ROM.bin'


def func(path, name):
    text = path.read_text()
    return text.split(name, 1)[1].split('\n}\n', 1)[0]


class Kbc(unittest.TestCase):
    def test_nothing_sent_at_power_on(self):
        """Regression kbc_lid_byte_at_boot: the controller queued the lid state (0xE0) at reset; the
        lid-open event during start-up stalled EFile's start-up thread (2 processes instead of 9)."""
        body = func(KBD, 'static void kbc_reset(DeviceState *dev)')
        self.assertNotIn('fifo[', body)
        self.assertNotIn('timer_mod', body)
        self.assertIn('cover_open = false', body)


class KeyMap(unittest.TestCase):
    """nokia9500_kbd.c RA2_KEYS: host key -> index into EKeyb's scan-code table ([0x5009E8FC])."""

    def keys(self):
        return {k: int(i) for k, i in re.findall(r'\{ KEY_(\w+), (\d+) \}', KBD.read_text())}

    def test_no_index_twice(self):
        idx = [i for k, i in self.keys().items() if k not in ('MENU',)]   # COMPOSE and MENU share 16
        self.assertEqual(len(idx), len(set(idx)))

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_letters_and_digits_match_the_rom_table(self):
        rom = ROM.read_bytes()
        table = struct.unpack_from('<I', rom, 0x9E8FC)[0] - 0x50000000
        for key, i in self.keys().items():
            if len(key) == 1 and key.isalnum():
                scan = struct.unpack_from('<H', rom, table + 6 * i)[0]
                self.assertEqual(scan, ord(key), 'KEY_%s -> index %d = scan code %#x' % (key, i, scan))

    def test_release_sets_bit_0(self):
        """byte = index << 1, | 1 on release (EKeyb's key-up path at 0x5009E8A0)."""
        self.assertIn('RA2_KEYS[i].index << 1 | !down', KBD.read_text())


class DspBoot(unittest.TestCase):
    def test_patch_checks_old_word(self):
        body = func(BOARD, 'static void ra2_dsp_boot_patch(void)')
        self.assertIn('0x507f60d4', body)
        self.assertIn('0x1afffffb', body)
        self.assertIn('0xe1a00000', body)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rom_has_the_dsp_boot_wait(self):
        """The patched word is the backward branch of the DSP boot wait (bne 0x507f60c8)."""
        w = struct.unpack_from('<I', ROM.read_bytes(), 0x7f60d4)[0]
        self.assertEqual(w, 0x1afffffb)
        target = 0x507f60d4 + 8 + ((w & 0xffffff) - 0x1000000) * 4
        self.assertEqual(target, 0x507f60c8)


class BalStandin(unittest.TestCase):
    def slots(self):
        body = func(BOARD, 'static void ra2_bal_standin(void)')
        return [(int(o), int(old, 16), sym) for o, old, sym in
                re.findall(r'\{ (\d+), (0x[0-9a-f]+), NGAGE_RA2_BAL_NOS_SYM_(\w+) \}', body)]

    def test_blob_does_not_cover_kept_exports(self):
        """The blob overwrites code at 0x50925ECC: no export left real may start inside it."""
        hdr = (REPO / 'src/qemu-ngage/hw/arm/ngage_ra2_bal_stub.h').read_text()
        size = int(re.search(r'ngage_ra2_bal_nos_stub\[(\d+)\]', hdr).group(1))
        patched = {o for o, _, _ in self.slots()}
        if ROM.exists():
            rom = ROM.read_bytes()
            for o in range(1, 29):
                a = struct.unpack_from('<I', rom, 0x926bc0 + 4 * (o - 1))[0]
                if o not in patched:
                    self.assertFalse(0x50925ecc <= a < 0x50925ecc + size, 'export %d' % o)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_old_slot_values_match_rom(self):
        rom = ROM.read_bytes()
        for o, old, _ in self.slots():
            self.assertEqual(struct.unpack_from('<I', rom, 0x926bc0 + 4 * (o - 1))[0], old, 'export %d' % o)


class Partitions(unittest.TestCase):
    def test_c_drive_is_partition_0(self):
        """Regression c_drive_partition: C: was on partition 2; EFile mounts partition 0 (blank ->
        KErrCorrupt -> EStart's format query waiting for a key)."""
        text = BOARD.read_text()
        table = text.split('static Ra2Part ra2_parts_9500[] = {', 1)[1].split('};', 1)[0]
        rows = [r for r in table.splitlines() if r.strip().startswith('{')]
        self.assertIn('RA2n6270.u18', rows[0])


if __name__ == '__main__':
    unittest.main()
