#!/usr/bin/env python3
"""2026-11-17 (SE P990i): the keypad flip as the phone itself sees it. flip.dll (dropped with the firmware's kernel
side) read a GPIO pin and published P&S 0x100039EA/2 and 0x102086EF/1 (1 = open); QUiConfigServer switches the UI
(softkey style on wsini mode 4, 240x256, when closed). Ours: QEMU syborg-sensor SWITCHES bit 0 (set = closed) ->
src/standins/flip -> the same two properties. And the camera: CamServer panicked "CamSrv:start err" -1 without
YUV2RGB.LDD, taking the Phone app with it (E32USER-CBase 63)."""
import os, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as D


def src(p):
    return open(os.path.join(ROOT, p)).read()


class Flip(unittest.TestCase):
    def test_qemu_switch_register(self):
        s = src('src/qemu-syborg/hw/arm/syborg_sensor.c')
        self.assertIn('R_SWITCHES = 6, R_SWITCH_SEQ = 7', s)
        self.assertIn('"switches", "uint32"', s)
        self.assertIn('VMSTATE_UINT32_V(switches', s)          # snapshots keep the flip

    def test_standin_publishes_what_flip_dll_did(self):
        s = src('src/standins/flip/flip.cpp')
        self.assertIn('{ 0x100039EA, 0x102086EF }', s)
        self.assertIn('{ 2, 1 }', s)
        self.assertIn('!(Reg(KRegSwitches) >> iBit & 1)', s)   # bit set = closed -> value 0

    def test_profile(self):
        prof = D.load('se-p990i')
        names = [x['name'] for x in prof['standins']]
        self.assertIn('flip', names)
        self.assertIn('yuv2rgbldd', names)
        self.assertEqual(prof['skin']['slides']['flip']['closed_view'], [0, 0, 240, 256])   # wsini mode 4
        self.assertEqual(D.get(prof, 'board.display.height'), 320)


class Keys(unittest.TestCase):
    """2026-11-17: the P1i draft keymap sent '0'..'9' (0x30-0x39), which the P990i's EKData.dll has no block for: digits
    did nothing. Its table maps the numeric keypad codes; the lock slide is scan code 0xB1, which e32keys.h has no
    name for (tools/device.py refused hex scan codes)."""

    def test_digits_are_numeric_keypad_codes(self):
        km = D.keymap(D.load('se-p990i'))
        sc = {k['key']: k['scancode'] for k in km['keys']}
        for d in '0123456789':
            self.assertEqual(sc[d], 'EStdKeyNkp' + d)
        self.assertEqual((sc['star'], sc['hash']), ('EStdKeyNkpAsterisk', 'EStdKeyHash'))
        self.assertEqual((sc['up'], sc['ok']), ('EStdKeyDevice4', 'EStdKeyDevice8'))

    def test_hex_scan_codes(self):
        lines = D.qemu_keymap(D.load('se-p990i')).splitlines() if isinstance(D.qemu_keymap(D.load('se-p990i')), str) \
            else D.qemu_keymap(D.load('se-p990i'))
        self.assertTrue(any(l.startswith('f8 0xb1') for l in lines))


if __name__ == '__main__':
    unittest.main()
