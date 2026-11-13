#!/usr/bin/env python3
"""UI timing of a hosted touch phone (the 808), headless: boots it with tools/hosted/htest.py --phone --gl on its own
files, then measures what a user feels, from the frame counter in the screen's shared memory (hdisplay.cpp):

    python3 tools/hosted/uibench.py [--rom nokia-808-rm807] [--rounds 4] [--out DIR] [--env NAME=VALUE ...]

  boot      seconds from QEMU start to system state 110 and to the home screen (frames settled 3 s)
  swipe     home screen page swipes (a 0.35 s finger move): frames per second over the move and 1 s after
  menu      Menu key -> first changed frame and settled (the application grid, and back)
  launch    Calendar from the application grid: tap -> settled; End closes it
The seed must be past the first-boot wizard (build/seeds/nokia-808-rm807, made 2026-11-13).

Timing only (docs/hosted-eka2/roadmap.md: measure time, profile only during a speed-up). Each run is one boot;
compare runs on an idle host. The VM, the GL server and the files are its own (--out, default a temp dir), so a
phone window that is open meanwhile is not touched. Original work for qembian.
"""
import argparse, json, mmap, os, signal, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHM_BYTES = 4 << 20
W, H = 360, 640


class Phone:
    def __init__(self, a):
        self.dir = a.out or tempfile.mkdtemp(prefix='uibench-')
        os.makedirs(self.dir, exist_ok=True)
        self.shm, self.log = os.path.join(self.dir, 'phone.shm'), os.path.join(self.dir, 'phone.log')
        for p in (self.shm, self.log):
            if os.path.exists(p):
                os.unlink(p)
        cmd = [sys.executable, os.path.join(ROOT, 'tools/hosted/htest.py'), '--rom', a.rom, '--phone', '--gl',
               '--shm', self.shm, '--serial', self.log]
        for e in a.env:
            cmd += ['--env', e]
        if a.smp:
            cmd += ['--smp', str(a.smp)]
        self.t0 = time.time()
        self.vm = subprocess.Popen(cmd, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.mm = None

    def stop(self):
        if self.vm.poll() is None:
            os.killpg(self.vm.pid, signal.SIGTERM)
            try:
                self.vm.wait(20)
            except subprocess.TimeoutExpired:
                os.killpg(self.vm.pid, signal.SIGKILL)

    def grep(self, s):
        try:
            with open(self.log, 'rb') as f:
                return s.encode() in f.read()
        except OSError:
            return False

    def frame(self):
        if self.mm is None:
            if not os.path.exists(self.shm) or os.path.getsize(self.shm) < SHM_BYTES:
                return -1
            self.f = open(self.shm, 'r+b')
            self.mm = mmap.mmap(self.f.fileno(), SHM_BYTES)
        return struct.unpack_from('<I', self.mm, 20)[0]

    def settle(self, quiet=0.4, limit=30.0):
        """wait until no frame changed for `quiet` s; returns (time of the first change or None, time settled)"""
        t, last, first, changed = time.time(), self.frame(), None, time.time()
        while time.time() - t < limit:
            f = self.frame()
            if f != last:
                last, changed = f, time.time()
                if first is None:
                    first = changed
            elif time.time() - changed >= quiet:
                break
            time.sleep(0.003)
        return first, changed

    def ptr(self, x, y, down):
        head, tail = struct.unpack_from('<2I', self.mm, 0x400)
        ax, ay = int(x * 32767 / (W - 1)), int(y * 32767 / (H - 1))
        struct.pack_into('<2I', self.mm, 0x408 + 8 * (head % 64), ax | ay << 16, 1 if down else 0)
        struct.pack_into('<I', self.mm, 0x400, (head + 1) & 0xffffffff)

    def key(self, code):
        head, _tail = struct.unpack_from('<2I', self.mm, 0x100)
        struct.pack_into('<I', self.mm, 0x108 + 4 * (head % 64), code | 0x80000000)
        struct.pack_into('<I', self.mm, 0x100, (head + 1) & 0xffffffff)
        time.sleep(0.08)
        t = time.time()
        struct.pack_into('<I', self.mm, 0x108 + 4 * ((head + 1) % 64), code)
        struct.pack_into('<I', self.mm, 0x100, (head + 2) & 0xffffffff)
        return t

    def tap(self, x, y):
        self.ptr(x, y, True)
        time.sleep(0.08)
        t = time.time()
        self.ptr(x, y, False)
        return t


def median(v):
    return sorted(v)[len(v) // 2] if v else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', default='nokia-808-rm807')
    ap.add_argument('--rounds', type=int, default=4)
    ap.add_argument('--out')
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--boot-limit', type=float, default=400)
    ap.add_argument('--smp', type=int, default=0, help='CPUs of the test VM (htest --smp)')
    a = ap.parse_args()
    MENU, END = 0xb4, 0xc5
    p = Phone(a)
    res = {'rom': a.rom}
    try:
        while not p.grep('101f8766/41 = 110'):
            if p.vm.poll() is not None or time.time() - p.t0 > a.boot_limit:
                res['error'] = 'no state 110 (log %s)' % p.log
                return 1
            time.sleep(0.2)
        res['boot_110_s'] = round(time.time() - p.t0, 1)
        p.settle(quiet=3.0, limit=120)
        res['boot_home_s'] = round(time.time() - p.t0 - 3.0, 1)
        fps = []
        for k in range(a.rounds):
            x0, x1 = (300, 60) if k % 2 == 0 else (60, 300)
            p.settle(quiet=1.0)
            f0, t = p.frame(), time.time()
            for i in range(7):
                p.ptr(x0 + (x1 - x0) * i // 6, 330, True)
                time.sleep(0.05)
            p.ptr(x1, 330, False)
            time.sleep(1.0)                                   # the page transition (page 2's empty slots animate)
            fps.append((p.frame() - f0) / (time.time() - t))
        res['swipe_fps'] = round(median(fps), 1)
        first, settled, launch = [], [], []
        for _ in range(a.rounds):
            for _twice in range(2):                           # open the application grid, close it again
                p.settle(quiet=1.0)
                t = p.key(MENU)
                f, s_ = p.settle(quiet=0.6)
                if f is not None:
                    first.append(f - t)
                    settled.append(s_ - t)
        res['menu_first_ms'] = round(1000 * median(first)) if first else None
        res['menu_settled_ms'] = round(1000 * median(settled)) if settled else None
        for _ in range(a.rounds):
            p.settle(quiet=1.0)
            p.key(MENU)
            p.settle(quiet=1.0)
            t = p.tap(180, 230)                               # Calendar
            f, s_ = p.settle(quiet=1.0, limit=60)
            if f is not None:
                launch.append(s_ - t)
            p.key(END)
            p.settle(quiet=1.0)
        res['launch_settled_ms'] = round(1000 * median(launch)) if launch else None
    finally:
        p.stop()
        print(json.dumps(res))
    return 0


if __name__ == '__main__':
    sys.exit(main())
