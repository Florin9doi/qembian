#!/usr/bin/env python3
"""The same UI actions on a running 808 in QEMU (a sydb session) and on the hosted kernel, timed the same way:
from the GL device's composed-frame counter (syborg-gl frame-file "FSEQ": word 1 is bumped twice per frame that
the device composes), not from a window. Both run the same syborg_gl.c, so a frame means the same thing.

    python3 tools/ui_compare.py --qemu build/sydb-x808                     # a sydb session at the home screen
    python3 tools/ui_compare.py --hosted RUNDIR                            # hosted phone: RUNDIR/phone.shm, gl.chan.frames

Actions, from the home screen, --rounds times: Menu (open the application grid) and Menu again (close);
a page swipe to the left and back; Calendar from the grid (tap), then End. For each: input -> first composed frame,
input -> settled (no new frame for 0.6 s; page 2 animates, so swipes use a 1.5 s window instead), and frames
composed in the first 1.5 s. Timing only, medians. Original work for qembian.
"""
import argparse, json, mmap, os, struct, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 360, 640


class Frames:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.m = mmap.mmap(self.f.fileno(), 4096, access=mmap.ACCESS_READ)

    def count(self):
        return struct.unpack_from('<I', self.m, 4)[0] // 2


class Qemu:
    def __init__(self, sdir):
        sys.path.insert(0, os.path.join(ROOT, 'tools/sydb'))
        import sydb as S
        self.c = S.Client(sdir)
        self.frames = Frames(os.path.join(sdir, 'frame.seq'))

    def ptr(self, x, y, down):
        ev = [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (W - 1))}},
              {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (H - 1))}},
              {'type': 'btn', 'data': {'down': down, 'button': 'left'}}]
        self.c.call({'op': 'qmp', 'cmd': 'input-send-event', 'args': {'events': ev}}, 5)

    def move(self, x, y):
        ev = [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (W - 1))}},
              {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (H - 1))}}]
        self.c.call({'op': 'qmp', 'cmd': 'input-send-event', 'args': {'events': ev}}, 5)

    QCODE = {'menu': 'f8', 'end': 'f3', 'lock': 'f13'}           # the 808's host keys (models/nokia-808-rm807/keymap.toml)

    def pixels(self, pts):
        import tempfile
        from PIL import Image
        path = os.path.join(tempfile.gettempdir(), 'ui_compare_%d.png' % os.getpid())
        self.c.call({'op': 'screenshot', 'path': path, 'scale': 1.0}, 30)
        im = Image.open(path).convert('RGB')
        return [im.getpixel(p) for p in pts]

    def key(self, name):
        def ev(down):
            return [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': self.QCODE[name]}}}]
        self.c.call({'op': 'qmp', 'cmd': 'input-send-event', 'args': {'events': ev(True)}}, 5)
        time.sleep(0.08)
        t = time.time()
        self.c.call({'op': 'qmp', 'cmd': 'input-send-event', 'args': {'events': ev(False)}}, 5)
        return t


class Hosted:
    SCAN = {'menu': 0xb4, 'end': 0xc5, 'lock': 0xb3}

    def __init__(self, rundir):
        f = open(os.path.join(rundir, 'phone.shm'), 'r+b')
        self.mm = mmap.mmap(f.fileno(), 4 << 20)
        self.frames = Frames(os.path.join(rundir, 'gl.chan.frames'))
        self.down = False

    def _ring(self, x, y, down):
        head, _ = struct.unpack_from('<2I', self.mm, 0x400)
        struct.pack_into('<2I', self.mm, 0x408 + 8 * (head % 64),
                         int(x * 32767 / (W - 1)) | int(y * 32767 / (H - 1)) << 16, 1 if down else 0)
        struct.pack_into('<I', self.mm, 0x400, (head + 1) & 0xffffffff)

    def pixels(self, pts):
        w, h, pitch = struct.unpack_from('<3I', self.mm, 8)
        out = []
        for x, y in pts:
            v = struct.unpack_from('<I', self.mm, 0x1000 + y * pitch + 4 * x)[0]
            out.append(((v >> 16) & 255, (v >> 8) & 255, v & 255))
        return out

    def ptr(self, x, y, down):
        self.down = down
        self._ring(x, y, down)

    def move(self, x, y):
        self._ring(x, y, self.down)

    def key(self, name):
        code = self.SCAN[name]
        head, _ = struct.unpack_from('<2I', self.mm, 0x100)
        struct.pack_into('<I', self.mm, 0x108 + 4 * (head % 64), code | 0x80000000)
        struct.pack_into('<I', self.mm, 0x100, (head + 1) & 0xffffffff)
        time.sleep(0.08)
        t = time.time()
        struct.pack_into('<I', self.mm, 0x108 + 4 * ((head + 1) % 64), code)
        struct.pack_into('<I', self.mm, 0x100, (head + 2) & 0xffffffff)
        return t


def watch(p, t0, quiet=0.6, window=1.5, limit=20.0, c0=None):
    """after an input at t0 (frame count c0 before it): (first frame delay, settled delay, frames in the first
    `window` s). Frames composed while the input call was still running count from the call's start."""
    if c0 is None:
        c0 = p.frames.count()
    first, last_change, last = None, time.time(), c0
    in_window = 0
    c = p.frames.count()
    if c != c0:                                  # frames already composed during the input call
        first, last_change, last, in_window = time.time() - t0, time.time(), c, c - c0
    while time.time() - t0 < limit:
        c = p.frames.count()
        now = time.time()
        if c != last:
            if first is None:
                first = now - t0
            if now - t0 <= window:
                in_window += c - last
            last, last_change = c, now
        elif first is not None and now - last_change >= quiet:
            break
        elif first is None and now - t0 > 8:
            break
        time.sleep(0.002)
    return first, (last_change - t0) if first is not None else None, in_window


# the application grid: the glyphs of its first row (Contacts, Messages, Internet) are near-white and the Gallery
# icon light blue; on the home screen those places are wallpaper (End from an app returns to the grid, not home)
GRID_PTS = [(60, 90), (180, 90), (300, 90), (60, 230)]


def in_grid(p):
    px = p.pixels(GRID_PTS)
    return all(min(c) > 200 for c in px[:3]) and px[3][2] > 200


# the status bar's key icon (keypad locked: the 808 locks itself after about a minute without input, and a locked
# phone ignores touch): white pixels in this box
LOCK_PTS = [(x, y) for y in range(6, 20, 2) for x in range(178, 206, 2)]


def unlock(p):
    for _ in range(2):
        if sum(1 for c in p.pixels(LOCK_PTS) if min(c) > 200) < 5:
            return True
        p.key('lock')
        quiet(p, 1.0)
    return sum(1 for c in p.pixels(LOCK_PTS) if min(c) > 200) < 5


def to_home(p):
    """Menu toggles the grid and the home screen: press it until the grid is gone (unlocked first)"""
    unlock(p)
    for _ in range(3):
        quiet(p, 1.0)
        if not in_grid(p):
            return True
        p.key('menu')
    quiet(p, 1.0)
    return not in_grid(p)


def quiet(p, secs=1.0, limit=30):
    t = time.time()
    last, since = p.frames.count(), time.time()
    while time.time() - t < limit:
        c = p.frames.count()
        if c != last:
            last, since = c, time.time()
        elif time.time() - since >= secs:
            return
        time.sleep(0.01)


def med(v):
    v = [x for x in v if x is not None]
    return round(1000 * sorted(v)[len(v) // 2]) if v else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--qemu')
    ap.add_argument('--hosted')
    ap.add_argument('--rounds', type=int, default=3)
    a = ap.parse_args()
    p = Qemu(a.qemu) if a.qemu else Hosted(a.hosted)
    res = {'backend': 'qemu' if a.qemu else 'hosted'}
    acc = {k: [] for k in ('menu_open_first', 'menu_open_settled', 'menu_open_frames', 'menu_close_first',
                           'menu_close_settled', 'swipe_first', 'swipe_frames', 'cal_first', 'cal_settled',
                           'cal_frames')}
    for _ in range(a.rounds):
        p.key('end')                                  # leave any app (it returns to the grid or home)
        if not to_home(p):
            res['error'] = 'could not reach the home screen'
            break
        c0, t0 = p.frames.count(), time.time()
        p.key('menu')
        f, s, n = watch(p, t0, c0=c0)
        acc['menu_open_first'].append(f); acc['menu_open_settled'].append(s); acc['menu_open_frames'].append(n)
        quiet(p)
        c0, t0 = p.frames.count(), time.time()
        p.key('menu')
        f, s, n = watch(p, t0, c0=c0)
        acc['menu_close_first'].append(f); acc['menu_close_settled'].append(s)
        to_home(p)
        for x0, x1 in ((300, 60), (60, 300)):
            quiet(p, 0.5, 5)
            c0, t0 = p.frames.count(), time.time()          # counted from the touch-down
            p.ptr(x0, 330, True)
            for i in range(1, 7):
                time.sleep(0.05)
                p.move(x0 + (x1 - x0) * i // 6, 330)
            p.ptr(x1, 330, False)
            f, s, n = watch(p, t0, quiet=10, window=1.5, limit=1.5, c0=c0)
            acc['swipe_first'].append(f); acc['swipe_frames'].append(n)
        to_home(p)
        p.key('menu')
        quiet(p)
        if not in_grid(p):
            res['error'] = 'Menu did not open the grid'
            break
        t0 = time.time()
        p.ptr(180, 230, True); time.sleep(0.08); p.ptr(180, 230, False)       # Calendar
        f, s, n = watch(p, t0, quiet=1.0, limit=30)
        acc['cal_first'].append(f); acc['cal_settled'].append(s); acc['cal_frames'].append(n)
        p.key('end')
        quiet(p, 1.0)
    for k, v in acc.items():
        res[k] = med(v) if not k.endswith('frames') else (sorted(v)[len(v) // 2] if v else None)
    res['swipe_fps'] = round(res['swipe_frames'] / 1.5, 1) if res['swipe_frames'] is not None else None
    print(json.dumps(res))
    return 0


if __name__ == '__main__':
    sys.exit(main())
