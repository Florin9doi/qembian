#!/usr/bin/env python3
"""Build an N95's modem NOR flash image (CMT side) from its core flash file (C0R).

  python3 tools/cmt/nor.py [C0R | --device ID] [--out build/n95-cmt/nor.img] [--variant 0]

Without C0R: the core flash file of the device's firmware in the ROM store (--device, $SYBORG_DEVICE, else the
registry default; the device needs [modem_cpu]), so the modem firmware matches the phone side.

Every CMT-ASIC (descriptor byte 0 = 00) partition goes to its location: the partition record's payload
(the 1 KB header: magic 0x809795a3, entry offset +0x1c, length +0x20, signature) at the record's location,
the data chunks at theirs; gaps are erased flash (0xFF). A partition has several records (4 on the N95: the
same code signed for different chip/key revisions, they differ at +0x08); --variant picks which one's header
goes in. Docs: docs/research/n95-cmt-emulation.md.
"""
import argparse, mmap, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from fiasco import blocks  # noqa: E402

# Protected-application (PA) directory: 32-byte entries {u32 offset from the directory, ..., name at +0x14},
# ended by a name starting with 0xFF; MCUSW finds a PA by name (FUN_f01e40b2) [V]. The directory and the PAs
# are per-phone data written at the factory: in no flash file, erased in this image.
PA_DIR = 0xb0000           # VA 0xefdd0000 - R/O offset 0xefd20000 [V VA, I offset]
PA_STUB_OFFSET = 0x1000    # where a stub entry points: erased flash (no PA code or configuration there)
NOR_SIZE = 16 << 20        # 128 Mbit NOR [I: parts list; the image ends at 0x8806c0]


def device_c0r(device=None):
    """the core flash file (C0R) of a device's firmware from the ROM store: the modem firmware it carries is the
    one that goes with that phone side (device = an id, else $SYBORG_DEVICE, else the registry default)"""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import device as devmodel
    prof = devmodel.load(device)
    if not devmodel.get(prof, 'modem_cpu'):
        raise SystemExit('%s has no [modem_cpu] in its profile' % prof['_id'])
    try:
        return devmodel.firmware_file(prof, 'core')
    except devmodel.DeviceError as e:
        raise SystemExit(str(e))


def pa_stub(img, names):
    """Stand-in PA directory: one entry per name, pointing at erased flash. A PA that is present but has no
    configuration is what MCUSW accepts as "not provisioned" (PA_ESN: FUN_f01e4506 returns -3, which
    ST_SECURITY_TEST's FUN_f01a32d2 passes) [V]; docs/hacks.md H-cmt-pa-stub."""
    for i, name in enumerate(names):
        e = PA_DIR + 32 * i
        img[e:e + 32] = b'\xff' * 32
        img[e:e + 4] = PA_STUB_OFFSET.to_bytes(4, 'little')
        n = name.encode() + b'\0'
        assert len(n) <= 12, name
        img[e + 0x14:e + 0x14 + len(n)] = n
    return img


PMM = 0xf00000             # permanent memory manager area, 1 MB (docs/research/n95-cmt-perm-data.md 3) [V]


def _pmm_rec(group, index, data, state=0xff):
    """One PMM record: 16-byte header {state, 0x30, group, index, 0xffff, len32, total32} + data, padded to 2."""
    tot = (16 + len(data) + 1) & ~1
    return (struct.pack('<BBHHHII', state, 0x30, group, index, 0xffff, len(data), tot) + data).ljust(tot, b'\xff')


def pmm_records(img, small=(), large=()):
    """Stand-in per-phone permanent data: format the (erased) PMM area as the firmware does (seven 128 KB pages,
    then four 32 KB; active pages 0x20000 large and 0xe8000 small) and put the records [(group, index, bytes)]
    on the active page of their category. The firmware mounts it and adds its own default records [V:
    perm-data doc 3]. Only for an erased area (an image from flash files); docs/hacks.md H-cmt-pmm-stub."""
    a = bytearray(b'\xff' * 0x100000)
    pages = [(o, 0x20000) for o in range(0, 0xe0000, 0x20000)] + \
            [(o, 0x8000) for o in range(0xe0000, 0x100000, 0x8000)]
    st = {0x0: 0xf0, 0x20000: 0xcc, 0xe0000: 0xf0, 0xe8000: 0xcc}       # f0 spare, cc active, fc free
    for o, sz in pages:
        a[o:o + 12] = b'\xf0\xf0\x03\x00\xff\xff\xff' + bytes([st.get(o, 0xfc)]) + b'\0\0\0\0'
        a[o + sz - 2:o + sz] = b'\xf0\xf0'
    for base, recs, end in ((0x20000 + 12, large, 0x40000 - 2), (0xe8000 + 12, small, 0xf0000 - 2)):
        p = base
        for g, i, d in recs:
            r = _pmm_rec(g, i, d)
            assert p + len(r) <= end, 'PMM page full'
            a[p:p + len(r)] = r
            p += len(r)
    assert img[PMM:PMM + 0x100000] == b'\xff' * 0x100000, 'PMM area not erased'
    img[PMM:PMM + 0x100000] = a
    return img


def build(c0r, variant=0):
    """Return (bytearray image, list of (name, header location, code lo, code hi))."""
    with open(c0r, 'rb') as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        img = bytearray(b'\xff' * NOR_SIZE)
        parts, cur, seen, pend = [], None, {}, None
        for pos, tag, d, name, size, loc, q in blocks(m):
            if tag in (0x27, 0x28):
                cur = name
                n = seen.get((name, loc), 0); seen[(name, loc)] = n + 1   # the ASIC shows on the chunks
                if n == variant and loc != 0xffffffff and loc + size <= NOR_SIZE:
                    pend = (name, loc, bytes(m[q:q + size]))
                continue
            if tag != 0x17 or d[0] != 0:   # 0x17 data chunks of the CMT (00) only
                continue
            if pend and pend[0] == cur:
                img[pend[1]:pend[1] + len(pend[2])] = pend[2]
                parts.append([cur, pend[1], loc, loc + size]); pend = None
            elif parts and parts[-1][0] == cur:
                parts[-1][2] = min(parts[-1][2], loc); parts[-1][3] = max(parts[-1][3], loc + size)
            else:
                parts.append([cur, None, loc, loc + size])
            img[loc:loc + size] = m[q:q + size]
    return img, parts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('c0r', nargs='?', help="core flash file; default: the device's (--device, $SYBORG_DEVICE, registry)")
    ap.add_argument('--device')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'n95-cmt', 'nor.img'))
    ap.add_argument('--variant', type=int, default=0)
    a = ap.parse_args()
    img, parts = build(a.c0r or device_c0r(a.device), a.variant)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    open(a.out, 'wb').write(img)
    for name, hdr, lo, hi in parts:
        print(f'  {str(name):10s} header {'-' if hdr is None else hex(hdr):>10}  data {lo:#09x}..{hi:#09x}')
    print(a.out)


if __name__ == '__main__':
    main()
