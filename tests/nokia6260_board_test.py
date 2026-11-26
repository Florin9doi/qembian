"""Checks on the Nokia 6260 board (-M nokia6260: src/qemu-ngage/hw/arm/ngage.c ngage_rm25, ngage_keys.c rm25_keys,
nos_standin_nhl10.S built for V 5.0536) that need no emulator run. Each regression test is named after the stall it
guards (docs/devices/nokia-6260-rm25/bring-up.md).

Run: python3 tests/nokia6260_board_test.py
ROM checks are skipped when the (git-ignored) core ROM is not extracted.
"""
import json
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
ROM = REPO / 'eka1/rm25/output/rom/ROM.bin'
MAN = REPO / 'eka1/rm25/output/rom/manifest.json'
B = 0x50000000


def model():
    t = (HW / 'ngage.c').read_text()
    return t.split('static const NgageModel ngage_rm25 = {', 1)[1].split('\n};\n', 1)[0]


def word(rom, va):
    return struct.unpack_from('<I', rom, va - B)[0]


class Board(unittest.TestCase):
    def test_rm25_rom_map_and_flash(self):
        """ROM 0x1D00000: 16 MB in chip 0, 13 MB in chip 1 (bootstrap bank table); the 3230's AMD/Samsung part."""
        m = model()
        self.assertIn('{ 0, 0x01000000, 0 }, { 0x01000000, 0x00d00000, 1 }', m)
        self.assertIn('.flash_manuf = 0xec, .flash_device = 0x22fa, .flash_amd = true', m)
        self.assertIn('.screen_chunk_ptr = 0x80001780', m)

    def test_rm25_machine_registered(self):
        t = (HW / 'ngage.c').read_text()
        self.assertIn('MACHINE_TYPE_NAME("nokia6260")', t)
        self.assertIn('type_register_static(&ngage_rm25_machine_type)', t)
        self.assertIn(': ngm == &ngage_rm25 ? NGAGE_KPD_RM25', t)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rm25_rom_size(self):
        rom = ROM.read_bytes()
        self.assertEqual(len(rom), 0x1d00000)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rm25_nos_exports_match_rom(self):
        """The hidden NOS module (header 0x504573F0) exports 1, 2, 4 at the addresses the board patches."""
        rom = ROM.read_bytes()
        h = struct.unpack_from('<24I', rom, 0x504573f0 - B)
        exp = struct.unpack_from('<%dI' % h[15], rom, h[16] - B)
        got = [int(a, 16) for a in re.findall(r'\{ (0x[0-9a-f]+), NGAGE_RM25_NOS_SYM',
                                               (HW / 'ngage.c').read_text())]
        self.assertEqual(got, [exp[0], exp[1], exp[3]])


class NosStandin(unittest.TestCase):
    @unittest.skipUnless(ROM.exists() and MAN.exists(), 'core ROM not extracted')
    def test_rm25_defsyms_match_rom(self):
        """The 3230's stand-in assembled with V 5.0536's ekern exports (by ordinal), NOS bss and enosdriver export 2."""
        rom = ROM.read_bytes()
        man = json.loads(MAN.read_text())
        mods = {m['name'].lower(): m for l in man['module_lists'].values() for m in l}
        enos = next(f for f in man['files'] if f['name'].lower() == 'enosdriver.ldd')

        def exports(addr):
            h = struct.unpack_from('<24I', rom, addr - B)
            return struct.unpack_from('<%dI' % h[15], rom, h[16] - B)
        build = (REPO / 'src/qemu-ngage/nos-standin/build.sh').read_text().split(
            'build nos_standin_nhl10.S 0x52894194', 1)[1].split('\nbuild ', 1)[0]
        d = {k: int(v, 16) for k, v in re.findall(r'--defsym (\w+)=(0x[0-9a-f]+)', build)}
        ek = exports(mods['ekern.exe']['address'])
        for name, ordinal in (('KERN_VARIANT', 426), ('SET_POWER_MODEL', 344), ('POWER_MODEL_CTOR', 451),
                              ('KERN_ADD_DFC', 7), ('HWCHUNK_CTOR', 454), ('HWCHUNK_CREATE', 93)):
            self.assertEqual(d[name], ek[ordinal - 1], name)
        self.assertEqual(d['ENOS_DELIVER'], exports(enos['rom_virtual_address'])[1])
        ns = mods['nosstarter.ext']['address']
        self.assertEqual(word(rom, ns + 0xa8), 0x52894194)      # NOS entry
        self.assertEqual(word(rom, ns + 0xb8), d['RAM'])        # NOS bss
        self.assertIn('.nos_rx_dfc = 0x%08x + 0x200' % d['RAM'], model())


class Isi(unittest.TestCase):
    def test_rm25_checkcrc_e4(self):
        """V 5.0536's CheckCrcMsgL compares data[1] with 0xE4 (the 7610: 0x5B, the 3230: 0xC0)."""
        t = (REPO / 'src/qemu-ngage/isi-replies-rm25.txt').read_text()
        line = next(l for l in t.splitlines() if l.startswith('reply 00 01 02'))
        self.assertEqual(line.split()[4:6], ['00', 'e4'])

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rm25_checkcrc_compare_in_rom(self):
        rom = ROM.read_bytes()
        self.assertEqual(rom[0x5037ab78 - B:0x5037ab78 - B + 2], bytes([0xe4, 0x28]))     # Thumb cmp r0, #0xE4


class Keys(unittest.TestCase):
    def table(self):
        t = (HW / 'ngage_keys.c').read_text().split('static const NgageKey rm25_keys[] = {', 1)[1].split('};', 1)[0]
        return {k: (int(r), int(c)) for k, r, c in re.findall(r'\{ KEY_(\w+), "[^"]*", (-?\d+), (-?\d+) \}', t)}

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rm25_zero_clear_end_dead_with_7610_layout(self):
        """Stall: with the 7610's keypad model, 0, Clear and End did nothing (owner's report). The 6260's ekeyb has
        its own 6x6 table (0x503955D8, row * 6 + line: Clear at (1, 1), not the 7610's (2, 5)), and its read helper
        takes sense line 5 (0 and End) from GPIO 0x0C120C04 bit 19, not the 7610's bit 18. Every key of the board's
        table must sit where the scan list has its code."""
        rom = ROM.read_bytes()
        lst = [rom[0x503955d8 - B + 4 * i] for i in range(36)]
        want = {'F1': 0xA4, 'F5': 0xB4, 'F2': 0xA5, 'BACKSPACE': 0x01, 'LEFTSHIFT': 0x12, 'F7': 0xA2,
                'KPASTERISK': 0x2A, 'F10': 0x7F, 'F8': 0xA3, 'F6': 0xAC, 'F3': 0xC4, 'F4': 0xC5}
        want.update({str(i): 0x30 + i for i in range(10)})
        t = self.table()
        for k, code in want.items():
            r, c = t[k]
            self.assertEqual(lst[r * 6 + c], code, k)
        self.assertEqual(word(rom, 0x50394a5c), 0x00120c04)     # the helper's GPIO literal
        self.assertEqual(word(rom, 0x50394984), 0xe3150020)     # tst r5, #0x20: virtual bit 5 = line 5
        self.assertEqual(word(rom, 0x50394998), 0xe3100702)     # tst r0, #0x80000: line 5 = GPIO bit 19
        k = (HW / 'ngage_keys.c').read_text()
        self.assertIn('kpd.gpio_line5 = layout == NGAGE_KPD_RM25 ? 1u << 19 : 1u << 18;', k)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rm25_joystick_on_7610_gpio_bits(self):
        """The joystick: GPIO bits 6, 3, 4, 5, 7 (virtual bits 16-20), as the 7610 [V helper 0x50394924]."""
        t = self.table()
        self.assertEqual({k: t[k] for k in ('LEFT', 'UP', 'RIGHT', 'DOWN', 'ENTER')},
                         {'LEFT': (-3, 6), 'UP': (-3, 3), 'RIGHT': (-3, 4), 'DOWN': (-3, 5), 'ENTER': (-3, 7)})
        rom = ROM.read_bytes()
        self.assertEqual([rom[a - B] for a in (0x50394938, 0x50394944, 0x50394950, 0x5039495c, 0x50394968)],
                         [0x40, 0x08, 0x10, 0x20, 0x80])

    def test_rm25_switch_lines_idle_high(self):
        """GPIO bits 22-26 (switch entries 0xB6-0xBB) idle high = not asserted, the 7610's GPIO bits unchanged."""
        k = (HW / 'ngage_keys.c').read_text()
        self.assertIn('| (layout == NGAGE_KPD_RM25 ? 0x7c00000 : 0)', k)
        self.assertEqual(0x7c00000, sum(1 << b for b in range(22, 27)))


if __name__ == '__main__':
    unittest.main()
