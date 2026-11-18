#!/usr/bin/env python3
"""Minimal EKA2 XIP ROM image reader (TRomHeader / TRomDir / TRomImageHeader, see e32rom.h).

  romimg.py IMG                 header summary + file list (address, size, name)
  romimg.py IMG --images        also TRomImageHeader per executable (code/data/exports/deps)
  romimg.py IMG --extract DIR   write every file (raw XIP bytes, as in ROM) under DIR
  romimg.py IMG --json OUT      machine-readable dump (used by tools/xip_analysis.py)

IMG must start at the TRomHeader (offset 0 == iRomBase).
"""
import argparse, json, os, struct, sys

E32_UIDS_EXE = 0x1000007A
E32_UIDS_DLL = 0x10000079


class RomError(Exception):
    """the image's root directory list is not usable (not a ROM image, or truncated)"""


class Rom:
    def __init__(self, data):
        self.d = data
        h = struct.unpack_from('<I Q I I I I I I I I I I Q I I I I I I I I I I I I I I I I', data, 124)
        (self.restart, self.time, self.timehi, self.base, self.size, self.rootlist, self.kerndata,
         self.kernlimit, self.primary, self.secondary, self.checksum, self.hardware, self.language,
         self.kcfgflags, self.excsearch, self.hdrsize, self.sectionhdr, self.svdata, self.variant,
         self.extension, self.relocinfo, self.oldtrace, self.userdata, self.userdatasize,
         self.debugport, self.version, self.comptype, self.compsize, self.uncompsize) = h

    def inrom(self, a):
        return self.base <= a < self.base + len(self.d)

    def u32(self, a):
        return struct.unpack_from('<I', self.d, a - self.base)[0]

    def bytes(self, a, n):
        o = a - self.base
        return self.d[o:o + n]

    def roots(self):
        """(hardware, address) pairs of the root directory list; RomError if it is not usable.

Other images (the phones' boot ROM: an ARM branch table at 0, no Symbian header) read
as plausible TRomHeader fields by accident; their root list is the first thing that does not
hold up, so every caller that wants to know "is this really a ROM" comes through here."""
        o = self.rootlist - self.base
        if not 0 <= o <= len(self.d) - 4:
            raise RomError('root list 0x%08x is outside the image (0x%08x..0x%08x, %d bytes)'
                           % (self.rootlist, self.base, self.base + len(self.d), len(self.d)))
        n = self.u32(self.rootlist)
        if not 1 <= n <= 64:
            raise RomError('implausible root list count %d at 0x%08x' % (n, self.rootlist))
        if o + 4 + 8 * n > len(self.d):
            raise RomError('root list of %d entries at 0x%08x runs past the image' % (n, self.rootlist))
        out = []
        for i in range(n):
            hw, addr = struct.unpack_from('<II', self.d, o + 4 + 8 * i)
            if not self.inrom(addr):
                raise RomError('root directory 0x%08x is outside the image' % addr)
            out.append((hw, addr))
        return out

    def walk(self):
        seen = set()
        for hw, addr in self.roots():
            yield from self._dir(addr, '', hw, seen)

    def _dir(self, addr, path, hw, seen):
        if addr in seen or not self.inrom(addr):
            return
        seen.add(addr)
        size = self.u32(addr)
        p = addr + 4
        end = min(p + size, self.base + len(self.d))        # a directory may claim more than the file has
        while p + 10 <= end:
            esz, ealin, att, nl = struct.unpack_from('<IIBB', self.d, p - self.base)
            if not nl:
                return          # no name: padding behind a directory size that overruns the file
            if p + 10 + nl * 2 > self.base + len(self.d):
                return                                      # entry name runs past the file
            name = self.bytes(p + 10, nl * 2).decode('utf-16le', 'replace')
            if att & 0x10:
                yield from self._dir(ealin, path + name + '/', hw, seen)
            else:
                yield dict(path=path + name, addr=ealin, size=esz, att=att, hw=hw)
            p += (10 + nl * 2 + 3) & ~3

    def image(self, addr):
        f = struct.unpack_from('<4I 10I I I I 4I I I I I I I I I I', self.d, addr - self.base)
        keys = ('uid1 uid2 uid3 uidck entry code data codesize textsize datasize bsssize heapmin heapmax '
                'stack dllref expcount expdir sid vid cap0 cap1 toolsver flags priority databsslin '
                'nextext hwvar totaldata modver excdesc').split()
        r = dict(zip(keys, f))
        deps = []
        if r['dllref'] and self.inrom(r['dllref']):
            fl, cnt = struct.unpack_from('<HH', self.d, r['dllref'] - self.base)
            deps = list(struct.unpack_from('<%dI' % cnt, self.d, r['dllref'] - self.base + 4))
        r['deps'] = deps
        return r


def is_image(rom, e):
    if e['size'] < 0x60 or not rom.inrom(e['addr']):
        return False
    u1 = rom.u32(e['addr'])
    return u1 in (E32_UIDS_EXE, E32_UIDS_DLL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('img')
    ap.add_argument('--images', action='store_true')
    ap.add_argument('--extract')
    ap.add_argument('--json')
    a = ap.parse_args()
    data = open(a.img, 'rb').read()
    rom = Rom(data)
    try:
        rom.roots()
    except RomError as e:
        raise SystemExit('%s: not an EKA2 XIP ROM: %s' % (a.img, e))
    print('# romBase=0x%08x romSize=0x%x root=0x%08x kernData=0x%08x kernLimit=0x%08x primary=0x%08x'
          % (rom.base, rom.size, rom.rootlist, rom.kerndata, rom.kernlimit, rom.primary))
    print('# hardware=0x%08x variantFile=0x%08x extensionFile=0x%08x version=%s compression=0x%x (%d->%d) hdrsize=0x%x'
          % (rom.hardware, rom.variant, rom.extension, hex(rom.version), rom.comptype, rom.compsize,
             rom.uncompsize, rom.hdrsize))
    files = list(rom.walk())
    byaddr = {}
    out = []
    for e in files:
        rec = dict(e)
        if is_image(rom, e):
            im = rom.image(e['addr'])
            rec['image'] = im
            byaddr[e['addr']] = e['path']
        out.append(rec)
    for rec in out:
        line = '0x%08x %9d %s' % (rec['addr'], rec['size'], rec['path'])
        if a.images and 'image' in rec:
            im = rec['image']
            line += ('\n    %s uid3=%08x code=%08x+%x text=%x data=%08x+%x bss=%x exports=%d@%08x sid=%08x vid=%08x '
                     'caps=%08x%08x tools=%08x flags=%08x dbl=%08x\n    deps: %s') % (
                'EXE' if im['uid1'] == E32_UIDS_EXE else 'DLL', im['uid3'], im['code'], im['codesize'],
                im['textsize'], im['data'], im['datasize'], im['bsssize'], im['expcount'], im['expdir'],
                im['sid'], im['vid'], im['cap1'], im['cap0'], im['toolsver'], im['flags'], im['databsslin'],
                ' '.join(byaddr.get(d, '?%08x' % d).split('/')[-1] for d in im['deps']))
        print(line)
    if a.extract:
        for rec in out:
            fn = os.path.join(a.extract, rec['path'].replace('\\', '/'))
            os.makedirs(os.path.dirname(fn), exist_ok=True)
            open(fn, 'wb').write(rom.bytes(rec['addr'], rec['size']))
    if a.json:
        json.dump(dict(base=rom.base, size=rom.size, kerndata=rom.kerndata, files=out), open(a.json, 'w'))


if __name__ == '__main__':
    main()
