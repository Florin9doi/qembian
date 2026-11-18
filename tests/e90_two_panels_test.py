#!/usr/bin/env python3
"""bug 2026-11-17 (E90, owner): the cover display was a crop of the inner frame buffer (a lid closed during boot
showed a cut-off inner picture, the cover dimmed with screen 0's backlight) and HAL screen 1 was patched away
(H-e90-one-screen). Now the cover is HAL screen 1 with its own panel in QEMU (H-e90-two-panels): these pin the
pieces that make it so."""
import os, re, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as D


def src(p):
    return open(os.path.join(ROOT, p), newline='').read()


class TwoPanels(unittest.TestCase):
    def test_qemu_fb_second_panel_registers(self):
        s = src('src/qemu-syborg/hw/arm/syborg_fb.c')
        for reg in ('base1', 'width1', 'height1', 'blank1'):
            self.assertIn('s->' + reg, s)
        self.assertIn('"active-panel"', s)                   # runtime switch (phone window's Lid button)
        self.assertIn('DEFINE_PROP_UINT32("panel"', s)        # start-up panel (lid closed at boot)
        self.assertIn('"fb"', src('src/qemu-syborg/hw/arm/syborg.c'))   # /machine/fb link for qom-set

    def test_keyboard_boot_keys(self):
        s = src('src/qemu-syborg/hw/arm/syborg_input.c')
        self.assertIn('DEFINE_PROP_STRING("boot-keys"', s)
        self.assertIn('kbd_send_boot_keys', s)

    def test_lcd_extension_patch_registers_screen1(self):
        p = src('patches/qemu-baseport/0028-fb-second-screen.patch')
        self.assertIn('FB_BASE1', p)
        self.assertIn('Kern::AddHalEntry(EHalGroupDisplay', p)
        self.assertIn('iScreen', p)
        mmp = src('src/standins/lcd/lcd_e90.mmp')
        self.assertIn('SYBORG_FB_SCREEN1_W=240', mmp)
        self.assertIn('SYBORG_FB_SCREEN1_H=320', mmp)

    def test_display_standin_serves_screen1(self):
        s = src('src/standins/d_display/d_display.cpp')
        self.assertIn('"d_display.screen1"', s)
        self.assertIn('iScreen', s)

    def test_e90_profile(self):
        prof = D.load('nokia-e90-ra6')
        self.assertEqual(D.get(prof, 'board.display.screen1'), '240x320')
        self.assertEqual(D.get(prof, 'kernel.lcd.target'), 'lcd_e90')
        self.assertEqual(D.get(prof, 'rom.code_patches'), [])        # no Hal.dll screen-count patch any more
        self.assertFalse(os.path.exists(os.path.join(ROOT, 'models/nokia-e90-ra6/code-patches.txt')))
        args = ' '.join(D.qemu_args(prof))
        self.assertIn('d_display.screen1=240x320', args)

    def test_one_screen_phones_untouched(self):
        args = ' '.join(D.qemu_args(D.load('nokia-n95-8gb-rm320')))
        self.assertNotIn('screen1', args)


if __name__ == '__main__':
    unittest.main()
