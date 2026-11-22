#!/usr/bin/env python3
"""Build the Nokia 6630's NAND image for -M nokia6630 (H-rm1-nand; docs/research/rm1-nand.md).

The chip is the one the board models: Samsung 0xEC/0x76, 4096 blocks x 32 pages x (512 + 16) bytes, the
file holds page after page (main then spare). mednand.pdd finds its partitions in a table (TOC) the boot
loader leaves in OCP SRAM at 0x20000100: 32-byte records {u32 start, u32 size (byte offsets in NAND), ...,
name[12] at +20}; it looks up "SOS-ROFS" (the ROFS, read straight from NAND) and "SOS-USER" (C:) [V].
On the phone the boot loaders' layout is unknown [I]; this image puts:
  pages 0x100-0x101  the TOC (the board copies them to SRAM+0x100 at reset; mednand's CopyTocToRam reads
                     the same pages [I])
  0x00100000         SOS-ROFS: the core's ROFS (xzip_core.py --rofs), then the variant ROFx (.v27) at the
                     ROFS's iMaxImageSize (header +0x2C; the ROFx's offsets count from the ROFS start) [V]
  0x01000000         SOS-USER: C:, blank, up to the chip's reserved blocks (0x4C at the end) [V count]
Pages with data get the controller's ECC in spare bytes 6..8 (main) and 9..10 (spare 0..2); erased pages
stay all 0xFF (the driver's erased-page case) [V].

  rm1_nand.py OUT.img --rofs eka1/rm1/output/rom/rofs.img --rofx eka1/rm1/fiasco/v27/_pre@01050000.bin
  rm1_nand.py OUT.img --rofs eka1/rm84/output/rom/rofs.img --rofx eka1/rm84/fiasco/v05/_pre@01ae0000.bin \
      --user-start 0x2600000          (Nokia N70, RM-84: same controller and chip [I: mednand.pdd is the same size])
"""
import argparse, pathlib, struct, sys

PAGE, OOB, PPB, BLOCKS, RESERVED = 512, 16, 32, 4096, 0x4c
RAW = PAGE + OOB
BLOCK = PAGE * PPB
SIZE = BLOCKS * PPB * RAW
TOC_PAGE = 0x100
ROFS_START = 0x00100000
USER_START = 0x01000000
USER_END = (BLOCKS - RESERVED) * BLOCK


def ecc(data):
    """The controller's ECC register for these bytes (nokia6630_nand.h rm1_ecc): reference, bit by bit."""
    lo = hi = 0
    for i, b in enumerate(data):
        for bit in range(8):
            if (b >> bit) & 1:
                a = i * 8 + bit
                for k in range(12):
                    if (a >> k) & 1:
                        lo ^= 1 << k
                    else:
                        hi ^= 1 << k
    return lo | hi << 16


def ecc_pages(main):
    """ecc() of every 512-byte page of `main` at once (the ECC is linear: XOR of per-byte parts).
    A byte b at index i: bits k 0..2 of the bit address come from the bit position (table by b); bits
    3..11 are index bits (k - 3) of i, toggled once per set bit, i.e. when popcount(b) is odd."""
    import numpy as np
    pages = np.frombuffer(bytes(main), dtype=np.uint8).reshape(-1, PAGE)
    tab = np.array([ecc(bytes([b])) for b in range(256)], dtype=np.uint32)   # index 0
    odd = np.array([bin(b).count('1') & 1 for b in range(256)], dtype=np.uint32)
    idx = np.arange(PAGE, dtype=np.uint32)
    lo = np.zeros(PAGE, dtype=np.uint32)
    hi = np.zeros(PAGE, dtype=np.uint32)
    for k in range(3, 12):
        one = ((idx >> (k - 3)) & 1).astype(bool)
        lo[one] |= 1 << k
        hi[~one] |= 1 << k
    # the table's k >= 3 part (index 0: all to hi) is replaced by the index masks
    base = tab & np.uint32(0x00070007)
    contrib = base[pages] ^ (odd[pages] * (lo | (hi << 16))[None, :])
    return np.bitwise_xor.reduce(contrib, axis=1)


def ecc_pack(r):
    return bytes((r & 0xff, (r >> 16) & 0xff, ((r >> 8) & 0x0f) | ((r >> 20) & 0xf0)))


def spare_for_ecc(r):
    sp = bytearray(b'\xff' * OOB)
    sp[6:9] = ecc_pack(r)
    sp[9:11] = ecc_pack(ecc(sp[0:3]))[:2]
    return bytes(sp)


def toc(records):
    """Two pages: 32 records of 32 bytes from SRAM+0x100 (= start of page 0x100); unused 0xFF."""
    t = bytearray(b'\xff' * (2 * PAGE))
    for i, (name, start, size) in enumerate(records):
        r = bytearray(32)
        struct.pack_into('<II', r, 0, start, size)
        r[20:32] = name.encode().ljust(12, b'\0')
        t[i * 32:i * 32 + 32] = r
    return bytes(t)


def layout(rofs, rofx, user_start=USER_START):
    """[(byte offset, data)], TOC records. USER_START: where C: begins (the N70's ROFS + ROFx pass 0x01000000:
    32 MB of them, so its image puts C: at 0x02600000)."""
    parts = [(ROFS_START, rofs)]
    end = ROFS_START + len(rofs)
    if rofx:
        # the ROFx extends the ROFS: it sits at the ROFS's iMaxImageSize (header +0x2C), its own offsets
        # count from the ROFS start (the .v27's directory at 0x9B0030 = 0x9B0000 + 0x30) [V], as on the N95
        max_size = struct.unpack_from('<I', rofs, 0x2c)[0] if len(rofs) >= 0x30 and rofs[:4] == b'ROFS' else 0
        if max_size < len(rofs):
            raise ValueError('the ROFS iMaxImageSize 0x%x is smaller than the image 0x%x' % (max_size, len(rofs)))
        at = ROFS_START + max_size
        parts.append((at, rofx))
        end = at + len(rofx)
    if user_start % BLOCK or not ROFS_START < user_start < USER_END:
        raise ValueError('SOS-USER at 0x%x: not a block inside the chip' % user_start)
    if end > user_start:
        raise ValueError('ROFS + ROFx end at 0x%x, past SOS-USER at 0x%x' % (end, user_start))
    recs = [('SOS-ROFS', ROFS_START, end - ROFS_START), ('SOS-USER', user_start, USER_END - user_start)]
    parts.insert(0, (TOC_PAGE * PAGE, toc(recs)))
    return parts, recs


def build(rofs, rofx=b'', user_start=USER_START):
    import numpy as np
    main = bytearray(b'\xff' * (BLOCKS * PPB * PAGE))
    parts, recs = layout(rofs, rofx, user_start)
    for at, data in parts:
        main[at:at + len(data)] = data
    m = np.frombuffer(bytes(main), dtype=np.uint8).reshape(-1, PAGE)
    used = np.nonzero((m != 0xff).any(axis=1))[0]
    eccs = ecc_pages(m[used].tobytes())
    out = np.full((BLOCKS * PPB, RAW), 0xff, dtype=np.uint8)
    for pg, r in zip(used, eccs):
        out[pg, :PAGE] = m[pg]
        out[pg, PAGE:] = np.frombuffer(spare_for_ecc(int(r)), dtype=np.uint8)
    return out.tobytes(), recs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--rofs', required=True)
    ap.add_argument('--rofx', default='')
    ap.add_argument('--user-start', type=lambda v: int(v, 0), default=USER_START,
                    help='SOS-USER (C:) start in NAND bytes (default 0x%x; Nokia N70: 0x2600000)' % USER_START)
    a = ap.parse_args()
    img, recs = build(pathlib.Path(a.rofs).read_bytes(), pathlib.Path(a.rofx).read_bytes() if a.rofx else b'',
                      a.user_start)
    pathlib.Path(a.out).write_bytes(img)
    for name, start, size in recs:
        print('%-8s 0x%08x 0x%08x' % (name, start, size))
    print('%s: %d bytes' % (a.out, len(img)))


if __name__ == '__main__':
    try:
        main()
    except ValueError as e:
        sys.exit('rm1_nand: %s' % e)
