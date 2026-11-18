#!/usr/bin/env python3
"""Tests of the phone window's geometry (tools/sydb/phone.py), offscreen.

  QT_QPA_PLATFORM=offscreen python3 tests/phone_window_test.py
"""
import os
import sys
import unittest

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sydb'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from PyQt6 import QtCore, QtGui, QtWidgets   # noqa: E402
import phone                          # noqa: E402
import device as D                    # noqa: E402

APP = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


class _Ev:
    def __init__(self, x, y):
        self.p = QtCore.QPointF(x, y)

    def position(self):
        return self.p

    def button(self):
        return QtCore.Qt.MouseButton.LeftButton


def touch_at(scr, x, y):
    got = []
    scr.touch = lambda gx, gy, down: got.append((round(gx, 3), round(gy, 3)))
    scr.finger(_Ev(x, y), True)
    return got[0]


class TurnViewDirection(unittest.TestCase):
    """Bug 2026-11-17: on the 5800/5230 'Turn view' turned Bounce upside down: the window always turned
    the picture anticlockwise (right for the N8's games, wrong for these). The direction is now the
    profile's [skin] turn (ccw / cw / none); touch must follow the picture."""

    def screen(self, cw):
        s = phone.Screen(1, (240, 320), touch=lambda *a: None)
        s.turned, s.turn_cw = True, cw
        s.resize_for()
        return s

    def test_turned_screen_is_landscape(self):
        s = self.screen(False)
        self.assertEqual((s.width(), s.height()), (320, 240))

    def test_ccw_touch_mapping(self):
        s = self.screen(False)
        # picture turned anticlockwise: the guest's top-left corner is at the window's bottom-left
        self.assertEqual(touch_at(s, 0, 240), (0.0, 0.0))
        self.assertEqual(touch_at(s, 320, 0), (1.0, 1.0))

    def test_cw_touch_mapping(self):
        s = self.screen(True)
        # picture turned clockwise: the guest's top-left corner is at the window's top-right
        self.assertEqual(touch_at(s, 320, 0), (0.0, 0.0))
        self.assertEqual(touch_at(s, 0, 240), (1.0, 1.0))

    def test_corners_reach_the_edges(self):
        # taps in the corners of a turned view must reach the guest's corners (owner: Bounce's corner buttons)
        for cw in (False, True):
            s = self.screen(cw)
            pts = {touch_at(s, x, y) for x in (0, 320) for y in (0, 240)}
            self.assertEqual(pts, {(0.0, 0.0), (0.0, 1.0), (1.0, 0.0), (1.0, 1.0)})

    def test_profiles_declare_valid_turn(self):
        for dev in D.device_ids():
            turn = (D.load(dev).get('skin', {}) or {}).get('turn', 'ccw')
            with self.subTest(dev=dev):
                self.assertIn(turn, ('ccw', 'cw', 'none'))
        self.assertEqual(D.load('nokia-5800-rm356')['skin']['turn'], 'cw')
        self.assertEqual(D.load('nokia-5230-rm588')['skin']['turn'], 'cw')


class Slides(unittest.TestCase):
    def test_slide_kinds_known(self):
        for dev in D.device_ids():
            for k in D.load(dev).get('hardware', {}).get('slides', []) or []:
                with self.subTest(dev=dev, slide=k):
                    self.assertIn(k, phone.SLIDE_DEFAULTS)

    def test_qwerty_sliders(self):
        for dev in ('nokia-5730-rm465', 'nokia-n97-rm505'):
            self.assertIn('qwerty', D.load(dev)['hardware']['slides'])
            sw = [w for w in D.keymap(D.load(dev)).get('switches', []) if w['name'] == 'qwerty-slide']
            self.assertEqual(len(sw), 1, dev)          # the window taps these keys (owner 2026-11-17)
            self.assertTrue(sw[0]['open']['host'] and sw[0]['close']['host'])
            self.assertEqual(sw[0]['open']['layout'], 'landscape')


class CommunicatorLid(unittest.TestCase):
    """2026-11-17 (E90): the lid switches the display: open = the inner 800x352 (EKeyDeviceA), closed = the cover
    (EKeyDeviceB) on HAL screen 1. Bug 2026-11-17 (owner): the first design cropped the inner frame buffer, so a
    lid closed during boot showed a cut-off inner picture and the cover was dimmed with screen 0; now the cover is
    its own panel in QEMU and the window follows the panel's size"""

    def test_e90_lid_keys_and_cover(self):
        prof = D.load('nokia-e90-ra6')
        self.assertEqual(prof['hardware']['slides'], ['lid'])
        sw = [w for w in D.keymap(prof).get('switches', []) if w['name'] == 'lid-slide']
        self.assertEqual((sw[0]['open']['scancode'], sw[0]['close']['scancode']), ('EStdKeyDeviceA', 'EStdKeyDeviceB'))
        self.assertNotIn('closed_view', prof.get('skin', {}).get('slides', {}).get('lid', {}))   # no crop any more
        self.assertEqual(prof['board']['display']['screen1'], '240x320')
        self.assertTrue(phone.SLIDE_DEFAULTS['lid']['initially_open'])

    def test_e90_qwerty_letters_under_the_lid(self):
        # 2026-11-17: the E90 profile started with the N95's keypad map; the lid opens onto a QWERTY keyboard
        km = D.keymap(D.load('nokia-e90-ra6'))
        letters = {k['scancode'] for k in km['keys'] if k.get('group') == 'qwerty'}
        for c in 'QWERTYUIOPASDFGHJKLZXCVBNM':
            self.assertIn("'%s'" % c, letters)
        self.assertEqual(phone.SLIDE_DEFAULTS['lid']['group'], 'qwerty')   # shown while the lid is open

    def test_screen_follows_the_panel_size(self):
        s = phone.Screen(1, (800, 352), touch=lambda *a: None)
        s.resize_for()
        self.assertEqual((s.width(), s.height()), (800, 352))
        s.set_frame(QtGui.QImage(240, 320, QtGui.QImage.Format.Format_RGB32))   # the cover panel's frame
        self.assertEqual((s.width(), s.height()), (240, 320))
        s.set_frame(QtGui.QImage(800, 352, QtGui.QImage.Format.Format_RGB32))   # lid opened again
        self.assertEqual((s.width(), s.height()), (800, 352))

    def test_lid_default_has_no_crop(self):
        self.assertNotIn('closed_view', phone.SLIDE_DEFAULTS['lid'])


class CandybarKeypadShown(unittest.TestCase):
    """Bug 2026-11-17: phones without a keypad slide (N79, N86, E52, E72, E5-00, 6720, E71, N73) showed no
    number keys in the window: the 'keypad' group was shown only while a keypad slide was open."""

    def groups_shown(self, slides, open_kind=None):
        class P:
            pass
        p = P()
        p.slide_cfg = {k: dict(phone.SLIDE_DEFAULTS[k]) for k in slides}
        bound = phone.Phone.slide_groups(p)
        shown = p.slide_cfg[open_kind]['group'] if open_kind else None
        return {g for g in ('keypad', 'media', 'qwerty') if g == shown or g not in bound}

    def test_candybar_always_shows_keypad(self):
        self.assertIn('keypad', self.groups_shown([]))

    def test_n95_keypad_only_with_slide_open(self):
        self.assertNotIn('keypad', self.groups_shown(['keypad', 'media']))
        self.assertIn('keypad', self.groups_shown(['keypad', 'media'], 'keypad'))

    def test_5730_front_keypad_and_qwerty_on_slide(self):
        self.assertEqual(self.groups_shown(['qwerty']), {'keypad', 'media'})
        self.assertIn('qwerty', self.groups_shown(['qwerty'], 'qwerty'))


class KeypadFlip(unittest.TestCase):
    """2026-11-17 (SE P990i): the keypad flip is a switch the phone reads (QEMU syborg-sensor `switches` -> the flip
    stand-in -> P&S 0x100039EA/2), not keys. Closed: the top 240x256 of the panel and the flip's keypad; open: the
    whole 240x320 and the QWERTY under it."""

    def window(self):
        import time
        class W:
            def __init__(self): self.on = None
            def setVisible(self, v): self.on = v
        class C:
            def __init__(self): self.calls = []
            def call(self, req, *a): self.calls.append(req); return {}
        class P:
            pass
        p = P()
        prof = D.load('se-p990i')
        cfg = dict(phone.SLIDE_DEFAULTS['flip'])
        cfg.update(prof['skin']['slides']['flip'])
        p.slide_cfg, p.slide_state, p.slide_btns = {'flip': cfg}, None, {}
        p.groups_w = {g: W() for g in ('keypad', 'qwerty', 'nav')}
        p.screen = phone.Screen(1, (240, 320))
        p.c, p.fit, p.status = C(), lambda: None, None
        p.slide_groups = lambda: phone.Phone.slide_groups(p)
        def go(kind):
            n = len(p.c.calls)
            phone.Phone.slide(p, kind)
            for _ in range(100):
                if len(p.c.calls) > n:
                    break
                time.sleep(0.01)
        return p, go

    def test_closed_and_open(self):
        p, go = self.window()
        go(None)                                   # close the flip
        self.assertEqual(p.c.calls[-1]['args'], {'path': '/machine/sensor', 'property': 'switches', 'value': 1})
        self.assertEqual((p.screen.width(), p.screen.height()), (240, 256))
        self.assertEqual((p.groups_w['keypad'].on, p.groups_w['qwerty'].on, p.groups_w['nav'].on), (True, False, True))
        go('flip')                                 # open it
        self.assertEqual(p.c.calls[-1]['args']['value'], 0)
        self.assertEqual((p.screen.width(), p.screen.height()), (240, 320))
        self.assertEqual((p.groups_w['keypad'].on, p.groups_w['qwerty'].on), (False, True))

    def test_profile(self):
        prof = D.load('se-p990i')
        self.assertEqual(prof['hardware']['slides'], ['flip'])
        self.assertIn('flip', [s['name'] for s in prof['standins']])
        km = D.keymap(prof)
        self.assertEqual(sum(1 for k in km['keys'] if k.get('group') == 'keypad'), 12)
        self.assertGreaterEqual(sum(1 for k in km['keys'] if k.get('group') == 'qwerty'), 26)


class TypedCharacters(unittest.TestCase):
    """Bug 2026-11-17: '*' (and every other character) could not be typed from the host keyboard: the window
    mapped only a fixed table (Asterisk -> numpad *), which the 5730's keymap does not use (its '*' is F9),
    so the 5730 could not be unlocked (LSK, then *). Characters now go to the phone key labelled with them."""

    def chars(self, dev):
        return phone.char_keys(D.keymap(D.load(dev)))

    def test_5730_star_and_hash(self):
        c = self.chars('nokia-5730-rm465')
        self.assertEqual(phone.host_key_for(QtCore.Qt.Key.Key_Asterisk, '*', c), 'f9')
        self.assertIsNotNone(phone.host_key_for(QtCore.Qt.Key.Key_NumberSign, '#', c))

    def test_5730_letters_on_qwerty(self):
        c = self.chars('nokia-5730-rm465')
        self.assertEqual(phone.host_key_for(QtCore.Qt.Key.Key_Q, 'Q', c), 'q')
        self.assertEqual(phone.host_key_for(QtCore.Qt.Key.Key_Q, 'q', c), 'q')

    def test_digits_every_keypad_phone(self):
        for dev in D.device_ids():
            c = self.chars(dev)
            if '5' not in c:
                continue                # touch phones: no number keys
            with self.subTest(dev=dev):
                self.assertIsNotNone(phone.host_key_for(QtCore.Qt.Key.Key_5, '5', c))
                self.assertIsNotNone(phone.host_key_for(QtCore.Qt.Key.Key_Asterisk, '*', c))

    def test_function_keys_win(self):
        c = self.chars('nokia-n95-8gb-rm320')
        self.assertEqual(phone.host_key_for(QtCore.Qt.Key.Key_F5, '', c), 'f5')
        self.assertEqual(phone.host_key_for(QtCore.Qt.Key.Key_Up, '', c), 'up')


if __name__ == '__main__':
    unittest.main()
