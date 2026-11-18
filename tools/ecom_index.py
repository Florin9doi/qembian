#!/usr/bin/env python3
"""ECom plugin index of a firmware: which plugin DLL implements which interface (implementation UIDs, display
names, default data), read from the ROM's ECom SPI files (Private/10009d8f/ecom-*.spi).

  ecom_index.py build [--device ID]          # -> <devdir>/ecom.json
  ecom_index.py iface UID                    # implementations of an interface
  ecom_index.py dll NAME                     # what a plugin DLL implements
  ecom_index.py impl UID                     # one implementation

Format [V source]: an SPI file is a 32-byte header (uid 0x10205C2B, 0x10205C2C, ...) then entries
{u32 name length, u32 data length, name (the plugin's resource name = its DLL name), data (a compiled resource
file), padded to 4}. The resource is a uid-0x101F4A6B file (oss.FCL.sf.os.ossrv BaRscImpl.cpp: index at the
offset in the last two bytes, a bit array at 19 of the resources holding compressed Unicode; such a resource is
runs alternating compressed Unicode (SCSU, TUnicodeExpander) and plain bytes; text is 2-aligned with 0xab).
The REGISTRY_INFO resource (pluginfw Registrar.cpp, ParseImplementationData.cpp): format v2/v3 start with
0x101FB0B9 / 0x10009E47 then the DLL UID (v1: the DLL UID first); WORD interface count; per interface its UID,
WORD implementation count; per implementation (v3: an info-format byte first) UID, version byte, display name,
default and opaque data (v3 type 1: BYTE count of LTEXT8s; type 2: WORD length + bytes), v3: WORD extended
interface count + UIDs, then a ROM-only/flags byte (v2, v3).
"""
import argparse, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rofs                     # noqa: E402

V2, V3 = 0x101FB0B9, 0x10009E47
RSC_UID = 0x101F4A6B

# ------------------------------------------------------------------ SCSU (Unicode Technical Standard #6)
STATIC = [0x0000, 0x0080, 0x0100, 0x0300, 0x2000, 0x2080, 0x2100, 0x3000]
DYN_INIT = [0x0080, 0x00C0, 0x0400, 0x0600, 0x0900, 0x3040, 0x30A0, 0xFF00]


FIXED_OFFSETS = {0xF9: 0x00C0, 0xFA: 0x0250, 0xFB: 0x0370, 0xFC: 0x0530, 0xFD: 0x3040, 0xFE: 0x30A0, 0xFF: 0xFF60}


def _window_offset(x):
    """SCSU window offset table (UTS #6 section 5)"""
    if 0x01 <= x <= 0x67:
        return x * 0x80
    if 0x68 <= x <= 0xA7:
        return x * 0x80 + 0xAC00
    if x in FIXED_OFFSETS:
        return FIXED_OFFSETS[x]
    raise ValueError('SCSU: reserved window offset 0x%02x' % x)


def scsu_decode(b):
    """bytes -> list of UTF-16 code units"""
    out, dyn, win, unicode_mode, i = [], list(DYN_INIT), 0, False, 0
    while i < len(b):
        c = b[i]; i += 1
        if not unicode_mode:
            if c >= 0x80:
                out.append(dyn[win] + c - 0x80)
            elif c in (0x00, 0x09, 0x0A, 0x0D) or c >= 0x20:
                out.append(c)
            elif 0x01 <= c <= 0x08:                     # SQn
                x = b[i]; i += 1
                out.append(STATIC[c - 1] + x if x < 0x80 else dyn[c - 1] + x - 0x80)
            elif c == 0x0B:                             # SDX
                h, l = b[i], b[i + 1]; i += 2
                n = h >> 5
                dyn[n] = 0x10000 + (((h & 0x1F) << 8 | l) << 7)
                win = n
            elif c == 0x0E:                             # SQU
                out.append(b[i] << 8 | b[i + 1]); i += 2
            elif c == 0x0F:                             # SCU
                unicode_mode = True
            elif 0x10 <= c <= 0x17:                     # SCn
                win = c - 0x10
            elif 0x18 <= c <= 0x1F:                     # SDn
                dyn[c - 0x18] = _window_offset(b[i]); i += 1
                win = c - 0x18
            else:
                raise ValueError('SCSU: reserved byte 0x%02x' % c)
        else:
            if 0xE0 <= c <= 0xE7:                       # UCn
                win, unicode_mode = c - 0xE0, False
            elif 0xE8 <= c <= 0xEF:                     # UDn
                dyn[c - 0xE8] = _window_offset(b[i]); i += 1
                win, unicode_mode = c - 0xE8, False
            elif c == 0xF0:                             # UQU
                out.append(b[i] << 8 | b[i + 1]); i += 2
            elif c == 0xF1:                             # UDX
                h, l = b[i], b[i + 1]; i += 2
                n = h >> 5
                dyn[n] = 0x10000 + (((h & 0x1F) << 8 | l) << 7)
                win, unicode_mode = n, False
            else:
                out.append(c << 8 | b[i]); i += 1
    res = []
    for u in out:                                       # supplementary characters as surrogate pairs
        if u >= 0x10000:
            u -= 0x10000
            res += [0xD800 + (u >> 10), 0xDC00 + (u & 0x3FF)]
        else:
            res.append(u)
    return res


# ------------------------------------------------------------------ resource files
def rsc_resources(data):
    """the resources of a uid-0x101F4A6B (non dictionary-compressed) resource file, Unicode expanded"""
    uid1 = struct.unpack_from('<I', data, 0)[0]
    if uid1 != RSC_UID:
        raise ValueError('resource uid 0x%08x not supported' % uid1)
    start = struct.unpack_from('<H', data, len(data) - 2)[0]
    index = list(struct.unpack_from('<%dH' % ((len(data) - start) // 2), data, start))
    n = len(index) - 1
    bits = data[19:19 + (n + 7) // 8]
    out = []
    for r in range(n):
        raw = data[index[r]:index[r + 1]]
        if not (bits[r // 8] >> (r % 8)) & 1:
            out.append(raw)
            continue
        buf, i, comp = bytearray(), 0, True
        while i < len(raw):
            ln = raw[i]; i += 1
            if ln & 0x80:
                ln = (ln & 0x7F) << 8 | raw[i]; i += 1
            run = raw[i:i + ln]; i += ln
            if comp and run:
                if len(buf) & 1:
                    buf.append(0xAB)        # AppendDecompressedUnicodeL: Unicode starts 2-aligned
                buf += b''.join(struct.pack('<H', u) for u in scsu_decode(run))
            elif run:
                buf += run
            comp = not comp
        out.append(bytes(buf))
    return out


class Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('<H', self.b, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v

    def ltext16(self):                      # TResourceReaderImpl::ReadTPtrC16L
        n = self.u8()
        if n and self.p & 1:
            self.p += 1                     # the 0xab padding byte
        s = self.b[self.p:self.p + 2 * n].decode('utf-16-le', 'replace'); self.p += 2 * n
        return s

    def ltext8(self):
        n = self.u8()
        s = self.b[self.p:self.p + n]; self.p += n
        return s.decode('latin-1')


def registry_info(res):
    """REGISTRY_INFO -> {'format', 'dll_uid', 'interfaces': [{'uid', 'implementations': [...]}]}"""
    r = Reader(res)
    first = r.u32()
    fmt = 2 if first == V2 else 3 if first == V3 else 1
    dll = r.u32() if fmt > 1 else first
    ifaces = []
    for _ in range(r.u16()):
        iu, impls = r.u32(), []
        for _ in range(r.u16()):
            info = r.u8() if fmt == 3 else None
            im = {'uid': '0x%08x' % r.u32(), 'version': r.u8(), 'name': r.ltext16()}
            if fmt == 3 and info == 1:
                im['default_data'] = [r.ltext8() for _ in range(r.u8())]
                im['opaque_data'] = [r.ltext8() for _ in range(r.u8())]
            elif fmt == 3:
                n = r.u16(); im['default_data'] = res[r.p:r.p + n].decode('latin-1'); r.p += n
                n = r.u16(); im['opaque_data'] = res[r.p:r.p + n].decode('latin-1'); r.p += n
            else:
                im['default_data'], im['opaque_data'] = r.ltext8(), r.ltext8()
            if fmt == 3:
                im['extended_interfaces'] = ['0x%08x' % r.u32() for _ in range(r.u16())]
            if fmt >= 2:
                im['rom_only'] = bool(r.u8() & 1)
            impls.append(im)
        ifaces.append({'uid': '0x%08x' % iu, 'implementations': impls})
    return {'format': fmt, 'dll_uid': '0x%08x' % dll, 'interfaces': ifaces}


def spi_entries(blob):
    """[(resource name, resource bytes)] of an ECom SPI file"""
    if struct.unpack_from('<I', blob, 0)[0] != 0x10205C2B:
        raise ValueError('not an SPI file')
    out, pos = [], 32
    while pos + 8 <= len(blob):
        nl, dl = struct.unpack_from('<II', blob, pos)
        if nl == 0 or nl > 256:
            break
        out.append((blob[pos + 8:pos + 8 + nl].decode('latin-1'), blob[pos + 8 + nl:pos + 8 + nl + dl]))
        pos = (pos + 8 + nl + dl + 3) & ~3
    return out


# ------------------------------------------------------------------ build / query
def _dev(dev_id=None):
    import importlib.util
    spec = importlib.util.spec_from_file_location('device', os.path.join(HERE, 'device.py'))
    d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)
    return d, d.load(dev_id or os.environ.get('SYBORG_DEVICE'))


def index_path(dev=None):
    d, prof = _dev(dev)
    return os.path.join(d.paths(prof)['dir'], 'ecom.json')


def build(dev_id=None, quiet=False):
    """every SPI in the ROFS images; a later layer's entry for the same plugin replaces the earlier one (the
    ECom server reads ecom-N-0.spi per ROM layer, the higher layer winning [I])"""
    d, prof = _dev(dev_id)
    p = d.paths_v2(prof) if hasattr(d, 'paths_v2') else d.paths(prof)
    plugins, errors = {}, []
    for n in range(1, 4):
        img = os.path.join(p['dir'], 'rofs%d.img' % n)
        if not os.path.exists(img):
            continue
        data = open(img, 'rb').read()
        base, entries = rofs.files(data)
        for fa, fs, path in entries:
            if not path.lower().endswith('.spi') or '10009d8f' not in path.lower():
                continue
            for name, rsc in spi_entries(data[fa - base:fa - base + fs]):
                try:
                    res = rsc_resources(rsc)
                    info = registry_info(res[0])
                except Exception as e:             # keep it visible, never invent a parse
                    errors.append({'plugin': name, 'spi': 'rofs%d:%s' % (n, path), 'error': '%s: %s' % (
                        type(e).__name__, e)})
                    continue
                info['spi'] = 'rofs%d:%s' % (n, path)
                plugins[name.lower()] = dict(info, name=name)
    out = {'schema': 1, 'device': d.get(prof, 'device.id'), 'firmware': p.get('firmware_key', ''),
           'plugins': dict(sorted(plugins.items())), 'errors': errors}
    with open(index_path(dev_id), 'w') as f:
        json.dump(out, f, indent=0, separators=(',', ':'))
    if not quiet:
        ni = sum(len(i['implementations']) for pl in plugins.values() for i in pl['interfaces'])
        print('%s: %d plugins, %d implementations, %d errors' % (index_path(dev_id), len(plugins), ni,
                                                                  len(errors)), file=sys.stderr)
    return out


def load(dev=None):
    p = index_path(dev)
    if not os.path.exists(p):
        raise SystemExit('no ECom index yet: run %s build' % os.path.basename(__file__))
    return json.load(open(p))


def implementations(idx):
    for name, pl in idx['plugins'].items():
        for i in pl['interfaces']:
            for im in i['implementations']:
                yield name, i['uid'], im


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build'); b.add_argument('--device'); b.add_argument('--quiet', action='store_true')
    for c in ('iface', 'dll', 'impl'):
        x = sub.add_parser(c); x.add_argument('what'); x.add_argument('--device')
    a = ap.parse_args()
    if a.cmd == 'build':
        build(a.device, a.quiet)
        return
    idx = load(a.device)
    want = a.what.lower()
    if want.startswith('0x'):
        want = '0x%08x' % int(want, 16)
    for name, iu, im in implementations(idx):
        if (a.cmd == 'iface' and iu == want) or (a.cmd == 'impl' and im['uid'] == want) or \
                (a.cmd == 'dll' and (name == want or name == os.path.splitext(want)[0])):
            print('%-36s iface %s impl %s v%d %-32s %s' % (name, iu, im['uid'], im['version'], im['name'][:32],
                                                        str(im.get('default_data'))[:40]))


if __name__ == '__main__':
    main()
