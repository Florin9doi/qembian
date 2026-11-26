"""Checks on the Nokia 3230 board (-M nokia3230: src/qemu-ngage/hw/arm/ngage.c ngage_rm51, ngage_keys.c rm51_keys,
nos_standin_nhl10.S built for the 3230) that need no emulator run. Each regression test is named after the stall it
guards (docs/devices/nokia-3230-rm51/bring-up.md).

Run: python3 tests/nokia3230_board_test.py
ROM checks are skipped when the (git-ignored) core ROM is not extracted.
"""
import json
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
ROM = REPO / 'eka1/rm51/output/rom/ROM.bin'
MAN = REPO / 'eka1/rm51/output/rom/manifest.json'
B = 0x50000000


def model():
    t = (HW / 'ngage.c').read_text()
    return t.split('static const NgageModel ngage_rm51 = {', 1)[1].split('\n};\n', 1)[0]


class Flash(unittest.TestCase):
    def test_rm51_flash_type_accepted(self):
        """Stall 1: the 6600's ST chip (type 11) halted the bootstrap's chip check; the 3230 needs an AMD/Samsung
        part on the AMD command set (CFI02)."""
        m = model()
        self.assertIn('.flash_amd = true', m)
        self.assertIn('.flash_manuf = 0xec, .flash_device = 0x22fa', m)
        self.assertIn('.nchips = 3', m)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_id_table_and_combination_table(self):
        rom = ROM.read_bytes()
        off, types = 0x7910, {}
        while True:
            e = struct.unpack_from('<8H', rom, off)
            if e[0] not in (0x01, 0x89, 0xec, 0x20):
                break
            types[(e[0], e[1])] = e[6]
            off += 16
        t = types[(0xec, 0x22fa)]
        self.assertEqual(t, 14)
        accepted = set(struct.unpack_from('<40I', rom, 0x7a60)[0::5]) - {0}
        self.assertIn(t, accepted)
        self.assertNotIn(types[(0x20, 0x88c0)], accepted)       # the 6600's ST part


class NosStandin(unittest.TestCase):
    @unittest.skipUnless(ROM.exists() and MAN.exists(), 'core ROM not extracted')
    def test_defsyms_match_rom(self):
        """The 6600's stand-in is assembled with the 3230's ekern exports (by ordinal), NOS bss and enosdriver
        export 2: check build.sh's values against the ROM's export tables and nosstarter's literals."""
        rom = ROM.read_bytes()
        mods = {m['name'].lower(): m for l in json.loads(MAN.read_text())['module_lists'].values() for m in l}

        def exports(name):
            h = struct.unpack_from('<24I', rom, mods[name]['address'] - B)
            return struct.unpack_from('<%dI' % h[15], rom, h[16] - B)
        build = (REPO / 'src/qemu-ngage/nos-standin/build.sh').read_text().split('build nos_standin_nhl10.S 0x528943a4', 1)[1].split('\nbuild ', 1)[0]
        d = {k: int(v, 16) for k, v in re.findall(r'--defsym (\w+)=(0x[0-9a-f]+)', build)}
        ek = exports('ekern.exe')
        for name, ordinal in (('KERN_VARIANT', 426), ('SET_POWER_MODEL', 344), ('POWER_MODEL_CTOR', 451),
                              ('KERN_ADD_DFC', 7), ('HWCHUNK_CTOR', 454), ('HWCHUNK_CREATE', 93)):
            self.assertEqual(d[name], ek[ordinal - 1], name)
        self.assertEqual(d['ENOS_DELIVER'], exports('enosdriver.ldd')[1])
        ns = mods['nosstarter.ext']['address']
        self.assertEqual(struct.unpack_from('<I', rom, ns + 0xa8 - B)[0], 0x528943a4)     # NOS entry
        self.assertEqual(struct.unpack_from('<I', rom, ns + 0xb8 - B)[0], d['RAM'])       # NOS bss


class Isi(unittest.TestCase):
    def test_rm51_checkcrc_c0(self):
        """Stall 2: the 3230's CheckCrcMsgL compares data[1] with 0xC0 (the 6600: 0x82)."""
        t = (REPO / 'src/qemu-ngage/isi-replies-rm51.txt').read_text()
        line = next(l for l in t.splitlines() if l.startswith('reply 00 01 02'))
        self.assertEqual(line.split()[4:6], ['00', 'c0'])


class IsiReplies(unittest.TestCase):
    TABLE = REPO / 'src/qemu-ngage/isi-replies-rm51.txt'

    def lines(self):
        return [l.split('#')[0].split() for l in self.TABLE.read_text().splitlines()
                if l.startswith(('reply ', 'reply+ '))]

    def find(self, kind, *head):
        return [l for l in self.lines() if l[0] == kind and [x.upper() for x in l[1:1 + len(head)]] == list(head)]

    def test_rm51_als_read_ends(self):
        """Stall 6: Phone.app hung in GetAlsLine: SIM 09 0F needs status != 01 (01 = another record follows)."""
        (l,) = self.find('reply', '09', '0F', '10')
        self.assertEqual([x.upper() for x in l[4:6]], ['LEN=13', '0B'])
        self.assertNotEqual(l[6], '01')

    def test_rm51_dsy_audio_subtype_01(self):
        """Stall 7: the DSY's 0B 11 {01 00} waits in a nested scheduler (handler 0x5083A800, sub-block id 00)."""
        (l,) = self.find('reply', '0B', '11', '12', '@01')
        self.assertEqual([x.upper() for x in l[5:8]], ['01', '01', '02'])

    def test_rm51_devsound_sync_waits_answered(self):
        """Stall 8: keys stopped after the second key click: DevSound's play end and close wait synchronously on
        object 0x56 for 0B 19 and 0B 04 (and Stop for E8 2A); each needs some reply. E8 20 needs 'started' then
        'ended' (a reply+ line), or the key-sound server never finishes its tone."""
        for head in (('0B', '19'), ('0B', '04'), ('E8', '2A'), ('E8', '20')):
            self.assertTrue(self.find('reply', *head), head)
        self.assertEqual([x.upper() for x in self.find('reply', 'E8', '20')[0][3:4]], ['21'])
        self.assertEqual([x.upper() for x in self.find('reply+', 'E8', '20')[0][3:4]], ['2B'])


class BtUart(unittest.TestCase):
    def test_rm51_bt_uart_ready(self):
        """Stall 4: BC02_UART1.pdd polls 0x0C150572 bit 7 without timeout; BTTestServer spun for ever."""
        t = (HW / 'ngage.c').read_text()
        self.assertIn('{ 0x0c150570, 0x00800000,', t)


class Keys(unittest.TestCase):
    def table(self):
        t = (HW / 'ngage_keys.c').read_text().split('static const NgageKey rm51_keys[] = {', 1)[1].split('};', 1)[0]
        return {k: (int(r), int(c)) for k, r, c in re.findall(r'\{ KEY_(\w+), "[^"]*", (-?\d+), (-?\d+) \}', t)}

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rm51_keys_match_scan_table(self):
        """Stall 5: the 6600 layout put OK on the 3230's Up. Every key's position must give its scan code in
        ekeyb's 6x6 table (0x503905F0, row * 6 + line, byte 0 of each word)."""
        rom = ROM.read_bytes()
        want = {'UP': 0x10, 'DOWN': 0x11, 'LEFT': 0x0E, 'RIGHT': 0x0F, 'F1': 0xA4, 'F2': 0xA5, 'ENTER': 0xA7,
                'F3': 0xC4, 'F4': 0xC5, 'F5': 0xB4, 'BACKSPACE': 0x01, 'LEFTSHIFT': 0x12, 'KPASTERISK': 0x2A,
                'F10': 0x7F, 'F6': 0xAC}
        want.update({str(i): 0x30 + i for i in range(10)})
        t = self.table()
        for k, code in want.items():
            r, c = t[k]
            self.assertEqual(rom[0x503905F0 - B + (r * 6 + c) * 4], code, k)


if __name__ == '__main__':
    unittest.main()
