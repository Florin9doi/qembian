"""Checks on the Nokia N90 board (-M nokian90: src/qemu-ngage/hw/arm/nokia9500.c rm1_board_init + rm1_patches_n90,
the 6630's nokia6630_soc.c devices + the OMAP1710 SPI, src/qemu-ngage/isi-replies-rm42.txt) that need no emulator
run. One regression test per stall fixed (docs/devices/nokia-n90-rm42/bring-up.md), named after it. The display
core's own cases (controller mode, pixel formats) are in tests/nokia6630_lcd_ssi_test.c.

Run: python3 tests/nokian90_board_test.py
The ROM checks are skipped when the (git-ignored) inflated core ROM eka1/rm42/core/ROM.bin is not there.
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9500.c'
SOC = REPO / 'src/qemu-ngage/hw/arm/nokia6630_soc.c'
ROM = REPO / 'eka1/rm42/core/ROM.bin'
NEED_ROM = unittest.skipUnless(ROM.exists(), 'N90 core ROM not inflated')


def func(path, name):
    return path.read_text().split(name, 1)[1].split('\n}\n', 1)[0]


def rom_word(va):
    return struct.unpack_from('<I', ROM.read_bytes(), va - 0x50000000)[0]


def rom_utf16(va, n):
    return ROM.read_bytes()[va - 0x50000000:va - 0x50000000 + 2 * n].decode('utf-16le')


class Machine(unittest.TestCase):
    """Stall 1: -M nokian90 is the 6630 board with the N90's phone description (OMAP1710, 352x416 controller)."""

    def test_registered(self):
        s = BOARD.read_text()
        self.assertIn('MACHINE_TYPE_NAME("nokian90")', s)
        self.assertIn('type_register_static(&nokian90_type);', s)
        self.assertRegex(s, r'rm1_phone_n90 = \{ "nokian90", "eka1/rm42/core", rm1_patches_n90, true, 352, 416, true \}')

    def test_ram_covers_the_bank(self):
        """the bootstrap's memory table (ROM +0xD04): 64 MB at 0x10100000, as the N70's"""
        body = func(BOARD, 'static void nokian90_class_init(ObjectClass *oc, const void *data)')
        mb = int(re.search(r'default_ram_size = (\d+) \* MiB', body).group(1))
        self.assertGreaterEqual(0x10000000 + mb * 0x100000, 0x10100000 + 0x04000000)

    @NEED_ROM
    def test_rom_bank_table(self):
        self.assertEqual(struct.unpack_from('<2I', ROM.read_bytes(), 0xd04), (0x10100000, 0x04000000))

    def test_tools_know_the_model(self):
        run = (REPO / 'eka1/tools/ngage_run.py').read_text()
        m = run.split("'nokian90': dict(", 1)[1].split('ttb=', 1)[0]
        self.assertIn('fatal=0x500203a4', m)
        self.assertIn('cur_thread=0x800018c4', m)
        self.assertIn('containers=0x80000488', m)
        self.assertIn('sdram=(0x10000000, 0x08000000)', m)
        phone = (REPO / 'eka1/tools/ngage_phone.py').read_text()
        self.assertIn("'nokian90': 'Nokia N90'", phone)
        self.assertIn("(352, 416)", phone)

    @NEED_ROM
    def test_rom_fault_routine(self):
        """Kern::Fault 0x5002039c adds 0x10000 to the reason and falls into the fault routine 0x500203a4"""
        self.assertEqual(rom_word(0x5002039c), 0xe2811801)       # add r1, r1, #0x10000
        self.assertEqual(rom_word(0x500203a0), 0xeaffffff)       # b 0x500203a4


class Rm42CorePatches(unittest.TestCase):
    """The N70's DSP/secure-environment patches at RM-42's addresses (16/16 code match), old words checked."""

    PATCHES = re.compile(r'rm1_patch\(core, len, "(H-rm42-[a-z-]+)", (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\);')

    def patches(self):
        return [(h, int(v, 16), int(o, 16), int(n, 16))
                for h, v, o, n in self.PATCHES.findall(func(BOARD, 'static void rm1_patches_n90('))]

    def test_all_six_hacks(self):
        self.assertEqual({p[0] for p in self.patches()},
                         {'H-rm42-dsp-boot', 'H-rm42-dsp-timeout', 'H-rm42-bal-run', 'H-rm42-eapgwc',
                          'H-rm42-secenvinit', 'H-rm42-drmhelper'})
        self.assertEqual(len(self.patches()), 11)

    def test_same_new_words_as_the_n70(self):
        n70 = re.findall(r'rm1_patch\(core, len, "H-rm84-[a-z-]+", 0x[0-9a-f]+, (0x[0-9a-f]+), (0x[0-9a-f]+)\);',
                         func(BOARD, 'static void rm1_patches_n70('))
        self.assertEqual([(int(o, 16), int(n, 16)) for o, n in n70], [(o, n) for _, _, o, n in self.patches()])

    @NEED_ROM
    def test_old_words_in_rom(self):
        for hack, va, old, new in self.patches():
            self.assertEqual(rom_word(va), old, '%s %08x' % (hack, va))


class SPI(unittest.TestCase):
    """The OMAP1710 SPI block (0xFFFB0C00) espi.dll drives; the N70 has no such driver."""

    def body(self):
        return SOC.read_text().split('OMAP1710 SPI (0xFFFB0C00', 1)[1].split('32 kHz watchdog', 1)[0]

    def test_rm42_spi_reset_failed(self):
        """Stall 2: espi polls SYSSTATUS (+0x14) bit 0 fifty times after a soft reset, else Kern::Fault
        "Hw-error:SPI reset failed" in the extension start-up (1.5 s in)."""
        self.assertRegex(func(SOC, 'static uint64_t spi_read('), r'case 0x14: return 1;')
        self.assertIn('rm1_spi_init(sys, mpu);', func(BOARD, 'static void rm1_board_init('))
        self.assertIn('0xfffb0c00', func(SOC, 'void rm1_spi_init('))

    @NEED_ROM
    def test_rom_spi_registers(self):
        self.assertEqual(rom_word(0x5003f79c), 0x580b0c10)       # SYSCONFIG
        self.assertEqual(rom_word(0x5003f7a0), 0x580b0c14)       # SYSSTATUS
        self.assertEqual(rom_utf16(0x50041058, 25), 'Hw-error:SPI reset failed')

    def test_rm42_spi_layout(self):
        """Stall 3 (the McSPI layout was wrong): CTRL 0x2C bit 1 = write, 0 = read; STATUS 0x30 and ISR 0x18 bit 1 =
        write done, bit 0 = read done; IER 0x1C; TX 0x34, RX 0x38; Lv2_SPI = level-2 line 49."""
        b = self.body()
        self.assertIn('qdev_get_gpio_in(mpu->ih[1], 49)', b)
        self.assertIn('s->r[0x30 / 4] |= 2;', b)
        self.assertIn('s->r[0x18 / 4] |= 2;', b)
        self.assertIn('s->r[0x38 / 4] = rm1_spi_answer(', b)
        self.assertIn('(s->r[0x18 / 4] & s->r[0x1c / 4] & 0xf)', b)

    @NEED_ROM
    def test_rom_kernel_spi_accessors(self):
        """ekern's accessors espi imports: add r0, r0, #offset (CTRL 0x2C write, STATUS 0x30 read, TX 0x34, RX 0x38)"""
        for va, off in ((0x5000f7a8, 0x2c), (0x5000f7d8, 0x30), (0x5000f7e0, 0x34), (0x5000f7e8, 0x38),
                        (0x5000f7f0, 0x18), (0x5000f7f8, 0x1c), (0x5000f778, 0x24)):
            self.assertEqual(rom_word(va), 0xe2800000 | off, hex(va))

    def test_rm42_spi_tx_dma(self):
        """Stall 4: the cover display's frame goes in DMA mode (SET1 bit 5): channel 0 with sync 1 waits for the
        SPI's request; nothing raised it and the LCD driver never read the panel ID."""
        b = self.body()
        self.assertIn('s->drq_tx = mpu->drq[1];', b)
        self.assertIn('(s->r[0x24 / 4] & 0x20) && (s->r[0x2c / 4] & 2)', b)

    def test_rm42_lcd_panel_type_zero(self):
        """Stall 4's symptom: "LCD.LDD panic: " 0x1000011 in EwSrv (panel type 0). RDDID must answer an ID
        genericlcd_lcd.ldd accepts (0x5004C4C8: low byte 0x15 or 0x31, bits 16-23 0x10 or 0x29)."""
        v = int(re.search(r'#define RM1_SPI_ID (0x[0-9a-f]+)u', SOC.read_text()).group(1), 16)
        self.assertIn(v & 0xff, (0x15, 0x31))
        self.assertIn((v >> 16) & 0xff, (0x10, 0x29))
        self.assertIn('return last_cmd == 0x04 ? RM1_SPI_ID : 0;', SOC.read_text())

    def test_rm42_spi_dma_words_masked(self):
        """DMA elements are 32 bits: a 9-bit word arrived as 0x5010 / 0x3D09; the word is cut to its length."""
        self.assertIn('tx &= bits >= 32 ? 0xffffffffu : (1u << bits) - 1;', self.body())


class Display(unittest.TestCase):
    """Stalls 5-7: the 352x416 display behind a controller (nokia6630_lcd.h "Controller mode")."""

    def test_rm42_hires_console(self):
        s = SOC.read_text()
        self.assertIn('qemu_console_resize(s->con, rm1_lcd_w(&s->l), rm1_lcd_h(&s->l));', s)
        self.assertIn('rm1_display_init(sys, mpu, ph->lcd_w, ph->lcd_h, ph->lcd_ctrl);', BOARD.read_text())
        lcd = (REPO / 'src/qemu-ngage/hw/arm/nokia6630_lcd.h').read_text()
        self.assertIn('#define RM1_LCD_MAX_W 352', lcd)
        self.assertIn('#define RM1_LCD_MAX_H 416', lcd)

    @NEED_ROM
    def test_rom_driver_sets_352x416(self):
        """genericlcd_lcd.ldd's set-up 0x5004E094: mov r0, #0x160 (352) and #0x1A0 (416) for registers 16 and 1A"""
        b = ROM.read_bytes()[0x4e094:0x4e100]
        words = struct.unpack('<%dI' % (len(b) // 4), b)
        self.assertIn(0xe3a00e16, words)                           # mov r0, #0x160
        self.assertIn(0xe3a00e1a, words)                           # mov r0, #0x1a0


if __name__ == '__main__':
    unittest.main()
