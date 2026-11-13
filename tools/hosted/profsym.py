#!/usr/bin/env python3
"""Name the hosted profiler's hot addresses (htest --env PROFILE=1; in phone mode each Ctrl+P prints the samples
since the previous one). Reads the last "PROF ..." report in a serial log and sums samples per function:

    python3 tools/hosted/profsym.py PHONE.LOG [--device nokia-808-rm807] [--top 40] [--report N]

kernel (0xd0000000..): our hosted kernel, named from htest's build for the device
(build/hosted/htest/dev/<device>/t/kn_efile, llvm-addr2line);
rom (0x80000000..): the phone's core ROM (tools/syb_sym.py; nearest export below, so a large +offset means an
internal function); ramcode (0x70000000..): RAM-loaded code, named module+offset from the log's SYBCODE lines.
The report covers CPU time of the whole process (all threads, user and system time): a pc right after a
system call stands for the time in the Linux kernel. Original work for qembian.
"""
import argparse, os, re, subprocess, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def elf(device):
    """the kernel binary htest built for this device's ROM (htest.py: build/hosted/htest/dev/<rom>/t/)"""
    for d in (device or '', 'generic'):
        p = os.path.join(ROOT, 'build/hosted/htest/dev', d, 't/kn_efile')
        if d and os.path.exists(p):
            return p
    return os.path.join(ROOT, 'build/hosted/htest/t/kn_efile')


def reports(text):
    """[(regions, [(pc, n)], total)] for every complete report in the log"""
    out, cur = [], None
    lrs, thr, tpc = [], [], []
    for line in text.splitlines():
        m = re.search(r'PROF region (\w+) (\d+) samples', line)
        if m:
            if cur is None or cur[2] is not None:
                cur = [{}, [], None]
                out.append(cur)
            cur[0][m.group(1)] = int(m.group(2))
            continue
        m = re.search(r'PROF tpc ([0-9a-f]+) ([0-9a-f]+) (\d+)', line)
        if m:
            tpc.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))))
            continue
        m = re.search(r'PROF thread ([0-9a-f]+) (\d+)', line)
        if m:
            thr.append((int(m.group(1), 16), int(m.group(2))))
            continue
        m = re.search(r'PROF lr ([0-9a-f]+) ([0-9a-f]+) (\d+)', line)
        if m:
            lrs.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))))
            continue
        m = re.search(r'PROF pc ([0-9a-f]+) (\d+)', line)
        if m and cur is not None:
            cur[1].append((int(m.group(1), 16), int(m.group(2))))
            continue
        m = re.search(r'PROF total (\d+) samples', line)
        if m and cur is not None:
            cur[2] = int(m.group(1))
            cur += [lrs, thr, tpc]
            lrs, thr, tpc = [], [], []
    return [r for r in out if r[2] is not None]


def threadmap(text):
    """nanothread address -> Symbian thread name (THREADMAP lines of Ctrl+P; the latest wins)"""
    return {int(m.group(1), 16): m.group(2) for m in re.finditer(r'THREADMAP ([0-9a-f]+) (\S+)', text)}


def ramcode(text):
    segs = []
    for m in re.finditer(r'SYBCODE: \S*\\([^\\\s]+) code=([0-9a-f]+)\+([0-9a-f]+)', text):
        segs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    return segs


def name_kernel(pcs, path):
    if not pcs:
        return {}
    r = subprocess.run(['llvm-addr2line', '-f', '-C', '-e', path] + ['%x' % (p & ~1) for p in pcs],
                       capture_output=True, text=True)
    lines = r.stdout.splitlines()
    return {p: lines[2 * i].split('(')[0] for i, p in enumerate(pcs) if 2 * i < len(lines)}


def name_rom(pcs, device):
    if not pcs:
        return {}
    env = dict(os.environ)
    if device:
        env['SYBORG_DEVICE'] = device
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/syb_sym.py'), 'addr'] + ['%x' % p for p in pcs],
                       capture_output=True, text=True, env=env)
    out = {}
    for line in r.stdout.splitlines():
        f = line.split(None, 2)
        if len(f) >= 2:
            try:
                a = int(f[0], 16)
            except ValueError:
                continue
            fn = f[2] if len(f) > 2 else ''
            fn = re.sub(r'\+[0-9a-f]+$', '', fn.strip())
            out[a] = '%s %s' % (f[1].split('+')[0], fn) if fn else f[1]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--device', default=os.environ.get('SYBORG_DEVICE'))
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--report', type=int, default=-1, help='which report (default: the last)')
    ap.add_argument('--pcs', action='store_true', help='also list the addresses')
    a = ap.parse_args()
    text = open(a.log, errors='replace').read()
    reps = reports(text)
    if not reps:
        print('no complete PROF report in', a.log)
        return 1
    regions, pcs, total = reps[a.report][:3]
    lrs = reps[a.report][3] if len(reps[a.report]) > 3 else []
    thr = reps[a.report][4] if len(reps[a.report]) > 4 else []
    tpc = reps[a.report][5] if len(reps[a.report]) > 5 else []
    tmap = threadmap(text)
    print('report %d of %d: %d samples (1 ms of process CPU each)' % (a.report % len(reps) + 1, len(reps), total))
    print('  ' + ', '.join('%s %d%%' % (k, v * 100 // max(total, 1)) for k, v in regions.items()))
    segs = ramcode(text)
    allpcs = set(p for p, _ in pcs) | set(p for p, _, _ in lrs) | set(l for _, l, _ in lrs) | set(p for _, p, _ in tpc)
    kern = name_kernel(sorted(p for p in allpcs if 0xd0000000 <= p < 0xd1000000), elf(a.device))
    rom = name_rom(sorted(set(p & ~1 for p in allpcs if 0x80000000 <= p < 0x84000000)), a.device)
    def name(p):
        q = p & ~1
        if p in kern:
            return 'kernel ' + kern[p]
        if q in rom:
            return 'rom ' + rom[q]
        if 0x70000000 <= q < 0x78000000:
            s = next(('ramcode %s+%x' % (nm, q - b) for b, sz, nm in segs if b <= q < b + sz), 'ramcode ?')
            return re.sub(r'\+[0-9a-f]+$', '', s) if not a.pcs else s
        return 'other %08x' % q

    fn = defaultdict(int)
    named = []
    for p, n in pcs:
        s = name(p)
        fn[s] += n
        named.append((n, p, s))
    shown = sum(n for _, n in pcs)
    print('  the %d hottest addresses hold %d%% of the samples' % (len(pcs), shown * 100 // max(total, 1)))
    print('\nby function (of the hottest addresses):')
    for s, n in sorted(fn.items(), key=lambda x: -x[1])[:a.top]:
        print('  %5.1f%%  %6d  %s' % (n * 100.0 / total, n, s))
    if thr:
        tname = lambda t: 'host helper threads' if t == 0 else tmap.get(t, '%08x' % t)
        print('\nby Symbian thread (the nanothread on the CPU at the sample):')
        for t, n in thr[:15]:
            print('  %5.1f%%  %6d  %s' % (n * 100.0 / total, n, tname(t)))
        for t, n in thr[:4]:
            per = defaultdict(int)
            for tt, p, c in tpc:
                if tt == t:
                    per[name(p)] += c
            if per:
                print('\n  %s (%.1f%%), its hottest functions:' % (tname(t), n * 100.0 / total))
                for f, c in sorted(per.items(), key=lambda x: -x[1])[:10]:
                    print('    %5.1f%%  %s' % (c * 100.0 / total, f))
    if lrs:
        calls = defaultdict(lambda: defaultdict(int))
        for p, l, n in lrs:
            calls[name(p)][name(l)] += n
        print('\ncallers (lr at the sample; for a leaf function that is its caller):')
        for f, _ in sorted(fn.items(), key=lambda x: -x[1])[:12]:
            if f in calls:
                top = sorted(calls[f].items(), key=lambda x: -x[1])[:5]
                print('  %s <- %s' % (f, ', '.join('%s %d' % (c, n) for c, n in top)))
    if a.pcs:
        print('\nby address:')
        for n, p, s in named[:a.top]:
            print('  %5.1f%%  %08x  %s' % (n * 100.0 / total, p, s))
    return 0


if __name__ == '__main__':
    sys.exit(main())
