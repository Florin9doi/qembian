#!/usr/bin/env python3
"""Keep the source patches' line endings equal to the files they patch (CR LF Symbian sources).

The patch sets that scripts/setup-epocroot.sh applies to the pinned Symbian checkouts must carry each
target file's own line endings, or GNU patch refuses them on a clean checkout ("different line
endings"). It went wrong twice (0003/0013, then 0019-0028): a file in a long-lived epocroot was
rewritten with LF, patches diffed from it came out LF, and only a fresh epocroot noticed.

  tools/patch_eol.py lint [--fix]       every hunk line has its target's line ending in the pinned
                                        source, and no hunk only changes line endings. --fix rewrites the
                                        patches (re-diffing the ones with ending-only changes)
  tools/patch_eol.py apply              apply every patch set, in setup order, to clean pinned sources
  tools/patch_eol.py epocroot [--fix]   the patched files in build/epocroot still have the pinned
                                        source's line endings (an LF rewrite there makes LF patches)

The patch sets (package dir -> patch dirs) and the checkouts are read from setup-epocroot.sh and
sources.lock, so a new patch directory is covered without touching this file.
scripts/setup-epocroot.sh runs `lint --fix` before applying and `epocroot --fix` after.
"""
import argparse
import difflib
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SETUP = os.path.join(ROOT, 'scripts', 'setup-epocroot.sh')
LOCK = os.path.join(ROOT, 'sources.lock')
EPOCROOT = os.path.join(ROOT, 'build', 'epocroot')

CRLF, LF = b'\r\n', b'\n'


def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def read_text(path):
    with open(path) as f:
        return f.read()


def write_bytes(path, data):
    with open(path, 'wb') as f:
        f.write(data)


# ---------------------------------------------------------------- patch sets
class PatchSet:
    def __init__(self, pkg, checkout, rev, patches):
        self.pkg = pkg            # package dir relative to the epocroot ("sf/adapt/qemu")
        self.checkout = checkout  # pinned checkout dir (ROOT/external/NAME)
        self.rev = rev            # pinned commit (sources.lock)
        self.patches = patches    # patch files in apply order


def patch_sets(root=ROOT):
    """The setup script's apply_patches calls, resolved to checkouts and ordered patch files."""
    text = read_text(os.path.join(root, 'scripts', 'setup-epocroot.sh')).replace('\\\n', ' ')
    revs = {}
    for line in read_text(os.path.join(root, 'sources.lock')).splitlines():
        f = line.split()
        if len(f) >= 4 and f[0] == 'git':
            revs[f[1]] = f[3]
    srcs = {}   # epocroot-relative dest -> external name
    for m in re.finditer(r'^\s*sync_src\s+external/(\S+)\s+"\$E/([^"]+)"', text, re.M):
        srcs[m.group(2)] = m.group(1)
    sets = []
    for m in re.finditer(r'^\s*apply_patches\s+"\$E/([^"]+)"\s+([^\n#]*)', text, re.M):
        pkg = m.group(1)
        if pkg not in srcs:
            raise SystemExit('patch_eol: %s has no sync_src line in setup-epocroot.sh' % pkg)
        files = []
        for d in m.group(2).split():
            d = d.replace('/work/', root + '/', 1)
            for dd in sorted(glob.glob(d)):
                files += glob.glob(os.path.join(dd, '*.patch'))
        files.sort(key=os.path.basename)   # file-name order across directories, as apply_patches
        name = srcs[pkg]
        sets.append(PatchSet(pkg, os.path.join(root, 'external', name), revs.get(name, 'HEAD'), files))
    return sets


def pinned_file(ps, path):
    """A file's bytes at the pinned commit, or None if it does not exist there."""
    r = subprocess.run(['git', '-C', ps.checkout, 'show', '%s:%s' % (ps.rev, path)],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return r.stdout if r.returncode == 0 else None


# ---------------------------------------------------------------- patch parsing
def split_lines(data):
    return re.findall(rb'[^\n]*\n|[^\n]+$', data)


def eol_of(line):
    return CRLF if line.endswith(CRLF) else LF if line.endswith(LF) else b''


def file_style(data):
    """CRLF or LF, whichever most lines use (None for an empty file)."""
    lines = split_lines(data)
    if not lines:
        return None
    crlf = sum(1 for l in lines if l.endswith(CRLF))
    return CRLF if 2 * crlf > len(lines) else LF


def strip_path(header, prefix):
    p = header[len(prefix):].rstrip(b'\r\n').split(b'\t')[0].decode()
    return None if p == '/dev/null' else p.split('/', 1)[1] if '/' in p else p


class Hunk:
    def __init__(self, header):
        self.header = header
        self.lines = []           # raw lines, first byte ' ', '-', '+' or '\\'


class FileDiff:
    def __init__(self, minus, plus):
        self.minus, self.plus = minus, plus   # header lines
        self.path = strip_path(plus, b'+++ ') or strip_path(minus, b'--- ')
        self.new_file = strip_path(minus, b'--- ') is None
        self.lead = []            # lines between the previous file's hunks and this header ("diff -ru ...")
        self.hunks = []
        self.trail = []           # lines after the last hunk of the patch


def parse_patch(data):
    """-> (preamble lines, [FileDiff]). Unified diffs only (what every patch here is)."""
    lines = split_lines(data)
    pre, files, i, lead = [], [], 0, []
    while i < len(lines):
        l = lines[i]
        if l.startswith(b'--- ') and i + 1 < len(lines) and lines[i + 1].startswith(b'+++ '):
            fd = FileDiff(l, lines[i + 1])
            fd.lead, lead = lead, []
            files.append(fd)
            i += 2
            while i < len(lines) and lines[i].startswith(b'@@'):
                m = re.match(rb'@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@', lines[i])
                old = int(m.group(1)) if m.group(1) is not None else 1
                new = int(m.group(2)) if m.group(2) is not None else 1
                h = Hunk(lines[i])
                i += 1
                while i < len(lines) and (old > 0 or new > 0 or lines[i].startswith(b'\\')):
                    l = lines[i]
                    if l in (b'\n', b'\r\n'):        # an empty context line some tools write bare
                        l = b' ' + l
                    c = l[:1]
                    if c == b' ':
                        old -= 1; new -= 1
                    elif c == b'-':
                        old -= 1
                    elif c == b'+':
                        new -= 1
                    elif c != b'\\':
                        break
                    h.lines.append(l)
                    i += 1
                fd.hunks.append(h)
            continue
        (lead if files else pre).append(l)
        i += 1
    if lead:
        files[-1].trail = lead
    return pre, files


def eol_only_changes(hunk):
    """Lines a hunk removes and adds again with only the line ending changed."""
    old = [l[1:] for l in hunk.lines if l[:1] in b' -']
    new = [l[1:] for l in hunk.lines if l[:1] in b' +']

    def changed(a, b):
        sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
        return sum(max(i2 - i1, j2 - j1) for op, i1, i2, j1, j2 in sm.get_opcodes() if op != 'equal')
    norm = lambda ls: [l.rstrip(b'\r\n') for l in ls]
    return changed(old, new) - changed(norm(old), norm(new))


def redundant(hunk):
    """True if a hunk removes lines and adds the same bytes back (e.g. once its endings are fixed)."""
    old = [l[1:] for l in hunk.lines if l[:1] in b' -']
    new = [l[1:] for l in hunk.lines if l[:1] in b' +']
    sm = difflib.SequenceMatcher(None, old, new, autojunk=False)
    same = sum(i2 - i1 for op, i1, i2, j1, j2 in sm.get_opcodes() if op == 'equal')
    return same > sum(1 for l in hunk.lines if l[:1] == b' ')


# ---------------------------------------------------------------- lint
class Finding:
    def __init__(self, patch, path, style, bad, eol_only):
        self.patch, self.path, self.style, self.bad, self.eol_only = patch, path, style, bad, eol_only

    def __str__(self):
        what = []
        if self.bad:
            what.append('%d line(s) not %s' % (self.bad, 'CR LF' if self.style == CRLF else 'LF'))
        if self.eol_only:
            what.append('%d line(s) only change their line ending' % self.eol_only)
        return '%s: %s: %s' % (os.path.relpath(self.patch, ROOT), self.path, ', '.join(what))


def lint_patch(patch, style_of):
    """style_of(path) -> CRLF/LF/None (None: new file or unknown, not checked)."""
    _, files = parse_patch(read_bytes(patch))
    out = []
    for fd in files:
        style = None if fd.new_file else style_of(fd.path)
        bad = eol_only = 0
        for h in fd.hunks:
            if style is not None:
                bad += sum(1 for l in h.lines if l[:1] in b' -+' and eol_of(l) not in (style, b''))
            eol_only += eol_only_changes(h)
        if bad or eol_only:
            out.append(Finding(patch, fd.path, style, bad, eol_only))
    return out


def styles_for(ps):
    cache = {}

    def style_of(path):
        if path not in cache:
            data = pinned_file(ps, path)
            cache[path] = file_style(data) if data is not None else None
        return cache[path]
    return style_of


def normalise(data, style_of):
    """Patch bytes with every hunk line's ending set to its target's style."""
    pre, files = parse_patch(data)
    out = list(pre)
    for fd in files:
        style = None if fd.new_file else style_of(fd.path)
        out += fd.lead + [fd.minus, fd.plus]
        for h in fd.hunks:
            out.append(h.header)
            for l in h.lines:
                e = eol_of(l)
                out.append(l[:len(l) - len(e)] + style if style and e and l[:1] in b' -+' else l)
        out += fd.trail
    return b''.join(out)


# ---------------------------------------------------------------- applying
def clean_tree(ps, dest):
    """The files this set's patches touch, at the pinned commit, into dest."""
    paths = set()
    for p in ps.patches:
        for fd in parse_patch(read_bytes(p))[1]:
            paths.add(fd.path)
    tracked = subprocess.run(['git', '-C', ps.checkout, 'ls-tree', '-r', '--name-only', ps.rev, '--'] + sorted(paths),
                             stdout=subprocess.PIPE, check=True).stdout.decode().split('\n')
    tracked = [t for t in tracked if t]
    os.makedirs(dest, exist_ok=True)
    if tracked:
        tar = subprocess.run(['git', '-C', ps.checkout, 'archive', ps.rev, '--'] + tracked,
                             stdout=subprocess.PIPE, check=True).stdout
        subprocess.run(['tar', '-x', '-C', dest], input=tar, check=True)


def run_patch(tree, data):
    r = subprocess.run(['patch', '-d', tree, '-p1', '--forward', '--batch', '--no-backup-if-mismatch', '-r', '-'],
                       input=data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode == 0, r.stdout.decode(errors='replace')


def apply_all(sets):
    """Apply every set to clean pinned sources. -> [(patch, patch output)] of the failures."""
    fails = []
    for ps in sets:
        if not ps.patches:
            continue
        with tempfile.TemporaryDirectory(prefix='patch_eol.') as t:
            clean_tree(ps, t)
            for p in ps.patches:
                ok, out = run_patch(t, read_bytes(p))
                if not ok:
                    fails.append((p, out))
    return fails


def unified(path, a, b, new_file=False):
    """A unified diff of two byte-line lists, each line keeping its own ending."""
    out = [b'--- ' + (b'/dev/null' if new_file else b'a/' + path.encode()) + b'\n',
           b'+++ b/' + path.encode() + b'\n']
    nonl = b'\n\\ No newline at end of file\n'
    for group in difflib.SequenceMatcher(None, a, b, autojunk=False).get_grouped_opcodes(3):
        i1, i2, j1, j2 = group[0][1], group[-1][2], group[0][3], group[-1][4]
        rng = lambda s, n: ('%d' % (s + 1) if n == 1 else '%d,%d' % (s + (1 if n else 0), n)).encode()
        out.append(b'@@ -' + rng(i1, i2 - i1) + b' +' + rng(j1, j2 - j1) + b' @@\n')
        for op, x1, x2, y1, y2 in group:
            parts = [(b' ', a[x] ) for x in range(x1, x2)] if op == 'equal' else \
                    [(b'-', a[x]) for x in range(x1, x2)] + [(b'+', b[y]) for y in range(y1, y2)]
            for c, l in parts:
                out.append(c + l if eol_of(l) else c + l + nonl)
    return b''.join(out)


def fix_set(ps):
    """Rewrite this set's patches with their targets' endings; re-diff those with ending-only changes.
    -> (changed patch files, [(patch, output)] that do not apply even after normalising)."""
    style_of = styles_for(ps)
    changed, fails = [], []
    with tempfile.TemporaryDirectory(prefix='patch_eol.') as t:
        clean_tree(ps, t)
        for p in ps.patches:
            data = read_bytes(p)
            new = normalise(data, style_of)
            pre, files = parse_patch(new)
            orig = [h for fd in parse_patch(data)[1] for h in fd.hunks]
            # re-diff only where fixing the endings left lines removed and added back unchanged
            regen = any(redundant(h) and not redundant(o)
                        for h, o in zip([h for fd in files for h in fd.hunks], orig))
            before = {}
            if regen:
                for fd in files:
                    f = os.path.join(t, fd.path)
                    before[fd.path] = read_bytes(f) if os.path.exists(f) else b''
            ok, out = run_patch(t, new)
            if not ok:
                fails.append((p, out))
                continue
            if regen:
                body = []
                for fd in files:
                    after = read_bytes(os.path.join(t, fd.path))
                    body += fd.lead
                    body.append(unified(fd.path, split_lines(before[fd.path]), split_lines(after), fd.new_file))
                    body += fd.trail
                new = b''.join(pre) + b''.join(body)
            if new != data:
                write_bytes(p, new)
                changed.append(p)
    return changed, fails


# ---------------------------------------------------------------- epocroot drift
def epocroot_drift(ps, epocroot=EPOCROOT, fix=False):
    """Patched files in the epocroot whose line endings differ from the pinned source. -> [(path, want)]"""
    style_of = styles_for(ps)
    paths = []
    for p in ps.patches:
        for fd in parse_patch(read_bytes(p))[1]:
            if not fd.new_file and fd.path not in paths:
                paths.append(fd.path)
    drift = []
    for path in paths:
        f = os.path.join(epocroot, ps.pkg, path)
        want = style_of(path)
        if want is None or not os.path.isfile(f):
            continue
        data = read_bytes(f)
        if any(eol_of(l) not in (want, b'') for l in split_lines(data)):
            drift.append((path, want))
            if fix:
                body = b''.join(l.rstrip(b'\r\n') + want if eol_of(l) else l for l in split_lines(data))
                write_bytes(f, body)
    return drift


# ---------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    l = sub.add_parser('lint'); l.add_argument('--fix', action='store_true')
    sub.add_parser('apply')
    e = sub.add_parser('epocroot'); e.add_argument('--fix', action='store_true')
    e.add_argument('--root', default=EPOCROOT)
    a = ap.parse_args(argv)
    sets = patch_sets()
    missing = [ps.checkout for ps in sets if ps.patches and not os.path.isdir(os.path.join(ps.checkout, '.git'))]
    if missing:
        print('patch_eol: pinned checkouts missing (scripts/fetch.sh): ' + ', '.join(missing), file=sys.stderr)
        return 2

    if a.cmd == 'lint':
        found = [f for ps in sets for p in ps.patches for f in lint_patch(p, styles_for(ps))]
        if not found:
            return 0
        for f in found:
            print(('fixing ' if a.fix else '') + str(f), file=sys.stderr)
        if not a.fix:
            print('patch_eol: run tools/patch_eol.py lint --fix', file=sys.stderr)
            return 1
        rc = 0
        for ps in sets:
            changed, fails = fix_set(ps)
            for p in changed:
                print('patch_eol: rewrote %s (commit it)' % os.path.relpath(p, ROOT), file=sys.stderr)
            for p, out in fails:
                print('patch_eol: %s does not apply to clean sources even with fixed line endings:\n%s'
                      % (os.path.relpath(p, ROOT), out), file=sys.stderr)
                rc = 1
        return rc

    if a.cmd == 'apply':
        fails = apply_all(sets)
        for p, out in fails:
            print('patch_eol: %s does not apply to clean sources:\n%s' % (os.path.relpath(p, ROOT), out), file=sys.stderr)
        return 1 if fails else 0

    if a.cmd == 'epocroot':
        if not os.path.isdir(a.root):
            return 0
        rc = 0
        for ps in sets:
            for path, want in epocroot_drift(ps, a.root, a.fix):
                print('patch_eol: %s %s/%s: not %s like the pinned source' % (
                    'converted' if a.fix else 'drifted:', ps.pkg, path, 'CR LF' if want == CRLF else 'LF'),
                    file=sys.stderr)
                rc = 0 if a.fix else 1
        return rc


if __name__ == '__main__':
    sys.exit(main())
