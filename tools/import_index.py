#!/usr/bin/env python3
"""Per-firmware import index: dll -> ordinal -> name, and module -> its import thunks.

Why: an import ordinal means nothing without the .dso of the *matching platform*, and the thunk order inside
a module is not ordinal order (gcerenderstage.dll imports surfacemanager ordinals 12,14,13,2,3,4). Guessing
either produces confidently wrong names. This tool builds one table per device so nothing has to be guessed:

  import_index.py build [--device ID] [--scope all|xip|rofs] [--limit N]   # -> <devdir>/imports.json
  import_index.py dll NAME [--ordinal N | --grep TEXT]     # the SDK side: what ordinal N of a DLL is
  import_index.py module NAME                              # one module's import table (dll, ord, thunk)
  import_index.py at MODULE THUNK_ADDR                     # what a thunk address imports
  import_index.py unresolved [--dll NAME]                  # every ordinal nobody could name
  import_index.py stats

Coverage: every XIP module in core.json (thunks are `ldr pc,[pc,#-4]` + absolute target, resolved through
the target module's export table) and every E32 image in the ROFS partitions (import block: dll name,
ordinal, code offset). Names come from the device profile's firmware.sdk_lib .dso files; ordinals that the
SDK does not know are kept visible as null and listed by `unresolved`.

The index is a cache under the device's build dir (git-ignored) and is rebuilt incrementally: a module's
entry is reused while its size and CRC32 are unchanged.
"""
import argparse, bisect, json, os, re, struct, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dso_ordinals            # noqa: E402
import e32load                 # noqa: E402
import rofs                    # noqa: E402

SCHEMA = 3                 # bump when a module entry gains/renames fields (invalidates the cache)
LDR_PC = 0xE51FF004
EXTS = ('.dll', '.exe', '.app', '.ldd', '.pdd', '.fsy', '.csy', '.tsy', '.dsy', '.esock', '.prt')


def _dev(dev_id=None):
    """the device module and the profile: --device, else $SYBORG_DEVICE, else the registry default.
    (Bug 2026-11-17: build --device X built from $SYBORG_DEVICE's directory and labelled the result X.)"""
    import importlib.util
    spec = importlib.util.spec_from_file_location('device', os.path.join(HERE, 'device.py'))
    d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)
    return d, d.load(dev_id or os.environ.get('SYBORG_DEVICE'))


def index_path(dev=None):
    d, prof = _dev(dev)
    return os.path.join(d.paths(prof)['dir'], 'imports.json')


# ------------------------------------------------------------------ names (SDK side)
class Names:
    """ordinal -> name per DLL, from the device's own import libraries"""

    def __init__(self, sdk_lib):
        self.dir = sdk_lib
        self.sdk = {}
        if os.path.isdir(sdk_lib):
            self.sdk = {n.lower(): n for n in os.listdir(sdk_lib)}
        self.cache = {}

    def table(self, dll):
        """{ordinal: name} for a DLL (empty when the SDK has no .dso for it)"""
        k = os.path.splitext(dll.lower())[0]
        if k not in self.cache:
            p = self.sdk.get(k + '.dso')
            self.cache[k] = dso_ordinals.load(os.path.join(self.dir, p)) if p else {}
        return self.cache[k]

    def name(self, dll, ordinal):
        v = self.table(dll).get(ordinal)
        return v[0] if v else None


# ------------------------------------------------------------------ XIP modules
class Xip:
    """one flat XIP core image: address -> owning module, and each module's imports"""

    def __init__(self, img, jsn):
        self.d = open(img, 'rb').read()
        j = json.load(open(jsn))
        self.base = j['base']
        self.files = [f for f in j['files'] if 'image' in f]
        self.spans = sorted((f['image']['code'], f['image']['code'] + f['image']['codesize'], f['path'])
                            for f in self.files)
        self.starts = [s[0] for s in self.spans]
        self.byname = {f['path'].lower(): f for f in self.files}

    def owner(self, addr):
        addr &= ~1
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            lo, hi, path = self.spans[i]
            if lo <= addr < hi:
                return path, addr - lo
        return None, None

    def ordinal_of(self, addr):
        """(dll file name, ordinal) when addr is exactly an export of some module, else (None, None)"""
        path, off = self.owner(addr)
        if path is None:
            return None, None
        im = self.byname[path.lower()]['image']
        if not im['expcount']:
            return None, None
        ex = struct.unpack_from('<%dI' % im['expcount'], self.d, im['expdir'] - self.base)
        try:
            k = [x & ~1 for x in ex].index(addr & ~1)
        except ValueError:
            return None, None
        return os.path.basename(path), k + 1

    def modules(self):
        """(path, origin, size, uid3, base, imports) for every XIP module: one without import thunks too
        (ekern.exe, data-only DLLs such as Palette.dll or the charset tables: 34 on the 808 were missing)"""
        out = []
        for lo, hi, path in self.spans:
            im = self.byname[path.lower()]['image']
            code = self.d[lo - self.base:hi - self.base]
            imps = []
            for i in range(0, len(code) - 7, 4):
                if struct.unpack_from('<I', code, i)[0] != LDR_PC:
                    continue
                tgt = struct.unpack_from('<I', code, i + 4)[0]
                dll, ordn = self.ordinal_of(tgt)
                if dll:
                    # the thunk's own offset (the ldr), as for ROFS images (bug 2026-11-17: i - 4 put XIP
                    # thunks 4 bytes before their ldr)
                    imps.append([dll, ordn, i, 'code'])
            out.append((path, 'xip@0x%x' % lo, hi - lo, '0x%08x' % im['uid3'], '0x%08x' % lo, imps))
        return out


# ------------------------------------------------------------------ ROFS E32 images
def is_e32(blob):
    """an E32 image by its header (iSignature 'EPOC' at +16), whatever its file name: .drv, .pxy, .agt,
    Elocl.NN, ... (an extension list missed 18 modules on the 808)"""
    return len(blob) >= 0x20 and blob[16:20] == b'EPOC'


def rofs_modules(dev_dir, wanted_exts=None):
    """[(partition-relative name, origin, bytes)] for every E32 image in the ROFS images (wanted_exts: only
    these file extensions, else by header)"""
    out = []
    for n in range(1, 4):
        img = os.path.join(dev_dir, 'rofs%d.img' % n)
        if not os.path.exists(img):
            continue
        d = open(img, 'rb').read()
        try:
            base, entries = rofs.files(d)
        except SystemExit:
            continue
        for fa, fs, p in entries:
            if fa < base:
                continue
            blob = d[fa - base:fa - base + fs]
            if not (p.lower().endswith(wanted_exts) if wanted_exts else is_e32(blob)):
                continue
            out.append((p, 'rofs%d@0x%x' % (n, fa - base), blob))
    return out


def e32_imports(blob):
    """(uid3, base, imports) of an E32 image: [[dll, ordinal, code offset, 'code'|'data']]"""
    h, img = e32load.parse_bytes(blob)
    out = []
    if h['importoff']:
        p = h['importoff'] + 4
        for _ in range(h['dllrefcount']):
            noff, n = struct.unpack_from('<Ii', img, p)
            nm = img[h['importoff'] + noff:img.index(b'\0', h['importoff'] + noff)].decode('latin-1')
            dll = re.sub(r'[\[{][0-9a-fA-F]+[\]}]', '', nm)
            for j in range(n):
                coff = struct.unpack_from('<I', img, p + 8 + 4 * j)[0]
                w = struct.unpack_from('<I', img, h['codeoff'] + coff)[0]
                stub = coff >= 4 and struct.unpack_from('<I', img, h['codeoff'] + coff - 4)[0] == LDR_PC
                out.append([dll, w & 0xffff, coff - 4 if stub else coff, 'code' if stub else 'data'])
            p += 8 + 4 * n
    return h, out


LAYER = {'xip': 0, 'rofs1': 1, 'rofs2': 2, 'rofs3': 3}


def mark_shadowed(mods):
    """a path in several layers (the 808: 5 XIP modules again in ROFS2): the highest layer is the one the
    loader gets (Z: is the core ROM with the ROFS layers over it, later ones on top [I: the composite file
    system's layering]); the others get 'shadowed_by' = the winning origin"""
    by = {}
    for k, v in mods.items():
        v.pop('shadowed_by', None)
        by.setdefault(k.lower(), []).append(k)
    for keys in by.values():
        if len(keys) < 2:
            continue
        rank = lambda k: LAYER.get(mods[k].get('origin', '').split('@')[0], -1)
        top = max(keys, key=rank)
        for k in keys:
            if k != top:
                mods[k]['shadowed_by'] = mods[top]['origin']


# ------------------------------------------------------------------ build
def build(dev_id=None, scope='all', limit=0, quiet=False):
    d, prof = _dev(dev_id)
    p = d.paths_v2(prof) if hasattr(d, 'paths_v2') else d.paths(prof)
    sdk = d.get(prof, 'firmware.sdk_lib') or e32load.sdk_lib_default()
    names = Names(os.path.expanduser(sdk))
    old = {}
    if os.path.exists(index_path(dev_id)):
        try:
            old = json.load(open(index_path(dev_id))).get('modules', {})
        except ValueError:
            old = {}
    mods = dict(old)                 # incremental: entries outside --scope survive, touched ones are refreshed
    t0 = time.time()

    if scope in ('all', 'xip'):
        for k in [k for k, v in mods.items() if v.get('origin', '').startswith('xip@')]:
            del mods[k]
        x = Xip(p['core_flat'], p['core_json'])
        xm = x.modules()
        for path, origin, size, uid3, base, imps in xm:
            mods[path] = {'origin': origin, 'size': size, 'codesize': size, 'schema': SCHEMA,
                          'uid3': uid3, 'base': base, 'imports': imps}
        if not quiet:
            print('%d XIP modules (%.1fs)' % (len(xm), time.time() - t0), file=sys.stderr)

    if scope in ('all', 'rofs'):
        for k in [k for k, v in mods.items() if v.get('origin', '').startswith('rofs')]:
            del mods[k]
        todo = rofs_modules(p['dir'])
        if limit:
            todo = todo[:limit]
        n = 0
        for path, origin, blob in todo:
            crc = zlib.crc32(blob)
            prev = old.get(path)
            if (prev and prev.get('schema') == SCHEMA and prev.get('size') == len(blob)
                    and prev.get('crc32') == crc):
                mods[path] = prev
                continue
            try:
                h, imps = e32_imports(blob)
            except Exception as e:                      # not an E32 image / unsupported compression
                mods[path] = {'origin': origin, 'size': len(blob), 'crc32': crc,
                              'error': '%s: %s' % (type(e).__name__, e), 'imports': []}
                continue
            mods[path] = {'origin': origin, 'size': len(blob), 'crc32': crc, 'schema': SCHEMA,
                          'codesize': h['codesize'], 'uid3': '0x%08x' % h['uid3'],
                          'base': '0x%08x' % h['codebase'], 'imports': imps}
            n += 1
            if not quiet and n % 200 == 0:
                print('  rofs: %d/%d parsed (%.1fs)' % (n, len(todo), time.time() - t0), file=sys.stderr)

    mark_shadowed(mods)

    # add names, keep the unresolved ones visible
    dlls = {}
    for m in mods.values():
        for dll, ordn, _off, _kind in m.get('imports', []):
            k = os.path.splitext(dll.lower())[0] + '.dll'
            t = dlls.setdefault(k, {})
            if str(ordn) not in t:
                t[str(ordn)] = names.name(dll, ordn)

    idx = {'schema': SCHEMA, 'device': d.get(prof, 'device.id') or dev_id or '',
           'firmware': p.get('firmware_key', ''),
           'sdk_lib': names.dir, 'built': time.strftime('%Y-%m-%dT%H:%M:%S'),
           'dlls': {k: v for k, v in sorted(dlls.items())}, 'modules': dict(sorted(mods.items()))}
    out = index_path(dev_id)
    with open(out, 'w') as f:
        json.dump(idx, f, separators=(',', ':'))
        f.write('\n')
    if not quiet:
        un = sum(1 for m in mods.values() for dll, ordn, _o, _k in m.get('imports', [])
                 if not idx['dlls'].get(os.path.splitext(dll.lower())[0] + '.dll', {}).get(str(ordn)))
        print('%s: %d modules, %d imports, %d unnamed, %.1fs'
              % (out, len(mods), sum(len(m.get('imports', [])) for m in mods.values()), un,
                 time.time() - t0), file=sys.stderr)
    return idx


def load():
    p = index_path()
    if not os.path.exists(p):
        raise SystemExit('no index yet: run %s build' % os.path.basename(__file__))
    return json.load(open(p))


def _find(idx, name):
    n = name.lower().strip('/')
    for k in idx['modules']:
        if k.lower() == n or k.lower().endswith('/' + n) or os.path.basename(k).lower() == n:
            return k, idx['modules'][k]
    raise SystemExit('module not in the index: ' + name)


def command_dll(idx, dll, ordinal=None, grep=None):
    k = os.path.splitext(dll.lower())[0] + '.dll'
    t = idx['dlls'].get(k)
    if t is None:
        near = [x for x in idx['dlls'] if x.startswith(os.path.splitext(dll.lower())[0])]
        raise SystemExit('no such dll in the index: %s%s' % (dll, ' (did you mean %s?)' % near if near else ''))
    users = {}
    for path, m in idx['modules'].items():
        for d, o, off, kind in m.get('imports', []):
            if os.path.splitext(d.lower())[0] + '.dll' == k:
                users.setdefault(o, []).append(path)
    rows = sorted((int(o), nm) for o, nm in t.items() if ordinal is None or int(o) == ordinal)
    for o, nm in rows:
        if grep and (not nm or grep.lower() not in nm.lower()):
            continue
        who = users.get(o, [])
        use = '' if not who else '%d user(s): %s' % (len(who), ', '.join(
            os.path.basename(w) for w in who[:4]))
        print('%6d  %-60s  %s' % (o, nm or '<unnamed>', use))
    if not rows:
        print('(no such ordinal in the index for %s)' % k)


def command_module(idx, name):
    key, m = _find(idx, name)
    print('%s  [%s] %d bytes uid3 %s' % (key, m['origin'], m['size'], m.get('uid3', '?')))
    if 'error' in m:
        print('  # not parsed: %s' % m['error'])
    for dll, ordn, off, kind in m.get('imports', []):
        nm = idx['dlls'].get(os.path.splitext(dll.lower())[0] + '.dll', {}).get(str(ordn))
        print('  0x%06x  %-18s ord %-6d %-4s %s' % (off, dll, ordn, kind, nm or '<unnamed>'))


def command_at(idx, name, addr):
    key, m = _find(idx, name)
    a = int(addr, 0)
    for dll, ordn, off, kind in m.get('imports', []):
        if off == a or off + 4 == a:
            nm = idx['dlls'].get(os.path.splitext(dll.lower())[0] + '.dll', {}).get(str(ordn))
            print('%s+0x%x: %s ordinal %d = %s' % (key, off, dll, ordn, nm or '<unnamed>'))
            return
    raise SystemExit('no import thunk at 0x%x in %s' % (a, key))


def command_unresolved(idx, dll=None):
    per = {}
    for path, m in idx['modules'].items():
        for d, o, off, kind in m.get('imports', []):
            nm = idx['dlls'].get(os.path.splitext(d.lower())[0] + '.dll', {}).get(str(o))
            if nm is None:
                per.setdefault(os.path.splitext(d.lower())[0] + '.dll', {}).setdefault(o, []).append(path)
    for d in sorted(per):
        if dll and os.path.splitext(dll.lower())[0] + '.dll' != d:
            continue
        print('%-26s %s' % (d, ','.join(str(o) for o in sorted(per[d]))))
        for o in sorted(per[d]):
            print('      ord %-6d %d user(s): %s' % (o, len(per[d][o]), ', '.join(
                os.path.basename(x) for x in per[d][o][:5])))


def command_stats(idx):
    mods = idx['modules']
    imps = sum(len(m.get('imports', [])) for m in mods.values())
    un = sum(1 for m in mods.values() for d, o, _o, _k in m.get('imports', [])
             if idx['dlls'].get(os.path.splitext(d.lower())[0] + '.dll', {}).get(str(o)) is None)
    print('device %s fw %s built %s' % (idx['device'], idx['firmware'], idx['built']))
    print('sdk_lib %s' % idx['sdk_lib'])
    print('modules %d, imports %d, named %d, unnamed %d, dlls %d'
          % (len(mods), imps, imps - un, un, len(idx['dlls'])))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='(re)build <devdir>/imports.json')
    b.add_argument('--device')
    b.add_argument('--scope', default='all', choices=('all', 'xip', 'rofs'))
    b.add_argument('--limit', type=int, default=0, help='stop after N ROFS modules (for a quick pass)')
    b.add_argument('--quiet', action='store_true')
    dl = sub.add_parser('dll'); dl.add_argument('name'); dl.add_argument('--ordinal', type=int)
    dl.add_argument('--grep')
    mo = sub.add_parser('module'); mo.add_argument('name')
    at = sub.add_parser('at'); at.add_argument('module'); at.add_argument('address')
    un = sub.add_parser('unresolved'); un.add_argument('--dll')
    sub.add_parser('stats')
    a = ap.parse_args()
    if a.cmd == 'build':
        build(a.device, a.scope, a.limit, a.quiet)
        return
    idx = load()
    {'dll': lambda: command_dll(idx, a.name, a.ordinal, a.grep),
     'module': lambda: command_module(idx, a.name),
     'at': lambda: command_at(idx, a.module, a.address),
     'unresolved': lambda: command_unresolved(idx, a.dll),
     'stats': lambda: command_stats(idx)}[a.cmd]()


if __name__ == '__main__':
    main()
