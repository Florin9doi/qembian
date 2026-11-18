#!/usr/bin/env python3
"""Name another EKA1 ROM's user-side exports by ordinal from the N-Gage's named list.

  port_exports.py ROM.bin FILES.json OUT.tsv [--names eka1/analysis/user-exports.tsv] [--base 0x50000000]

Symbian freezes export ordinals, so a module that exists in both ROMs keeps its names at the same
ordinals (EUser 621/812 RThread::Kill/Panic sit 0x58 apart in the N-Gage's and the P910i's EUser).
Names of functions added after 6.1 are missing; a module whose .def changed would get wrong names, so
treat the result as [I] until a call site confirms it. FILES.json: [{path, address, size}, ...]
(tools/fw/semc.py-style). OUT has the columns of user-exports.tsv (module, ordinal, address, mode, name),
which eka1/tools/ngage_run.py's stack namer reads from ROOT/analysis/user-exports.tsv.
"""
import argparse
import collections
import csv
import json
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom')
    ap.add_argument('files')
    ap.add_argument('out')
    ap.add_argument('--names', default='eka1/analysis/user-exports.tsv')
    ap.add_argument('--base', type=lambda s: int(s, 0), default=0x50000000)
    a = ap.parse_args()
    rom = open(a.rom, 'rb').read()
    names = collections.defaultdict(dict)
    for r in csv.DictReader(open(a.names), delimiter='\t'):
        names[r['module'].lower()][int(r['ordinal'])] = r['name']
    n = 0
    with open(a.out, 'w') as out:
        out.write('module\tordinal\taddress\tmode\tname\n')
        for f in json.load(open(a.files)):
            mod = f['path'].split('/')[-1]
            o = f['address'] - a.base
            if mod.lower() not in names or o < 0 or o + 26 * 4 > len(rom):
                continue
            h = struct.unpack_from('<26I', rom, o)          # TRomImageHeader: +15 count, +16 export dir
            cnt, ed = h[15], h[16]
            if not (0 < cnt < 5000 and f['address'] <= ed < f['address'] + f['size']):
                continue
            for i, adr in enumerate(struct.unpack_from('<%dI' % cnt, rom, ed - a.base)):
                nm = names[mod.lower()].get(i + 1)
                if nm:
                    out.write('%s\t%d\t0x%08x\t%s\t%s\n' % (mod, i + 1, adr & ~1, 'thumb' if adr & 1 else 'arm', nm))
                    n += 1
    print('%d names -> %s' % (n, a.out))


if __name__ == '__main__':
    main()
