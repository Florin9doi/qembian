#!/usr/bin/env python3
"""isirecord: what the real modem firmware answered, compared with what the fake modem answers.

  python3 tools/cmt/isirecord.py LOG... [--device ID] [--md OUT.md] [--emit]

LOG: access.log files of linked runs (a `sydb start --modem real` session's <session>/modem/access.log; the
n95-cmt probe logs every ISI message both ways: "ISI APE->CMT res=.. tid=.. id=.. robj=.. sobj=..: bytes").
Each phone request is paired with the modem's answers to it (same resource and transaction id, sent to the
requesting channel object, before that channel's next request with that resource and tid). Every distinct
request (resource, message id, data) is then sent to the fake modem core as the device runs it in table mode
(tests/modem_core_harness.c: reply tables + the stateful code, the device's board config), and the two answers
are compared:

  same      the fake modem answers as the real one
  differs   both answer, with other bytes (the report shows both)
  missing   the fake modem does not answer (the phone would wait or time out)
  extra     the real modem answered with more than one message (only the first is compared)

--emit prints `reply` lines (isi-replies.txt format) for requests whose real answer was the same every time it
was seen and that the fake modem answers from its table differently or not at all, marked
"# on: <device> real modem firmware (isirecord)". Answers that depend on state (a second, different answer to the
same request) are reported, never emitted: they belong in the modem core's code. Indications (to object 0) are
listed separately.
"""
import argparse, collections, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as devmodel   # noqa: E402

LINE = re.compile(r'ISI (APE->CMT|CMT->APE) (queued )?res=(\w+) tid=(\w+) id=(\w+) robj=(\w+) sobj=(\w+) len=\d+: '
                  r'((?:[0-9a-f]{2} ?)+)')


def parse(paths):
    """(requests, indications): requests = [(res, id, data, [answers])], an answer = (id, data); data = bytes
    after the message id"""
    reqs, inds = [], []
    for path in paths:
        open_reqs = {}                              # (res, tid, channel) -> the request's answer list
        for ln in open(path, errors='replace'):
            m = LINE.search(ln)
            if not m or m.group(2):
                continue
            d, _, res, tid, mid, robj, sobj, hx = m.groups()
            b = bytes.fromhex(hx.replace(' ', ''))
            res, tid, mid, robj, sobj = (int(x, 16) for x in (res, tid, mid, robj, sobj))
            if d == 'APE->CMT':
                ans = []
                reqs.append((res, mid, b[10:], ans))
                open_reqs[(res, tid, sobj)] = ans
            elif robj == 0:
                inds.append((res, mid, b[10:]))
            else:
                ans = open_reqs.get((res, tid, robj))
                if ans is not None:
                    ans.append((mid, b[10:]))
    return reqs, inds


def build_harness(tmp):
    exe = os.path.join(tmp, 'core')
    core = os.path.join(ROOT, 'src/qemu-syborg/hw/arm')
    subprocess.run(['gcc', '-std=gnu11', '-O1', '-w', '-DSYBMODEM_HOSTED', '-I', core,
                    '-I', os.path.join(ROOT, 'src/hosted/include/board'), '-o', exe,
                    os.path.join(ROOT, 'tests', 'modem_core_harness.c'), os.path.join(core, 'syborg_ccpu.c')],
                   check=True)
    return exe


def board_config(prof):
    """the board config string table mode runs with (tools/device.py qemu-args: syborg-platform.config)"""
    out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'device.py'), 'qemu-args', prof['_id']],
                         capture_output=True, text=True, check=True).stdout
    m = re.search(r'syborg-platform\.config=(\S+)', out)
    return m.group(1) if m else ''


def fake_answers(exe, cfg, table, res, mid, data):
    req = ','.join('%02X' % x for x in [res, mid] + list(data))
    r = subprocess.run([exe, cfg, table, '-1', req], capture_output=True, text=True)
    out = []
    for ln in r.stdout.splitlines():
        f = ln.split()
        if len(f) >= 3 and int(f[0], 16) == res and int(f[2], 16) == 0x42:     # answers to this request
            out.append((int(f[1], 16), bytes(int(x, 16) for x in f[3:])))
    return out


def hx(b):
    return ' '.join('%02X' % x for x in b)


# answers that carry per-phone identity (serial number / IMEI): never written to a report or a reply line
PRIVATE = {(0x1b, 0x00), (0x1b, 0x01)}


def main():
    ap = argparse.ArgumentParser(prog='isirecord.py')
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--device', default=os.environ.get('SYBORG_DEVICE'))
    ap.add_argument('--md', help='write the comparison as a Markdown report')
    ap.add_argument('--emit', action='store_true', help='print reply lines for constant answers the table lacks')
    a = ap.parse_args()
    prof = devmodel.load(a.device)
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import isi
    names_res, names_msg, table = isi.load(isi.dbs_of(prof))
    in_table = {(r, q) for r, q, *_ in table}
    reqs, inds = parse(a.logs)
    tmp = tempfile.mkdtemp()
    exe = build_harness(tmp)
    tab = os.path.join(tmp, 'replies.tab')
    open(tab, 'w').write(isi.table_text(prof))
    cfg = board_config(prof)

    groups = collections.OrderedDict()          # (res, id, data) -> list of real answer lists
    for res, mid, data, ans in reqs:
        groups.setdefault((res, mid, bytes(data)), []).append(tuple(ans))
    rows, emit = [], []
    stats = collections.Counter()
    for (res, mid, data), seen in groups.items():
        real = [s for s in seen if s]
        if not real:
            continue                            # the real modem did not answer either (or not yet)
        variants = collections.Counter(s[0] for s in real)
        first = variants.most_common(1)[0][0]
        fake = fake_answers(exe, cfg, tab, res, mid, data)
        if not fake:
            kind = 'missing'
        elif fake[0] == first:
            kind = 'same'
        else:
            kind = 'differs'
        if (res, mid) in PRIVATE:
            stats['private (not shown)'] += 1
            continue
        stats[kind] += 1
        name = isi.name(names_res, names_msg, res, mid)
        rows.append((kind, name, res, mid, data, variants, fake, max(real, key=len)))
        if kind != 'same' and len(variants) == 1 and ((res, mid) in in_table or not fake):
            # one table line serves every request data: only emit when all recorded requests of this (res, id)
            # got this same answer
            same_key = [g for g in groups if g[:2] == (res, mid)]
            answers = {collections.Counter(s[0] for s in groups[g] if s).most_common(1)[0][0]
                       for g in same_key if any(groups[g])}
            if len(answers) == 1:
                emit.append('reply %02X %02X %02X %s  # on: %s real modem firmware (isirecord): %s' % (
                    res, mid, first[0], hx(first[1]), prof['_id'], name))
    emit = list(dict.fromkeys(emit))

    out = ['# Real modem vs fake modem: %s' % prof['_id'], '',
           'Generated by `tools/cmt/isirecord.py` from %s. %d distinct requests answered by the real modem: %s.' % (
               ', '.join(os.path.relpath(p, ROOT) for p in a.logs), sum(stats.values()),
               ', '.join('%d %s' % (n, k) for k, n in stats.most_common())), '']
    for kind in ('missing', 'differs', 'same'):
        sel = [r for r in rows if r[0] == kind]
        if not sel:
            continue
        out += ['## %s (%d)' % (kind, len(sel)), '', '| request | data | real answer | fake answer |', '|---|---|---|---|']
        for _, name, res, mid, data, variants, fake, longest in sel:
            real = '<br>'.join('%02X: %s%s' % (s[0], hx(s[1]), ' (x%d)' % n if len(variants) > 1 else '')
                               for s, n in variants.most_common())
            if len(longest) > 1:
                real += '<br>then %s' % '; '.join('%02X: %s' % (x[0], hx(x[1])) for x in longest[1:])
            fk = '<br>'.join('%02X: %s' % (f[0], hx(f[1])) for f in fake) or '-'
            out.append('| %02X %02X %s | %s | %s | %s |' % (res, mid, name, hx(data) or '-', real, fk))
        out.append('')
    ind = collections.Counter((r, m, bytes(d)) for r, m, d in inds)
    if ind:
        out += ['## Indications from the real modem (%d distinct)' % len(ind), '', '| indication | data | times |',
                '|---|---|---|']
        for (r, m, d), n in ind.most_common():
            out.append('| %02X %02X %s | %s | %d |' % (r, m, isi.name(names_res, names_msg, r, m), hx(d), n))
        out.append('')
    text = '\n'.join(out)
    if a.md:
        open(a.md, 'w').write(text)
    else:
        print(text)
    if a.emit:
        print('\n'.join(emit))
    print('%s' % ', '.join('%d %s' % (n, k) for k, n in stats.most_common()), file=sys.stderr)


if __name__ == '__main__':
    main()
