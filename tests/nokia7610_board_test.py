"""Checks on the Nokia 7610 / 6670 board (-M nokia7610 / nokia6670: src/qemu-ngage/hw/arm/ngage.c ngage_rh51,
ngage_keys.c rh51_keys, nos_standin_nhl10.S built for V 4.20) that need no emulator run. Each regression test is
named after the stall it guards (docs/devices/nokia-7610-rh51/bring-up.md).

Run: python3 tests/nokia7610_board_test.py
ROM checks are skipped when the (git-ignored) core ROM is not extracted.
"""
import json
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
ROM = REPO / 'eka1/rh51/output/rom/ROM.bin'
MAN = REPO / 'eka1/rh51/output/rom/manifest.json'
B = 0x50000000


def model():
    t = (HW / 'ngage.c').read_text()
    return t.split('static const NgageModel ngage_rh51 = {', 1)[1].split('\n};\n', 1)[0]


def word(rom, va):
    return struct.unpack_from('<I', rom, va - B)[0]


class Board(unittest.TestCase):
    def test_rh51_rom_map_and_flash(self):
        """ROM 0x1900000: 16 MB in chip 0, 9 MB in chip 1 (bootstrap bank table); the 3230's AMD/Samsung part."""
        m = model()
        self.assertIn('{ 0, 0x01000000, 0 }, { 0x01000000, 0x00900000, 1 }', m)
        self.assertIn('.flash_manuf = 0xec, .flash_device = 0x22fa, .flash_amd = true', m)

    def test_rh51_both_machines_registered(self):
        t = (HW / 'ngage.c').read_text()
        for name in ('nokia7610', 'nokia6670'):
            self.assertIn('MACHINE_TYPE_NAME("%s")' % name, t)
            self.assertIn('type_register_static(&ngage_rh%s_machine_type)' % ('51' if name == 'nokia7610' else '67'), t)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rh51_nos_exports_match_rom(self):
        """The hidden NOS module (header 0x5042D620) exports 1, 2, 4 at the addresses the board patches."""
        rom = ROM.read_bytes()
        h = struct.unpack_from('<24I', rom, 0x5042d620 - B)
        exp = struct.unpack_from('<%dI' % h[15], rom, h[16] - B)
        got = [int(a, 16) for a in re.findall(r'\{ (0x[0-9a-f]+), NGAGE_RH51_NOS_SYM',
                                               (HW / 'ngage.c').read_text())]
        self.assertEqual(got, [exp[0], exp[1], exp[3]])


class NosStandin(unittest.TestCase):
    @unittest.skipUnless(ROM.exists() and MAN.exists(), 'core ROM not extracted')
    def test_rh51_defsyms_match_rom(self):
        """The 3230's stand-in assembled with V 4.20's ekern exports (by ordinal), NOS bss and enosdriver export 2."""
        rom = ROM.read_bytes()
        mods = {m['name'].lower(): m for l in json.loads(MAN.read_text())['module_lists'].values() for m in l}
        man = json.loads(MAN.read_text())
        enos = next(f for f in man['files'] if f['name'].lower() == 'enosdriver.ldd')

        def exports(addr):
            h = struct.unpack_from('<24I', rom, addr - B)
            return struct.unpack_from('<%dI' % h[15], rom, h[16] - B)
        build = (REPO / 'src/qemu-ngage/nos-standin/build.sh').read_text().split('build nos_standin_nhl10.S 0x528929e4', 1)[1].split('\nbuild ', 1)[0]
        d = {k: int(v, 16) for k, v in re.findall(r'--defsym (\w+)=(0x[0-9a-f]+)', build)}
        ek = exports(mods['ekern.exe']['address'])
        for name, ordinal in (('KERN_VARIANT', 426), ('SET_POWER_MODEL', 344), ('POWER_MODEL_CTOR', 451),
                              ('KERN_ADD_DFC', 7), ('HWCHUNK_CTOR', 454), ('HWCHUNK_CREATE', 93)):
            self.assertEqual(d[name], ek[ordinal - 1], name)
        self.assertEqual(d['ENOS_DELIVER'], exports(enos['rom_virtual_address'])[1])
        ns = mods['nosstarter.ext']['address']
        self.assertEqual(word(rom, ns + 0xa8), 0x528929e4)      # NOS entry
        self.assertEqual(word(rom, ns + 0xb8), d['RAM'])        # NOS bss


class Isi(unittest.TestCase):
    def test_rh51_checkcrc_5b(self):
        """V 4.20's CheckCrcMsgL compares data[1] with 0x5B (the 3230: 0xC0, the 6600: 0x82)."""
        t = (REPO / 'src/qemu-ngage/isi-replies-rh51.txt').read_text()
        line = next(l for l in t.splitlines() if l.startswith('reply 00 01 02'))
        self.assertEqual(line.split()[4:6], ['00', '5b'])

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rh51_checkcrc_compare_in_rom(self):
        rom = ROM.read_bytes()
        self.assertEqual(rom[0x503520f8 - B:0x503520f8 - B + 2], bytes([0x5b, 0x28]))     # Thumb cmp r0, #0x5B


class Keys(unittest.TestCase):
    def table(self):
        t = (HW / 'ngage_keys.c').read_text().split('static const NgageKey rh51_keys[] = {', 1)[1].split('};', 1)[0]
        return {k: (int(r), int(c)) for k, r, c in re.findall(r'\{ KEY_(\w+), "[^"]*", (-?\d+), (-?\d+) \}', t)}

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rh51_keys_dead_with_6600_joystick(self):
        """Stall: with the 6600's layout no key worked at the time query. V 4.20's ekeyb has no joystick port
        (0x0C15020A); its read helper takes the joystick from GPIO 0x0C120C04 bits 6, 3, 4, 5, 7 (left, up,
        right, down, select) and sense line 5 from bit 18. The joystick keys must sit on those GPIO bits and the
        helper must still read them there."""
        t = self.table()
        self.assertEqual({k: t[k] for k in ('LEFT', 'UP', 'RIGHT', 'DOWN', 'ENTER')},
                         {'LEFT': (-3, 6), 'UP': (-3, 3), 'RIGHT': (-3, 4), 'DOWN': (-3, 5), 'ENTER': (-3, 7)})
        rom = ROM.read_bytes()
        self.assertEqual(word(rom, 0x5036a7c4), 0x00120c04)     # the helper's GPIO literal
        self.assertEqual(word(rom, 0x5036a758), 0xe3100701)     # tst r0, #0x40000: line 5 = GPIO bit 18
        # the joystick bits tested in order: 0x40 (bit 16), 8, 0x10, 0x20, 0x80 (bit 20)
        self.assertEqual([rom[a - B] for a in (0x5036a6f8, 0x5036a704, 0x5036a710, 0x5036a71c, 0x5036a728)],
                         [0x40, 0x08, 0x10, 0x20, 0x80])
        # the joystick scan list: left, up, right, down, select
        self.assertEqual([rom[0x5036adac - B + 4 * i] for i in range(5)], [0x0e, 0x10, 0x0f, 0x11, 0xa7])

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rh51_matrix_keys_match_scan_list(self):
        """The 6600's two sub-matrices: rows 0-1 x lines 0-1 (list entries 0-3), rows 2-5 x lines 2-5 (4-19)."""
        rom = ROM.read_bytes()
        lst = [rom[0x5036ad50 - B + 4 * i] for i in range(20)]
        want = {'F1': 0xA4, 'F5': 0xB4, 'F2': 0xA5, 'F6': 0xAA, 'LEFTSHIFT': 0x12, 'KPASTERISK': 0x2A,
                'F10': 0x7F, 'BACKSPACE': 0x01, 'F3': 0xC4, 'F4': 0xC5}
        want.update({str(i): 0x30 + i for i in range(10)})
        t = self.table()
        for k, code in want.items():
            r, c = t[k]
            idx = r * 2 + c if r < 2 else 4 + (r - 2) * 4 + (c - 2)
            self.assertEqual(lst[idx], code, k)

    def test_rh51_joystick_raises_keypad_irq(self):
        """H-rh51-joy-ctsi: GPIO interrupts are not modelled, so a joystick edge must raise the keypad CTSI bit."""
        t = (HW / 'ngage_keys.c').read_text()
        body = t.split('if (key->row == -3) {', 1)[1].split('return;', 1)[0]
        self.assertIn('kpd.ctsi |= NGAGE_CTSI_KPD', body)


if __name__ == '__main__':
    unittest.main()
