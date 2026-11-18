#!/usr/bin/env python3
"""bug 2026-11-17 (6720, E52): one fixed d_display pitch striped the screen: 960 broke 320x240 modes (Photos turns the
screen), 1280 broke portrait. ScDv draws with the stride control 31 reports and the frame buffer scans picture width x 4,
so the stand-in answers per rotation (d_display.pitch_turned, profile line_pitch_turned)."""
import os, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as D


class TurnedPitch(unittest.TestCase):
    def test_standin_answers_per_rotation(self):
        s = open(os.path.join(ROOT, 'src/standins/d_display/d_display.cpp')).read()
        self.assertIn('BoardConfigInt("d_display.pitch_turned", 0)', s)
        self.assertIn('TInt pitch = CurPitch();', s)              # ctl 31
        self.assertIn('TInt fbPitch = CurPitch();', s)            # DSB copy
        self.assertIn('rowsMax = PanelBytes / pitch', s)          # panel copy

    def test_portrait_panels_with_landscape_modes(self):
        for dev in ('nokia-6720-rm424', 'nokia-e52-rm469'):
            cfg = D.platform_config(D.load(dev)) if hasattr(D, 'platform_config') else None
            disp = D.get(D.load(dev), 'board.display', {})
            self.assertEqual((disp.get('line_pitch'), disp.get('line_pitch_turned')), (960, 1280), dev)


if __name__ == '__main__':
    unittest.main()
