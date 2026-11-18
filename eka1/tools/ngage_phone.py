#!/usr/bin/env python3
"""ngage_phone: an emulator window for the virtual EKA1 phones (N-Gage QD, 6600, 7650, 9500; like `sydb phone`).

  python3 eka1/tools/ngage_phone.py [--model ngage|nokia6600|nokia7650|nokia9500|p910i] [--phone NAME] [--fresh]
                                    [--scale 2] [--keep]

--model picks the phone (default ngage = N-Gage QD; nokia6600 = Nokia 6600, -M nokia6600); --phone
names its flash file (default: the model's name).

One command: it starts the phone (an ngage session, eka1/tools/ngage_sess.py, named phone-NAME)
when it isn't running, shows the screen and the phone's keys, and ends the session when the window
closes (--keep leaves it running; a session it joined is never stopped).
The phone keeps its flash (C:, settings) in build/ngage/phones/NAME.flash between boots (Q55);
--fresh starts factory-fresh (the first boot formats C: and restores factory settings, ~75 s more).

- Screen: QMP screendump of the board's console, i.e. what the board shows (the window server's
  screen chunk, H-ngage-fb, or the panel), refreshed 10 times a second.
- Keys: the buttons and the host keyboard send real key down/up events (QMP input-send-event,
  QEMU key codes); the board maps them to the N-Gage keypad (ngage.c NGAGE_KEYS), so a long
  press is a long press.
  Host keyboard: arrows, Enter = select, F1/F2 = left/right softkey, F3 = call (green),
  F4 = end (red), F5 = menu, Backspace = clear, Shift = edit, 0-9, * and #, F12 = power.
- Nokia 9500 Communicator: two screens, the cover (128x128, its own console: screendump
  device=cover) and the inner 640x200 (the OMAP LCD controller, console 0), and a Lid button
  (qom-set /machine cover-open). The phone starts with the lid closed. The host keyboard is the
  phone's QWERTY while the lid is open (nokia9500_kbd.c maps the keys): letters, digits, Esc, Tab,
  arrows, Enter, Space, Backspace, Shift, Ctrl, Alt = Chr, Menu; F1-F8 = the application keys.
  F9-F12 = the four command buttons beside the inner screen (Cmd 1 = OK), lid open or closed.
  With the lid closed the same keys drive the cover keypad: 0-9, * and # (or \\), F1/F2 = left/right
  softkey, Enter = select, F3 = call, F4/Esc = end, arrows = scroll (not confirmed).
- Sony Ericsson P910i: one 208x320 screen; the mouse is the pen (pressed = touching); a Flip button
  (qom-set /machine flip-closed; the phone starts with the flip open). Keys go to the flip keypad's
  pads: with the flip closed 0-9, * and # (or \\), F1-F4 the keys above the digits; with it open the
  QWERTY letters, Backspace, Enter, Space, Shift, comma, period; F5-F7 the corner pads, Enter =
  Confirm, Up/Down = the jog dial, F8 = the side key (p910i_input.c; key names partly [I]).
"""
import argparse, os, pathlib, subprocess, sys, time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ngage_sess as ns  # noqa: E402

from PyQt6 import QtCore, QtGui, QtWidgets
Qt = QtCore.Qt

# (label, QEMU qcode) rows: the N-Gage QD's keys; the board maps each qcode (ngage.c NGAGE_KEYS)
KEY_ROWS = [
    [('Left soft', 'f1'), ('Menu', 'f5'), ('Right soft', 'f2')],
    [('Call', 'f3'), ('▲', 'up'), ('End', 'f4')],
    [('◀', 'left'), ('Select', 'ret'), ('▶', 'right')],
    [('C', 'backspace'), ('▼', 'down'), ('Power', 'f12')],
    [('1', '1'), ('2', '2'), ('3', '3')],
    [('4', '4'), ('5', '5'), ('6', '6')],
    [('7', '7'), ('8', '8'), ('9', '9')],
    [('*', 'kp_multiply'), ('0', '0'), ('#', 'f10')],
]

HOST_KEYS = {
    Qt.Key.Key_Up: 'up', Qt.Key.Key_Down: 'down', Qt.Key.Key_Left: 'left', Qt.Key.Key_Right: 'right',
    Qt.Key.Key_Return: 'ret', Qt.Key.Key_Enter: 'ret', Qt.Key.Key_Backspace: 'backspace',
    Qt.Key.Key_F1: 'f1', Qt.Key.Key_F2: 'f2', Qt.Key.Key_F3: 'f3', Qt.Key.Key_F4: 'f4',
    Qt.Key.Key_F5: 'f5', Qt.Key.Key_F12: 'f12', Qt.Key.Key_Asterisk: 'kp_multiply',
    Qt.Key.Key_NumberSign: 'f10', Qt.Key.Key_Shift: 'shift',
}
for i in range(10):
    HOST_KEYS[getattr(Qt.Key, 'Key_%d' % i)] = str(i)


TITLES = {'ngage': 'N-Gage QD', 'nokia6600': 'Nokia 6600', 'nokia7650': 'Nokia 7650',
          'nokia9500': 'Nokia 9500 Communicator', 'p910i': 'Sony Ericsson P910i'}

# Nokia 9500: every host key goes to the board as a qcode (nokia9500_kbd.c RA2_KEYS maps them)
QWERTY = {getattr(Qt.Key, 'Key_' + c): c.lower() for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'}
QWERTY.update({getattr(Qt.Key, 'Key_%d' % i): str(i) for i in range(10)})
QWERTY.update({getattr(Qt.Key, 'Key_F%d' % i): 'f%d' % i for i in range(1, 13)})
QWERTY.update({
    Qt.Key.Key_Escape: 'esc', Qt.Key.Key_Tab: 'tab', Qt.Key.Key_CapsLock: 'caps_lock',
    Qt.Key.Key_Shift: 'shift', Qt.Key.Key_Control: 'ctrl', Qt.Key.Key_Alt: 'alt', Qt.Key.Key_Menu: 'menu',
    Qt.Key.Key_Up: 'up', Qt.Key.Key_Down: 'down', Qt.Key.Key_Left: 'left', Qt.Key.Key_Right: 'right',
    Qt.Key.Key_Space: 'spc', Qt.Key.Key_Return: 'ret', Qt.Key.Key_Enter: 'ret',
    Qt.Key.Key_Backspace: 'backspace', Qt.Key.Key_Apostrophe: 'apostrophe', Qt.Key.Key_Semicolon: 'semicolon',
    Qt.Key.Key_Period: 'dot', Qt.Key.Key_Comma: 'comma', Qt.Key.Key_Equal: 'equal', Qt.Key.Key_Slash: 'slash',
    Qt.Key.Key_Backslash: 'backslash', Qt.Key.Key_NumberSign: 'backslash', Qt.Key.Key_Insert: 'insert',
    Qt.Key.Key_Asterisk: 'kp_multiply',
})
RA2_BUTTONS = [[('Cover: left soft', 'f1'), ('Select', 'ret'), ('Right soft', 'f2'), ('Call', 'f3'),
                ('End', 'f4'), ('*', 'kp_multiply'), ('#', 'backslash'), ('Menu', 'menu')],
               [('Cmd 1 (OK)', 'f9'), ('Cmd 2', 'f10'), ('Cmd 3', 'f11'), ('Cmd 4', 'f12'),
                ('Esc', 'esc'), ('Enter', 'ret'), ('Chr', 'alt'), ('Desk', 'f1')]]


# Sony Ericsson P910i (p910i_input.c maps them): the flip keypad's pads (flip closed: digits, *, #,
# F1-F4 the four keys above them; flip open: the QWERTY letters, Backspace, Enter, Space, Shift,
# comma, period, Alt = RightFunc), F5-F7 the corner pads, Enter = (1023, 1023) = Confirm [I],
# arrows up/down = jog dial, F8 = side key, F9 = flip. The pen is the mouse on the screen.
P910_KEYS = dict(QWERTY)
P910_KEYS.update({Qt.Key.Key_NumberSign: 'backslash', Qt.Key.Key_Asterisk: 'kp_multiply'})
P910_BUTTONS = [[('Jog ▲', 'up'), ('Jog ▼', 'down'), ('Confirm', 'ret'), ('Side', 'f8'),
                 ('Pad F1', 'f1'), ('Pad F2', 'f2'), ('Pad F3', 'f3'), ('Pad F4', 'f4')],
                [('1', '1'), ('2', '2'), ('3', '3'), ('4', '4'), ('5', '5'), ('6', '6'),
                 ('Corner F5', 'f5'), ('Corner F7', 'f7')],
                [('7', '7'), ('8', '8'), ('9', '9'), ('*', 'kp_multiply'), ('0', '0'), ('#', 'backslash'),
                 ('Back', 'backspace'), ('Space', 'spc')]]


class Phone(QtWidgets.QWidget):
    def __init__(self, session, scale, model='ngage'):
        super().__init__()
        self.session, self.scale, self.model = session, scale, model
        self.qmp = ns.Qmp(session)
        self.shot = str(ns.sdir(session) / 'screen.ppm')
        self.setWindowTitle('%s (%s, session %s)' % (TITLES.get(model, model), model, session))
        lay = QtWidgets.QVBoxLayout(self)
        # (label, QMP screendump arguments, size in phone pixels, scale)
        if model == 'p910i':
            self.views = [('screen', {}, (208, 320), scale)]
            self.host_keys = P910_KEYS
        elif model == 'nokia9500':
            self.views = [('cover', {'device': 'cover'}, (128, 128), scale),
                          ('inner', {}, (640, 200), max(1, scale // 2 + scale % 2))]
            self.host_keys = QWERTY
        else:
            self.views = [('screen', {}, (176, 208), scale)]
            self.host_keys = HOST_KEYS
        self.labels = []
        row = QtWidgets.QHBoxLayout()
        for name, _, (w, h), sc in self.views:
            l = QtWidgets.QLabel()
            l.setFixedSize(w * sc, h * sc)
            l.setStyleSheet('background: black')
            l.setToolTip(name)
            row.addWidget(l, alignment=Qt.AlignmentFlag.AlignVCenter)
            self.labels.append(l)
        lay.addLayout(row)
        self.status = QtWidgets.QLabel('booting...')
        lay.addWidget(self.status)
        grid = QtWidgets.QGridLayout()
        if model == 'nokia9500':
            self.lid = QtWidgets.QPushButton('Open lid')
            self.lid.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            self.lid.clicked.connect(self.toggle_lid)
            lay.addWidget(self.lid)
        if model == 'p910i':
            self.flip = QtWidgets.QPushButton('Close flip')
            self.flip.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            self.flip.clicked.connect(self.toggle_flip)
            lay.addWidget(self.flip)
            self.labels[0].installEventFilter(self)       # the pen
        rows = {'nokia9500': RA2_BUTTONS, 'p910i': P910_BUTTONS}.get(model, KEY_ROWS)
        for r, row in enumerate(rows):
            for c, (label, code) in enumerate(row):
                b = QtWidgets.QPushButton(label)
                b.setFocusPolicy(Qt.FocusPolicy.NoFocus)
                b.pressed.connect(lambda code=code: self.key(code, True))
                b.released.connect(lambda code=code: self.key(code, False))
                grid.addWidget(b, r, c)
        lay.addLayout(grid)
        self.t0 = time.time()
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(100)

    def key(self, code, down):
        try:
            self.qmp.cmd('input-send-event', events=[
                {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}])
        except OSError as e:
            self.status.setText('key: %s' % e)

    def keyPressEvent(self, e):
        code = self.host_keys.get(e.key())
        if code and not e.isAutoRepeat():
            self.key(code, True)

    def keyReleaseEvent(self, e):
        code = self.host_keys.get(e.key())
        if code and not e.isAutoRepeat():
            self.key(code, False)

    def pen(self, e, down=None):
        """The P910i's touch screen: absolute pointer events on the board's console."""
        l, (w, h) = self.labels[0], self.views[0][2]
        x = min(max(int(e.position().x() * w / l.width()), 0), w - 1)
        y = min(max(int(e.position().y() * h / l.height()), 0), h - 1)
        ev = [{'type': 'abs', 'data': {'axis': 'x', 'value': (2 * x + 1) * 32768 // (2 * w)}},
              {'type': 'abs', 'data': {'axis': 'y', 'value': (2 * y + 1) * 32768 // (2 * h)}}]
        if down is not None:
            ev.append({'type': 'btn', 'data': {'down': down, 'button': 'left'}})
        try:
            self.qmp.cmd('input-send-event', events=ev)
        except OSError as err:
            self.status.setText('pen: %s' % err)

    def eventFilter(self, obj, e):
        t = e.type()
        if t == QtCore.QEvent.Type.MouseButtonPress:
            self.pen(e, True)
            return True
        if t == QtCore.QEvent.Type.MouseButtonRelease:
            self.pen(e, False)
            return True
        if t == QtCore.QEvent.Type.MouseMove and e.buttons():
            self.pen(e)
            return True
        return super().eventFilter(obj, e)

    def toggle_flip(self):
        """The P910i's flip: a GPIO pin read by Exyin and ekeypad (p910i_input.c flip-closed)."""
        try:
            closed = bool(self.qmp.cmd('qom-get', path='/machine', property='flip-closed').get('return'))
            self.qmp.cmd('qom-set', path='/machine', property='flip-closed', value=not closed)
            self.flip.setText('Open flip' if not closed else 'Close flip')
        except OSError as err:
            self.status.setText('flip: %s' % err)

    def cover_open(self):
        r = self.qmp.cmd('qom-get', path='/machine', property='cover-open')
        return bool(r.get('return'))

    def toggle_lid(self):
        """The 9500's lid: the keyboard controller sends 0xE0 / 0xE1 (nokia9500_kbd.c)."""
        try:
            self.qmp.cmd('qom-set', path='/machine', property='cover-open', value=not self.cover_open())
            self.lid.setText('Close lid' if self.cover_open() else 'Open lid')
        except OSError as e:
            self.status.setText('lid: %s' % e)

    def tick(self):
        for (name, args, (w, h), sc), label in zip(self.views, self.labels):
            try:
                r = self.qmp.cmd('screendump', filename=self.shot, **args)
            except OSError as e:
                self.status.setText('QEMU gone: %s' % e)
                self.timer.stop()
                return
            if 'error' in r:
                continue
            img = QtGui.QImage(self.shot)
            if not img.isNull():
                label.setPixmap(QtGui.QPixmap.fromImage(img).scaled(
                    w * sc, h * sc, Qt.AspectRatioMode.IgnoreAspectRatio,
                    Qt.TransformationMode.FastTransformation))
        st = ns.call(self.session, {'op': 'status'}, 2)
        hit = st.get('hit') or {}
        self.status.setText('%s, %.0f s%s' % (st['state'], time.time() - self.t0,
                            ('  (%s: see %s)' % (hit.get('label'), hit.get('dump', ''))) if hit else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--scale', type=int, default=2)
    ap.add_argument('--model', default='ngage', choices=sorted(TITLES),
                    help='which phone model (ngage = N-Gage QD, nokia6600 = Nokia 6600, nokia7650 = Nokia 7650, '
                         'nokia9500 = Nokia 9500 Communicator, p910i = Sony Ericsson P910i)')
    ap.add_argument('--phone', help="which phone (its flash file); default: the model's name")
    ap.add_argument('--mmc', default='', help='memory card image (E:), made with eka1/tools/ngage_card.py')
    ap.add_argument('--fresh', action='store_true', help='factory-fresh: forget the saved flash')
    ap.add_argument('--keep', action='store_true', help='leave the session running when the window closes')
    a = ap.parse_args()
    a.phone = a.phone or a.model
    a.session = 'phone-' + a.phone
    flash = ns.ROOT / 'build/ngage/phones' / (a.phone + '.flash')
    flash.parent.mkdir(parents=True, exist_ok=True)
    started = False
    try:
        running = ns.call(a.session, {'op': 'status'}, 2)['state'] in ('running', 'frozen', 'paused')
    except OSError:
        running = False
    if running and a.fresh:
        subprocess.run([sys.executable, ns.__file__, '-s', a.session, 'stop'], stdout=subprocess.DEVNULL)
        running = False
    if a.fresh and flash.exists():
        flash.unlink()
    if not running:
        # crash catches only: a fault freezes the machine and the status line names the dump
        subprocess.run([sys.executable, ns.__file__, '-s', a.session, 'start', '--model', a.model,
                        '--flash', str(flash)]
                       + (['--mmc', os.path.abspath(a.mmc)] if a.mmc else []),
                       check=True, stdout=subprocess.DEVNULL)
        started = True
    app = QtWidgets.QApplication(sys.argv)
    import signal
    for sig in (signal.SIGINT, signal.SIGTERM):        # Ctrl+C closes the window like the X button
        signal.signal(sig, lambda *_: app.quit())
    w = Phone(a.session, a.scale, a.model)
    w.show()
    rc = app.exec()
    if started and not a.keep:
        subprocess.run([sys.executable, ns.__file__, '-s', a.session, 'stop'], stdout=subprocess.DEVNULL)
    return rc


if __name__ == '__main__':
    sys.exit(main())
