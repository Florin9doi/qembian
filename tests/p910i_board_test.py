"""Tests for the Sony Ericsson P910i board (-M p910i, src/qemu-ngage/hw/arm/p910i*.c) and its stand-ins.

Run: python3 -m unittest -v tests/p910i_board_test.py
The ROM checks are skipped when the (git-ignored) core ROM is not extracted (eka1/p910i/core/ROM.bin,
docs/devices/se-p910i/bring-up.md "Files").
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ARM = REPO / 'src/qemu-ngage/hw/arm'
STUB_SRC = REPO / 'src/qemu-ngage/nos-standin/doc_standin_p910.S'
STUB_HDR = ARM / 'ngage_p910_doc_stub.h'
ROM = REPO / 'eka1/p910i/core/ROM.bin'
BASE = 0x50000000

DISPATCH = 0x500A27C4            # udiskonchip's TrueFFS dispatcher [V]
DISPATCH_END = 0x500A2F4E        # the next function starts at 0x500A2F50 [V]
PROLOGUE = bytes.fromhex('f0b557464e464546')


def stub():
    text = STUB_HDR.read_text()
    va = int(re.search(r'NGAGE_P910_DOC_NOS_STUB_VA (0x[0-9a-f]+)', text).group(1), 16)
    body = text.split('{', 1)[1].split('}', 1)[0]
    return va, bytes(int(b, 16) for b in re.findall(r'0x([0-9a-f]{2})', body))


class DocStandin(unittest.TestCase):
    def test_stub_replaces_the_dispatcher_from_thumb(self):
        """The dispatcher is reached with BL from Thumb code: the stub starts with `bx pc; nop`."""
        va, code = stub()
        self.assertEqual(va, DISPATCH)
        self.assertEqual(code[:4], bytes.fromhex('7847c046'))

    def test_stub_fits_before_the_next_function(self):
        va, code = stub()
        self.assertLessEqual(va + len(code), DISPATCH_END)

    def test_doc_blk_user_mapping(self):
        """Regression doc_blk_kernel_only_mapping: udiskonchip runs in EFile's thread in user mode; the
        stand-in first wrote its block device through 0x5C001800 (the window's kernel-only mapping),
        then 0x58000F00 (the kernel peripheral page): a data abort both times. TrueFFS itself uses the
        user-accessible alias 0x5E000000 [V]."""
        blk = int(re.search(r'\.equ\s+BLK,\s+(0x[0-9a-f]+)', STUB_SRC.read_text()).group(1), 16)
        self.assertEqual(blk & 0xFFFFE000, 0x5E000000)
        soc = (ARM / 'p910i_soc.c').read_text()
        self.assertIn('#define DOC_BLK   (DOC_BASE + 0x1800)', soc)
        self.assertEqual(blk & 0x1FFF, 0x1800)

    def test_defragment_nothing_to_do(self):
        """Regression docbgth_busy_loop: 0x32F (defragment) answered 0, so ELocal's DOCBGTH thread called
        it again at once for ever (Unlock -> defragment -> Lock) and starved the window server: the UI
        stopped at the start-up logo. 63 becomes -4 in the media driver (0x500AFBBA) and the thread sleeps."""
        src = STUB_SRC.read_text()
        self.assertRegex(src, r'ldr\s+r2, =0x32f\s')
        self.assertRegex(src, r'moveq\s+r0, #63')

    @unittest.skipUnless(ROM.exists(), 'P910i ROM not extracted')
    def test_old_code_matches_the_rom(self):
        rom = ROM.read_bytes()
        at = lambda va, n: rom[va - BASE:va - BASE + n]
        self.assertEqual(at(DISPATCH, 16), PROLOGUE + bytes.fromhex('e0b487b0804605 91'.replace(' ', '')))
        self.assertEqual(at(0x500A4868, 8), PROLOGUE)    # TrueFFS FAT filter
        self.assertEqual(at(0x500A31D8, 8), PROLOGUE)    # flExit-like

    def test_board_checks_old_code(self):
        board = (ARM / 'p910i.c').read_text()
        self.assertIn('memcmp(p, old, oldlen)', board)
        self.assertIn('p910_doc_standin(memory_region_get_ram_ptr(flash))', board)


class Soc(unittest.TestCase):
    def test_ic_level_irq_not_latched(self):
        """Regression ic_level_irq_latched: the interrupt controller latched every rising edge until a
        write to +04; elh4drv's UART interrupt is a level the kernel never acknowledges there, so its
        ISR ran for ever (IIR 0xC1). Only lines in the edge-select register (+0x10, the kernel writes
        6 = HsTimer0/1) latch."""
        soc = (ARM / 'p910i_soc.c').read_text()
        self.assertIn('uint32_t edge = s->reg[(0x210 + n * 0x100) >> 2];', soc)
        self.assertIn('((s->latch[n] & edge) | (level & ~edge))', soc)

    def test_sctlr_te_patch(self):
        """Regression sctlr_te_on_arm926: the bootstrap ORs 0x40001000 into SCTLR; QEMU took bit 30 as
        TE and took every exception in Thumb state (undefined-instruction loop at 0xFFFF0004)."""
        p = (REPO / 'patches/qemu-ngage/0018-arm-sctlr-te-v6t2-only.patch').read_text()
        self.assertIn('if (arm_feature(env, ARM_FEATURE_THUMB2)) {', p)
        self.assertIn('env->thumb = 0;', p)

    @unittest.skipUnless(ROM.exists(), 'P910i ROM not extracted')
    def test_bootstrap_sets_sctlr_bit30(self):
        rom = ROM.read_bytes()
        self.assertEqual(struct.unpack_from('<I', rom, 0x944)[0], 0x40001000)


class GsmLink(unittest.TestCase):
    """The link protocol itself is tested in tests/p910i_gsm_peer_test.c."""

    def test_startsignal_in_table(self):
        """EStart waits for OSE signal 0xC384, sent unasked after sync [V by run]."""
        t = (REPO / 'src/qemu-ngage/p910i-ose-replies.txt').read_text()
        self.assertRegex(t, r'(?m)^unasked \d+ C384 00$')

    def test_iir_60_byte_trap(self):
        """elh4drv reads exactly 60 bytes on IIR 0x04 without checking LSR: 0x04 only with >= 60 queued."""
        g = (ARM / 'p910i_gsm.c').read_text()
        self.assertIn('n >= 60', g)

    def test_phone_bytes_flushed_by_timer(self):
        """Regression gsm_log_not_flushed: the phone's 0x4398 frame stayed in the log buffer (flushed
        only on an LSR read), so the link looked silent after the StartSignal."""
        g = (ARM / 'p910i_gsm.c').read_text()
        self.assertIn('timer_mod(g->flush, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 5);', g)


class I2c(unittest.TestCase):
    def test_i2c_transfer_completes(self):
        """Regression esdrv_owner_null: no I2C controller answered the codec init, so Esdrv's state
        objects never got their owner and POPCORN's first write aborted the kernel (0x500CE004)."""
        c = (ARM / 'p910i_i2c.c').read_text()
        self.assertIn('#define I2C_BASE 0x58000800', c)
        self.assertIn('#define I2C_IRQ  17', c)
        self.assertIn('if (v & 0x200) {', c)
        self.assertIn('s->sts |= 1;', c)
        self.assertIn('(s->sts & s->ctl & 5) != 0', c)


def touch_raw(px, py):
    """p910i_input.c p910_touch_raw (C integer division: both numerators are positive here)"""
    rx = ((2 * px + 1) * 32768 + 2176426 + 17451 // 2) // 17451
    ry = (22867567 - (2 * py + 1) * 32768 + 24899 // 2) // 24899
    return max(0, min(1023, rx)), max(0, min(1023, ry))


class Input(unittest.TestCase):
    def test_touch_inverse_calibration(self):
        """Every pixel's raw value maps back to that pixel through the phone's default calibration
        (Exyin, superpage 0x40000994): x = (17451*rawX - 2176426) >> 16, y = (22867567 - 24899*rawY) >> 16."""
        for px in range(208):
            for py in range(0, 320, 7):
                rx, ry = touch_raw(px, py)
                self.assertEqual((17451 * rx - 2176426) >> 16, px)
                self.assertEqual((22867567 - 24899 * ry) >> 16, py)

    def test_c_uses_the_same_constants(self):
        c = (ARM / 'p910i_input.c').read_text()
        self.assertIn('(int64_t)(2 * px + 1) * 32768 + 2176426 + 17451 / 2) / 17451', c)
        self.assertIn('(22867567 - (int64_t)(2 * py + 1) * 32768 + 24899 / 2) / 24899', c)

    def test_gpio_pins_have_polarity(self):
        """Jog, flip and side-key drivers flip the IC polarity (+0x0C) after every edge: pin lines
        42, 43, 54 must honour it, and the side key idles high (else it reads held at boot)."""
        soc = (ARM / 'p910i_soc.c').read_text()
        self.assertIn('uint32_t level = s->level[n] ^ (p910_pin_lines[n] & ~pol);', soc)
        inp = (ARM / 'p910i_input.c').read_text()
        self.assertIn('GPIO_JOG_A | GPIO_JOG_B | GPIO_KEY_APP | (in->flip_closed ? GPIO_FLIP : 0)', inp)

    def test_adc_read_drops_irq(self):
        """The touch ISR reads the sample three times and never masks the line: reading drops it."""
        inp = (ARM / 'p910i_input.c').read_text()
        self.assertIn('a->pending = false;', inp.split('static uint64_t adc_read', 1)[1].split('}', 2)[0] + '}')


class Uarts(unittest.TestCase):
    def test_comms_uart_lsr(self):
        """Regression socketserver_uart_spin: the comms server polls UART unit 1's LSR for "transmitter
        empty" (Euart1.pdd 0x500B84CC); on the catch-all it read 0 for ever and starved the UI on the home
        screen (no tap taken). Units 0, 1, 3, 5 are 16550s now."""
        soc = (ARM / 'p910i_soc.c').read_text()
        for unit, irq in ((0, 5), (1, 6), (3, 8), (5, 10)):
            self.assertIn('serial_mm_init(sys, UART_BASE + %d * 0x20, 2, p910_irq(%d)' % (unit, irq), soc)


class Display(unittest.TestCase):
    def test_dma_completes(self):
        """Without DMA completion the display driver's queue fills and faults ("ScreenDriver FAULT")."""
        l = (ARM / 'p910i_lcd.c').read_text()
        self.assertIn('c[4] &= ~1u;', l)
        self.assertIn('l->tc_raw |= 1u << n;', l)


if __name__ == '__main__':
    unittest.main()
