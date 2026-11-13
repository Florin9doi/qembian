#!/usr/bin/env python3
"""Apply a hosted NVMemory delta (htest --env NVDELTA=1 kn_efile: /out/nvmem0.delta, the run's writes to C:)
to a copy of the drive image, so the phone's files after the run can be read on the host (mtools, sqlite3).

    python3 tools/hosted/nvdelta.py SEED.img DELTA OUT.img
"""
import os, shutil, struct, sys

def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    seed, delta, out = sys.argv[1:]
    shutil.copyfile(seed, out)
    d = open(delta, 'rb').read()
    n = 0
    with open(out, 'r+b') as f:
        for off in range(0, len(d), 4 + 65536):
            (blk,) = struct.unpack_from('<I', d, off)
            f.seek(blk * 65536)
            f.write(d[off + 4:off + 4 + 65536])
            n += 1
    print('%d blocks applied -> %s' % (n, out))

if __name__ == '__main__':
    main()
