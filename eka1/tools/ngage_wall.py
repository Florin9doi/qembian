#!/usr/bin/env python3
"""ngage_wall: several EKA1 phones side by side in one window, screens only (a clone of ngage_phone.py).

  python3 eka1/tools/ngage_wall.py MODEL[:NAME] [MODEL[:NAME] ...] [--cols N] [--scale 2] [--fresh] [--keep]

  e.g.  ngage_wall.py ngage ngageqd nokia6600 nokia7650
        ngage_wall.py nokia6600 nokia6600:6600-b        two 6600s, each with its own flash

Each phone is an ngage session (eka1/tools/ngage_sess.py) named phone-NAME with its flash in
build/ngage/phones/NAME.flash, as ngage_phone.py --phone NAME (NAME defaults to the model; a model
given twice gets NAME-2, NAME-3, ...: two emulators must never share one flash file). Phones that are
not running are started (all in parallel), and the ones this window started are stopped when it
closes (--keep leaves them running; a session it joined is never stopped). Don't open the same phone
in this window and in ngage_phone.py at once: both would drive it (harmless, but confusing).

No keypads: each tile is the phone's display(s) under its name (hover it for the session state,
errors and a crash dump's path). Input goes to the phone under the
mouse (its tile is outlined): the host keyboard sends key down/up events with the same per-model key
maps as ngage_phone.py (arrows, Enter, F1/F2 softkeys, F3/F4 call/end, 0-9, ...); a key is released
on the phone that got the press even if the mouse has moved on. On touch phones (P800, P910i, 7710)
the mouse on the screen is the pen. Right-click a tile for the phone's switches (7650 slide, P910i
flip, 9300/9500 lid); F7 still toggles the 7650's slide.
"""
import argparse, math, os, pathlib, signal, subprocess, sys, time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ngage_sess as ns  # noqa: E402
import ngage_phone as np_  # noqa: E402  (the key maps and titles; nothing runs on import)

from PyQt6 import QtCore, QtGui, QtWidgets
Qt = QtCore.Qt

TOUCH = ('p910i', 'p800', 'nokia7710')
# model -> (QMP property, label when it is True, label when False) of its switch
SWITCHES = {'nokia7650': ('slide-closed', 'Open slide', 'Close slide'),
            'p910i': ('flip-closed', 'Open flip', 'Close flip')}
for _m in np_.COMMUNICATORS:
    SWITCHES[_m] = ('cover-open', 'Close lid', 'Open lid')


def model_config(model, scale):
    """The displays ((label, screendump args, size, scale) list) and host key map of a model, as ngage_phone.py."""
    half = max(1, scale // 2 + scale % 2)
    if model in ('p910i', 'p800'):
        return [('screen', {}, (208, 320), scale)], np_.P910_KEYS
    if model == 'nokia7710':
        return [('screen', {}, (640, 320), half)], np_.RM12_KEYS
    if model in np_.COMMUNICATORS:
        return [('cover', {'device': 'cover'}, (128, 128), scale), ('inner', {}, (640, 200), half)], np_.QWERTY
    if model in ('nokia6630', 'nokia6680'):
        return [('screen', {}, (176, 208), scale)], np_.RM1_KEYS
    if model in ('nokia9210', 'nokia9210i'):
        return [('screen', {}, (640, 200), half)], np_.RAE3_HOST
    if model in ('nokian70', 'nokian72'):
        return [('screen', {}, (176, 208), scale)], np_.RM84_KEYS
    if model == 'nokian90':
        return [('screen', {}, (352, 416), scale / 2)], np_.RM84_KEYS   # twice 176x208: drawn the size of the others
    return [('screen', {}, (176, 208), scale)], np_.HOST_KEYS


class Tile(QtWidgets.QFrame):
    """One phone: its display(s); state and errors in its tooltip; the wall routes keys to the hovered tile."""

    def __init__(self, wall, session, model, phone, scale):
        super().__init__()
        self.wall, self.session, self.model, self.phone = wall, session, model, phone
        self.qmp = None
        self.dead = False
        self.shot = str(ns.sdir(session) / 'screen.ppm')
        self.views, self.host_keys = model_config(model, scale)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setFrameShape(QtWidgets.QFrame.Shape.Box)
        self.setLineWidth(1)
        self.set_hot(False)
        lay = QtWidgets.QVBoxLayout(self)
        lay.setContentsMargins(4, 4, 4, 4)
        title = QtWidgets.QLabel('%s  [%s]' % (np_.TITLES.get(model, model), phone))
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        lay.addWidget(title)
        row = QtWidgets.QHBoxLayout()
        self.labels = []
        for name, _, (w, h), sc in self.views:
            l = QtWidgets.QLabel()
            l.setFixedSize(round(w * sc), round(h * sc))
            l.setStyleSheet('background: black')
            row.addWidget(l, alignment=Qt.AlignmentFlag.AlignVCenter)
            self.labels.append(l)
        lay.addLayout(row)
        self.note('booting...')
        if model in TOUCH:
            self.labels[0].installEventFilter(self)
            self.labels[0].setMouseTracking(False)
        self.t0 = time.time()

    def set_hot(self, hot):
        self.setStyleSheet('Tile { border: 1px solid %s; border-radius: 4px; }'
                           % ('rgba(61, 174, 233, 0.45)' if hot else 'transparent'))

    def note(self, text):
        """Session state / errors: the tile's tooltip (no status line)."""
        self.setToolTip('%s: %s' % (self.phone, text))

    def enterEvent(self, e):
        self.wall.hover(self)
        super().enterEvent(e)

    # ---- QMP ----
    def cmd(self, name, **args):
        if self.qmp is None:
            self.qmp = ns.Qmp(self.session)
        try:
            return self.qmp.cmd(name, **args)
        except (OSError, ValueError):
            self.qmp = None
            raise

    def key(self, code, down):
        try:
            self.cmd('input-send-event', events=[
                {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}])
        except (OSError, ValueError) as e:
            self.note('key: %s' % e)

    def pen(self, e, down=None):
        l, (w, h) = self.labels[0], self.views[0][2]
        x = min(max(int(e.position().x() * w / l.width()), 0), w - 1)
        y = min(max(int(e.position().y() * h / l.height()), 0), h - 1)
        ev = [{'type': 'abs', 'data': {'axis': 'x', 'value': (2 * x + 1) * 32768 // (2 * w)}},
              {'type': 'abs', 'data': {'axis': 'y', 'value': (2 * y + 1) * 32768 // (2 * h)}}]
        if down is not None:
            ev.append({'type': 'btn', 'data': {'down': down, 'button': 'left'}})
        try:
            self.cmd('input-send-event', events=ev)
        except (OSError, ValueError) as err:
            self.note('pen: %s' % err)

    def eventFilter(self, obj, e):
        t = e.type()
        if t == QtCore.QEvent.Type.MouseButtonPress and e.button() == Qt.MouseButton.LeftButton:
            self.pen(e, True)
            return True
        if t == QtCore.QEvent.Type.MouseButtonRelease and e.button() == Qt.MouseButton.LeftButton:
            self.pen(e, False)
            return True
        if t == QtCore.QEvent.Type.MouseMove and e.buttons() & Qt.MouseButton.LeftButton:
            self.pen(e)
            return True
        return super().eventFilter(obj, e)

    # ---- switches (slide / flip / lid) ----
    def toggle_switch(self):
        prop = SWITCHES[self.model][0]
        try:
            cur = bool(self.cmd('qom-get', path='/machine', property=prop).get('return'))
            self.cmd('qom-set', path='/machine', property=prop, value=not cur)
        except (OSError, ValueError) as err:
            self.note('%s: %s' % (prop, err))

    def contextMenuEvent(self, e):
        if self.model not in SWITCHES:
            return
        prop, on_label, off_label = SWITCHES[self.model]
        try:
            cur = bool(self.cmd('qom-get', path='/machine', property=prop).get('return'))
        except (OSError, ValueError) as err:
            self.note('%s: %s' % (prop, err))
            return
        menu = QtWidgets.QMenu(self)
        menu.addAction(on_label if cur else off_label, self.toggle_switch)
        menu.exec(e.globalPos())

    # ---- refresh ----
    def refresh(self, with_status):
        if self.dead:
            return
        for (name, args, (w, h), sc), label in zip(self.views, self.labels):
            try:
                r = self.cmd('screendump', filename=self.shot, **args)
            except (OSError, ValueError) as e:
                self.note('QEMU gone: %s' % e)
                self.dead = True
                return
            if 'error' in r:
                continue
            img = QtGui.QImage(self.shot)
            if not img.isNull():
                label.setPixmap(QtGui.QPixmap.fromImage(img).scaled(
                    round(w * sc), round(h * sc), Qt.AspectRatioMode.IgnoreAspectRatio,
                    Qt.TransformationMode.SmoothTransformation if sc < 1 else Qt.TransformationMode.FastTransformation))
        if with_status:
            try:
                st = ns.call(self.session, {'op': 'status'}, 2)
            except (OSError, ValueError) as e:
                self.note('session gone: %s' % e)
                return
            hit = st.get('hit') or {}
            self.note('%s, %.0f s%s' % (st.get('state'), time.time() - self.t0,
                                ('  (%s: see %s)' % (hit.get('label'), hit.get('dump', ''))) if hit else ''))


class Wall(QtWidgets.QWidget):
    def __init__(self, phones, scale, cols):
        super().__init__()
        self.setWindowTitle('EKA1 phones: ' + ', '.join(p for _, _, p in phones))
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        grid = QtWidgets.QGridLayout(self)
        self.tiles = []
        cols = cols or math.ceil(math.sqrt(len(phones)))
        for i, (session, model, phone) in enumerate(phones):
            t = Tile(self, session, model, phone, scale)
            grid.addWidget(t, i // cols, i % cols, alignment=Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter)
            self.tiles.append(t)
        self.hot = None
        self.held = {}                 # Qt key -> (tile, qcode): a release goes where the press went
        self.n = 0
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(100)

    def hover(self, tile):
        if tile is self.hot:
            return
        if self.hot:
            self.hot.set_hot(False)
        self.hot = tile
        tile.set_hot(True)

    def focusNextPrevChild(self, nxt):
        return False                   # Tab is a key for the phone (Communicators), not focus movement

    def keyPressEvent(self, e):
        t = self.hot
        if t is None or e.isAutoRepeat():
            return
        if e.key() == Qt.Key.Key_F7 and t.model == 'nokia7650':
            t.toggle_switch()
            return
        code = t.host_keys.get(e.key())
        if code:
            self.held[e.key()] = (t, code)
            t.key(code, True)

    def keyReleaseEvent(self, e):
        if e.isAutoRepeat():
            return
        t, code = self.held.pop(e.key(), (None, None))
        if t:
            t.key(code, False)

    def leaveEvent(self, e):
        super().leaveEvent(e)          # keep the last tile hot: keys still go there (no surprise drop)

    def focusOutEvent(self, e):
        for t, code in self.held.values():   # alt-tab away with a key held: release it on the phone
            t.key(code, False)
        self.held.clear()
        super().focusOutEvent(e)

    def tick(self):
        self.n += 1
        for t in self.tiles:
            t.refresh(self.n % 10 == 0)


def parse_phones(specs):
    """MODEL[:NAME] -> [(session, model, name)], a repeated default NAME gets -2, -3, ..."""
    out, used = [], set()
    for s in specs:
        model, _, name = s.partition(':')
        if model not in np_.TITLES:
            sys.exit('unknown model %s (one of: %s)' % (model, ', '.join(sorted(np_.TITLES))))
        if not name:
            name, k = model, 2
            while name in used:
                name, k = '%s-%d' % (model, k), k + 1
        elif name in used:
            sys.exit('phone %s given twice (two emulators on one flash file corrupt it)' % name)
        used.add(name)
        out.append(('phone-' + name, model, name))
    return out


def running(session):
    try:
        return ns.call(session, {'op': 'status'}, 2)['state'] in ('running', 'frozen', 'paused')
    except (OSError, ValueError, KeyError):
        return False


def stop(session):
    subprocess.run([sys.executable, ns.__file__, '-s', session, 'stop'], stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('phones', nargs='+', metavar='MODEL[:NAME]')
    ap.add_argument('--scale', type=int, default=1)
    ap.add_argument('--cols', type=int, default=0, help='tiles per row (default: about a square)')
    ap.add_argument('--fresh', action='store_true', help='factory-fresh: forget the saved flash of every phone')
    ap.add_argument('--keep', action='store_true', help='leave the sessions running when the window closes')
    a = ap.parse_args()
    phones = parse_phones(a.phones)
    pdir = ns.ROOT / 'build/ngage/phones'
    pdir.mkdir(parents=True, exist_ok=True)
    started, procs = [], []
    for session, model, name in phones:
        flash = pdir / (name + '.flash')
        up = running(session)
        if up and a.fresh:
            stop(session)
            up = False
        if a.fresh and flash.exists():
            flash.unlink()
        if not up:
            procs.append((session, subprocess.Popen(
                [sys.executable, ns.__file__, '-s', session, 'start', '--model', model, '--flash', str(flash)],
                stdout=subprocess.DEVNULL)))
    for session, p in procs:            # the starts run in parallel; each returns once its daemon answers
        if p.wait() == 0:
            started.append(session)
        else:
            print('%s did not start (see %s)' % (session, ns.sdir(session) / 'daemon.out'), file=sys.stderr)
    app = QtWidgets.QApplication(sys.argv)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: app.quit())
    w = Wall(phones, a.scale, a.cols)
    w.show()
    rc = app.exec()
    if not a.keep:
        for session in started:
            stop(session)
    return rc


if __name__ == '__main__':
    sys.exit(main())
