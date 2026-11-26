#!/usr/bin/env python3
"""One N-Gage (EKA1) boot experiment on the ngage QEMU board, with a report.

Boots ROM.bin for SECS seconds with the catch-all log on, then asks the monitor for the CPU
state, stops QEMU and prints:
  - where the CPU is (PC, mode, CP15 control/TTB when the MMU is on);
  - mode switches / exceptions taken (count, first ones);
  - every physical 4 KB page the board does not model yet: reads/writes, address span,
    first values (the 'ngage:' lines of ngage.c's catch-all).
Each run's log stays in build/ngage/<name>.log for later questions (docs/eka1-ngage-questions.md).

usage: ngage_run.py [--secs 5] [--name runN] [--rom eka1/nem4/output/rom/ROM.bin] [-- extra qemu args]
"""
import argparse, collections, os, pathlib, re, socket, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
QEMU = pathlib.Path(os.environ['NGAGE_QEMU']) if os.environ.get('NGAGE_QEMU') else ROOT / 'build/qemu-ngage/build/qemu-system-arm'   # NGAGE_QEMU: another build (A/B runs)

# The phones on the ngage board (src/qemu-ngage/hw/arm/ngage.c NgageModel). Per phone: the QEMU
# machine, its ROM, PPM segments and ISI table, the extraction root (manifest, user-exports.tsv)
# and the kernel addresses the tools read. 6600 values marked [I] are unconfirmed.
MODELS = {
    'ngage': dict(                          # N-Gage (NEM-4 V 4.03): the QD's kernel generation at other addresses
        machine='ngage', root=ROOT / 'eka1/nem4',
        rom=ROOT / 'eka1/nem4/output/rom/ROM.bin', ppm=ROOT / 'eka1/nem4/extracted/nem440.326',
        isi=ROOT / 'src/qemu-ngage/isi-replies.txt',
        fatal=0x50324240,                   # the QD's 0x5031E340, port_addr.py: unique 12/12 match
        cur_thread=0x80006740,              # the QD's kernel-data layout (the same constants, as often) [V static]
        containers=0x80000398,
        code=(0x50320f9c, 0x511c0000),      # ekern start .. ROM end
        ttb=0x0A074000),
    'ngageqd': dict(
        machine='ngageqd', root=ROOT / 'eka1/rh29', analysis=ROOT / 'eka1/analysis',
        rom=ROOT / 'eka1/rh29/output/rom/ROM.bin', ppm=ROOT / 'eka1/rh29/extracted/RH2941.026',
        isi=ROOT / 'src/qemu-ngage/isi-replies.txt',
        fatal=0x5031e340,                   # ekern's fatal-fault routine
        cur_thread=0x80006740,              # the kernel's current DThread
        containers=0x80000398,              # K:: object containers (processes first)
        code=(0x5031af9c, 0x51170000),      # ROM code range for return-address scans
        ttb=0x0A074000),
    'nokia3650': dict(
        machine='nokia3650', root=ROOT / 'eka1/nhl8',
        rom=ROOT / 'eka1/nhl8/output/rom/ROM.bin', ppm=ROOT / 'eka1/nhl8/extracted/NHL83041.726',
        isi=ROOT / 'src/qemu-ngage/isi-replies.txt',
        fatal=0x5034dd44,                   # Kern::Fault's body (0x5034dd3c: reason + 0x10000, falls in; r0 = category, r1 = reason), same code as the 3660's [V static]
        cur_thread=0x80006740,              # N-Gage kernel-data layout (the 3650/3660 ROMs hold the same constants) [I]
        containers=0x80000398,
        code=(0x5033af9c, 0x51230000),      # ekern start .. an upper bound [I]
        ttb=0x0A074000),
    'nokia3660': dict(
        machine='nokia3660', root=ROOT / 'eka1/nhl8x',
        rom=ROOT / 'eka1/nhl8x/output/rom/ROM.bin', ppm=ROOT / 'eka1/nhl8x/extracted/NHL83050.826',
        isi=ROOT / 'src/qemu-ngage/isi-replies.txt',
        fatal=0x5032ae68,                   # Kern::Fault's body (0x5032ae60: reason + 0x10000, falls in; r0 = category, r1 = reason); it ends at the halt 0x5031b244 [V static]
        cur_thread=0x80006740,              # N-Gage kernel-data layout (the 3650/3660 ROMs hold the same constants) [I]
        containers=0x80000398,
        code=(0x50317f9c, 0x51230000),      # ekern start .. an upper bound [I]
        ttb=0x0A074000),
    'nokia6600': dict(
        machine='nokia6600', root=ROOT / 'eka1/nhl10',
        rom=ROOT / 'eka1/nhl10/output/rom/ROM.bin', ppm=ROOT / 'eka1/nhl10/extracted/NHL105530.26',
        isi=ROOT / 'src/qemu-ngage/isi-replies-nhl10.txt',
        fatal=0x50312b34,                   # [I] same bytes as the N-Gage's routine
        cur_thread=0x800017d4,              # ported from N-Gage users of 0x80006740 (3 agree)
        containers=0x800003b8,              # ported from users of 0x80000398/0x8000039c (5, 9)
        code=(0x5030ff9c, 0x51600000),
        ttb=0x0A074000),                    # [I]
    'nokia3230': dict(
        machine='nokia3230', root=ROOT / 'eka1/rm51',
        rom=ROOT / 'eka1/rm51/output/rom/ROM.bin', ppm=ROOT / 'eka1/rm51/extracted/RM51PE507170.26',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rm51.txt',
        fatal=0x5033eb34,                   # ported from the 6600 (16/16 words) [V]
        cur_thread=0x800017d4,              # as the 6600 (48 users agree)
        containers=0x800003b8,              # as the 6600 (17 users agree)
        code=(0x5033bf9c, 0x51b00000),
        ttb=0x0A074000),                    # [I] as the 6600
    'nokia7610': dict(                      # Nokia 7610 V 4.20 (RH-51): ekern = the 3230's, 0x22000 lower [V]
        machine='nokia7610', root=ROOT / 'eka1/rh51',
        rom=ROOT / 'eka1/rh51/output/rom/ROM.bin', ppm=ROOT / 'eka1/rh51/extracted/RH51PE706420.26',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rh51.txt',
        fatal=0x5031cb34,                   # the 3230's 0x5033EB34 - 0x22000 [V same bytes]
        cur_thread=0x800017d4,              # as the 3230 (identical ekern)
        containers=0x800003b8,
        code=(0x50319f9c, 0x51a00000),
        ttb=0x0A074000),                    # [I] as the 6600
    'nokia6670': dict(                      # Nokia 6670 V 4.20 (RH-67): the 7610's core image, its own PPM
        machine='nokia6670', root=ROOT / 'eka1/rh67',
        rom=ROOT / 'eka1/rh67/output/rom/ROM.bin', ppm=ROOT / 'eka1/rh67/extracted/RH67PE706420.26',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rh51.txt',
        fatal=0x5031cb34,
        cur_thread=0x800017d4,
        containers=0x800003b8,
        code=(0x50319f9c, 0x51a00000),
        ttb=0x0A074000),
    'nokia6260': dict(                      # Nokia 6260 V 5.0536 (RM-25): ekern = the 3230's, 0x4000 higher [V]
        machine='nokia6260', root=ROOT / 'eka1/rm25',
        rom=ROOT / 'eka1/rm25/output/rom/ROM.bin', ppm=ROOT / 'eka1/rm25/extracted/RM25PE505360.26',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rm25.txt',
        fatal=0x5033eb34 + 0x4000,          # the 3230's Kern::Fault + 0x4000 [V same bytes]
        cur_thread=0x800017d4,              # as the 3230 (identical ekern)
        containers=0x800003b8,
        code=(0x5033ff9c, 0x51d00000),
        ttb=0x0A074000),                    # [I] as the 6600
    'nokia6630': dict(
        machine='nokia6630', root=ROOT / 'eka1/rm1/core',
        rom=ROOT / 'eka1/rm1/core/ROM.bin', ppm=ROOT / 'eka1/rm1/core',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rm1.txt',
        fatal=0x5001e664,                   # the kernel's fault routine (IRQs off, reason, variant hook +0x210); Kern::Fault 0x5001e65c (reason + 0x10000) falls into it [V]
        cur_thread=0x800018c8,              # ported from the 3230 (40 users agree)
        containers=0x8000048c,              # ported from the 3230 (13 users agree)
        code=(0x50009f9c, 0x50c00000),
        vectors=0xffff0000,
        sdram=(0x10000000, 0x04000000),
        ttb=0x10000000),
    'nokia6680': dict(
        machine='nokia6680', root=ROOT / 'eka1/rm36/core',
        rom=ROOT / 'eka1/rm36/core/ROM.bin', ppm=ROOT / 'eka1/rm36/core',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rm1.txt',
        fatal=0x5001e664,
        cur_thread=0x800018c8,
        containers=0x8000048c,
        code=(0x50009f9c, 0x50c00000),
        vectors=0xffff0000,
        sdram=(0x10000000, 0x04000000),
        ttb=0x10000000),
    'nokia9210': dict(
        machine='nokia9210', root=ROOT / 'eka1/rae3/core',
        rom=ROOT / 'eka1/rae3/core/ROM.bin', ppm=ROOT / 'eka1/rae3/core',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rae3.txt',
        fatal=0x50000000,                   # not found yet (catch pabt/dabt/undef)
        cur_thread=0x80007214,              # ported from the N-Gage (32 users agree) [V code match]
        containers=0x80000264,              # ported from the N-Gage (11 users agree) [V code match]
        code=(0x50000f70, 0x50bd0000),
        sdram=(0x03000000, 0x01000000),
        thread_sp=0x160,                    # DThread saved SP (6.0 kernel; +0x158 on 6.1+) [V a thread's saved registers]
        ttb=0x03000000),
    'nokia9210i': dict(
        machine='nokia9210i', root=ROOT / 'eka1/rae5/core',
        rom=ROOT / 'eka1/rae5/core/ROM.bin', ppm=ROOT / 'eka1/rae5/core',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rae3.txt',
        fatal=0x50016270,                   # Kern::Fault body [V static ROM]
        cur_thread=0x80007214,              # 21 literal users in the RAE-5 ROM [V code match]
        containers=0x80000264,              # 17 literal users [V code match]
        code=(0x50001f9c, 0x50fd0000),
        sdram=(0x03000000, 0x01000000),
        thread_sp=0x160,                    # DThread saved SP (6.0 kernel; +0x158 on 6.1+) [V a thread's saved registers]
        ttb=0x03000000),
    'nokian70': dict(
        machine='nokian70', root=ROOT / 'eka1/rm84/core',
        rom=ROOT / 'eka1/rm84/core/ROM.bin', ppm=ROOT / 'eka1/rm84/core',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rm84.txt',
        fatal=0x5001fa8c,                   # the fault routine after Kern::Fault (export 278, 0x5001fa84: reason + 0x10000, then falls in) [V Ghidra]
        cur_thread=0x8000295c,              # ported from the 6630 (34 users agree, 1 other)
        containers=0x8000048c,              # ported from the 6630 (11 users agree)
        code=(0x50008f9c, 0x50b00000),
        vectors=0xffff0000,
        sdram=(0x10000000, 0x08000000),
        objstride=2,                        # container entries {DObject*, key} (as the 7710's kernel) [V run]
        ttb=0x10000000),
    'nokian90': dict(
        machine='nokian90', root=ROOT / 'eka1/rm42/core',
        rom=ROOT / 'eka1/rm42/core/ROM.bin', ppm=ROOT / 'eka1/rm42/core',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rm42.txt',
        fatal=0x500203a4,                   # the fault routine after Kern::Fault (0x5002039c: reason + 0x10000, then falls in), as the N70's [V code]
        cur_thread=0x800018c4,              # ported from the N70 (40 users agree)
        containers=0x80000488,              # ported from the N70 (16 users agree)
        code=(0x5000bf9c, 0x51000000),
        vectors=0xffff0000,
        sdram=(0x10000000, 0x08000000),
        objstride=2,                        # [I] as the N70
        ttb=0x10000000),
    'nokia7650': dict(
        machine='nokia7650', root=ROOT / 'eka1/nhl2',
        rom=ROOT / 'eka1/nhl2/output/rom/ROM.bin', ppm=ROOT / 'eka1/nhl2/extracted/NHL23250.677',
        isi=ROOT / 'src/qemu-ngage/isi-replies-nhl2.txt',
        fatal=0x5030b0f4,                   # ported (unique 6-word match)
        cur_thread=0x80006740,              # as N-Gage (10 ported users agree)
        containers=0x80000398,              # as N-Gage (12 ported users agree)
        code=(0x50307f9c, 0x50f00000),
        ttb=0x0A074000),                    # [I]
    'nokia9300': dict(
        machine='nokia9300', root=ROOT / 'eka1/rae6/core',
        rom=ROOT / 'eka1/rae6/core/ROM.bin', ppm=ROOT / 'eka1/rae6/images',
        isi=ROOT / 'src/qemu-ngage/isi-replies-ra2.txt',
        fatal=0x50020260,                   # Kern::Fault(desc, reason): 0x50020260 [V]
        cur_thread=0x800018c0,              # as 9500/6600
        containers=0x80000430,              # as 9500/6600
        code=(0x5000cf9c, 0x51100000),
        vectors=0xffff0000,                 # high vectors
        panic=0x500d8e58,                   # EUser RThread::Panic(cat, reason) [V]
        ttb=0x10000000),                    # [I]
    'nokia9300i': dict(
        machine='nokia9300i', root=ROOT / 'eka1/ra8/core',
        rom=ROOT / 'eka1/ra8/core/ROM.bin', ppm=ROOT / 'eka1/ra8/images',
        isi=ROOT / 'src/qemu-ngage/isi-replies-ra2.txt',
        fatal=0x50021260,                   # Kern::Fault(desc, reason): 0x50021260 [V]
        cur_thread=0x800018c0,              # as 9500/6600
        containers=0x80000430,              # as 9500/6600
        code=(0x5000df9c, 0x51200000),
        vectors=0xffff0000,                 # high vectors
        panic=0x500dd0b8,                   # EUser RThread::Panic(cat, reason) [V]
        ttb=0x10000000),                    # [I]
    'nokia9500': dict(
        machine='nokia9500', root=ROOT / 'eka1/ra2/core',
        rom=ROOT / 'eka1/ra2/core/ROM.bin', ppm=ROOT / 'eka1/ra2/images',
        isi=ROOT / 'src/qemu-ngage/isi-replies-ra2.txt',
        fatal=0x500212c4,                   # Kern::Fault(desc, reason): stores the reason at 0x40000410, restarts via 0x50014090 [V]
        cur_thread=0x800018c0,              # ported from the 6600 (7 users agree)
        containers=0x80000430,              # ported from the 6600 (15/16 users agree)
        code=(0x5000df9c, 0x51200000),
        vectors=0xffff0000,                 # high vectors (the low catches never hit) [V]
        panic=0x500d9d48,                   # EUser RThread::Panic(cat, reason): r1 = category [V: IsaApi 4]
        ttb=0x10000000),                    # [I]
    'nokia7710': dict(
        machine='nokia7710', root=ROOT / 'eka1/rm12/core',
        rom=ROOT / 'eka1/rm12/core/ROM.bin', ppm=ROOT / 'eka1/rm12/images',
        isi=ROOT / 'src/qemu-ngage/isi-replies-rm12.txt',
        fatal=0x50028304,                   # Kern::Fault, export 102 as the 9500's (16/16 words) [V]
        cur_thread=0x800018bc,              # ported from the 9500 (46 users agree)
        containers=0x8000042c,              # ported from the 9500 (17 users agree)
        code=(0x50014f9c, 0x51d00000),      # core + the V extension ROM at 0x51B00000
        vectors=0xffff0000,                 # [I] as the 9500
        panic=0x5006a844,                   # EUser RThread::Panic, ported from the 9500 (12/12 words)
        objstride=2,                        # container entries are {DObject*, key word} [V: process array]
        ttb=0x10000000),                    # [I] as the 9500
    'p800': dict(
        machine='p800', root=ROOT / 'eka1/p800/core',
        rom=ROOT / 'eka1/p800/core/ROM.bin', ppm=ROOT / 'eka1/p800',
        isi=ROOT / 'src/qemu-ngage/p800-ose-replies.txt',
        fatal=0x500221c8,                   # Kern::Fault, export 119 as the P910i's (16/16 words) [V]
        cur_thread=0x80001e88,              # ported from the P910i (47 users agree)
        containers=0x800000c4,              # ported from the P910i (18 users agree)
        code=(0x5000ef9c, 0x51000000),
        vectors=0xffff0000,                 # [I] as the P910i
        panic=0x5005788c,                   # EUser RThread::Panic, ported from the P910i (12/12 words)
        sdram=(0xd0000000, 0x01000000),     # 16 MB (bootstrap table) [V]
        ttb=0xd0024000),                    # PORT
    'p910i': dict(
        machine='p910i', root=ROOT / 'eka1/p910i/core',
        rom=ROOT / 'eka1/p910i/core/ROM.bin', ppm=ROOT / 'eka1/p910i',
        isi=ROOT / 'src/qemu-ngage/p910i-ose-replies.txt',
        fatal=0x50026468,                   # Kern::Fault (EKern export 119): the 9500's body [V]
        cur_thread=0x80001e94,              # ported from the 9500 (242 users agree)
        containers=0x800000d0,              # ported from the 9500 (73 users agree)
        code=(0x50012f9c, 0x51b00000),
        vectors=0xffff0000,                 # high vectors: bootstrap control word 0x627a has V set [V]
        panic=0x5005bec0,                   # EUser RThread::Panic (ordinal 812, as the N-Gage's EUser) [V: ordinal spacing]
        sdram=(0xd0000000, 0x02000000),     # RAM for crash dumps (the ngage default is 0x0A000000)
        ttb=0xd0044000),                    # page directory: maps 0x80000000 and the vector page [V: RAM search]
}
M = MODELS[os.environ.get('NGAGE_MODEL', 'ngage')]


def use_model(name):
    """Select the phone for this process (and the daemons it starts: NGAGE_MODEL)."""
    global M
    M = MODELS[name]
    os.environ['NGAGE_MODEL'] = name
    return M


def monitor(path, cmds):
    s = socket.socket(socket.AF_UNIX); s.connect(str(path)); s.settimeout(2)
    out = b''

    def drain():
        nonlocal out
        try:
            while True:
                b = s.recv(65536)
                if not b:
                    break
                out += b
                if out.rstrip().endswith(b'(qemu)'):
                    break
        except socket.timeout:
            pass
    drain()
    res = {}
    for c in cmds:
        out = b''
        s.sendall(c.encode() + b'\n'); drain()
        res[c] = out.decode('latin1')
    s.close()
    return res


# EKA1 kernel object containers (K:: globals, from the Find/FromHandle exports, Q16 era):
CONTAINER_NAMES = ['processes', 'threads', 'chunks', 'semaphores', 'mutexes', 'ldds', 'pdds',
                   'channels', 'servers', 'sessions', 'timers', 'libraries']


def containers():
    return [(n, M['containers'] + 4 * i) for i, n in enumerate(CONTAINER_NAMES)]


def read_words(mon, addr, n):
    out = monitor(mon, ['x/%dwx 0x%x' % (n, addr)])
    words = []
    for l in list(out.values())[0].splitlines():
        if re.match(r'^[0-9a-f]{8}:', l):
            words += [int(w, 16) for w in l.split(':', 1)[1].split()]
    return words[:n]


def read_name(mon, ptr):
    """An EKA1 object name: HBufC (length word, then UTF-16; descriptor type in the top bits)."""
    if not 0x80000000 <= ptr < 0x81000000:
        return '?'
    w = read_words(mon, ptr, 1)
    if not w:
        return '?'
    n = w[0] & 0x0fffffff
    if not 0 < n <= 64:
        return '?len %d' % n
    ws = read_words(mon, ptr + 4, (n * 2 + 3) // 4)
    b = b''.join(x.to_bytes(4, 'little') for x in ws)[:n * 2]
    return b.decode('utf-16-le', 'replace')


def print_objects(mon):
    """DObjectCon: +8 count, +16 array of DObject*; DObject: +8 owner, +16 name."""
    names = {}
    for kind, glob in containers():
        con = read_words(mon, glob, 1)[0]
        hdr = read_words(mon, con, 6)
        count, arr = hdr[2], hdr[4]
        if not 0 <= count <= 512:
            print('%s: container %08x looks wrong (count %d)' % (kind, con, count)); continue
        st = M.get('objstride', 1)
        objs = read_words(mon, arr, count * st)[::st] if count else []
        print('%s (%d):' % (kind, count))
        for o in objs:
            f = read_words(mon, o, 5)
            nm = read_name(mon, f[4]); names[o] = nm
            owner = names.get(f[2], '%08x' % f[2])
            print('   %08x  %-28s owner %s' % (o, nm, owner if kind != 'processes' else '-'))


def container(mon, glob):
    con = read_words(mon, glob, 1)[0]
    hdr = read_words(mon, con, 6)
    st = M.get('objstride', 1)          # words per container entry: 2 = {DObject*, key} (the 7710's kernel)
    return read_words(mon, hdr[4], hdr[2] * st)[::st] if 0 < hdr[2] <= 512 else []


def rom_namer():
    import bisect, csv, json
    root = M['root']
    man = json.loads((root / 'output/rom/manifest.json').read_text())
    files = sorted((f['rom_virtual_address'], f['size'], f['path'].split('/')[-1])
                   for f in man['files'] if f.get('rom_virtual_address'))
    tsv = M.get('analysis', root / 'analysis') / 'user-exports.tsv'
    names = sorted((int(r['address'], 16), r['module'], r['name'])
                   for r in csv.DictReader(tsv.open(), delimiter='\t')) if tsv.exists() else []
    keys = [n[0] for n in names]

    def name(v):
        mod = next((p for va, sz, p in files if va <= v < va + sz), None)
        if not mod:
            return None
        j = bisect.bisect_right(keys, v) - 1
        if j >= 0 and names[j][1] == mod:
            return '%s %s+%x' % (mod, names[j][2][:60], v - names[j][0])
        return mod
    return name


def print_stack(mon, tname):
    """$STK chunk (DObject owner = the thread): +0x24 committed top, +0x28 base."""
    procs = {o: read_name(mon, read_words(mon, o, 5)[4]) for o in container(mon, M['containers'])}
    threads = {}
    for o in container(mon, M['containers'] + 4):
        f = read_words(mon, o, 5)
        threads[o] = '%s::%s' % (procs.get(f[2], '?'), read_name(mon, f[4]))
    tid = next((o for o, n in threads.items() if n == tname or n.split('::')[1] == tname), None)
    if tid is None:
        print('stack %s: no such thread (%s)' % (tname, ', '.join(threads.values()))); return
    for c in container(mon, M['containers'] + 8):
        f = read_words(mon, c, 11)
        if (f[2] == tid and read_name(mon, f[4]) == '$STK') or (
                tname == 'Supervisor' and read_name(mon, f[4]) == 'SvStack'):
            size, top, base = f[8], f[9], f[10]
            break
    else:
        print('stack %s: no $STK chunk' % tname); return
    run_base = None
    if base > top:
        # moving model: the process is not the running one, so its chunk is at its home
        # address (base); top is the run address of the committed end and +0x20 the committed
        # size, so the run base is top - size (0x00400000 for a first chunk, 0x00500000 for the
        # stack after a process's $DAT: Starter's)
        run_base = top - size
        top = base + size
    # read the mapped part only: a stack chunk is committed from its top down, the rest of its
    # reservation is unmapped ("Cannot access memory"), so read page by page from the top
    page_top, words = top, []
    while page_top > base:
        page = max(base, (page_top - 1) & ~0xfff)
        try:
            words = read_words(mon, page, (page_top - page) // 4) + words
        except ValueError:
            break
        page_top = page
    base = page_top
    name = rom_namer()
    # the saved SP (DThread +0x158 [V on both kernels]); in the moving model a run address
    # (0x004xxxxx) is at the chunk's home base. Words below it are stale.
    sp = read_words(mon, tid + M.get('thread_sp', 0x158), 1)[0]
    if run_base is not None and 0x00400000 <= sp < 0x00800000 and not (base <= sp < top):
        sp = base + (sp - run_base)
    print('stack %s (%08x-%08x), saved SP %08x, ROM return-address candidates, deepest first '
          '(above the line: stale):' % (tname, base, top, sp))
    marked = not (base <= sp < top)
    for i, v in enumerate(words):
        if not marked and base + 4 * i >= sp:
            print('   ---- saved SP %08x ----' % sp)
            marked = True
        # ARM return addresses are word aligned; Thumb ones (EFile, EFSRV, ...) have bit 0 set
        if M['code'][0] <= v < M['code'][1] and (v & 3 == 0 or v & 1):
            nm = name(v & ~1)
            if nm:
                print('   %08x: %08x %s%s' % (base + 4 * i, v, 'T ' if v & 1 else '', nm))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--model', default=os.environ.get('NGAGE_MODEL', 'ngage'), choices=sorted(MODELS),
                    help='the phone (default ngage = N-Gage NEM-4; ngageqd = N-Gage QD; nokia6600 ...)')
    ap.add_argument('--secs', type=float, default=5)
    ap.add_argument('--name', default='run')
    ap.add_argument('--rom', help='default: the model\'s ROM.bin')
    ap.add_argument('--debug', default='unimp,guest_errors,int')
    ap.add_argument('--ppm', help='extracted PPM segments (the second flash file); "" = none (Q28)')
    ap.add_argument('--isi', help='ISI reply table (Q50); "" = none')
    ap.add_argument('--break', dest='brk', action='append', default=[],
                    help='hardware breakpoint (virtual address); gdb prints registers and stack '
                         'at the first hit, then the run ends')
    ap.add_argument('--watch', action='append', default=[],
                    help='write watchpoint, ADDR[:SKIP] (virtual; SKIP = hits to ignore first)')
    ap.add_argument('--mon', action='append', default=[],
                    help='QEMU monitor command at the end of the run (e.g. "xp/8wx 0x0a074000")')
    ap.add_argument('--objects', action='store_true',
                    help='at the end, list the kernel object containers (processes, threads, ...) '
                         'by name: the EKA1 "ps" (Q26)')
    ap.add_argument('--stack', action='append', default=[],
                    help='at the end, scan THREAD\'s stack for ROM return addresses (named)')
    ap.add_argument('--trace', action='append', default=[],
                    help='logging breakpoint VA=NAME: print r0-r3/lr at each hit and continue '
                         '(the run lasts --secs; output in <name>.trace)')
    ap.add_argument('--shot', help='save a PNG of the screen at the end of the run')
    ap.add_argument('--every', type=float, default=0,
                    help='with --shot X.png: also save X-<seconds>.png every this many seconds')
    ap.add_argument('--gdb-cmd', action='append', default=[], help='extra gdb command at the hit')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    m = use_model(a.model)
    a.rom = str(m['rom']) if a.rom is None else a.rom
    a.ppm = str(m['ppm']) if a.ppm is None else a.ppm
    a.isi = (str(m['isi']) if m['isi'].exists() else '') if a.isi is None else a.isi
    out = ROOT / 'build/ngage'; out.mkdir(parents=True, exist_ok=True)
    log, mon, ser = out / (a.name + '.log'), out / (a.name + '.mon'), out / (a.name + '.serial')
    for p in (log, mon, ser):
        p.unlink(missing_ok=True)
    mach = m['machine'] + (',ppm=%s' % a.ppm if a.ppm else '') + (',isi=%s' % a.isi if a.isi else '')
    cmd = [str(QEMU), '-M', mach, '-bios', a.rom, '-display', 'none', '-serial', 'file:%s' % ser,
           '-monitor', 'unix:%s,server,nowait' % mon, '-d', a.debug, '-D', str(log)] + a.extra
    if a.brk or a.watch or a.trace:
        s_ = socket.socket(); s_.bind(('127.0.0.1', 0)); port = s_.getsockname()[1]; s_.close()
        cmd += ['-gdb', 'tcp:127.0.0.1:%d' % port, '-S']
    q = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if a.trace:
        time.sleep(0.5)
        script = out / (a.name + '.gdb')
        lines = ['set architecture arm', 'set pagination off', 'set confirm off',
                 'target remote 127.0.0.1:%d' % port]
        for t in a.trace:
            # VA=NAME[;gdb command;...]: extra commands run at each hit (e.g. 'x/16sh $r1+4')
            va, _, rest = t.partition('=')
            nm, *extra = rest.split(';')
            lines += ['break *%s' % va, 'commands', 'silent',
                      'printf "%s r0=%%08x r1=%%08x r2=%%08x r3=%%08x lr=%%08x\\n", $r0, $r1, $r2, $r3, $lr' % (nm or va)]
            lines += extra + ['continue', 'end']
        lines += ['continue']
        script.write_text('\n'.join(lines) + '\n')
        tr = out / (a.name + '.trace')
        with tr.open('w') as fh:
            g = subprocess.Popen(['gdb', '-nx', '--batch', '-x', str(script)], stdout=fh, stderr=subprocess.STDOUT)
            time.sleep(a.secs)
            g.kill()
        hits = [l for l in tr.read_text(errors='replace').splitlines() if ' r0=' in l]
        print('trace: %d hits (%s)' % (len(hits), tr))
        for l in hits[-40:]:
            print('   ', l)
    elif a.brk or a.watch:
        time.sleep(0.5)
        g = ['gdb', '-nx', '--batch', '-ex', 'set architecture arm', '-ex', 'set pagination off',
             '-ex', 'target remote 127.0.0.1:%d' % port]
        for n, b in enumerate(a.brk, start=1):
            addr, _, cond = b.partition(':')      # VA[:gdb condition]
            g += ['-ex', 'hbreak *%s' % addr]
            if cond:
                g += ['-ex', 'condition %d %s' % (n, cond)]
        for n, wspec in enumerate(a.watch, start=len(a.brk) + 1):
            addr, _, skip = wspec.partition(':')
            g += ['-ex', 'watch *(unsigned short *)%s' % addr]
            if skip:
                g += ['-ex', 'ignore %d %s' % (n, skip)]
        g += ['-ex', 'continue', '-ex', 'info registers', '-ex', 'x/32wx $sp']
        for c in a.gdb_cmd:
            g += ['-ex', c]
        try:
            r = subprocess.run(g, capture_output=True, text=True, timeout=a.secs)
            print(r.stdout[-4000:])
        except subprocess.TimeoutExpired:
            print('gdb: no breakpoint hit within %s s' % a.secs)
    elif a.every and a.shot:
        t0 = time.time()
        while time.time() - t0 < a.secs:
            time.sleep(min(a.every, a.secs - (time.time() - t0)))
            n = int(round(time.time() - t0))
            try:
                monitor(mon, ['screendump %s -f png' % os.path.abspath(a.shot.replace('.png', '-%03d.png' % n))])
            except OSError:
                pass
    else:
        time.sleep(a.secs)
    state = {}
    if q.poll() is None:
        try:
            state = monitor(mon, ['info registers'])
            sp = re.search(r'R13=([0-9a-f]{8})', state.get('info registers', ''))
            if sp:
                state.update(monitor(mon, ['x/48wx 0x%s' % sp.group(1)]))
            if a.shot:
                monitor(mon, ['screendump %s -f png' % os.path.abspath(a.shot)])
                print('screen saved: %s' % a.shot)
            if a.objects:
                print_objects(mon)
            for t in a.stack:
                print_stack(mon, t)
            if a.mon:
                for k, v in monitor(mon, a.mon).items():
                    print('(qemu) %s\n%s' % (k, v.replace('(qemu)', '').strip()))
        except OSError as e:
            state = {'error': str(e)}
        q.terminate()
    try:
        q.wait(5)
    except subprocess.TimeoutExpired:
        q.kill()
    qout = q.stdout.read().decode('latin1').strip()

    print('== %s: %s' % (a.name, ' '.join(cmd[1:])))
    if qout:
        print('qemu:', qout[-600:])
    regs = state.get('info registers', '')
    m = re.findall(r'R(\d\d)=([0-9a-f]{8})', regs)
    if m:
        r = dict(m)
        print('PC=%s LR=%s SP=%s  %s' % (r.get('15'), r.get('14'), r.get('13'),
                                        ' '.join(re.findall(r'PSR=\S+ \S+ \S+', regs)) or regs.splitlines()[-2:]))
    elif state:
        print('monitor:', state)
    stack = [v for k, v in state.items() if k.startswith('x/')]
    if stack:
        words = re.findall(r'\b([0-9a-f]{8})\b', ' '.join(l.split(':', 1)[1] for l in stack[0].splitlines() if ':' in l))
        code = [w for w in words if 0x50000000 <= int(w, 16) < 0x51170000]
        print('stack words in ROM (return-address candidates, top first):', ' '.join(code[:16]))
    lines = ser.read_text('latin1').splitlines() if ser.exists() else []
    print('kernel debug output: %d lines (%s)' % (len(lines), ser))
    for l in lines[-25:]:
        print('   |', l)
    text = log.read_text('latin1') if log.exists() else ''
    ex = [l for l in text.splitlines() if l.startswith(('Taking exception', 'AArch32 mode switch', 'Exception return'))]
    print('exceptions/mode switches: %d' % len(ex))
    for l in ex[:8]:
        print('   ', l)
    pages = collections.OrderedDict()
    for l in text.splitlines():
        mm = re.match(r'ngage: ([RW]) ([0-9a-f]{8}) (\d) ([0-9a-f]+)', l)
        if not mm:
            continue
        k, ad, sz, v = mm.group(1), int(mm.group(2), 16), mm.group(3), mm.group(4)
        p = pages.setdefault(ad >> 12, {'R': 0, 'W': 0, 'lo': ad, 'hi': ad, 'first': []})
        p[k] += 1; p['lo'] = min(p['lo'], ad); p['hi'] = max(p['hi'], ad)
        if len(p['first']) < 4:
            p['first'].append('%s%x=%s' % (k, ad & 0xfff, v))
    print('unmodelled pages touched: %d' % len(pages))
    for pg, p in pages.items():
        print('  %08x  R%-5d W%-5d %08x-%08x  %s' % (pg << 12, p['R'], p['W'], p['lo'], p['hi'], ' '.join(p['first'])))


if __name__ == '__main__':
    main()
