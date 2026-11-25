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
ROM_I = REPO / 'eka1/rae5/core/ROM.bin'          # the 9210i (RAE-5) flash image, rae3_extract.py --rae5
NEED_ROM_I = unittest.skipUnless(ROM_I.exists(), '9210i flash image not extracted')


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


class Rae5PmmUnlockRunsFromSameBank(unittest.TestCase):
    """Crash rae5_pmm_unlock_nop_sled (9210i): cmt.ext's PMM set-up (0x50DC3AEC..0x50DC3B0E) writes 0x60, 0xD0, 0xFF
    to each 8 KB block of bank 1 0x7F0000..0x7FE000 while running from bank 1. With one command mode per chip,
    the fetches after 0xD0 read status 0x0080 (Thumb lsls: a NOP sled) up to 0x51000000, a prefetch abort.
    The flash is read-while-write: a command switches only its own partition."""

    def test_mode_is_per_partition(self):
        s = BOARD.read_text()
        self.assertIn('Rae3FlashMode mode[RAE3_FLASH_NPART];', s)
        self.assertIn('#define RAE3_FLASH_PART  0x80000u', s)
        r = func('static uint64_t rae3_flash_read(')
        self.assertIn('f->mode[off / RAE3_FLASH_PART]', r)
        w = func('static void rae3_flash_write(')
        self.assertIn('&f->mode[off / RAE3_FLASH_PART]', w)
        self.assertNotRegex(w, r'f->mode\s*=')                # no chip-wide mode switch left

    def test_romd_only_when_every_partition_reads_array(self):
        w = func('static void rae3_flash_write(')
        self.assertIn('romd &= rae3_flash_reads_array(f->mode[i]);', w)

    def test_top_parameter_blocks_are_8k(self):
        """the PMM steps by 8 KB in the chip's top 64 KB: a 64 KB erase there would wipe its neighbours"""
        b = func('static uint32_t rae3_flash_block(')
        self.assertIn('f->size - RAE3_FLASH_BLOCK ? RAE3_FLASH_PARAM : RAE3_FLASH_BLOCK', b)
        self.assertIn('#define RAE3_FLASH_PARAM 0x2000u', BOARD.read_text())

    def test_partition_model(self):
        """the same rule as the C code: the unlock's partition is not the one the code runs from"""
        part = 0x80000
        code, pmm = 0x50dc3af6 - 0x50000000 - 0x800000, 0x7f0000      # bank 1 offsets
        self.assertNotEqual(code // part, pmm // part)

    @NEED_ROM_I
    def test_rom_unlock_loop(self):
        rom = ROM_I.read_bytes()
        h = lambda va: struct.unpack_from('<H', rom, va - 0x50000000)[0]
        self.assertEqual(h(0x50dc3aec), 0x2560)                  # movs r5, #0x60
        self.assertEqual(h(0x50dc3aee), 0x23d0)                  # movs r3, #0xD0
        self.assertEqual(h(0x50dc3af2), 0x0371)                  # lsls r1, r6, #13: 8 KB steps
        self.assertEqual(h(0x50dc3af6), 0x8005)                  # strh r5 (0x60)
        self.assertEqual(h(0x50dc3afc), 0x8003)                  # strh r3 (0xD0)
        self.assertEqual(h(0x50dc3b02), 0x20ff)                  # movs r0, #0xFF
        self.assertEqual(h(0x50dc3b0c), 0x2e06)                  # cmp r6, #6: 7 blocks


class Rae5DocStandin(unittest.TestCase):
    """Stall rae5_doc_c_mount (9210i): EFile's C: is a DiskOnChip Millennium behind TrueFFS 4.x in Medata.pdd
    ("Media.DiskOnChip"), which the board does not model. H-rae5-doc: TrueFFS's flCall (0x5008C730, ARM) is
    replaced by doc_standin_rae5.S, which asks the block device at 0x0503F000 (VA 0x5803F000) for sectors."""

    STUB = REPO / 'src/qemu-ngage/nos-standin/doc_standin_rae5.S'
    HDR = REPO / 'src/qemu-ngage/hw/arm/ngage_rae5_doc_stub.h'

    def test_stub_at_flcall_and_fits(self):
        h = self.HDR.read_text()
        self.assertIn('#define NGAGE_RAE5_DOC_NOS_STUB_VA 0x5008c730', h)
        n = int(re.search(r'ngage_rae5_doc_nos_stub\[(\d+)\]', h).group(1))
        self.assertLessEqual(n, 0x5008ca84 - 0x5008c730)         # flCall's body ends at the DOC init
        self.assertIn('build doc_standin_rae5.S  0x5008c730 ngage_rae5_doc',
                      (REPO / 'src/qemu-ngage/nos-standin/build.sh').read_text())

    def test_stub_function_codes(self):
        """4 mount, 0x12 info, 5 read, 6 write [V the wrapper's callers]; only 0x12, 5, 6 do work"""
        a = self.STUB.read_text()
        for code in ('#0x12', '#5', '#6'):
            self.assertIn('cmp     r1, %s' % code, a)
        self.assertNotIn('#0x38', a)       # info: word 0 only (the caller's buffer is 44 bytes, P910i cleared 56)

    def test_blk_address_agrees(self):
        """the stub's VA is the board's physical address in the kernel's peripheral window (0x05000000 -> 0x58000000)"""
        va = int(re.search(r'\.equ\s+BLK,\s+(0x[0-9a-f]+)', self.STUB.read_text()).group(1), 16)
        pa = int(re.search(r'#define RAE5_BLK_BASE\s+(0x[0-9a-f]+)u', BOARD.read_text()).group(1), 16)
        self.assertEqual(va - 0x58000000, pa - 0x05000000)
        self.assertLess(pa - 0x05000000, 0x40000)              # inside the 256 KB window

    def test_patch_checks_old_code_only_on_9210i(self):
        p = func('static void rae5_doc_patch(')
        self.assertIn('0xf0, 0x40, 0x2d, 0xe9', p)              # stmdb sp!, {r4-r7, lr}
        self.assertIn('memcmp(p, old, sizeof(old))', p)
        self.assertRegex(func('static void nokia9210_init('), r'if \(is_i\) \{\s+rae5_doc_patch\(')

    def test_dataflash_does_not_share_the_flash_file(self):
        """on the 9210i flash=FILE is the DiskOnChip C:; the DataFlash writing it too would corrupt it"""
        self.assertIn('if (rae3_flash_file && !rae3_is_i)', func('static void rae3_dataflash_init('))

    @NEED_ROM_I
    def test_rom_flcall_and_callers(self):
        rom = ROM_I.read_bytes()
        w = lambda va: struct.unpack_from('<I', rom, va - 0x50000000)[0]
        self.assertEqual([w(0x5008c730 + 4 * i) for i in range(4)],
                         [0xe92d40f0, 0xe1a06000, 0xe1a07001, 0xe1a04002])
        self.assertEqual(w(0x5008d220), 0xe3a01004)              # mount: mov r1, #4
        self.assertEqual(w(0x5008d254), 0x03a01012)              # moveq r1, #0x12 (volume info)
        self.assertEqual(w(0x5008d268), 0x059d3018)              # ldreq r3, [sp, #0x18]: irData word 0
        self.assertEqual(w(0x5008d26c), 0x01a03483)              # moveq r3, r3, lsl #9: sectors -> bytes
        self.assertEqual(rom.find('Media.DiskOnChip'.encode('utf-16le')) + 0x50000000, 0x50098648)


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
