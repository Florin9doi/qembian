#!/usr/bin/env python3
"""ngage_phone: an emulator window for the virtual EKA1 phones (N-Gage, N-Gage QD, 6600, 7650, 3230, 6630, 7710, 9500, P800, P910i; like `sydb phone`).

  python3 eka1/tools/ngage_phone.py [--model ngage|ngageqd|nokia6600|nokia7650|nokia7710|nokia9500|p910i] [--phone NAME] [--fresh]
                                    [--scale 2] [--keep] [--irda a|b|CHARDEV]

--model picks the phone (default ngage = N-Gage, NEM-4; ngageqd = N-Gage QD; nokia6600 = Nokia 6600, -M nokia6600); --phone
names its flash file (default: the model's name).

One command: it starts the phone (an ngage session, eka1/tools/ngage_sess.py, named phone-NAME)
when it isn't running, shows the screen and the phone's keys, and ends the session when the window
closes (--keep leaves it running; a session it joined is never stopped).
The phone keeps its flash (C:, settings) in build/ngage/phones/NAME.flash between boots (Q55);
--fresh starts factory-fresh: the first boot formats C: before drawing anything (a blank/white screen: N-Gage ~40 s,
6600 ~60 s, 3230 ~150 s; not a hang, docs/eka1-ngage.md "First boot is slow"); later boots are fast.

- Screen: QMP screendump of the board's console, i.e. what the board shows (the window server's
  screen chunk, H-ngage-fb, or the panel), refreshed 10 times a second.
- Keys: the buttons and the host keyboard send real key down/up events (QMP input-send-event,
  QEMU key codes); the board maps them to the N-Gage keypad (ngage.c NGAGE_KEYS), so a long
  press is a long press.
  Host keyboard: arrows, Enter = select, F1/F2 = left/right softkey, F3 = call (green),
  F4 = end (red), F5 = menu, Backspace = clear, Shift = edit, 0-9, * and #, F12 = power.
- Nokia 6600 infrared: --irda a on one phone and --irda b on another (different --phone names) links them: Menu,
  Connect., Infrared on one, Send, Via infrared on the other (docs/eka1-ngage.md "Infrared").
- Nokia 7650: a Slide button (or F7) opens and closes the keypad slide (qom-set /machine slide-closed); the
  phone starts with it open. Closed, the keypad locks and the camera (behind the slide) is on stand-by; open,
  the camera shows the board's test pattern (ngage_cam.c, docs/devices/nokia-7650-nhl2/camera.md).
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
- Nokia 7710: one 640x320 touch screen; the mouse is the pen (pressed = touching). Keys
  (nokia7710_input.c, names partly [I]): arrows + Enter = the 5-way key, F1 = Menu, F2 or Home = Desk,
  F3 = call, F4 = end, Esc or Backspace = back, F5-F7 = the other hardware keys. C: is kept in
  build/ngage/phones/NAME.flash (the partition image).
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


TITLES = {'ngage': 'N-Gage', 'ngageqd': 'N-Gage QD', 'nokia6600': 'Nokia 6600', 'nokia7650': 'Nokia 7650',
          'nokia9300': 'Nokia 9300', 'nokia9300i': 'Nokia 9300i', 'nokia9500': 'Nokia 9500 Communicator',
          'p910i': 'Sony Ericsson P910i', 'nokia7710': 'Nokia 7710', 'p800': 'Sony Ericsson P800', 'nokia3230': 'Nokia 3230', 'nokia7610': 'Nokia 7610', 'nokia6670': 'Nokia 6670', 'nokia6260': 'Nokia 6260',
          'nokia6630': 'Nokia 6630', 'nokian70': 'Nokia N70', 'nokia9210': 'Nokia 9210 Communicator', 'nokia9210i': 'Nokia 9210i Communicator',
          'nokia3650': 'Nokia 3650', 'nokia3660': 'Nokia 3660', 'nokia6680': 'Nokia 6680', 'nokian90': 'Nokia N90', 'nokian72': 'Nokia N72'}

# the clamshell Communicators (cover panel, lid, QWERTY): the 9300 and the 9500
COMMUNICATORS = ('nokia9300', 'nokia9300i', 'nokia9500')

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


# Nokia 7710 (nokia7710_input.c RM12_KEYS maps the qcodes to ekeyp's matrix)
RM12_KEYS = {Qt.Key.Key_Up: 'up', Qt.Key.Key_Down: 'down', Qt.Key.Key_Left: 'left', Qt.Key.Key_Right: 'right',
             Qt.Key.Key_Return: 'ret', Qt.Key.Key_Enter: 'ret', Qt.Key.Key_Escape: 'esc',
             Qt.Key.Key_Backspace: 'backspace', Qt.Key.Key_Home: 'home'}
RM12_KEYS.update({getattr(Qt.Key, 'Key_F%d' % i): 'f%d' % i for i in range(1, 8)})
RM12_BUTTONS = [[('Menu', 'f1'), ('Desk', 'f2'), ('Call', 'f3'), ('End', 'f4'), ('Back', 'esc')],
                [('◀', 'left'), ('▲', 'up'), ('Select', 'ret'), ('▼', 'down'), ('▶', 'right')],
                [('Key F5', 'f5'), ('Key F6', 'f6'), ('Key F7', 'f7')]]


# Nokia 6630 (nokia6630_keys.h maps the qcodes to ekeyp's MPUIO matrix): as the N-Gage's rows, # = f8
RM1_KEYS = dict(HOST_KEYS)
RM1_KEYS.update({Qt.Key.Key_NumberSign: 'f8', Qt.Key.Key_Escape: 'esc', Qt.Key.Key_F6: 'f6', Qt.Key.Key_F9: 'f9'})
RM1_BUTTONS = [row[:] for row in KEY_ROWS]
RM1_BUTTONS[3] = [('C', 'backspace'), ('▼', 'down'), ('Edit', 'f6')]
RM1_BUTTONS[7] = [('*', 'kp_multiply'), ('0', '0'), ('#', 'f8')]

# Nokia N70 (nokia6630_keys.h RM84_KEYS): the 6630's keys + F10 (0xE3) and F11 (0xBA), two keys the N70's
# matrix adds [V ekeyp table; which physical keys: I]
RM84_KEYS = dict(RM1_KEYS)
RM84_KEYS.update({Qt.Key.Key_F10: 'f10', Qt.Key.Key_F11: 'f11'})
RM84_BUTTONS = [row[:] for row in RM1_BUTTONS] + [[('Key E3', 'f10'), ('Key BA', 'f11'), ('Key AA', 'f9')]]

# Nokia 9210 (nokia9210_keys.h RAE3_KEYS maps the qcodes to EKeyb's matrix): the QWERTY as typed, F1-F4 the
# command buttons beside the screen (F1 = the top one, OK), F9 Menu, F10 Help, F5-F8 / F11 / F12 / Insert /
# Delete the application keys (Desk, Telephone, ... [I order])
RAE3_BUTTONS = [[('Cmd 1 (OK)', 'f1'), ('Cmd 2', 'f2'), ('Cmd 3', 'f3'), ('Cmd 4', 'f4'), ('Menu', 'f9'),
                 ('Esc', 'esc'), ('Enter', 'ret'), ('Help', 'f10')],
                [('App 1', 'f5'), ('App 2', 'f6'), ('App 3', 'f7'), ('App 4', 'f8'), ('App 5', 'f11'),
                 ('App 6', 'f12'), ('App 7', 'insert'), ('App 8', 'delete')]]
RAE3_HOST = dict(QWERTY)
RAE3_HOST.update({Qt.Key.Key_Delete: 'delete'})


class Phone(QtWidgets.QWidget):
    def __init__(self, session, scale, model='ngage'):
        super().__init__()
        self.session, self.scale, self.model = session, scale, model
        self.qmp = ns.Qmp(session)
        self.shot = str(ns.sdir(session) / 'screen.ppm')
        self.setWindowTitle('%s (%s, session %s)' % (TITLES.get(model, model), model, session))
        lay = QtWidgets.QVBoxLayout(self)
        # (label, QMP screendump arguments, size in phone pixels, scale)
        if model in ('p910i', 'p800'):
            self.views = [('screen', {}, (208, 320), scale)]
            self.host_keys = P910_KEYS
        elif model == 'nokia7710':
            self.views = [('screen', {}, (640, 320), max(1, scale // 2 + scale % 2))]
            self.host_keys = RM12_KEYS
        elif model in COMMUNICATORS:
            self.views = [('cover', {'device': 'cover'}, (128, 128), scale),
                          ('inner', {}, (640, 200), max(1, scale // 2 + scale % 2))]
            self.host_keys = QWERTY
        elif model in ('nokia6630', 'nokia6680'):       # the 6680: the 6630's keys [I]
            self.views = [('screen', {}, (176, 208), scale)]
            self.host_keys = RM1_KEYS
        elif model in ('nokia9210', 'nokia9210i'):
            self.views = [('screen', {}, (640, 200), max(1, scale // 2 + scale % 2))]
            self.host_keys = RAE3_HOST
        elif model in ('nokian70', 'nokian72'):        # the N72: the N70's ekeyp.dll (same code) [V]
            self.views = [('screen', {}, (176, 208), scale)]
            self.host_keys = RM84_KEYS
        elif model == 'nokian90':                       # 352x416 main display; the N70's keys for now [I]
            self.views = [('screen', {}, (352, 416), max(1, scale // 2 + scale % 2))]
            self.host_keys = RM84_KEYS
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
        if model in COMMUNICATORS:
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
        if model == 'nokia7650':
            self.slide = QtWidgets.QPushButton('Close slide')
            self.slide.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            self.slide.clicked.connect(self.toggle_slide)
            lay.addWidget(self.slide)
        if model in ('nokia7710', 'p800'):
            self.labels[0].installEventFilter(self)       # the pen
        rows = {'nokia9300': RA2_BUTTONS, 'nokia9300i': RA2_BUTTONS, 'nokia9500': RA2_BUTTONS, 'p910i': P910_BUTTONS,
                'nokia7710': RM12_BUTTONS, 'p800': P910_BUTTONS, 'nokia6630': RM1_BUTTONS, 'nokia6680': RM1_BUTTONS, 'nokian70': RM84_BUTTONS, 'nokian72': RM84_BUTTONS, 'nokian90': RM84_BUTTONS,
                'nokia9210': RAE3_BUTTONS, 'nokia9210i': RAE3_BUTTONS}.get(model, KEY_ROWS)
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
        if e.key() == Qt.Key.Key_F7 and hasattr(self, 'slide') and not e.isAutoRepeat():
            self.toggle_slide()                     # 7650: F7 = the slide (keeps the button's label right)
            return
        code = self.host_keys.get(e.key())
        if code and not e.isAutoRepeat():
            self.key(code, True)

    def keyReleaseEvent(self, e):
        code = self.host_keys.get(e.key())
        if code and not e.isAutoRepeat():
            self.key(code, False)

    def pen(self, e, down=None):
        """The P910i's / 7710's touch screen: absolute pointer events on the board's console."""
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

    def toggle_slide(self):
        """The 7650's keypad slide: the switch line ekeyb reads (ngage_keys.c, H-nhl2-slide)."""
        try:
            closed = bool(self.qmp.cmd('qom-get', path='/machine', property='slide-closed').get('return'))
            self.qmp.cmd('qom-set', path='/machine', property='slide-closed', value=not closed)
            self.slide.setText('Open slide' if not closed else 'Close slide')
        except OSError as err:
            self.status.setText('slide: %s' % err)

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
                    help='which phone model (ngage = N-Gage, ngageqd = N-Gage QD, nokia6600 = Nokia 6600, nokia7650 = Nokia 7650, '
                         'nokia9500 = Nokia 9500 Communicator, p910i = Sony Ericsson P910i, nokia7710 = Nokia 7710, p800 = Sony Ericsson P800, nokia3230 = Nokia 3230, nokia7610 = Nokia 7610, nokia6670 = Nokia 6670, nokia6260 = Nokia 6260)')
    ap.add_argument('--phone', help="which phone (its flash file); default: the model's name")
    ap.add_argument('--mmc', default='', help='memory card image (E:), made with eka1/tools/ngage_card.py')
    ap.add_argument('--irda', default='', help='Nokia 6600 infrared: "a" or "b" joins two phones (one each), or a QEMU '
                    'chardev spec (docs/eka1-ngage.md "Infrared")')
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
                       + (['--mmc', os.path.abspath(a.mmc)] if a.mmc else [])
                       + (['--irda', a.irda] if a.irda else []),
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
