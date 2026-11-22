#!/usr/bin/env python3
"""Sony Ericsson UIQ 2.x (P800, P910i): which reply signal(s) each swbp.dll request stub waits for.

  swbp_waits.py ROOT SIGNO [SIGNO...]     ROOT = an EKA1 core dir (ROM.bin + files.json), e.g. eka1/p800/core
  swbp_waits.py ROOT --table FILE         check every `reply REQ RESP ...` line of an OSE reply table

Recipe (docs/devices/se-p910i/how-it-was-done.md s. 3): every OSE request has a stub in swbp.dll that loads
the request number as a literal, allocates the signal, then waits for a list of signals given by a literal
pointer to `{u16 count, u16 signo...}`. The reply numbers are NOT shared between phone generations (P910i
4398 -> 4F4E, P800 4398 -> 4E86 [V]), so a table seeded from another phone must be checked with this.
Static reading only; untrusted input: run with python3 -I.
"""
import argparse
import json
import struct
import sys

B = 0x50000000


def swbp(root):
    rom = open(root + '/ROM.bin', 'rb').read()
    f = next(e for e in json.load(open(root + '/files.json')) if e['path'].lower().endswith('/swbp.dll'))
    return rom, f['address'], f['size']


def waits(rom, base, size, signo):
    """[(stub literal VA, [reply signos])] for every literal == signo in swbp.dll"""
    out = []
    w = lambda a: struct.unpack_from('<I', rom, a - B)[0]
    for a in range(base, base + size - 4, 4):
        if w(a) != signo:
            continue
        lists = []
        for k in range(1, 5):                       # the literals that follow in the pool
            p = w(a + 4 * k)
            if not (base <= p < base + size + 0x20000) or p & 1:
                continue
            o = p - B
            if o + 4 > len(rom):
                continue
            cnt = struct.unpack_from('<H', rom, o)[0]
            if 1 <= cnt <= 8:
                sig = list(struct.unpack_from('<%dH' % cnt, rom, o + 2))
                if all(0x100 <= s <= 0xFFFF for s in sig):
                    lists.append(sig)
        out.append((a, lists[0] if lists else None))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root')
    ap.add_argument('signo', nargs='*')
    ap.add_argument('--table')
    a = ap.parse_args()
    rom, base, size = swbp(a.root)
    reqs = [(int(s, 16), None) for s in a.signo]
    if a.table:
        for line in open(a.table):
            t = line.split('#')[0].split()
            if len(t) >= 3 and t[0] == 'reply':
                req = t[1].split('/')[0]
                reqs.append((int(req, 16), int(t[2], 16)))
    bad = 0
    for req, resp in reqs:
        hits = waits(rom, base, size, req)
        found = sorted({tuple(l) for _, l in hits if l})
        note = ''
        if resp is not None:
            ok = any(resp in l for l in found)
            note = '  table %04X %s' % (resp, 'ok' if ok else 'MISMATCH' if found else '(no stub found)')
            bad += bool(found) and not ok
        print('%04X: %s%s' % (req, ', '.join('{%s}' % ' '.join('%04X' % s for s in l) for l in found) or '-', note))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
