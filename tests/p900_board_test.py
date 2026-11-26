"""Checks on the Sony Ericsson P900 board (-M p900: src/qemu-ngage/hw/arm/p910i*.c with p910_phone_p900,
docs/devices/se-p900/bring-up.md): the P910i's DiskOnChip stand-in at the P900's addresses, the P800's
display generation, and the ported kernel words.

Run: python3 -m unittest -v tests/p900_board_test.py
ROM checks are skipped when the (git-ignored) images are not extracted (eka1/p900/, bring-up.md "Files").
"""
import re
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
ROM = REPO / 'eka1/p900/core/ROM.bin'
C_IMG = REPO / 'eka1/p900/c.img'
BASE = 0x50000000
PROLOGUE = bytes.fromhex('f0b557464e464546')
DISP_OLD = PROLOGUE + bytes.fromhex('e0b487b080460591')

sys.path.insert(0, str(REPO / 'eka1/tools'))
import ngage_run as nr  # noqa: E402


def phone():
    board = (HW / 'p910i.c').read_text()
    m = re.search(r'p910_phone_p900 = \{(.*?)\};', board, re.S)
    return {k: v for k, v in re.findall(r'\.(\w+) = (0x[0-9a-f]+|true|false|"\w+")', m.group(1))}


class Board(unittest.TestCase):
    def test_p900_is_doc_with_late_dma_rgb565(self):
        """The P900 has the P910i's DiskOnChip (udiskonchip.dll) and the P800's videodriver.dll (DMA finishes
        late, H-p800-dma); its screen is RGB565 (the logo's colours were right), not the P800's RGB444."""
        p = phone()
        self.assertEqual(p['doc'], 'true')
        self.assertEqual(p['dma_late'], 'true')
        self.assertNotIn('rgb444', p)

    def test_machine_registered_16mb(self):
        board = (HW / 'p910i.c').read_text()
        self.assertIn('type_register_static(&p900_type);', board)
        body = board.split('static void p900_class_init', 1)[1].split('}', 1)[0]
        self.assertIn('default_ram_size = 16 * MiB', body)

    def test_standin_addresses_per_phone(self):
        """The stand-in once had the P910i's addresses built in; the P900's udiskonchip sits 0x114C lower."""
        board = (HW / 'p910i.c').read_text()
        self.assertIn('p910_patch(nor, ph->doc_dispatch', board)
        self.assertNotIn('NGAGE_P910_DOC_NOS_STUB_VA', board.split('static void p910_doc_standin', 1)[1].split('\n}\n')[0])

    @unittest.skipUnless(ROM.exists(), 'P900 ROM not extracted')
    def test_standin_addresses_hold_the_expected_code(self):
        rom = ROM.read_bytes()
        p = phone()
        at = lambda va, n: rom[va - BASE:va - BASE + n]
        self.assertEqual(at(int(p['doc_dispatch'], 16), 16), DISP_OLD)
        self.assertEqual(at(int(p['doc_filter'], 16), 8), PROLOGUE)
        self.assertEqual(at(int(p['doc_exit'], 16), 8), PROLOGUE)
        self.assertEqual(rom.count(DISP_OLD), 1)

    @unittest.skipUnless(ROM.exists(), 'P900 ROM not extracted')
    def test_bootstrap_table_16mb_sdram(self):
        rom = ROM.read_bytes()
        self.assertEqual(struct.unpack_from('<4I', rom, 0x314 + 16), (0x2009, 0, 0xD0000000, 0x01000000))
        self.assertEqual(nr.MODELS['p900']['sdram'], (0xD0000000, 0x01000000))

    @unittest.skipUnless(ROM.exists(), 'P900 ROM not extracted')
    def test_panic_is_the_p910i_body(self):
        rom = ROM.read_bytes()
        va = nr.MODELS['p900']['panic']
        self.assertEqual(rom[va - BASE:va - BASE + 32].hex(),
                         '00402de910d04de2003090e500308de504208de508108de52f00a0e30d10a0e1')

    def test_ttb_not_the_bootstrap_guess(self):
        """dumpx read garbage with 0xD0024000 (the P800's value); the page directory is at 0xD0028000 [V]."""
        self.assertEqual(nr.MODELS['p900']['ttb'], 0xD0028000)

    @unittest.skipUnless(C_IMG.exists(), 'eka1/p900/c.img not extracted')
    def test_c_img_whole_sectors(self):
        """The P910i's c.img once lost 3 bytes and the board refused it; the P900's must stay whole."""
        self.assertEqual(C_IMG.stat().st_size % 512, 0)
        boot = C_IMG.read_bytes()[:64]
        self.assertEqual(boot[0], 0xE9)
        self.assertEqual(boot[3:7], b'EPOC')


if __name__ == '__main__':
    unittest.main()
