#!/usr/bin/env python3
"""IPC server index of a firmware: which module starts which named server, and which modules connect to it.

  ipc_index.py build [--device ID]   # -> <devdir>/ipc.json  (needs <devdir>/imports.json: import_index.py build)
  ipc_index.py server NAME           # provider, candidates and clients of one server ("!Windowserver", or a part)
  ipc_index.py module NAME           # what one module serves and connects to
  ipc_index.py stats                 # counts; servers without a provider

Method [V: prototype on the 808, 2026-11-19]. A protected server's name starts with '!' (e32 TServer naming: only
a process with ProtServ can start one, and a client must spell the '!' too), so the name is one literal present in
the server's module (CServer2::StartL(name)) and in every client's module (RSessionBase::CreateSession(name)).
The literal is scanned as UTF-16LE; a TLitC16 ("lit": its u32 length precedes the characters) is exact, a bare
wide string ("bare": `_L("...")`) is accepted only when it looks like a name. The role per module comes from the
import index: a module that imports CServer2::Start/StartL/ReStart is a provider of the names it holds, a module
that imports RSessionBase::CreateSession is a client of the names it holds (a server EXE that names several
servers: the one its file name resembles is its own, the others it is a client of). Servers whose name is built
at run time, and unnamed (anonymous or kernel-side) servers, are not seen: `stats` lists what has no provider.

The output keys modules by the same path strings as imports.json, so tools/registry.py ingest joins them
(one ipc-client-server edge per client module -> provider binary, and one "serves" row per provider).
"""
import argparse, json, os, re, struct, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import e32load                  # noqa: E402
import import_index as II       # noqa: E402

SCHEMA = 1
_WIDE = re.compile(rb'(?:[\x20-\x7e]\x00){2,80}')
# not "!:\path" (any drive), not "!x->Check()", not "!iAnimating" / "!aBitmap" (assert texts on members/arguments)
_NAME = re.compile(r'^!(?![ia][A-Z])[A-Za-z][A-Za-z0-9_ .*-]{2,60}$')
_STRIP = re.compile(r'^!|server$|srv$|[^a-z0-9]', re.I)
START_RE = re.compile(r'^_ZN8CServer2(5Start|6StartL|7ReStart)E')
CONNECT_RE = re.compile(r'^_ZN12RSessionBase13CreateSessionERK7TDesC16')


def index_path(dev=None):
    return os.path.join(os.path.dirname(II.index_path(dev)), 'ipc.json')


def literals(img):
    """[(name, form)] for every '!'-named wide literal in an image: form 'lit' = TLitC16 (length-prefixed),
    'bare' = a wide C string that looks like a name"""
    out = []
    for m in _WIDE.finditer(img):
        s = m.group().decode('utf-16le')
        if not s.startswith('!'):
            continue
        o = m.start()
        n = struct.unpack_from('<I', img, o - 4)[0] if o >= 4 else -1
        if 0 < n <= len(s) and _NAME.match(s[:n]):
            out.append((s[:n], 'lit'))                  # a TLitC16, possibly followed by printable bytes
        elif _NAME.match(s):
            out.append((s, 'bare'))
    return out


def euser_ordinals(idx):
    """(start ordinals, create-session ordinals) of this firmware's euser.dll, by name from the import index"""
    eu = idx.get('dlls', {}).get('euser.dll', {})
    start = {int(o) for o, nm in eu.items() if nm and START_RE.match(nm)}
    conn = {int(o) for o, nm in eu.items() if nm and CONNECT_RE.match(nm)}
    return start, conn


def roles(m, start, conn):
    """(starts a server, creates sessions) from one module's import list"""
    s = c = False
    for dll, ordn, _off, _kind in m.get('imports', []):
        if os.path.splitext(os.path.basename(dll).lower())[0] != 'euser':
            continue
        s = s or ordn in start
        c = c or ordn in conn
    return s, c


def _stem(path):
    return _STRIP.sub('', os.path.splitext(os.path.basename(path))[0]).lower()


def _resembles(name, path):
    a, b = _STRIP.sub('', name).lower(), _stem(path)
    return bool(a and b) and (a in b or b in a)


def classify(modules, names, start, conn):
    """modules: path -> import-index entry; names: path -> [(name, form)]. Returns (per-module, per-server)."""
    mods, servers = {}, {}
    for path, m in modules.items():
        s, c = roles(m, start, conn)
        forms = {}
        for n, f in names.get(path, []):                # one entry per name; 'lit' wins over 'bare'
            if f == 'lit' or n not in forms:
                forms[n] = f
        found = sorted(forms.items())
        mods[path] = {'starts': s, 'connects': c, 'names': found}
        for n, f in found:
            sv = servers.setdefault(n, {'provider': None, 'candidates': [], 'clients': [], 'mentions': []})
            if s:
                sv['candidates'].append(path)
            elif c:
                sv['clients'].append(path)
            else:
                sv['mentions'].append(path)
    for n, sv in servers.items():
        # the same EXE in two ROM layers (the 808: sqlsrv.exe in XIP and ROFS2) is one provider: the loaded copy
        cands = [p for p in sv['candidates'] if not modules[p].get('shadowed_by')] or sv['candidates']
        if len(cands) == 1:
            sv['provider'] = cands[0]
        elif cands:
            # several starters hold the name: the one whose file name resembles it, else the one that is not
            # also a client of something (a pure server); the others name a server they talk to
            own = [p for p in cands if _resembles(n, p)] or [p for p in cands if not mods[p]['connects']]
            if len(own) == 1:
                sv['provider'] = own[0]
        if sv['provider']:
            for p in cands:
                if p != sv['provider'] and mods[p]['connects']:
                    sv['clients'].append(p)
        sv['clients'].sort()
    return mods, dict(sorted(servers.items()))


def scan(dev_id=None, quiet=False):
    """path -> [(name, form)] for every module of the device's ROM (XIP code, decompressed ROFS images)"""
    d, prof = II._dev(dev_id)
    p = d.paths_v2(prof) if hasattr(d, 'paths_v2') else d.paths(prof)
    names = {}
    t0 = time.time()
    x = II.Xip(p['core_flat'], p['core_json'])
    for lo, hi, path in x.spans:
        lits = literals(x.d[lo - x.base:hi - x.base])
        if lits:
            names[path] = lits
    for path, _origin, blob in II.rofs_modules(p['dir']):
        try:
            _h, img = e32load.parse_bytes(blob)
        except Exception:
            continue
        lits = literals(img)
        if lits:
            names[path] = lits
    if not quiet:
        print('%d modules hold a !name (%.1fs)' % (len(names), time.time() - t0), file=sys.stderr)
    return names


def build(dev_id=None, quiet=False):
    ip = II.index_path(dev_id)
    if not os.path.exists(ip):
        raise SystemExit('no import index: tools/import_index.py build (%s)' % ip)
    idx = json.load(open(ip))
    start, conn = euser_ordinals(idx)
    if not start or not conn:
        raise SystemExit('euser.dll CServer2::Start/RSessionBase::CreateSession not named in %s' % ip)
    names = scan(dev_id, quiet)
    mods, servers = classify(idx['modules'], names, start, conn)
    out = {'schema': SCHEMA, 'device': idx.get('device', ''), 'firmware': idx.get('firmware', ''),
           'built': time.strftime('%Y-%m-%dT%H:%M:%S'),
           'euser': {'start': sorted(start), 'create_session': sorted(conn)},
           'modules': {k: v for k, v in sorted(mods.items()) if v['names']},
           'servers': servers}
    path = index_path(dev_id)
    with open(path, 'w') as f:
        json.dump(out, f, separators=(',', ':'))
        f.write('\n')
    if not quiet:
        print('%s: %d servers, %d with a provider, %d client links' % (
            path, len(servers), sum(1 for s in servers.values() if s['provider']),
            sum(len(s['clients']) for s in servers.values())), file=sys.stderr)
    return out


def load(dev=None):
    p = index_path(dev)
    if not os.path.exists(p):
        raise SystemExit('no IPC index: tools/ipc_index.py build (%s)' % p)
    return json.load(open(p))


def command_server(idx, needle):
    hit = [n for n in idx['servers'] if n.lower() == needle.lower()] or \
          [n for n in idx['servers'] if needle.lower() in n.lower()]
    if not hit:
        print('no server named like %r' % needle)
        return
    for n in hit:
        s = idx['servers'][n]
        print('%s\n  provider: %s' % (n, s['provider'] or '(none found)'))
        if s['candidates'] and s['provider'] not in s['candidates'] or len(s['candidates']) > 1:
            print('  candidates: %s' % ', '.join(s['candidates']))
        for c in s['clients']:
            print('  client: %s' % c)
        for c in s['mentions']:
            print('  mentions (neither starts nor connects): %s' % c)


def command_module(idx, name):
    low = name.lower()
    for path, m in idx['modules'].items():
        if os.path.basename(path).lower() == low or path.lower() == low:
            print('%s  starts=%s connects=%s' % (path, m['starts'], m['connects']))
            for n, f in m['names']:
                s = idx['servers'].get(n, {})
                role = 'serves' if s.get('provider') == path else ('client of' if m['connects'] else 'mentions')
                print('  %s %s [%s]%s' % (role, n, f, '' if s.get('provider') else '  (no provider found)'))
            return
    print('no module %r holds a !name' % name)


def command_stats(idx):
    sv = idx['servers']
    print('servers %d, with provider %d, client links %d' % (
        len(sv), sum(1 for s in sv.values() if s['provider']), sum(len(s['clients']) for s in sv.values())))
    for n, s in sv.items():
        if not s['provider']:
            print('  no provider: %-40s candidates=%s clients=%d' % (n, ','.join(s['candidates']) or '-',
                                                                     len(s['clients'])))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build'); b.add_argument('--device'); b.add_argument('--quiet', action='store_true')
    s = sub.add_parser('server'); s.add_argument('name'); s.add_argument('--device')
    m = sub.add_parser('module'); m.add_argument('name'); m.add_argument('--device')
    st = sub.add_parser('stats'); st.add_argument('--device')
    a = ap.parse_args()
    if a.cmd == 'build':
        build(a.device, a.quiet)
        return
    idx = load(a.device)
    {'server': lambda: command_server(idx, a.name),
     'module': lambda: command_module(idx, a.name),
     'stats': lambda: command_stats(idx)}[a.cmd]()


if __name__ == '__main__':
    main()
