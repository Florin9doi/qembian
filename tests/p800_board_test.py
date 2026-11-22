"""Checks on the Sony Ericsson P800 board (-M p800: src/qemu-ngage/hw/arm/p910i*.c with p910_phone_p800,
p800_nor.c) that need no emulator run. Each regression test is named after the stall it guards
(docs/devices/se-p800/bring-up.md).

Run: python3 tests/p800_board_test.py
ROM checks are skipped when the (git-ignored) core ROM is not extracted.
"""
import re
import struct
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
TABLE = REPO / 'src/qemu-ngage/p800-ose-replies.txt'
ROM = REPO / 'eka1/p800/core/ROM.bin'
ROM910 = REPO / 'eka1/p910i/core/ROM.bin'
B = 0x50000000


def body(path, start):
    return path.read_text().split(start, 1)[1].split('\n}\n', 1)[0]


class Display(unittest.TestCase):
    def test_p800_dma_completes_after_enable(self):
        """Stall 2: the panel driver takes the DMA completion only after it moved to state 4; a channel
        finished inside the Config write was dropped and popcorn waited for ever. The P800 finishes
        later from a timer; the P910i keeps finishing at once."""
        run = body(HW / 'p910i_lcd.c', 'static void dma_run(P910Lcd *l, int n)')
        self.assertIn('if (!p910_phone->doc)', run)
        self.assertIn('timer_mod(l->dma_timer', run)
        # the immediate path (P910i) still disables the channel and raises terminal count
        tail = run.split('return;', 1)[1]
        self.assertIn('l->tc_raw |= 1u << n;', tail)
        write = body(HW / 'p910i_lcd.c', 'static void dma_write(void *opaque, hwaddr a, uint64_t val, unsigned size)')
        self.assertIn('!(l->dma_pending & (1u << n))', write)     # no restart of a pending channel

    def test_p800_rgb444(self):
        """Stall 3: white showed cyan: the P800's frame buffer is RGB444 in 16 bits."""
        text = (HW / 'p910i_lcd.c').read_text()
        self.assertIn('(p >> 8) & 15', text)


class Nor(unittest.TestCase):
    SRC = HW / 'p800_nor.c'

    def test_32bit_store_is_two_cycles(self):
        """medint issues (count << 16) | 0xE8 and 0x00D00060 as single 32-bit stores [V]."""
        w = body(self.SRC, 'static void p800_nor_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)')
        self.assertIn('p800_nor_cycle(s, addr, val & 0xffff);', w)
        self.assertIn('p800_nor_cycle(s, addr + 2, val >> 16);', w)

    def test_cfi_values(self):
        """The CFI indices medint reads (docs/research/p800-storage.md): block size 128 KB matches the LFFS
        block headers; 0x36 bit 5 clear (no unlock-all loop)."""
        cfi = dict((int(i, 16), int(v, 16)) for i, v in
                   re.findall(r'case (0x[0-9a-f]+): return (0x[0-9a-f]+);', body(self.SRC, 'static uint16_t p800_nor_cfi(unsigned idx)')))
        self.assertEqual((cfi[0x2a], cfi[0x2d], cfi[0x2f], cfi[0x30], cfi[0x36]), (5, 0x7f, 0, 2, 0))
        self.assertEqual(cfi[0x2f] | cfi[0x30] << 8, 0x20000 // 256)

    def test_irq_lowered_by_clear_status_and_read_array(self):
        c = body(self.SRC, 'static void p800_nor_cycle(P800Nor *s, uint32_t off, uint16_t v)')
        for cmd in ('0xff', '0x50'):
            part = c.split('case %s:' % cmd, 1)[1].split('break;', 1)[0]
            self.assertIn('qemu_irq_lower(s->irq);', part)

    @unittest.skipUnless((REPO / 'eka1/p800/lng.bin').exists(), 'images not extracted')
    def test_lng_is_lffs_blocks(self):
        d = (REPO / 'eka1/p800/lng.bin').read_bytes()
        self.assertEqual(len(d), 0xC00000)
        self.assertEqual(struct.unpack_from('<I', d, 12)[0], 0x20000)    # erase block size in the header [V]


class Ose(unittest.TestCase):
    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_p800_reply_numbers_match_stubs(self):
        """Stall 4: the P910i's reply 4398 -> 4F4E; the P800's stub waits for 4E86. Every reply line whose
        stub is found must name a signal the stub waits for."""
        r = subprocess.run([sys.executable, '-I', str(REPO / 'eka1/tools/swbp_waits.py'), str(REPO / 'eka1/p800/core'),
                            '--table', str(TABLE)], capture_output=True, text=True)
        self.assertNotIn('MISMATCH', r.stdout)
        self.assertIn('4398: {4E86}  table 4E86 ok', r.stdout)

    @unittest.skipUnless(ROM910.exists(), 'P910i core ROM not extracted')
    def test_tool_on_p910i_table(self):
        r = subprocess.run([sys.executable, '-I', str(REPO / 'eka1/tools/swbp_waits.py'), str(REPO / 'eka1/p910i/core'),
                            '--table', str(REPO / 'src/qemu-ngage/p910i-ose-replies.txt')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_p800_emergency_numbers_not_empty(self):
        """Stall 6: 0533/A6's list of 3-byte entries (emergency numbers [I]) must not be empty: a zero
        count crashed the P910i's Tele.app (its stall 14)."""
        line = next(l for l in TABLE.read_text().splitlines() if l.startswith('reply 0533/A6 '))
        b = [int(x, 16) for x in line.split('#')[0].split()[3:]]
        self.assertEqual(b[1], 1)           # OK
        self.assertGreater(b[4], 0)         # count
        self.assertEqual(len(b), 5 + 3 * b[4])


if __name__ == '__main__':
    unittest.main()
