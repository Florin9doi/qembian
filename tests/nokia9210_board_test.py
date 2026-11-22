"""Checks on the Nokia 9210 board (-M nokia9210: src/qemu-ngage/hw/arm/nokia9210.c, nokia9210_keys.h,
nokia9210_dataflash.h) that need no emulator run: one regression per stall
(docs/devices/nokia-9210-rae3/bring-up.md), named after it. The DataFlash protocol: tests/nokia9210_dataflash_test.c.

Run: python3 tests/nokia9210_board_test.py   (ROM checks are skipped without eka1/rae3/core/ROM.bin)
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9210.c'
KEYS = REPO / 'src/qemu-ngage/hw/arm/nokia9210_keys.h'
ROM = REPO / 'eka1/rae3/core/ROM.bin'
NEED_ROM = unittest.skipUnless(ROM.exists(), '9210 core ROM not extracted')


def func(name):
    return BOARD.read_text().split(name, 1)[1].split('\n}\n', 1)[0]


def rom_word(va):
    return struct.unpack_from('<I', ROM.read_bytes(), va - 0x50000000)[0]


class Machine(unittest.TestCase):
    def test_registered_and_built(self):
        s = BOARD.read_text()
        self.assertIn('MACHINE_TYPE_NAME("nokia9210")', s)
        self.assertIn('ARM_CPU_TYPE_NAME("ti925t")', s)
        b = (REPO / 'scripts/build-qemu-ngage.sh').read_text()
        self.assertIn('nokia9210*.[ch]', b)
        self.assertIn('nokia9210*.c', b)

    def test_tools_know_the_model(self):
        run = (REPO / 'eka1/tools/ngage_run.py').read_text().split("'nokia9210': dict(", 1)[1].split('ttb=', 1)[0]
        self.assertIn('cur_thread=0x80007214', run)
        self.assertIn('containers=0x80000264', run)
        self.assertIn('thread_sp=0x160', run)

    @NEED_ROM
    def test_cpu_table_accepts_ti925t(self):
        """the bootstrap's CPU table: Main ID & 0xFF00FFF0 == 0x54009250 (QEMU ti925t 0x54029252)"""
        self.assertEqual((rom_word(0x50000a74), rom_word(0x50000a78)), (0xff00fff0, 0x54009250))
        self.assertEqual(0x54029252 & 0xff00fff0, 0x54009250)


class Rae3FlashProbe(unittest.TestCase):
    """Stall rae3_error_led: bank 0 must answer Intel ID 0x88640089 (the probe's r6), else every chip select
    must be a 0x8896 part and the bootstrap blinked its error LED."""

    def test_bank0_device(self):
        self.assertRegex(func('static void nokia9210_init('),
                         r'rae3_flash\(sys, RAE3_BANK0, .*0x8864, "nokia9210.flash0"\)')

    @NEED_ROM
    def test_rom_probe_literals(self):
        self.assertEqual(rom_word(0x50000b6c), 0x88640089)     # r6: bank 0 ends the probe
        self.assertEqual(rom_word(0x50000b68), 0x88960089)     # r3: the per-bank check

    def test_intel_commands(self):
        w = func('static void rae3_flash_write(')
        for c in ('0x90', '0xff', '0x40', '0x20', '0x50', '0x70', '0xb0', '0xd0'):
            self.assertIn('case %s' % c, w)


class Rae3Ram(unittest.TestCase):
    """Stall rae3_ram_16mb: the bootstrap stacks at 0x03FFFFF0 and sizes RAM at 0x03800000: 16 MB."""

    def test_ram(self):
        self.assertIn('default_ram_size = 16 * MiB', BOARD.read_text())
        self.assertIn('#define RAE3_RAM_BASE   0x03000000u', BOARD.read_text())


class Rae3RomOnCs2(unittest.TestCase):
    """Stall rae3_rom_cs2: ROM 0x800000.. is at physical 0x02000000 + offset (the kernel mapped 0x50BCF000 to
    0x023CF000); chip select 2 is 8 MB (the cellular PMM at its top, VA 0x5A040000 -> 0x023F0000)."""

    def test_cs2(self):
        self.assertRegex(func('static void nokia9210_init('),
                         r'rae3_flash\(sys, 0x02000000, \(uint8_t \*\)rom \+ RAE3_BANK0_SIZE, len - RAE3_BANK0_SIZE, 8 \* MiB')


class Rae3Interrupts(unittest.TestCase):
    """Stalls rae3_pmm_erase_suspend (pending = raw & enabled), rae3_tick_55_56 and rae3_tick_stops (+0x14)."""

    def test_pending_is_masked(self):
        self.assertIn('case 0x10: return pend;', func('static uint64_t rae3_intc_read('))

    def test_gpt_drives_55_and_56(self):
        s = BOARD.read_text()
        self.assertIn('rae3_timers[i].source = i < 5 ? i : 55;', s)
        self.assertIn('rae3_set_source(56, true);', func('static void rae3_timer_expire('))

    def test_next_period_register(self):
        self.assertIn('t->reg[4] = t->reg[5];', func('static void rae3_timer_expire('))
        self.assertIn('case 5:', func('static void rae3_timer_write('))

    @NEED_ROM
    def test_rom_tick_handler_writes_0x14(self):
        self.assertEqual(rom_word(0x5000395c), 0x16014)


class Rae3Ctsi(unittest.TestCase):
    """Stall rae3_ctsi_storm: the CTSI interrupt follows +0x24 & ~+0x2C (the handler's own loop condition)."""

    def test_level(self):
        self.assertIn('(rae3_ctsi.reg[0x04] & ~rae3_ctsi.reg[0x0c]) != 0', func('static void rae3_ctsi_update('))


class Rae3LevelsAtBoot(unittest.TestCase):
    """Stalls rae3_file_server_waits (lids and card door) and rae3_format_ready: the levels the board holds."""

    def test_sources(self):
        s = BOARD.read_text()
        for n in (10, 11, 28, 30, 51):
            self.assertIn('rae3_set_source(%d, true);' % n, s)
        self.assertNotIn('rae3_set_source(12, true);', s)       # the card door closed made the boot wait


class Rae3Dsp(unittest.TestCase):
    """Stall rae3_dsp_boot: each boot-loader handshake is answered at once and a version is left."""

    def test_handshake(self):
        w = func('static void rae3_api_write(')
        self.assertIn("stw_le_p(rae3_api + (a == 0x100 ? 0xfe : 0x100), 1);", w)
        self.assertIn('stw_le_p(rae3_api + 0x002, 0x0100);', w)


class Rae3Keys(unittest.TestCase):
    """The key map: each host key's (column, row) gives its scan code in EKeyb's table (0x508A17F1)."""

    def entries(self):
        return [(k, int(c), int(r), sc) for k, c, r, sc in
                re.findall(r"\{ KEY_\((\w+)\), (\d+), (\d+), ('.'|0x[0-9a-f]+) \}", KEYS.read_text())]

    def test_macro_parameter_is_no_key(self):
        """Regression rae3_keys_macro: the keys Q and K once collided with the macro parameter name."""
        self.assertIn('#define RAE3_KEYS(KEY_)', KEYS.read_text())
        self.assertGreaterEqual(len(self.entries()), 60)

    def test_ok_is_f1(self):
        self.assertIn(('F1', 1, 3, '0xa4'), self.entries())

    @NEED_ROM
    def test_scan_codes_match_the_rom(self):
        table = ROM.read_bytes()[0x508a17f1 - 0x50000000:][:100]
        for k, c, r, sc in self.entries():
            v = ord(sc[1]) if sc.startswith("'") else int(sc, 16)
            self.assertEqual(table[c * 10 + r], v, 'KEY %s (%d, %d)' % (k, c, r))


if __name__ == '__main__':
    unittest.main()
