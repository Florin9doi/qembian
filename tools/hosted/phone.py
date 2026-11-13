#!/usr/bin/env python3
"""The hosted EKA2 phone in a window: boots the device's user side on the hosted kernel (the test VM,
tools/hosted/htest.py --phone) and draws its screen from the shared memory the VM writes it into
(src/hosted/variant/hdisplay.cpp: header, key ring, XRGB8888 pixels). Closing the window ends the VM.

    python3 tools/hosted/phone.py [--rom DEVICE] [--scale 2] [--gl] [--shm FILE --serial FILE]
    python3 tools/hosted/phone.py --attach /tmp/qembian/phone.shm [--rom DEVICE]     (a phone already running)

Keys: the host keyboard as in the sydb phone window (tools/sydb/phone.py HOST_KEYS: arrows, Enter = OK,
F5/F7 = soft keys, 0-9, * and #, Backspace = C, ...), mapped to the device's scan codes by its keymap
(models/<model>/keymap.toml through tools/device.py qemu_keymap), and the keymap's front, nav and side keys as
buttons beside the screen (the 808: Menu, Call, End, Power, Lock, volume, camera).
Touch phones (profile board.display.touch): the mouse is the finger (press, drag, release).
The window takes the screen size of the device profile and then of the frames the VM writes.

Ctrl+P writes a diagnosis into the serial log (PHONEDIAG: servers with unanswered messages, waiting threads).
Ctrl+T opens the Linux console beside the phone: a `top` of the VM (src/hosted/init lxtop, every 2 s on the
serial log) with the one Linux process "symbian" and the Symbian threads as its Linux threads.
"""
import argparse, importlib.util, mmap, os, re, signal, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHM = os.path.join(ROOT, 'build/hosted/htest/phone.shm')
SHM_BYTES = 4 << 20             # htest.py PHONE_SHM_BYTES
MAGIC = 0x42465048
PIXELS = 0x1000
KEY_HEAD, KEY_TAIL, KEY_RING, KEY_SLOTS = 0x100, 0x104, 0x108, 64
PTR_HEAD, PTR_TAIL, PTR_RING, PTR_SLOTS = 0x400, 0x404, 0x408, 64
SERIAL = os.path.join(ROOT, 'build/hosted/htest/phone.log')
CONSOLE_W = 600


class TopReader:
    """the latest complete LXTOP block from the VM's serial log (lines between other output are skipped)"""
    def __init__(self, path):
        self.path, self.pos, self.partial, self.block, self.latest = path, 0, b'', [], []

    def poll(self):
        try:
            with open(self.path, 'rb') as f:
                if os.fstat(f.fileno()).st_size < self.pos:
                    self.pos = 0            # a new VM run
                f.seek(self.pos)
                data = f.read()
                self.pos += len(data)
        except OSError:
            return False
        lines = (self.partial + data).split(b'\n')
        self.partial = lines.pop()
        changed = False
        for l in lines:
            if l.startswith(b'LXTOP-END'):
                self.latest, self.block, changed = self.block, [], True
            elif l.startswith(b'LXTOP'):
                self.block.append(l[6:].decode('latin-1').rstrip())
        return changed


def device_info(device):
    """(Qt key -> scan code, keymap buttons [(label, scan code, group)], display dict) of the device profile:
    sydb's Qt key -> QEMU qcode table, then the device keymap's qcode -> scan code"""
    spec = importlib.util.spec_from_file_location('sydb_phone', os.path.join(ROOT, 'tools/sydb/phone.py'))
    sp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sp)
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import device as devmodel
    prof = devmodel.load(device)
    qcode = {}
    for line in devmodel.qemu_keymap(prof):
        m = re.match(r'(\S+) (0x[0-9a-f]+)', line)
        if m:
            qcode.setdefault(m.group(1), int(m.group(2), 16))
    buttons = [(k.get('label', k['key']), qcode[k['host'][0]], k.get('group'))
               for k in devmodel.keymap(prof).get('keys', [])
               if k.get('host') and k['host'][0] in qcode and k.get('group') in ('front', 'nav', 'side')]
    return ({k: qcode[q] for k, q in sp.HOST_KEYS.items() if q in qcode}, buttons,
            prof.get('board', {}).get('display', {}))


def push_key(mm, code, pressed):
    """one entry into the key ring (the VM's display thread feeds it to the keyboard model); False when full"""
    head, tail = struct.unpack_from('<2I', mm, KEY_HEAD)
    if (head - tail) & 0xffffffff >= KEY_SLOTS:
        return False
    struct.pack_into('<I', mm, KEY_RING + 4 * (head % KEY_SLOTS), (code & 0xffff) | (0x80000000 if pressed else 0))
    struct.pack_into('<I', mm, KEY_HEAD, (head + 1) & 0xffffffff)
    return True


def push_pointer(mm, x, y, down):
    """one sample into the pointer ring (absolute 0..32767); False when full"""
    head, tail = struct.unpack_from('<2I', mm, PTR_HEAD)
    if (head - tail) & 0xffffffff >= PTR_SLOTS:
        return False
    struct.pack_into('<2I', mm, PTR_RING + 8 * (head % PTR_SLOTS), (x & 0x7fff) | (y & 0x7fff) << 16, 1 if down else 0)
    struct.pack_into('<I', mm, PTR_HEAD, (head + 1) & 0xffffffff)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', default=os.environ.get('SYBORG_DEVICE', 'nokia-n95-8gb-rm320'))
    ap.add_argument('--scale', type=float, default=0, help='screen pixels per phone pixel (default: 2, 1 for big screens)')
    ap.add_argument('--gl', action='store_true',
                    help='syborg-gl on the host GL server (htest --gl): phones that compose on the GPU (the 808)')
    ap.add_argument('--shm', default=SHM, help='the shared screen/keys file (a second phone at once needs its own)')
    ap.add_argument('--serial', default=SERIAL, help='the VM serial log (with --shm)')
    ap.add_argument('--attach', metavar='FILE',
                    help='show a phone that already runs and writes its screen into FILE (on a real device: '
                         'tools/hosted/device-bundle.py, run.sh); no VM is started, closing the window leaves it running')
    a = ap.parse_args()
    shm, serial = (a.attach, a.serial) if a.attach else (a.shm, a.serial)

    from PyQt6 import QtCore, QtGui, QtWidgets

    # the VM (it creates the shared file before QEMU starts)
    if a.attach:
        vm = None
    else:
      if os.path.exists(shm):
        os.unlink(shm)
      vm = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools/hosted/htest.py'), '--rom', a.rom, '--phone',
                           '--shm', shm, '--serial', serial] + (['--gl'] if a.gl else []), start_new_session=True)

    app = QtWidgets.QApplication(sys.argv)
    keys, key_buttons, disp = device_info(a.rom)
    touch = bool(disp.get('touch'))
    size = [disp.get('width', 240), disp.get('height', 320)]       # until the first frame says otherwise
    scale = a.scale or (2 if size[1] <= 400 else 1)
    state = {'mm': None}

    def push(code, pressed):
        if state['mm'] is not None:
            push_key(state['mm'], code, pressed)

    class Screen(QtWidgets.QWidget):
        def __init__(self):
            super().__init__()
            self.setFixedSize(int(size[0] * scale), int(size[1] * scale))
            self.image = None
            self.down = False

        def set_frame(self, img):
            if [img.width(), img.height()] != size:
                size[:] = [img.width(), img.height()]
                self.setFixedSize(int(size[0] * scale), int(size[1] * scale))
            self.image = img
            self.update()

        def finger(self, e, down):
            if not touch or state['mm'] is None:
                return
            p = e.position()
            x = min(max(int(p.x() * 32767 / max(1, self.width() - 1)), 0), 32767)
            y = min(max(int(p.y() * 32767 / max(1, self.height() - 1)), 0), 32767)
            push_pointer(state['mm'], x, y, down)

        def mousePressEvent(self, e):
            if e.button() == QtCore.Qt.MouseButton.LeftButton:
                self.down = True
                self.finger(e, True)

        def mouseMoveEvent(self, e):
            if self.down:
                self.finger(e, True)

        def mouseReleaseEvent(self, e):
            if e.button() == QtCore.Qt.MouseButton.LeftButton and self.down:
                self.down = False
                self.finger(e, False)

        def paintEvent(self, _e):
            p = QtGui.QPainter(self)
            if self.image is None:
                p.fillRect(self.rect(), QtGui.QColor(0, 0, 0))
                p.setPen(QtGui.QColor(200, 200, 200))
                p.drawText(self.rect(), QtCore.Qt.AlignmentFlag.AlignCenter, 'booting...  (Ctrl+T: Linux console)')
            else:
                if scale != int(scale):
                    p.setRenderHint(QtGui.QPainter.RenderHint.SmoothPixmapTransform)
                p.drawImage(self.rect(), self.image)

    class Console(QtWidgets.QWidget):
        def __init__(self):
            super().__init__()
            self.setFixedWidth(CONSOLE_W)
            self.font = QtGui.QFont('monospace', 9)
            self.font.setStyleHint(QtGui.QFont.StyleHint.Monospace)
            self.top = TopReader(serial)

        def paintEvent(self, _e):
            p = QtGui.QPainter(self)
            r = self.rect()
            p.fillRect(r, QtGui.QColor(0, 0, 0))
            p.setFont(self.font)
            p.setPen(QtGui.QColor(80, 255, 120))
            lh = QtGui.QFontMetrics(self.font).lineSpacing()
            lines = ['root@hosted:~# top        (Ctrl+T: back)', ''] + (self.top.latest or ['waiting for the VM...'])
            for i, l in enumerate(lines):
                y = 8 + (i + 1) * lh
                if y > r.bottom():
                    break
                p.drawText(8, y, l)

    class Window(QtWidgets.QWidget):
        def __init__(self):
            super().__init__()
            self.setWindowTitle('hosted %s' % a.rom)
            self.setStyleSheet('QWidget#body { background: #2b2b2e; } QPushButton { background: #4a4a50; color: #eee; '
                               'border: 1px solid #666; border-radius: 6px; padding: 4px 10px; min-height: 24px; } '
                               'QPushButton:pressed { background: #8a8a95; }')
            self.setFocusPolicy(QtCore.Qt.FocusPolicy.StrongFocus)
            row = QtWidgets.QHBoxLayout(self)
            row.setContentsMargins(0, 0, 0, 0)
            row.setSpacing(0)
            body = QtWidgets.QWidget(objectName='body')
            col = QtWidgets.QVBoxLayout(body)
            col.setContentsMargins(8, 8, 8, 8)
            self.screen = Screen()
            col.addWidget(self.screen, alignment=QtCore.Qt.AlignmentFlag.AlignHCenter)
            front = QtWidgets.QHBoxLayout()
            side = QtWidgets.QVBoxLayout()
            for label, code, group in key_buttons:
                b = QtWidgets.QPushButton(label)
                b.setFocusPolicy(QtCore.Qt.FocusPolicy.NoFocus)
                b.pressed.connect(lambda c=code: push(c, True))
                b.released.connect(lambda c=code: push(c, False))
                (side if group == 'side' else front).addWidget(b)
            col.addLayout(front)
            row.addWidget(body)
            if side.count():
                side.addStretch(1)
                sw = QtWidgets.QWidget(objectName='body')
                sw.setLayout(side)
                row.addWidget(sw)
            self.console = Console()
            self.console.hide()
            row.addWidget(self.console)
            self.frame = -1
            t = QtCore.QTimer(self)
            t.timeout.connect(self.poll)
            t.start(20)

        def poll(self):
            if vm is not None and vm.poll() is not None:
                self.close()
                return
            if self.console.top.poll() and self.console.isVisible():
                self.console.update()
            if state['mm'] is None:
                if not os.path.exists(shm) or os.path.getsize(shm) < SHM_BYTES:
                    return
                f = open(shm, 'r+b')
                state['mm'] = mmap.mmap(f.fileno(), SHM_BYTES)
            mm = state['mm']
            magic, _ver, fw, fh, pitch, frame = struct.unpack_from('<6I', mm, 0)
            if magic != MAGIC or not fw or frame == self.frame or PIXELS + pitch * fh > SHM_BYTES:
                return
            self.frame = frame
            data = bytes(mm[PIXELS:PIXELS + pitch * fh])
            self.screen.set_frame(QtGui.QImage(data, fw, fh, pitch, QtGui.QImage.Format.Format_RGB32).copy())
            self.adjustSize()

        def key(self, e, pressed):
            code = keys.get(e.key())
            if code is None or e.isAutoRepeat() or state['mm'] is None:
                return False
            push(code, pressed)
            return True

        def keyPressEvent(self, e):
            if e.key() == QtCore.Qt.Key.Key_T and e.modifiers() & QtCore.Qt.KeyboardModifier.ControlModifier:
                self.console.setVisible(not self.console.isVisible())
                self.console.setFixedHeight(self.screen.height() + 16)
                self.adjustSize()
                return
            if e.key() == QtCore.Qt.Key.Key_P and e.modifiers() & QtCore.Qt.KeyboardModifier.ControlModifier:
                if state['mm'] is not None:      # a diagnosis into the serial log (kn_efile PhoneDiag: who waits on whom)
                    n = struct.unpack_from('<I', state['mm'], 0x80)[0]
                    struct.pack_into('<I', state['mm'], 0x80, (n + 1) & 0xffffffff)
                return
            if not self.key(e, True):
                super().keyPressEvent(e)

        def keyReleaseEvent(self, e):
            if not self.key(e, False):
                super().keyReleaseEvent(e)

        def closeEvent(self, e):
            stop_vm()
            e.accept()

    def stop_vm():
        if vm is not None and vm.poll() is None:
            os.killpg(vm.pid, signal.SIGTERM)
            try:
                vm.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(vm.pid, signal.SIGKILL)

    s = Window()
    s.show()
    # Ctrl-C / kill: close the window (and the VM) instead of leaving QEMU running
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: app.quit())
    tick = QtCore.QTimer()
    tick.timeout.connect(lambda: None)		# lets Python run its signal handlers inside Qt's loop
    tick.start(200)
    try:
        return app.exec()
    finally:
        stop_vm()


if __name__ == '__main__':
    sys.exit(main())
