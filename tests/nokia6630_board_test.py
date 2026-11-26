"""Checks on the Nokia 6630 board (-M nokia6630: src/qemu-ngage/hw/arm/nokia9500.c rm1_board_init + rm1_patches_6630,
nokia6630_soc.c, patches/qemu-ngage/0021-0022) that need no emulator run.

Run: python3 tests/nokia6630_board_test.py   (the GP timer counting: tests/nokia6630_gpt_test.c)
The ROM checks are skipped when the (git-ignored) inflated core ROM is not there.
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9500.c'
SOC = REPO / 'src/qemu-ngage/hw/arm/nokia6630_soc.c'
PATCHES = REPO / 'patches/qemu-ngage'
ROM = REPO / 'eka1/rm1/core/ROM.bin'


def func(path, name):
    return path.read_text().split(name, 1)[1].split('\n}\n', 1)[0]


def rom_word(va):
    return struct.unpack_from('<I', ROM.read_bytes(), va - 0x50000000)[0]


class Level2Banks(unittest.TestCase):
    """Regression rm1_no_tick, part 1: the OMAP1610's level-2 handler has 4 banks (lines 32..159);
    QEMU's 1510 model had one, so the kernel's ILR/MIR writes to banks 1-3 went nowhere."""

    def test_board_uses_the_1610_init(self):
        body = func(BOARD, 'static void rm1_board_init(MachineState *machine, const Rm1Phone *ph)')
        self.assertIn('omap1610_mpu_init(', body)
        self.assertNotIn('omap310_mpu_init(', body)

    def test_patch_gives_ih2_four_banks(self):
        p = (PATCHES / '0021-omap1610-ih2-banks.patch').read_text()
        self.assertIn('qdev_prop_set_uint8(s->ih[1], "nbanks", 4)', p)
        self.assertIn('bank[4]', p)
        self.assertIn('bank_no >= s->nbanks', p)      # banks past nbanks are not array overruns

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_kernel_dispatches_128_level2_lines(self):
        """0x5000e60c..: SIR of level 2 & 0x7F, + 32 [V]."""
        self.assertEqual(rom_word(0x5000e614), 0xe200407f)   # and r4, r0, #127
        self.assertEqual(rom_word(0x5000e618), 0xe2844020)   # add r4, r4, #32


class GpTimers(unittest.TestCase):
    """Regression rm1_no_tick, part 2: the tick is GP timer 2 (0xFFFB1C00) on level-1 line 18."""

    def test_irq_lines(self):
        body = func(SOC, 'void rm1_gptimers_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu)')
        self.assertIn('qdev_get_gpio_in(mpu->ih[0], 17 + i)', body)    # GP timers 1, 2
        self.assertIn('qdev_get_gpio_in(mpu->ih[1], 32 + i)', body)    # 3..8: level 2, 34..39
        self.assertIn('0xfffb1400 + 0x800 * i', body)

    def test_twps_reads_zero(self):
        """The kernel polls TWPS (posted-write pending) after each write: it must read 0."""
        self.assertRegex(func(SOC, 'static uint64_t gpt_read('), r'case 0x34: v = 0;')

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_rom_programs_gptimer2(self):
        self.assertEqual(rom_word(0x5000e150), 0x580b1c10)   # TIOCP_CFG of GP timer 2
        self.assertEqual(rom_word(0x5000e164), 0x580b1c38)   # TMAR


class Ulpd(unittest.TestCase):
    """Regression rm1_gpio_func_clock: epower.dll faulted ("Someone has disabled GPIO func.clock !")
    because QEMU's 1510 ULPD dropped the 32-bit RMW of 0x7C (SOFT_REQ_REG2) and read back 0."""

    def test_patch_keeps_1610_registers(self):
        p = (PATCHES / '0022-omap1610-ulpd.patch').read_text()
        self.assertIn('ulpd_1610_regs[(addr - 0x54) >> 2] = value', p)
        self.assertIn('s->omap1610 && size == 4', p)

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_epower_checks_bit2_of_0x7c(self):
        self.assertEqual(rom_word(0x5003b600), 0x580e087c)
        self.assertEqual(rom_word(0x5003b5c0), 0xe2106004)   # ands r6, r0, #4


class Sossi(unittest.TestCase):
    """Regression rm1_lcd_sossi_wait: lcd.ldd spun on bit 2 of SoSSI 0x08 (no SoSSI on the 1510)."""

    def test_bit2_reads_set(self):
        body = (REPO / 'src/qemu-ngage/hw/arm/nokia6630_lcd.h').read_text().split('rm1_sossi_read', 1)[1]
        self.assertIn('return l->reg[2] | 0x0c;', body)                  # + bit 3 idle (rm1-lcd-sossi.md)



class LaterPatches(unittest.TestCase):
    def test_no_lcdc_on_1610(self):
        """0023: the panel's console is console 0 (QEMU's LCD controller would take it, never used)."""
        p = (PATCHES / '0023-omap1610-no-lcdc.patch').read_text()
        self.assertIn('if (!is_1610) {', p)
        self.assertIn('if (mpu->lcd) {', p)

    def test_wdt_32bit(self):
        """Regression rm1_watchdog_32bit: the bootstrap's 32-bit TIMER_MODE writes (watchdog off) were
        dropped; 0024 takes them as 16-bit."""
        p = (PATCHES / '0024-omap-wdt-32bit-access.patch').read_text()
        self.assertEqual(p.count('if (size == 4) {'), 2)

    def test_armv5_prefetch(self):
        """Regression rm1_secpdd_mmu_off: SecPdd turns the MMU off with 'mcr SCTLR; nop; mov pc, r1' and
        relies on the prefetched instructions; QEMU ended the TB at the mcr and fetched from PA = VA."""
        p = (PATCHES / '0025-armv5-prefetch-after-sctlr-write.patch').read_text()
        self.assertIn('crn == 1 && crm == 0 && opc1 == 0 && opc2 == 0', p)
        self.assertIn('!arm_dc_feature(s, ARM_FEATURE_V6)', p)
        self.assertIn('s->prefetch_left = 2;', p)
        self.assertIn('--dc->prefetch_left == 0', p)

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_rom_has_the_mmu_off_sequence(self):
        self.assertEqual(rom_word(0x500aade8), 0xee012f10)   # mcr p15, 0, r2, c1, c0, 0
        self.assertEqual(rom_word(0x500aadec), 0xe1a00000)   # nop
        self.assertEqual(rom_word(0x500aadf0), 0xe1a0f001)   # mov pc, r1


class Board(unittest.TestCase):
    def test_devices_mapped(self):
        body = func(BOARD, 'static void rm1_board_init(MachineState *machine, const Rm1Phone *ph)')
        for call in ('rm1_display_init(sys, mpu, ph->lcd_w, ph->lcd_h, ph->lcd_ctrl);', 'rm1_gpio_init(sys, mpu);', 'rm1_wdt_init(sys);',
                     'rm1_ssi_init(sys, mpu, ra2_isi);', 'rm1_nand_init(sys, mpu, nand, ra2_flash);'):
            self.assertIn(call, body)

    def test_ssi_gdd_base(self):
        """the GDD channels are at 0x30001800 (the PDD writes CCR at +0x1802/+0x1842) [V run]"""
        self.assertIn('if (a >= 0x1800 && a < 0x1a00) {', SOC.read_text())

    def test_isi_checkcrc_byte(self):
        t = (REPO / 'src/qemu-ngage/isi-replies-rm1.txt').read_text()
        self.assertIn('reply 00 01 02 00 1d', t)

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_rom_checkcrc_compare(self):
        half = struct.unpack_from('<H', ROM.read_bytes(), 0x142b88)[0]
        self.assertEqual(half, 0x281d)                       # Thumb cmp r0, #0x1D


class DspBoot(unittest.TestCase):
    """H-rm1-dsp-boot: the patched word is the backward branch of EHWAOMAP's DSP boot wait."""

    def test_board_patches_with_old_word(self):
        self.assertIn('rm1_patch(core, len, "H-rm1-dsp-boot", 0x50242d84, 0x1afffffb, 0xe1a00000);',
                      BOARD.read_text())

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_rom_has_the_wait(self):
        w = rom_word(0x50242d84)
        self.assertEqual(w, 0x1afffffb)
        self.assertEqual(0x50242d84 + 8 + ((w & 0xffffff) - 0x1000000) * 4, 0x50242d78)


class DspTimeout(unittest.TestCase):
    """Regression rm1_dms_timed_out: DeviceManagementServer returned -33 when EHWAOMAP's DSP request timer
    completed its request with KErrTimedOut (no DSP); it completes with KErrNone now."""

    def test_board_patches_with_old_word(self):
        self.assertIn('"H-rm1-dsp-timeout", 0x50238888, 0xe3e02020, 0xe3a02000', BOARD.read_text())

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_rom_completes_with_timed_out(self):
        self.assertEqual(rom_word(0x50238888), 0xe3e02020)      # mvn r2, #0x20 = -33
        w = rom_word(0x5023888c)                                 # bl Kern::RequestComplete-like 0x50248038
        self.assertEqual(0x5023888c + 8 + ((w & 0xffffff) << 2), 0x50248038)


class BalRun(unittest.TestCase):
    """Regression rm1_dms_not_ready: DeviceManagementServer (then EikAppUiServer) exited -18 because each DSP
    plug-in's open ends in Bal.dll ord 22, which EHWA.ldd answers KErrNotReady with no DSP; ord 22 returns 0."""

    def test_board_patches_with_old_words(self):
        b = BOARD.read_text()
        self.assertIn('"H-rm1-bal-run", 0x50285bd8, 0xe92d40f0, 0xe3a00000', b)
        self.assertIn('"H-rm1-bal-run", 0x50285bdc, 0xe24dd008, 0xe12fff1e', b)

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_ord22_is_the_export(self):
        self.assertEqual(rom_word(0x50286490 + 4 * 21), 0x50285bd8)   # Bal export directory, ordinal 22
        self.assertEqual(rom_word(0x5024adf8), 0xe3e00011)             # EHWA.ldd mvn r0, #0x11 = -18


class DspReplyWaits(unittest.TestCase):
    """Regressions rm1_dms_bal_wait / rm1_dms_gwc_reply_wait: DMS's main thread blocked in Bal ord 19 (send to
    the node and wait) and then in the plug-ins' reply loops (EapGwc, MuMdrcGwc: User::WaitForRequest for a
    DSP reply); EikAppUiServer hung behind it in CAknKeySoundSystem::NewL."""

    def test_patches(self):
        b = BOARD.read_text()
        self.assertIn('"H-rm1-bal-run", 0x50285b10, 0xe92d4030, 0xe3a00000', b)
        self.assertIn('"H-rm1-eapgwc", 0x502287b4, 0x2600d12c, 0x2600e02c', b)
        self.assertIn('"H-rm1-eapgwc", 0x50229708, 0xd1282800, 0xe0282800', b)

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_rom_loops_and_ord19(self):
        self.assertEqual(rom_word(0x50286490 + 4 * 18), 0x50285b10)   # Bal ordinal 19
        self.assertEqual(rom_word(0x502287b4), 0x2600d12c)            # EapGwc: bne ret; movs r6, #0
        self.assertEqual(rom_word(0x50229708), 0xd1282800)            # MuMdrcGwc: cmp r0, #0; bne ret


class SecEnvInit(unittest.TestCase):
    """Regression rm1_plat91_secenvinit: SecEnvInit's KERN-EXEC panic (no secure environment) became
    Kern::Fault("PLAT", 91) once its thread was system-critical; its work function returns at once."""

    def test_patch(self):
        self.assertIn('"H-rm1-secenvinit", 0x5030652c, 0x4657b5f0, 0x47702000', BOARD.read_text())

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_rom_function_start(self):
        self.assertEqual(rom_word(0x5030652c), 0x4657b5f0)       # push {r4-r7, lr}; mov r7, r10


class IsiTable(unittest.TestCase):
    def test_tsy_net_status_has_no_subblocks(self):
        """Regression rm1_tsy_user21: the N-Gage NET reply (1 sub-block) made CTelServer panic USER 21."""
        t = (REPO / 'src/qemu-ngage/isi-replies-rm1.txt').read_text()
        self.assertIn('\nreply 0A 00 01 00 01 00 08 ', t)       # sub-block 0 of 8 bytes
        self.assertNotIn('\nreply 0A 00 01 00 01 00 04 04 00', t)

    def test_selftest_reply(self):
        """Regression rm1_startup_failed: Startup's self-test (DSY 59 42) unanswered -> 'Phone start-up failed'."""
        self.assertIn('\nreply 59 42 43 ', (REPO / 'src/qemu-ngage/isi-replies-rm1.txt').read_text())

    def test_asc_reply(self):
        self.assertIn('reply 1C 2D 2E 01 01 00', (REPO / 'src/qemu-ngage/isi-replies-rm1.txt').read_text())


class Keypad(unittest.TestCase):
    """nokia6630_keys.h: each host key's (row, column) gives its scan code in ekeyp.dll's table."""

    def entries(self):
        t = (REPO / 'src/qemu-ngage/hw/arm/nokia6630_keys.h').read_text().split('#define RM84_KEYS', 1)[0]
        return [(k, int(r), int(c), int(sc, 16)) for k, r, c, sc in
                re.findall(r'\{ Q\((\w+)\), (\d+), (\d+), (0x[0-9a-f]+) \}', t)]

    def test_wired(self):
        self.assertIn('rm1_keys_init(mpu, ph->omap1710);', func(BOARD, 'static void rm1_board_init(MachineState *machine, const Rm1Phone *ph)'))

    def test_f5_is_menu(self):
        """F5 = Menu (0xB4, EStdKeyApplication0) as on the other EKA1 phones; the window maps the same keys."""
        self.assertIn(('F5', 6, 3, 0xb4), self.entries())
        w = (REPO / 'eka1/tools/ngage_phone.py').read_text()
        self.assertIn("'nokia6630': RM1_BUTTONS", w)
        self.assertIn("'nokia6630': 'Nokia 6630'", w)
        self.assertGreaterEqual(len(self.entries()), 25)

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_scan_codes_match_the_rom(self):
        rom = ROM.read_bytes()
        table = rom_word(0x50080efc) - 0x50000000            # literal: the scan-code table 0x5008145E
        for k, r, c, sc in self.entries():
            self.assertLess(c, 4)
            self.assertLess(r, 7)
            self.assertEqual(rom[table + c + r * 4], sc, 'KEY %s (row %d, col %d)' % (k, r, c))

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_column_words(self):
        """step n drives KBC bit n low"""
        o = rom_word(0x50080dd8) - 0x50000000
        self.assertEqual(struct.unpack_from('<4H', ROM.read_bytes(), o), (0x7e, 0x7d, 0x7b, 0x77))


class Mpuio8Rows(unittest.TestCase):
    """Regression rm1_keys_rows_5_6: QEMU's MPUIO keypad had 5 rows; the 6630 scans 7 (End, Menu, 0, 4, C)."""

    def test_patch(self):
        p = (PATCHES / '0026-omap-mpuio-8-keypad-rows.patch').read_text()
        self.assertIn('uint8_t buttons[8];', p)
        self.assertIn('row > 7', p)


class ApllLock(unittest.TestCase):
    """Regression rm1_apll_lock_spin: the kernel polled ULPD APLL_CTRL bit 15 (no timeout) after the Bluetooth
    UART's clock request; the Supervisor spun and no DFC ran (the keypad scan stopped after the home screen)."""

    def test_patch(self):
        p = (PATCHES / '0027-omap1610-apll-lock.patch').read_text()
        self.assertIn('s->omap1610 && addr == 0x4c', p)
        self.assertIn('| 0x8000', p)

    @unittest.skipUnless(ROM.exists(), 'core ROM not inflated')
    def test_rom_polls_apll(self):
        self.assertEqual(rom_word(0x5000d888), 0x580e084c)       # ULPD + 0x4C


class DrmHelper(unittest.TestCase):
    """Regression rm1_menu_grid_empty: the Menu's DRM plug-in hung on DRMHelperServer's start semaphore."""

    def test_patch(self):
        b = BOARD.read_text()
        self.assertIn('"H-rm1-drmhelper", 0x50abd41c, 0x4c19b510, 0x42402001', b)
        self.assertIn('"H-rm1-drmhelper", 0x50abd420, 0x1c0444a5, 0x46c04770', b)

    def test_sim_service_table_subtypes(self):
        t = (REPO / 'src/qemu-ngage/isi-replies-rm1.txt').read_text()
        self.assertIn('reply 09 13 14 @28 28 01', t)
        self.assertIn('reply 09 13 14 @74 74 01', t)


class Build(unittest.TestCase):
    def test_build_script_compiles_the_6630_files(self):
        s = (REPO / 'scripts/build-qemu-ngage.sh').read_text()
        self.assertIn('nokia6630*.[ch]', s)
        self.assertIn('nokia6630*.c', s)


if __name__ == '__main__':
    unittest.main()
