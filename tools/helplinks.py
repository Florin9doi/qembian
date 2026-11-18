#!/usr/bin/env python3
"""helplinks: extract the "open this app/view" links of the S60 Help app (cshelp.exe) from a firmware.

S60 3rd FP1 .. Symbian^3 help lives in ROFS as z:\\resource\\xhtml\\<lang>\\<uid>\\{contents.zip,index.xml,
keywords.xml,meta.xml} (plus z:\\resource\\applicationhelps\\<lang>\\<uid>\\ for separately installed
modules); contents.zip holds one XHTML page per topic. Links that open a phone app use two URL forms,
which cshelp.exe parses by counting '/' (docs/research/help-app-links.md):
  app://cmd@localhost/<Exe.exe>/<appUid>[/<msgUid>/<msg>]   -> TApaTaskList::FindApp(appUid): BringToForeground
        (+ TApaTask::SendMessage(msgUid, utf8(msg))) else RApaLsSession::StartApp(exe, EApaCommandRun)
  app://vsa@localhost/<appUid>/<viewUid>[/<msgUid>/<msg>]   -> CCoeAppUi::ActivateViewL(TVwsViewId(app, view)
        [, msgUid, utf8(msg)])
Numbers are hex with "0x", else decimal. Output is JSON: one entry per distinct (url, link text, page).

  helplinks.py [DEVICE] [--lang 01] [--all-links] [--unique] [-o OUT.json]
"""
import argparse, io, json, os, re, struct, sys, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HELP_DIRS = ('resource/xhtml/', 'resource/applicationhelps/')
LINK_RE = re.compile(r'<a\s[^>]*?href="([^"]+)"[^>]*>(.*?)</a>', re.S | re.I)
TITLE_RE = re.compile(r'<title>(.*?)</title>', re.S | re.I)
COLL_RE = re.compile(r'<collection[^>]*\bid="([^"]+)"[^>]*\bnavtitle="([^"]*)"', re.I)
META_TITLE_RE = re.compile(r'<title[^>]*>(.*?)</title>', re.S | re.I)


def num(s):
    """cshelp's rule: '0x' prefix -> hex (TLex::Val(TUint, EHex)), else decimal TLex::Val(TInt)"""
    s = s.strip()
    i = s.lower().find('0x')
    try:
        return int(s[i + 2:], 16) if i >= 0 else int(s, 10)
    except ValueError:
        return None


def parse_url(url):
    """dict for an app:// URL, None for anything else (help-internal links, http, ...)"""
    m = re.match(r'app://(cmd|vsa)@localhost/(.*)$', url, re.I)
    if not m:
        return None
    kind, rest = m.group(1).lower(), m.group(2)
    parts = rest.split('/', 3)          # field 4 (the message) may itself contain '/'
    slashes = url.count('/')
    d = {'kind': kind, 'url': url}
    if slashes == 4 or slashes >= 6:
        pass
    else:
        d['invalid'] = 'cshelp needs 4 or >= 6 slashes, has %d' % slashes
    if kind == 'cmd':
        d['exe'] = parts[0]
        d['app_uid'] = num(parts[1]) if len(parts) > 1 else None
    else:
        d['app_uid'] = num(parts[0])
        d['view_uid'] = num(parts[1]) if len(parts) > 1 else None
    if len(parts) >= 4:
        d['msg_uid'] = num(parts[2])
        d['msg'] = parts[3]
    for k in ('app_uid', 'view_uid', 'msg_uid'):
        if isinstance(d.get(k), int):
            d[k] = '0x%08x' % d[k]
    return d


def text_of(html):
    return re.sub(r'\s+', ' ', re.sub(r'<[^>]+>', '', html)).strip()


def rom_files(prof):
    """yield (path, bytes) for every file in the device's ROFS images and XIP core ROM"""
    import device as devmodel, rofs
    p = devmodel.paths(prof)
    imgs = p['rofs'] if isinstance(p['rofs'], (list, tuple)) else str(p['rofs']).split()
    d1 = None
    for img in imgs:
        if not os.path.exists(img):
            continue
        d = open(img, 'rb').read()
        d1 = d1 or d
        base, fl = rofs.files(d)
        for fa, fs, path in fl:
            if not path.lower().startswith(HELP_DIRS):
                continue
            src, off = (d, fa - base) if 0 <= fa - base < len(d) else (d1, fa)
            yield path, src[off:off + fs]
    cj, cf = p.get('core_json'), p.get('core_flat')
    if cj and cf and os.path.exists(cj) and os.path.exists(cf):
        j = json.load(open(cj))
        flat = open(cf, 'rb').read()
        for f in j['files']:
            if f['path'].lower().startswith(HELP_DIRS):
                off = f['addr'] - j['base']
                yield f['path'], flat[off:off + f['size']]


def extract(files, lang='01', all_links=False):
    lang = lang.lower()
    modules, pages = {}, []
    for path, data in files:
        parts = path.split('/')
        if len(parts) < 4 or parts[2].lower() != lang:
            continue
        if len(parts) == 4:
            if parts[3].lower() == 'index.xml':   # the collection list: module uid -> title
                for uid, title in COLL_RE.findall(data.decode('utf-8', 'replace')):
                    modules.setdefault(uid.lower(), {})['title'] = title
            continue
        mod, name = parts[3].lower(), parts[4].lower()
        if name == 'meta.xml':
            m = META_TITLE_RE.search(data.decode('utf-8', 'replace'))
            if m:
                modules.setdefault(mod, {}).setdefault('title', text_of(m.group(1)))
        elif name == 'contents.zip':
            try:
                z = zipfile.ZipFile(io.BytesIO(data))
            except zipfile.BadZipFile:
                continue
            for zn in z.namelist():
                if zn.lower().endswith(('.html', '.htm', '.xhtml')):
                    pages.append((mod, '/'.join(parts[:5]), zn, z.read(zn)))
    out, seen = [], set()
    for mod, zpath, zn, raw in pages:
        html = raw.decode('utf-8' if b'UTF-8' in raw[:100] else 'latin-1', 'replace')
        tm = TITLE_RE.search(html)
        for href, inner in LINK_RE.findall(html):
            d = parse_url(href)
            if d is None and not all_links:
                continue
            d = d or {'kind': 'other', 'url': href}
            d.update(text=text_of(inner), page=zn, page_title=text_of(tm.group(1)) if tm else '',
                     module=mod, module_title=modules.get(mod, {}).get('title', ''), source=zpath)
            key = (d['url'], d['text'], d['page'], mod)
            if key not in seen:
                seen.add(key)
                out.append(d)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('device', nargs='?')
    ap.add_argument('--lang', default='01', help='language dir (01 = UK English)')
    ap.add_argument('--all-links', action='store_true', help='also help-internal and web links')
    ap.add_argument('--unique', action='store_true', help='one entry per (url, text): drop page detail')
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    import device as devmodel
    prof = devmodel.load(a.device)
    links = extract(rom_files(prof), a.lang, a.all_links)
    if a.unique:
        u = {}
        for d in links:
            k = (d['url'], d['text'])
            if k not in u:
                u[k] = dict(d, pages=[])
                for f in ('page', 'source'):
                    u[k].pop(f, None)
            u[k]['pages'].append(d['page'])
        links = list(u.values())
    links.sort(key=lambda d: (d['kind'], d.get('app_uid') or '', d.get('view_uid') or '', d['text']))
    s = json.dumps(links, indent=1, ensure_ascii=False)
    if a.out:
        open(a.out, 'w').write(s + '\n')
        print('%d links -> %s' % (len(links), a.out), file=sys.stderr)
    else:
        print(s)


if __name__ == '__main__':
    main()
