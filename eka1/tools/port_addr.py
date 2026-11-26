#!/usr/bin/env python3
"""Find a function of one EKA1 ROM in another (e.g. N-Gage QD -> Nokia 6600) by its code.

  port_addr.py VA [VA...] [--src ngageqd] [--dst nokia6600] [--words 12] [--module ekern.exe]

For each source address, the first N instruction words are compared with every word-aligned
position of the destination ROM (or of one module), with the parts that move between builds
masked out:
  - B/BL immediates (24 bits): branch targets move;
  - the immediate of a PC-relative LDR/STR (12 bits): literal pools move;
  - literal-pool words themselves are not instructions: a word whose top nibble is not a
    condition seen in code (0xF) or which looks like an address (0x5xxxxxxx, 0x8xxxxxxx) is
    skipped by the mask.
It prints the best positions with their score (matching words of N) and, when the destination
address is in ekern's export table, the export ordinal. A unique full-score hit is a strong
candidate [I]; confirm by reading the code before relying on it.
"""
import argparse, struct, sys, pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ngage_run as nr  # noqa: E402

BASE = 0x50000000


def mask(w):
    if (w & 0x0e000000) == 0x0a000000:          # B / BL
        return w & 0xff000000, 0xff000000
    if (w & 0x0c0f0000) == 0x040f0000 and (w & 0x02000000) == 0:   # LDR/STR [pc, #imm]
        return w & 0xfffff000, 0xfffff000
    if (w >> 28) in (0x5, 0x8) and (w & 0x0e000000) not in (0x0a000000,):
        # likely a literal (an address in ROM or kernel data): don't compare
        return 0, 0
    return w, 0xffffffff


def words(rom, va, n):
    o = va - BASE
    return struct.unpack_from('<%dI' % n, rom, o)


def find(src_rom, dst_rom, va, n, lo, hi):
    pat = [mask(w) for w in words(src_rom, va, n)]
    out = []
    dw = struct.unpack_from('<%dI' % ((hi - lo) // 4), dst_rom, lo - BASE)
    for i in range(len(dw) - n):
        score = 0
        for k, (v, m) in enumerate(pat):
            if m == 0:
                continue
            if dw[i + k] & m == v:
                score += 1
            elif k < 2:                          # the first two must match: prune
                score = -1
                break
        if score > 0:
            out.append((score, lo + 4 * i))
    need = sum(1 for _, m in pat if m)
    out.sort(reverse=True)
    return need, out[:5]


def exports(model):
    """{address: ordinal} of ekern's export table (from the ROM's own TRomImageHeader)."""
    import json
    man = json.loads((model['root'] / 'output/rom/manifest.json').read_text())
    k = next(m for m in man['module_lists']['primary_file'] if m['name'].lower() == 'ekern.exe')
    rom = model['rom'].read_bytes()
    hdr = struct.unpack_from('<17I', rom, k['address'] - BASE)
    cnt, d = hdr[15], hdr[16]
    tab = struct.unpack_from('<%dI' % cnt, rom, d - BASE)
    return {a: i + 1 for i, a in enumerate(tab)}


def module_range(model, name):
    import json
    man = json.loads((model['root'] / 'output/rom/manifest.json').read_text())
    for lst in ('primary_file', 'secondary_file'):
        for m in man['module_lists'][lst]:
            if m['name'].lower() == name.lower():
                return m['address'], m['address'] + m['size']
    sys.exit('no module %s' % name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('va', nargs='+')
    ap.add_argument('--src', default='ngageqd')
    ap.add_argument('--dst', default='nokia6600')
    ap.add_argument('--words', type=int, default=12)
    ap.add_argument('--module', default='ekern.exe', help='destination module to search ("" = whole ROM)')
    a = ap.parse_args()
    s, d = nr.MODELS[a.src], nr.MODELS[a.dst]
    srom, drom = s['rom'].read_bytes(), d['rom'].read_bytes()
    lo, hi = module_range(d, a.module) if a.module else (BASE, BASE + len(drom))
    sexp, dexp = exports(s), exports(d)
    for v in a.va:
        va = int(v, 16)
        need, hits = find(srom, drom, va, a.words, lo, hi)
        tag = ' (export %d)' % sexp[va] if va in sexp else ''
        print('%08x%s: %d comparable words' % (va, tag, need))
        for score, at in hits:
            print('   %08x  %d/%d%s' % (at, score, need, ' export %d' % dexp[at] if at in dexp else ''))


if __name__ == '__main__':
    main()
