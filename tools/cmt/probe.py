#!/usr/bin/env python3
"""Run the N95 modem firmware (MCUSW) on the n95-cmt probe machine and report what it touched.

  python3 tools/cmt/probe.py [--boot nolo|mcusw] [--secs 5] [--map tools/cmt/n95-cmt.map] [--out DIR] [--exact]

--boot nolo (default): the whole modem NOR (tools/cmt/nor.py) at NOR_BASE, the PC at NOLO's code
(NOR 0xa0400), as the boot chain would start it. --boot mcusw: MCUSW alone at its link address
0xf0100000 (the first probe: it stops in bse_pre_init without NOLO's hw_conf records).

Input: build/n95-cmt/ (MCUSW@003e0000.hdr + MCUSW@00000200.bin, extracted from the RM-320 C0R by
build/n95-cmt/fpsx_parts.py; docs/research/modem-firmware-in-flash-files.md). Builds mcusw.img (the 1 KB
header + code, linked at 0xf0100000), runs QEMU (exact PCs only with --exact), samples the PC
over QMP, then prints: exceptions, PC hot spots (where it spins), and the learned accesses grouped by 64 KB
region. Docs: docs/research/n95-cmt-emulation.md.
"""
import signal
import argparse, collections, shutil, json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CMT = os.path.join(ROOT, 'build', 'n95-cmt')
QEMU = os.path.join(ROOT, 'build', 'qemu-modern', 'install', 'bin', 'qemu-system-arm')
NOR_FILE_BASE = 0x200            # MCUSW@00000200.bin starts at NOR offset 0x200
MCUSW_NOR = 0x3e0000             # header at NOR 0x3e0000, code from 0x3e0400 (linked at 0xf0100000)
MCUSW_END = 0x7cca00
NOR_BASE = 0x00000000            # [V] NOLO sets its TOC pointer to 0x1000 = NOR 0x1000 ("RAP3" TOC)
NOLO_CODE = 0xa0400              # NOLO header at NOR 0xa0000 (entry offset +0x1c = 0x400)


def build_image(out):
    hdr = open(os.path.join(CMT, 'MCUSW@003e0000.hdr'), 'rb').read()
    assert len(hdr) == 0x400, 'header must be 1 KB'
    with open(os.path.join(CMT, 'MCUSW@00000200.bin'), 'rb') as f:
        f.seek(MCUSW_NOR + 0x400 - NOR_FILE_BASE)
        code = f.read(MCUSW_END - MCUSW_NOR - 0x400)
    img = os.path.join(out, 'mcusw.img')
    with open(img, 'wb') as f:
        f.write(hdr + code)
    return img


class Qmp:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX); self.s.connect(path); break
            except OSError:
                time.sleep(0.05)
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n'); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line}).get('return', '')


def regs(text):
    r = dict(re.findall(r'\b(R\d\d|PSR)=([0-9a-f]{8})', text))
    return r


def _die_with_parent():
    """the modem QEMU dies with the probe (Linux PR_SET_PDEATHSIG), so a killed session leaves none behind"""
    import ctypes
    ctypes.CDLL(None).prctl(1, signal.SIGKILL)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--secs', type=float, default=5,
                    help='how long to run; 0 = until the modem machine stops (power off) or the probe is '
                         'terminated (sydb --modem real runs it so), without PC sampling')
    ap.add_argument('--map', default=os.path.join(ROOT, 'tools', 'cmt', 'n95-cmt.map'))
    ap.add_argument('--out', default=os.path.join(CMT, 'probe'))
    ap.add_argument('--exact', action='store_true',
                    help='one instruction per TB: exact PCs in the access log, but MCUSW faults at its MMU enable '
                         '(the SCTLR prefetch, QEMU patch 0011, needs multi-instruction TBs)')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--dump', action='append', default=[], metavar='ADDR:LEN:FILE',
                    help='save guest physical memory at the end of the run (repeatable)')
    ap.add_argument('--gdb', type=int, metavar='PORT', help='gdbstub on PORT; with --gdb-cmds FILE, gdb runs it at the end')
    ap.add_argument('--gdb-cmds', metavar='FILE')
    ap.add_argument('--gdb-after', type=float, metavar='SECS',
                    help='run --gdb-cmds once SECS (wall clock) into the run, the guest running untouched until then '
                         '(breakpoints from reset slow the guest and change what it reaches); put "continue" in it')
    ap.add_argument('--gdb-from-start', action='store_true',
                    help='start paused and run --gdb-cmds from reset (put "continue" in it), instead of at the end')
    ap.add_argument('--link', metavar='SOCKET',
                    help='serve the SSI APE peer on this unix socket (the phone connects: SYDB_QEMU_EXTRA='
                         '"-chardev socket,id=cmt,path=SOCKET -global syborg-modem.link=cmt")')
    ap.add_argument('--icount', default='shift=2,sleep=off', metavar='OPTS',
                    help='-icount options (default shift=2,sleep=off: 4 ns per instruction, ~250 MIPS, deterministic '
                         'virtual time; "off" = free-running TCG, where the modem\'s tick calibrations can see 0 '
                         'elapsed ticks and divide by zero)')
    ap.add_argument('--qemu-arg', action='append', default=[], metavar='ARG',
                    help='extra QEMU argument (repeatable), e.g. --qemu-arg=--trace --qemu-arg="pflash_*"')
    ap.add_argument('--boot', choices=('nolo', 'mcusw'), default='nolo')
    ap.add_argument('--device', help="whose firmware: the core flash file (C0R) of that device's firmware in the ROM "
                    'store (default $SYBORG_DEVICE, else the registry default); an id may name a firmware, MODEL@FW')
    ap.add_argument('--c0r', metavar='FILE', help='this core flash file instead of the device\'s')
    ap.add_argument('--nor-base', type=lambda x: int(x, 0), default=NOR_BASE)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    runmap = os.path.join(a.out, 'run.map')
    lines = open(a.map).read() if os.path.exists(a.map) else ''
    if a.boot == 'nolo':
        import nor
        c0r = a.c0r or nor.device_c0r(a.device)
        print(f'modem firmware from {os.path.relpath(c0r, ROOT)}', flush=True)
        img_bytes, _ = nor.build(c0r)
        pas = [l.split()[1] for l in lines.splitlines() if l.split()[:1] == ['pa'] and len(l.split()) > 1]
        if pas:                             # map "pa NAME": stand-in PA directory entries (nor.pa_stub)
            nor.pa_stub(img_bytes, pas)
        small, large = [], []               # map "pmm GROUP INDEX LEN [HEX] [large]": stand-in PMM records
        for l in lines.splitlines():         # (HEX: the first bytes, the rest zeros)
            w = l.split('#')[0].split()
            if w[:1] == ['pmm'] and len(w) >= 4:
                data = bytearray(int(w[3], 0))
                hx = [x for x in w[4:] if x != 'large']
                if hx:
                    b = bytes.fromhex(hx[0])
                    data[:len(b)] = b
                (large if 'large' in w[4:] else small).append((int(w[1], 0), int(w[2], 0), bytes(data)))
        if small or large:
            nor.pmm_records(img_bytes, small, large)
        norimg = os.path.join(a.out, 'nor.img'); open(norimg, 'wb').write(img_bytes)
        # Intel-command-set NOR, 16 MB, ids 0x89/0x8809 (one of NOLO's supported parts; the N95's own part
        # is not known [I])
        lines += f'\npflash {a.nor_base:#x} 0x1000000 0x89 0x8809\n'
        drive = ['-drive', f'if=pflash,format=raw,file={norimg},snapshot=on']
        machine_extra = f',entry={a.nor_base + NOLO_CODE:#x}'
    else:
        machine_extra = f',image={build_image(a.out)}'
        drive = []
    if a.link:
        lines = lines.replace('\nssi -\n', '\nssi ape\n')
        a.qemu_arg += ['-chardev', f'socket,id=ape,path={a.link},server=on,wait=off']
    # "pa" / "pmm" lines shape the NOR image (above), the board does not know them
    lines = '\n'.join(l for l in lines.splitlines() if l.split()[:1] not in (['pa'], ['pmm'])) + '\n'
    open(runmap, 'w').write(lines)
    log, intlog, qmp = (os.path.join(a.out, n) for n in ('access.log', 'int.log', 'qmp.sock'))
    for p in (qmp,):
        if os.path.exists(p): os.unlink(p)
    m = f'n95-cmt,log={log},map={runmap}' + machine_extra
    cmd = [QEMU, '-M', m, '-display', 'none', '-monitor', 'none', '-serial', 'none', '-nodefaults',
           '-accel', 'tcg' + (',one-insn-per-tb=on' if a.exact else ''),
           '-d', 'int,unimp,guest_errors', '-D', intlog, *drive,
           *(['-gdb', f'tcp:127.0.0.1:{a.gdb}'] if a.gdb else []), *(['-S'] if a.gdb_from_start else []), *a.qemu_arg,
           *(['-icount', a.icount] if a.icount != 'off' else []), '-qmp', f'unix:{qmp},server=on,wait=off']
    q = subprocess.Popen(cmd, stderr=open(os.path.join(a.out, 'qemu.err'), 'w'), preexec_fn=_die_with_parent)
    if a.secs == 0:                         # serve the link until the modem powers off or we are told to stop
        signal.signal(signal.SIGTERM, lambda *_: q.terminate())
        signal.signal(signal.SIGINT, lambda *_: q.terminate())
        print(f'modem running (pid {q.pid}), logs in {a.out}', flush=True)
        q.wait()
        print(f'modem stopped ({q.returncode})', flush=True)
        return
    try:
        c = Qmp(qmp)
        if a.gdb_from_start:
            try:
                g = subprocess.run(['gdb', '-q', '-batch', '-ex', 'set architecture arm', '-ex',
                                    f'target remote 127.0.0.1:{a.gdb}', '-x', a.gdb_cmds],
                                   capture_output=True, text=True, timeout=max(60, a.secs * 4))
                gout = g.stdout + g.stderr
            except subprocess.TimeoutExpired as e:      # a breakpoint never hit: keep what gdb said
                gout = ((e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
                        + '\n[gdb timed out: no breakpoint hit]\n')
                c.cmd('stop')
            open(os.path.join(a.out, 'gdb.txt'), 'w').write(gout)
            print('== gdb (from reset)'); print(gout.strip()[-6000:])
            c.cmd('cont')
        pcs = collections.Counter(); last = None; t0 = time.time(); after_done = False
        while time.time() - t0 < a.secs and q.poll() is None:
            if a.gdb_after is not None and a.gdb_cmds and not after_done and time.time() - t0 >= a.gdb_after:
                left = max(5, a.secs - (time.time() - t0))
                try:
                    g = subprocess.run(['gdb', '-q', '-batch', '-ex', 'set architecture arm', '-ex',
                                        f'target remote 127.0.0.1:{a.gdb}', '-x', a.gdb_cmds],
                                       capture_output=True, text=True, timeout=left)
                    gout = g.stdout + g.stderr
                except subprocess.TimeoutExpired as e:
                    gout = ((e.stdout or b'').decode(errors='replace') + (e.stderr or b'').decode(errors='replace')
                            + '\n[gdb stopped at the end of the run]\n')
                    c.cmd('stop')
                open(os.path.join(a.out, 'gdb.txt'), 'w').write(gout)
                print(f'== gdb (after {a.gdb_after} s)'); print(gout.strip()[-6000:])
                after_done = True
                c.cmd('cont')
                continue
            r = regs(c.hmp('info registers'))
            if 'R15' in r:
                pcs[r['R15']] += 1; last = r
            time.sleep(0.02)
        final = c.hmp('info registers')
        if a.gdb and a.gdb_cmds and not a.gdb_from_start and a.gdb_after is None:
            c.cmd('stop')
            g = subprocess.run(['gdb-multiarch' if shutil.which('gdb-multiarch') else 'gdb', '-q', '-batch', '-ex',
                                'set architecture arm', '-ex', f'target remote 127.0.0.1:{a.gdb}', '-x', a.gdb_cmds],
                               capture_output=True, text=True, timeout=60)
            open(os.path.join(a.out, 'gdb.txt'), 'w').write(g.stdout + g.stderr)
            print('== gdb'); print(g.stdout.strip()[-4000:])
        for spec in a.dump:
            ad, ln, fn = spec.split(':', 2)
            c.cmd('pmemsave', val=int(ad, 0), size=int(ln, 0), filename=os.path.abspath(fn))
        c.cmd('quit')
    finally:
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()
    open(os.path.join(a.out, 'final-regs.txt'), 'w').write(final)

    print('== final registers'); print(final.strip())
    print(f'\n== PC samples ({sum(pcs.values())}), top')
    for pc, n in pcs.most_common(a.top): print(f'  {pc}  {n}')
    ints = open(intlog, errors='replace').read().splitlines() if os.path.exists(intlog) else []
    exc = [l for l in ints if 'Taking exception' in l or 'exception' in l.lower()]
    print(f'\n== exceptions / guest errors ({len(ints)} log lines), first 30')
    for l in ints[:30]: print('  ' + l)
    acc = [l.split() for l in open(log) if l[:2] in ('R ', 'W ')]
    reg = collections.defaultdict(lambda: [0, 0, set()])
    for d, pc, ad, sz, v in acc:
        addr = int(ad[2:], 16); k = addr >> 16
        reg[k][0 if d == 'R' else 1] += 1; reg[k][2].add(pc[3:])
    print(f'\n== learned accesses: {len(acc)} distinct (pc, addr) by 64 KB region')
    for k in sorted(reg):
        r, w, p = reg[k]
        print(f'  {k << 16:08x}  R {r:6d}  W {w:6d}  sites {len(p)}')
    print(f'\nlogs: {a.out}/ (access.log, int.log, final-regs.txt)')


if __name__ == '__main__':
    main()
