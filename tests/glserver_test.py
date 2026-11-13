#!/usr/bin/env python3
"""glserver (src/hosted/glserver: QEMU's syborg-gl device as a host process) driven over its channel
(src/hosted/include/board/syborg_glchan.h) the way the hosted EKA2 board drives it: register reads/writes,
memory callbacks served from a fake guest RAM. Needs a host GL (Mesa surfaceless EGL); skips without one.

    python3 tests/glserver_test.py        (builds the server first)

Checks: ID and CAPS; a kernel-run command batch in physical memory (OWNER, SURF, CLEAR red, READ) returns
red pixels through the read-back area; a user write to a kernel register is refused; a run without PID is
refused (result ~0); the server exits when the channel is cleared.
"""
import mmap, os, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = {'MAGIC': 0, 'VERSION': 1, 'UP': 2, 'REQ_SEQ': 8, 'REQ_OP': 9, 'REQ_REG': 10, 'REQ_VALUE': 11, 'REQ_USER': 12,
     'REQ_PID': 13, 'RESP_SEQ': 16, 'RESP_VALUE': 17, 'CB_SEQ': 24, 'CB_OP': 25, 'CB_ADDR': 26, 'CB_LEN': 27,
     'CB_DONE': 28, 'CB_STATUS': 29, 'NPRE': 36, 'NPF': 37, 'WLOG_N': 38, 'WLOG_BYTES': 39, 'PRE': 40, 'PF': 96}
WLOG, PF = 24 << 20, 32 << 20
DATA, BYTES = 0x10000, 64 << 20
REG = {'ID': 0, 'CAPS': 1, 'BUF': 2, 'LEN': 3, 'EXEC': 4, 'OUT': 5, 'OUTLEN': 6, 'KICK': 7, 'PID': 8, 'FLAGS': 9}
PHYS = 0x20000000
RAM = bytearray(1 << 20)          # fake guest physical RAM at PHYS
fails = 0


def check(cond, what):
    global fails
    print('  %s %s' % ('ok' if cond else 'FAIL', what))
    fails += 0 if cond else 1


class Chan:
    def __init__(self, path):
        self.f = open(path, 'r+b')
        self.m = mmap.mmap(self.f.fileno(), BYTES)

    def get(self, k):
        return struct.unpack_from('<I', self.m, 4 * W[k])[0]

    def put(self, k, v):
        struct.pack_into('<I', self.m, 4 * W[k], v & 0xffffffff)

    def serve_callbacks(self):
        seq = self.get('CB_SEQ')
        if seq == self.get('CB_DONE'):
            return
        op, addr, n = self.get('CB_OP'), self.get('CB_ADDR'), self.get('CB_LEN')
        ok = op in (3, 4) and PHYS <= addr and addr + n <= PHYS + len(RAM)
        if ok and op == 3:
            self.m[DATA:DATA + n] = bytes(RAM[addr - PHYS:addr - PHYS + n])
        elif ok and op == 4:
            RAM[addr - PHYS:addr - PHYS + n] = self.m[DATA:DATA + n]
        self.put('CB_STATUS', 0 if ok else 1)
        self.put('CB_DONE', seq)

    def req(self, op, reg, value=0, user=0, pid=0, pre=(), pf=()):
        for i, (r, v) in enumerate(pre):             # v3: deferred writes
            struct.pack_into('<2I', self.m, 4 * (W['PRE'] + 2 * i), REG[r] * 4, v)
        self.put('NPRE', len(pre))
        off = 0
        for i, (va, data) in enumerate(pf):           # v3: prefetched virtual memory
            self.m[PF + off:PF + off + len(data)] = data
            struct.pack_into('<3I', self.m, 4 * (W['PF'] + 3 * i), va, len(data), off)
            off += (len(data) + 63) & ~63
        self.put('NPF', len(pf))
        self.put('REQ_OP', op); self.put('REQ_REG', REG[reg] * 4); self.put('REQ_VALUE', value)
        self.put('REQ_USER', user); self.put('REQ_PID', pid)
        seq = (self.get('REQ_SEQ') + 1) & 0xffffffff
        self.put('REQ_SEQ', seq)
        t0 = time.time()
        while self.get('RESP_SEQ') != seq:
            self.serve_callbacks()
            if time.time() - t0 > 20:
                raise RuntimeError('glserver did not answer')
        return self.get('RESP_VALUE')

    def rd(self, reg, **kw):
        return self.req(1, reg, **kw)

    def wr(self, reg, v, **kw):
        return self.req(2, reg, v, **kw)


def main():
    r = subprocess.run(['bash', os.path.join(ROOT, 'scripts/build-glserver.sh')], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 1
    d = tempfile.mkdtemp()
    path = os.path.join(d, 'gl.chan')
    open(path, 'wb').truncate(BYTES)
    frames = path + '.frames'
    srv = subprocess.Popen([os.path.join(ROOT, 'build/hosted/glserver/glserver'), '--chan', path, '--frame-file', frames],
                           stderr=subprocess.PIPE, text=True)
    c = Chan(path)
    t0 = time.time()
    while c.get('UP') != 1 and time.time() - t0 < 10:
        time.sleep(0.01)
    check(c.get('MAGIC') == 0x434c4753 and c.get('UP') == 1, 'server up, channel magic')
    check(c.rd('ID') == 0xc51d0011, 'ID register: SYGL_ID')
    # 2026-11-13: the composed-frame counter (as QEMU's syborg-gl.frame-file) drives publishing each frame at once
    # and tools/ui_compare.py; before, the server published on a 16 ms timer and the window showed half the frames
    check(os.path.exists(frames) and open(frames, 'rb').read(4) == b'FSEQ', '--frame-file: the FSEQ counter file')
    caps = c.rd('CAPS')
    if not caps & 1:
        print('  SKIP: no host GL here (CAPS %x)' % caps)
        c.put('MAGIC', 0)
        srv.wait(10)
        return 77
    check(True, 'CAPS: host GL ready')
    # a kernel run (EXEC) of a command buffer in physical memory: surface 1 (8x4), clear red, read 2 rows
    cmds = [8, 3, 5,                              # OWNER 5
            1, 6, 1, 8, 4, 0,                     # SURF id 1, 8x4, no depth
            5, 10, 1, 1, 0xff0000, 0, 0, 0, 8, 4,  # CLEAR surf 1, colour, red, z, x y w h
            7, 6, 1, 0, 0, 2]                     # READ surf 1 -> OUT + 0, rows 0..1
    buf = struct.pack('<%dI' % len(cmds), *cmds)
    RAM[0:len(buf)] = buf
    out = 0x1000
    RAM[out:out + 64] = b'\xaa' * 64
    c.wr('BUF', PHYS); c.wr('LEN', len(buf)); c.wr('OUT', PHYS + out); c.wr('OUTLEN', 64)
    c.wr('EXEC', 1)
    check(c.rd('EXEC') == 0xffffffff, 'a kernel run without PID is refused')
    c.wr('PID', 5); c.wr('FLAGS', 1)
    c.wr('EXEC', 1)
    res = c.rd('EXEC')
    px = struct.unpack_from('<16I', RAM, out)
    check(res == 0, 'kernel run with PID 5: result %d' % res)
    check(all((p & 0xffffff) == 0xff0000 for p in px), 'read-back: 2 rows of 8 red pixels through the callbacks (%08x)' % px[0])
    # v3: a kick from "virtual" memory the fake guest never serves (virtual callbacks are refused): the descriptor and
    # the command words come from the prefetch area, PID/FLAGS as deferred writes, the result and the read-back
    # pixels through the write log (bug 2026-11-13 was the cost: 4 round trips and ~3 callbacks per kick)
    kva, cva, ova = 0x00500000, 0x00600000, 0x00700000
    cmds2 = [8, 3, 6, 1, 6, 2, 8, 4, 0, 5, 10, 2, 1, 0x00ff00, 0, 0, 0, 8, 4, 7, 6, 2, 0, 0, 1]
    cbuf = struct.pack('<%dI' % len(cmds2), *cmds2)
    kick = struct.pack('<5I', cva, len(cbuf), ova, 32, 0xdeadbeef)
    cb0 = c.get('CB_SEQ')
    res = c.req(2, 'KICK', kva, pre=(('PID', 6), ('FLAGS', 1)), pf=((kva, kick), (cva, cbuf)))
    n = c.get('WLOG_N')
    log, off = [], 0
    for _ in range(n):
        va, ln = struct.unpack_from('<2I', c.m, WLOG + off)
        log.append((va, bytes(c.m[WLOG + off + 8:WLOG + off + 8 + ln])))
        off += 8 + ((ln + 3) & ~3)
    check(c.get('CB_SEQ') == cb0, 'v3 kick: no memory callback (descriptor and commands prefetched)')
    check(res == 0, 'v3 kick: the response is the result read after the write (%#x)' % res)
    pix = [d for va, d in log if va == ova]
    check(len(pix) == 1 and all((p & 0xffffff) == 0x00ff00 for p in struct.unpack('<8I', pix[0])),
          'v3 kick: the read-back row (8 green pixels) is in the write log')
    check(any(va == kva + 16 and d == b'\0\0\0\0' for va, d in log), 'v3 kick: the result word is in the write log')
    c.wr('BUF', 0x12345678, user=1)
    check(c.rd('BUF') == PHYS, 'a user-mode write to a kernel register is refused')
    # bug 2026-11-13 (pacing): EGL window swaps answered at once, so the 808's home-screen spinners drew back to back,
    # took the whole CPU and starved the widgets' loaders. A run that presents (SYGL_VG_PRESENT) answers at the next
    # vsync (default 60 Hz, as eglSwapBuffers with swap interval 1); a run without one is not held.
    def run(cmds):
        b = struct.pack('<%dI' % len(cmds), *cmds)
        RAM[0:len(b)] = b
        c.wr('BUF', PHYS); c.wr('LEN', len(b)); c.wr('OUTLEN', 0); c.wr('PID', 5); c.wr('FLAGS', 1)
        c.wr('EXEC', 1)
        return time.time()
    present = [8, 3, 5, 13, 7, 0x77, 1, 2, 3, 4]       # OWNER 5; VG_PRESENT surf 0x77 (none: a no-op swap) -> sid
    plain = [8, 3, 5]
    done = [run(present) for _ in range(5)]
    gaps = [b - a for a, b in zip(done, done[1:])]
    check(min(gaps) > 0.012, 'presenting runs answer one vsync apart (gaps %s ms)' % [round(g * 1000) for g in gaps])
    t0 = time.time()
    for _ in range(5):
        run(plain)
    check(time.time() - t0 < 0.05, 'runs without a present are not held (%d ms for 5)' % ((time.time() - t0) * 1000))
    c.put('MAGIC', 0)
    try:
        srv.wait(10)
        check(True, 'the server exits when the channel is cleared')
    except subprocess.TimeoutExpired:
        srv.kill()
        check(False, 'the server exits when the channel is cleared')
    print('glserver_test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
