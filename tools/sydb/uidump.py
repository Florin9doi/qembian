#!/usr/bin/env python3
"""uidump: the foreground app's UI as a tree of controls (S60 / CONE), read from a frozen phone.

  python3 tools/sydb/uidump.py [--session DIR | --instance NAME] [--tid HEX] [--all] [--json] [--items N]

Like Android's `uiautomator dump`, for an unmodified Symbian app: no code runs in the phone and
nothing is injected. The tool stops the phone for a moment through the session's gdb, then reads
the app's own objects:

  1. the focused window group's thread (agent `wg`, or --tid) -> DThread (kernel heap scan, as
     kwalk.py) -> its DProcess -> the process's page directory (multiple memory model: user memory
     of any process is read by walking its page tables and reading physical memory through QEMU's
     gdbstub, `Qqemu.PhyMemMode`);
  2. the thread's DLL TLS array (DThread::iTls) -> the pointer that is a CCoeEnv (CONE keeps
     CCoeEnv::Static() there) -> the CCoeAppUi and its control stack;
  3. every object reached from there is named by its C++ RTTI (the ROM's vtables carry typeinfo:
     vtable[-1]; base classes from __si/__vmi_class_type_info), so "is a CCoeControl / CEikListBox"
     needs no symbols and no per-firmware class table;
  4. a control's parent and component array come from CONE's per-control data store (iData, key 0
     = parent, key 2 = CCoeControlArray), the same place CCoeControl::Parent() and
     CountComponentControls() read; controls an Avkon class keeps in its own members (no array)
     hang under the control whose memory points to them;
  5. adapters turn known classes into semantics: CEikListBox (and CAknGrid) items + current
     index, CEikMenuPane items + selection, text members (HBufC) of labels, titles, softkeys.

Offsets were read from the N95 8GB (S60 3rd FP1, 9.2) ROM's own accessors (Ghidra, `syb_sym.py fn`):
CCoeControl Position +0x14, Size +0x1c, flags +0x24 (4 owns window, 8 focused, 0x10 activated,
0x20 invisible, 0x40 dimmed), window +0x28, data store +0x2c; CEikListBox View +0x60, Model +0x68;
CListBoxView top/bottom/current +0x14/+0x18/+0x20; CTextListBoxModel item array +8; CEikMenuPane
items +0x70, selected +0x80; CCoeControlArray RArray +0xc. docs/ui-dump.md has the method and limits.
"""
import argparse, json, os, re, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# guest-side helpers (memory, RTTI, descriptors): no gdb calls at load, so tests run them on a fake gdb
GDB_HELPERS = r'''
import gdb, json
inf = gdb.selected_inferior()
PD = None

def rdk(a, n=4):
    try:
        return int.from_bytes(inf.read_memory(a, n).tobytes(), 'little')
    except Exception:
        return None

def kname(obj):
    p = rdk(obj + 20)
    if not p:
        return ''
    n = (rdk(p) or 0) & 0xfffffff
    try:
        return inf.read_memory(p + 8, min(n, 80)).tobytes().decode('latin-1')
    except Exception:
        return ''

# user memory of that process: page-table walk + physical reads, page cache
pcache = {}
def phys_page(pa):
    pg = pa & ~0xfff
    b = pcache.get(pg)
    if b is None:
        gdb.execute('maint packet Qqemu.PhyMemMode:1', to_string=True)
        try:
            b = inf.read_memory(pg, 0x1000).tobytes()
        except Exception:
            b = b''
        finally:
            gdb.execute('maint packet Qqemu.PhyMemMode:0', to_string=True)
        pcache[pg] = b
    return b
def pword(pa):
    b = phys_page(pa)
    o = pa & 0xfff
    return int.from_bytes(b[o:o + 4], 'little') if len(b) >= o + 4 else None
tcache = {}
def v2p(va):
    pgva = va & ~0xfff
    base = tcache.get(pgva, -1)
    if base == -1:
        base = None
        pde = pword(PD + (va >> 20) * 4)
        if pde is not None:
            if pde & 3 == 2:
                base = (pde & 0xfff00000) | (pgva & 0xff000)
            elif pde & 3 == 1:
                pte = pword((pde & 0xfffffc00) + ((va >> 12) & 0xff) * 4)
                if pte is not None:
                    if pte & 2:
                        base = pte & 0xfffff000
                    elif pte & 3 == 1:
                        base = (pte & 0xffff0000) | (pgva & 0xf000)
        tcache[pgva] = base
    return None if base is None else base | (va & 0xfff)
rom = {}
def u(va):
    va &= 0xffffffff
    if va & 3:
        return None
    if va >= 0x80000000:
        if va not in rom:
            rom[va] = rdk(va)
        return rom[va]
    pa = v2p(va)
    return None if pa is None else pword(pa)
def ubytes(va, n):
    out = bytearray()
    while n > 0:
        pa = v2p(va) if va < 0x80000000 else None
        if va >= 0x80000000:
            try:
                return bytes(out) + inf.read_memory(va, n).tobytes()
            except Exception:
                return bytes(out)
        if pa is None:
            break
        b = phys_page(pa)
        o = pa & 0xfff
        k = min(n, 0x1000 - o)
        out += b[o:o + k]
        va += k
        n -= k
    return bytes(out)
def s32(x):
    return x - (1 << 32) if x is not None and x & 0x80000000 else x

def cstr(a):
    """NUL-terminated string (typeinfo names); read in 16-byte steps so one near the end of mapped
    memory is not lost"""
    out = b''
    while len(out) < 128:
        try:
            b = inf.read_memory(a + len(out), 16).tobytes()
        except Exception:
            b = b''
            for i in range(16):
                try:
                    b += inf.read_memory(a + len(out) + i, 1).tobytes()
                except Exception:
                    break
            if b'\0' not in b:
                return ''
        if b'\0' in b:
            return (out + b.split(b'\0')[0]).decode('latin-1')
        out += b
    return out.decode('latin-1')

# RTTI
tinfo = {}
def ti_info(ti):
    """(mangled name, [base typeinfos]) of a typeinfo address."""
    r = tinfo.get(ti)
    if r is None:
        name = cstr(rdk(ti + 4) or 0)
        kind = cstr(rdk((rdk(ti) or 4) - 4) and rdk(rdk((rdk(ti) or 4) - 4) + 4) or 0)
        bases = []
        if 'si_class' in kind:
            bases = [rdk(ti + 8)]
        elif 'vmi_class' in kind:
            for k in range(min(rdk(ti + 12) or 0, 8)):
                bases.append(rdk(ti + 16 + 8 * k))
        r = (name, [b for b in bases if b])
        tinfo[ti] = r
    return r
ancestry = {}
def lineage(ti):
    r = ancestry.get(ti)
    if r is None:
        name, bases = ti_info(ti)
        r = [name]
        for b in bases:
            for n in lineage(b):
                if n not in r:
                    r.append(n)
        ancestry[ti] = r
    return r
def otype(obj):
    """typeinfo of the most-derived object whose vtable is at obj (None if no ROM vtable)."""
    vt = u(obj)
    if not vt or vt < 0x80000000 or vt >= 0xc0000000:
        return None
    ti = rdk(vt - 4)
    if not ti or ti < 0x80000000 or ti >= 0xc0000000:
        return None
    nm = ti_info(ti)[0]
    if not nm or not (nm[0].isdigit() or nm[0] == 'N'):
        return None
    return ti
def top_of(p):
    """the complete object an interface pointer (MFoo*) points into: vtable[-2] is offset-to-top"""
    vt = u(p)
    if not vt or vt < 0x80000000:
        return p
    o = s32(rdk(vt - 8))
    return p + o if o is not None and -0x400 < o <= 0 else p
def isa(ti, cls):
    return any(n.endswith(cls) and n[:len(n) - len(cls)].isdigit() for n in lineage(ti)) if ti else False

def cell_len(p):
    if p < 0x400000 or p >= 0x80000000 or p & 3:
        return None
    n = u(p - 4)
    if n is None or n < 8 or n > 0x40000 or n & 3:
        return None
    return n - 4

def hbufc(p):
    """text of an HBufC16 at p, or None"""
    n = cell_len(p)
    w = u(p)
    if n is None or w is None or w >> 28 != 0:
        return None
    k = w & 0xfffffff
    if k == 0 or k > 400 or 4 + 2 * k > n:
        return None
    b = ubytes(p + 4, 2 * k)
    if len(b) < 2 * k:
        return None
    s = b.decode('utf-16-le', 'replace')
    if any(ord(c) < 0x20 and c not in '\t\n\r ' or c in '�￿' for c in s):
        return None
    if not any(c.isalnum() for c in s):
        return None
    return s

def tdes(p):
    """TPtrC16 / TBuf16 / HBufC16 descriptor header at p -> text"""
    w = u(p)
    if w is None:
        return None
    typ, k = w >> 28, w & 0xfffffff
    if k > 400:
        return None
    if typ == 0:
        a = p + 4
    elif typ == 1:
        a = u(p + 4)
    elif typ == 3:
        a = p + 8
    elif typ == 2:
        a = u(p + 8)
    else:
        return None
    if a is None:
        return None
    return ubytes(a, 2 * k).decode('utf-16-le', 'replace')

# arrays (CArrayFixBase: vptr, iCount +4, iGranularity +8, iLength +0xc, iCreateRep +0x10, iBase +0x14)
def arr_items(top):
    cnt, ln, buf = u(top + 4), u(top + 0xc), u(top + 0x14)
    if cnt is None or ln is None or not buf or cnt > 5000 or ln not in (4, 8, 12, 16):
        return None
    bti = otype(buf)
    if not bti or not isa(bti, 'CBufFlat'):
        return None                      # segmented buffers not read (yet)
    data = u(buf + 0x10)
    if not data:
        return [] if cnt == 0 else None
    return [(data + i * ln) for i in range(cnt)], ln

def desc_array(p):
    """MDesC16Array* -> (class, [texts]) for the arrays CONE/Eikon apps use"""
    top = top_of(p)
    ti = otype(top)
    cls = demangle(ti_info(ti)[0]) if ti else '?'
    r = arr_items(top) if ti and (isa(ti, 'CArrayFixBase') or 'Array' in cls) else None
    if r is None:
        return cls, None
    slots, ln = r
    out = []
    for s in slots:
        if ln == 4:                      # CDesC16ArrayFlat: HBufC16*
            q = u(s)
            out.append((hbufc(q) if q else '') or tdes(q) or '' if q else '')
        else:                            # CPtrC16Array: TPtrC16
            out.append(tdes(s) or '')
    return cls, out

def plausible(s):
    """UI text, not pointer bytes read as UTF-16: Latin..Cyrillic, Hebrew/Arabic, Thai, general punctuation,
    Avkon's private-use icon characters; at least two letters; no file paths"""
    for c in s:
        o = ord(c)
        if not (c in '\t\n' or 0x20 <= o < 0x0800 or 0x0e00 <= o < 0x0e80 or 0x2000 <= o < 0x2200 or 0xe000 <= o < 0xf900):
            return False
    if sum(c.isalpha() for c in s) < 2:
        return False
    return not (len(s) > 2 and s[1] == ':' and s[2] == '\\')

def desc_strings(p, n):
    """descriptor-headed UTF-16 strings inside [p, p+n): TBufC/HBufC (length word, text) and
    TBuf (length, max length, text)"""
    b = ubytes(p, min(n, 0x2000))
    out = []
    i = 0
    while i + 8 <= len(b):
        w = int.from_bytes(b[i:i + 4], 'little')
        typ, k = w >> 28, w & 0xfffffff
        t = None
        if 1 <= k <= 256:
            if typ == 0:
                t = i + 4
            elif typ == 3:
                mx = int.from_bytes(b[i + 4:i + 8], 'little')
                t = i + 8 if k <= mx <= 1024 else None
        if t is not None and t + 2 * k <= len(b):
            s = b[t:t + 2 * k].decode('utf-16-le', 'replace')
            if plausible(s):
                out.append(s)
                i = (t + 2 * k + 3) & ~3
                continue
        i += 4
    return out

def harvest(p, hops=3, limit=400):
    """strings in an object and the cells it points to (custom MDesCArray: no stored text array to read)"""
    out, seen, q = [], set(), [(p, 0)]
    while q and len(seen) < limit:
        a, h = q.pop(0)
        if a in seen:
            continue
        seen.add(a)
        n = cell_len(a)
        if n is None:
            continue
        ti = otype(a)
        if a != p and ti is not None and (isa(ti, 'CCoeControl') or isa(ti, 'CCoeEnv') or isa(ti, 'CCoeAppUi')):
            continue
        for t in desc_strings(a, n):
            if t not in out:
                out.append(t)
        if h < hops:
            words = ubytes(a, min(n, 0x800))
            for i in range(0, len(words) - 3, 4):
                v = int.from_bytes(words[i:i + 4], 'little')
                if 0x400000 <= v < 0x80000000 and not v & 3 and v not in seen:
                    q.append((v, h + 1))
    return out

def buf_bytes(buf, limit=4096):
    """contents of a CBufFlat (vptr, iSize, iExpandSize, iMaxSize, iPtr) or CBufSeg (segments on a
    TDblQue at +0xc: link {next, prev, iLen}, data after the link)"""
    ti = otype(buf)
    size = u(buf + 4)
    if not ti or size is None or size > 0x100000:
        return None
    if isa(ti, 'CBufFlat'):
        return ubytes(u(buf + 0x10) or 0, min(size, limit))
    if isa(ti, 'CBufSeg'):
        head, out = buf + 0xc, b''
        link = u(head)
        for _ in range(256):
            if not link or link == head or len(out) >= min(size, limit):
                break
            n = u(link + 8) or 0
            out += ubytes(link + 12, min(n, limit))
            link = u(link)
        return out[:min(size, limit)]
    return None

def demangle(m):
    def one(s, i):
        j = i
        while j < len(s) and s[j].isdigit():
            j += 1
        n = int(s[i:j] or 0)
        return s[j:j + n], j + n
    if m.startswith('N'):
        parts, i = [], 1
        while i < len(m) and m[i] != 'E':
            if not m[i].isdigit():
                break
            p, i = one(m, i)
            parts.append(p)
        return '::'.join(parts)
    if m[:1].isdigit():
        return one(m, 0)[0] + (m[one(m, 0)[1]:] and '<..>')
    return m

'''

GDB_WALK = r'''
TID = %(tid)d
OUT = %(out)r
MAX_OBJS = %(max_objs)d
def off(expr):
    return int(gdb.parse_and_eval('(unsigned)&((%%s*)0x10000000)->%%s' %% tuple(expr.split()))) - 0x10000000

DT_PROC, DT_ID, DT_TLS, DT_NTH = off('DThread iOwningProcess'), off('DThread iId'), off('DThread iTls'), off('DThread iNThread')
DP_PD = off('DMemModelProcess iLocalPageDir')

# 1. the thread
cur = int(gdb.parse_and_eval('(unsigned)TheScheduler.iCurrentThread'))
dthread_vt = rdk(cur - DT_NTH)
thread = None
for page in range(0xc8000000, 0xc9200000, 0x1000):
    try:
        mem = inf.read_memory(page, 0x1000).tobytes()
    except Exception:
        continue
    for i in range(0, 0x1000, 4):
        if int.from_bytes(mem[i:i + 4], 'little') == dthread_vt and rdk(page + i + DT_ID) == TID:
            thread = page + i
            break
    if thread:
        break
res = {'tid': TID}
if not thread:
    res['error'] = 'thread %%#x not found' %% TID
    json.dump(res, open(OUT, 'w'))
    raise SystemExit
proc = rdk(thread + DT_PROC)
PD = rdk(proc + DP_PD)
res.update(thread=kname(thread), process=kname(proc))
cs = rdk(proc + off('DProcess iCodeSeg'))
fn = rdk(cs + off('DCodeSeg iFileName')) if cs else None
if fn:
    n = (rdk(fn) or 0) & 0xfffffff
    try:
        res['exe'] = inf.read_memory(fn + 8, min(n, 128)).tobytes().decode('latin-1')
    except Exception:
        pass
res['uid3'] = '%%#010x' %% (rdk(proc + off('DProcess iUids') + 8) or 0)

# 2. CCoeEnv from the thread's TLS, then the AppUi
env = appui = None
tcnt, tent = rdk(thread + DT_TLS), rdk(thread + DT_TLS + 4)
for k in range(min(tcnt or 0, 64)):
    p = rdk(tent + 12 * k + 8)
    if p and p < 0x80000000:
        ti = otype(p)
        if ti and isa(ti, 'CCoeEnv'):
            env = p
if not env:
    res['error'] = 'no CCoeEnv in the TLS of thread %%#x (not a CONE app?)' %% TID
    json.dump(res, open(OUT, 'w'))
    raise SystemExit
for k in range(0, 0x60, 4):
    p = u(env + k)
    ti = otype(p) if p and 0x400000 <= p < 0x80000000 else None
    if ti and isa(ti, 'CCoeAppUi'):
        appui = p
        break
res['env'] = '%%#x' %% env
res['env_class'] = demangle(ti_info(otype(env))[0])
res['appui_class'] = demangle(ti_info(otype(appui))[0]) if appui else None

stack = []
if appui:
    st = u(appui + 12)
    r = arr_items(st) if st else None
    if r:
        for s in r[0]:
            stack.append({'ctl': u(s), 'prio': s32(u(s + 4)), 'flags': u(s + 8)})
res['stack'] = [{'ctl': '%%#x' %% e['ctl'], 'prio': e['prio'], 'flags': e['flags']} for e in stack if e['ctl']]

# 3. reach every object from the env, the AppUi and the stack; keep the controls
def ds_get(ctl, key):
    ds = u(ctl + 0x2c)
    if not ds:
        return None
    lo, hi, slots = u(ds), u(ds + 4), u(ds + 8)
    if lo is None or hi is None or not slots:
        return None
    idx = (((hi << 32) | lo) >> (4 * key)) & 0xf
    return None if idx == 0xf else u(slots + 4 * idx)

controls = {}
order = []
seen = set()
classes = {}
# queue: (address, typed objects since the last control, the control it hangs under, referrer typed?)
queue = [(p, 0, None, True) for p in [env, appui] + [e['ctl'] for e in stack] if p]
MAXHOP = 4
while queue and len(seen) < MAX_OBJS:
    p, hop, via, from_typed = queue.pop(0)
    if p in seen:
        continue
    seen.add(p)
    n = cell_len(p)
    if n is None:
        continue
    ti = otype(p)
    if ti is None and not from_typed:
        continue                         # raw cells (array buffers) only straight from a typed object
    is_ctl = ti is not None and isa(ti, 'CCoeControl')
    if ti is not None:
        nm = demangle(ti_info(ti)[0])
        classes[nm] = classes.get(nm, 0) + 1
    if is_ctl:
        controls[p] = {'ti': ti, 'via': via}
        order.append(p)
        via, hop = p, 0
    elif hop >= MAXHOP:
        continue
    if ti is not None and not is_ctl:
        nm = ti_info(ti)[0]
        if p not in (env, appui) and ('Env' in nm or 'AppUi' in nm):
            continue                     # hubs: reached again from everywhere
    nhop = 0 if is_ctl else hop + (1 if ti is not None else 0)
    words = ubytes(p, min(n, 0x1000))
    for i in range(0, len(words) - 3, 4):
        q = int.from_bytes(words[i:i + 4], 'little')
        if 0x400000 <= q < 0x80000000 and not q & 3 and q not in seen:
            queue.append((q, nhop, via, ti is not None))
    if is_ctl:
        par = ds_get(p, 0)
        if par and par not in seen:
            queue.append((par, 0, None, True))
res['classes'] = classes
res['objects_seen'] = len(seen)

# 4. describe each control
def ctl_node(p):
    ti = controls[p]['ti']
    lin = lineage(ti)
    flags = u(p + 0x24) or 0
    node = {
        'addr': '%%#x' %% p,
        'class': demangle(lin[0]),
        'kind': next((demangle(n) for n in lin if n.lstrip('0123456789') in KINDS), None),
        'pos': [s32(u(p + 0x14)), s32(u(p + 0x18))],
        'size': [s32(u(p + 0x1c)), s32(u(p + 0x20))],
        'flags': flags,
        'win': u(p + 0x28) or 0,
        'seq': order.index(p),
    }
    par = ds_get(p, 0)
    node['parent'] = '%%#x' %% par if par in controls else None
    node['via'] = '%%#x' %% controls[p]['via'] if controls[p]['via'] else None
    arr = ds_get(p, 2)
    kids = []
    if arr:
        cnt, ent = u(arr + 0xc), u(arr + 0x10)
        for k in range(min(cnt or 0, 200)):
            c = u(ent + 8 * k)
            if c:
                kids.append('%%#x' %% c)
    node['components'] = kids
    # text members: HBufC16 pointers in the control's own memory
    texts = []
    n = cell_len(p) or 0
    words = ubytes(p, min(n, 0x400))
    for i in range(0x34, len(words) - 3, 4):
        q = int.from_bytes(words[i:i + 4], 'little')
        if 0x400000 <= q < 0x80000000 and q not in controls:
            s = hbufc(q)
            s = s.strip() if s else s
            if s and s not in texts:
                texts.append(s)
    node['texts'] = texts
    if isa(ti, 'CEikListBox'):
        view, model = u(p + 0x60), u(p + 0x68)
        lb = {}
        if view:
            lb.update(top=s32(u(view + 0x14)), bottom=s32(u(view + 0x18)), current=s32(u(view + 0x20)))
        if model:
            mt = top_of(model)
            mti = otype(mt)
            lb['model'] = demangle(ti_info(mti)[0]) if mti else '?'
            ia = u(mt + 8) if mti and isa(mti, 'CTextListBoxModel') else None
            if ia:
                cls, items = desc_array(ia)
                lb['array'] = cls
                if items is not None:
                    lb['items'] = items
                else:
                    lb['strings'] = harvest(top_of(ia))
        node['list'] = lb
    if isa(ti, 'CEikEdwin'):
        txt = u(p + 0x70)                        # CEikEdwin::Text(): CPlainText*, byte store +0x10
        b = buf_bytes(u(txt + 0x10)) if txt else None
        if b is not None:
            node['edit'] = b.decode('utf-16-le', 'replace').rstrip('\u2029').replace('\u2029', '\n')
    if isa(ti, 'CEikMenuPane'):
        ia = u(p + 0x70)
        mp = {'selected': s32(u(p + 0x80)), 'items': []}
        r = arr_items(ia) if ia else None
        for s in (r[0] if r else []):
            it = u(s)
            if it:
                d = it + 0xc
                mp['items'].append({'cmd': u(d), 'cascade': u(d + 4), 'flags': u(d + 8), 'text': tdes(d + 0xc) or ''})
        node['menu'] = mp
    return node

KINDS = {'CEikListBox', 'CAknGrid', 'CEikMenuPane', 'CEikMenuBar', 'CEikLabel', 'CEikCba', 'CEikCbaButton',
         'CEikEdwin', 'CEikDialog', 'CAknNoteDialog', 'CAknPopupList', 'CAknTitlePane', 'CAknNavigationControlContainer',
         'CEikImage', 'CAknQueryControl', 'CAknSetStyleListBox', 'CEikButtonGroupContainer', 'CAknView'}
res['controls'] = [ctl_node(p) for p in order]
json.dump(res, open(OUT, 'w'))
'''


def sydb_base(a):
    base = [sys.executable, os.path.join(ROOT, 'tools/sydb/sydb.py')]
    if a.instance:
        base += ['--instance', a.instance]
    elif a.session:
        base += ['--session', a.session]
    return base


def focused_tid(base):
    r = subprocess.run(base + ['--json', 'wg'], capture_output=True, text=True, timeout=30)
    try:
        items = json.loads(r.stdout)['items']
    except Exception:
        sys.exit('uidump: cannot list window groups (agent up? phone not stopped?): %s' % (r.stdout + r.stderr).strip())
    for it in items:
        if it.get('focus'):
            return int(it['tid'], 16), it.get('name', '')
    sys.exit('uidump: no focused window group')


def snapshot(base, tid, max_objs=6000):
    """Stop the phone, read the app with `tid`, resume it. Returns the raw dict from the guest walk."""
    fd, out = tempfile.mkstemp(prefix='uidump-', suffix='.json')
    os.close(fd)
    code = GDB_HELPERS.replace('%%', '%') + GDB_WALK % {'tid': tid, 'out': out, 'max_objs': max_objs}
    fd, src = tempfile.mkstemp(prefix='uidump-', suffix='.py')
    os.write(fd, code.encode())
    os.close(fd)
    try:
        subprocess.run(base + ['dbg', 'attach'], capture_output=True, text=True)
        subprocess.run(base + ['dbg', 'interrupt'], capture_output=True, text=True)
        r = subprocess.run(base + ['dbg', 'cmd', "python exec(open(%r).read())" % src],
                           capture_output=True, text=True)
        subprocess.run(base + ['dbg', 'detach'], capture_output=True, text=True)
        try:
            return json.load(open(out))
        except Exception:
            sys.exit('uidump: the gdb walk failed: %s' % ((r.stdout or '') + (r.stderr or '')).strip()[-2000:])
    finally:
        for f in (out, src):
            try:
                os.unlink(f)
            except OSError:
                pass


# ---- host side: tree, screen rectangles, summary

F_OWNWIN, F_FOCUS, F_ACTIVE, F_INVIS, F_DIM = 0x4, 0x8, 0x10, 0x20, 0x40


def build_tree(d):
    """Link the controls: CONE parent first, else the control whose memory pointed to it."""
    nodes = {c['addr']: c for c in d.get('controls', [])}
    for c in nodes.values():
        c['children'] = []
    for c in nodes.values():
        # a window-owning control without a CONE parent is a top-level window (menu panes, dialogs, CBA):
        # a root, even when some other control's memory points to it
        par = c.get('parent')
        if not par and not c['flags'] & F_OWNWIN and c.get('via') in nodes and c.get('via') != c['addr']:
            par = c['via']
        # no cycles
        q, seen = par, set()
        while q and q not in seen:
            seen.add(q)
            if q == c['addr']:
                par = None
                break
            q = nodes[q].get('_par') if q in nodes else None
        c['_par'] = par
    for c in nodes.values():
        if c['_par'] in nodes:
            nodes[c['_par']]['children'].append(c)
    roots = [c for c in nodes.values() if c['_par'] not in nodes]

    def screen(c):
        """screen rectangle: a window-owning control's position is relative to its parent window,
        any other control's to the window it draws in (the nearest window-owning ancestor)"""
        if 'rect' in c:
            return c['rect']
        x, y = c['pos']
        p = nodes.get(c['_par'])
        while p is not None and not p['flags'] & F_OWNWIN:
            p = nodes.get(p['_par'])
        if p is not None:
            px, py, _, _ = screen(p)
            x, y = x + px, y + py
        c['rect'] = [x, y, c['size'][0], c['size'][1]]
        return c['rect']

    def vis(c, parent_visible):
        # invisibility is inherited; a zero size hides only the control itself (the keypad-lock control
        # is 0x0 and its CBA, a component, is on screen)
        shown = parent_visible and not c['flags'] & F_INVIS
        c['visible'] = shown and c['size'][0] > 0 and c['size'][1] > 0
        for k in c['children']:
            vis(k, shown)
    for c in nodes.values():
        screen(c)
    for r in roots:
        vis(r, True)

    # text drawn while each window redrew: window coordinates -> screen -> the deepest visible control there
    for t in d.get('drawn', []):
        win = nodes.get(t['win'])
        if win is None or not t['text'].strip():
            continue
        x, y = win['rect'][0] + t['x'], win['rect'][1] + t['y']
        px, py = (x + t['w'] // 2, y + t['h'] // 2) if t['w'] else (x + 1, y - 2)   # a point is the baseline

        def deepest(c):
            if not c.get('visible'):
                return None
            rx, ry, rw, rh = c['rect']
            if not (rx <= px < rx + rw and ry <= py < ry + rh):
                return None
            for k in c['children']:
                hit = deepest(k)
                if hit:
                    return hit
            return c
        owner = deepest(win) or win
        item = {'text': t['text'], 'rect': [x, y, t['w'], t['h']]}
        if item not in owner.setdefault('drawn', []):
            owner['drawn'].append(item)

    def sort(c):
        comp = c.get('components') or []
        c['children'].sort(key=lambda k: (comp.index(k['addr']) if k['addr'] in comp else 999, k['seq']))
        for k in c['children']:
            sort(k)
    roots.sort(key=lambda r: (not r['visible'], r['rect'][1], r['rect'][0]))
    for r in roots:
        sort(r)

    def paths(sibs, prefix):
        # a control's id that survives restarts: class names from the root, [n] among same-class siblings
        seen = {}
        for c in sibs:
            k = seen.get(c['class'], 0)
            seen[c['class']] = k + 1
            same = sum(1 for o in sibs if o['class'] == c['class'])
            c['path'] = prefix + '/' + c['class'] + ('[%d]' % k if same > 1 else '')
            paths(c['children'], c['path'])
    paths(roots, '')
    return roots, nodes


# ---- readable names for pictures: icon_names.json (hand-made: "File.mif#id" or a pixel hash -> label),
# the SDK's avkon.mbg (Avkon's icon ids -> qgn_ names), and app icons (<exe>_aif.mif -> the app's caption)
ICON_LABELS = {}
ICON_TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icon_names.json')


def load_icon_labels(base=None):
    labels = {}
    try:
        labels.update({k.lower(): v for k, v in json.load(open(ICON_TABLE)).items() if not k.startswith('_')})
    except (OSError, ValueError):
        pass
    sdk = os.environ.get('S60SDK', os.path.expanduser('~/work/S60_3rd_FP1/Epoc32'))
    try:
        for m in re.finditer(r'EMbmAvkon(\w+)\s*=\s*(\d+)', open(os.path.join(sdk, 'include', 'avkon.mbg')).read()):
            labels.setdefault('avkon2.mif#%s' % m.group(2), m.group(1).lower())
            labels.setdefault('avkon2.mbm#%s' % m.group(2), m.group(1).lower())
    except OSError:
        pass
    if base:
        sess = base[base.index('--session') + 1] if '--session' in base else None
        cache = os.path.join(sess, 'ui-apps.json') if sess else None
        apps = None
        try:
            apps = json.load(open(cache))
        except (OSError, ValueError, TypeError):
            r = subprocess.run(base + ['--json', 'apps'], capture_output=True, text=True, timeout=30)
            try:
                apps = json.loads(r.stdout).get('items', [])
                if cache:
                    json.dump(apps, open(cache, 'w'))
            except ValueError:
                apps = []
        for a in apps or []:
            exe = (a.get('path') or a.get('exe') or '').replace('\\', '/').split('/')[-1].rsplit('.', 1)[0].lower()
            if exe and a.get('caption'):
                labels.setdefault('app:' + exe, a['caption'])
    ICON_LABELS.clear()
    ICON_LABELS.update(labels)


def icon_id(ic):
    """the picture's identity across restarts: Avkon icon File.mif#id, else size + pixel hash"""
    if ic.get('file'):
        return '%s#%d' % (ic['file'].replace('\\', '/').split('/')[-1], ic['bmp'])
    return '%dx%d:%s' % (ic['size'][0], ic['size'][1], ic['hash'])


def icon_label(ic):
    """a readable label for a picture, or None"""
    k = icon_id(ic).lower()
    if k in ICON_LABELS:
        return ICON_LABELS[k]
    if ic.get('hash') and ic['hash'].lower() in ICON_LABELS:
        return ICON_LABELS[ic['hash'].lower()]
    m = re.match(r'(.+)_aif\.mif#', k)
    if m:
        return ICON_LABELS.get('app:' + m.group(1))
    return None


def icon_name(ic):
    lab = icon_label(ic)
    return '%s"%s"' % (icon_id(ic), lab) if lab else icon_id(ic)


def item_icons(c):
    """icon index -> name for a list/grid's icon array (items name their icons by index: "1\tMessaging")"""
    out = {}
    for ic in c.get('icons', []):
        m = re.match(r'icon\[(\d+)\]$', ic['slot'])
        if m:
            out[int(m.group(1))] = icon_name(ic)
    return out


def item_text(c, s):
    """an item's label; an item that is only icon indices ("0") is named by its icons"""
    label = item_label(s)
    if label.strip() and not label.isdigit():
        return label
    icons = item_icons(c)
    names = [icons.get(int(p)) for p in s.split('\t') if p.isdigit()]
    labels = [re.search(r'"(.*)"$', n).group(1) for n in names if n and n.endswith('"')]
    if labels and len(labels) == len([n for n in names if n]):
        return ' | '.join(labels)
    names = [n for n in names if n]
    return ('[icon %s]' % ', '.join(names)) if names else label


def item_label(s):
    """Avkon list item text "1\\tMessaging\\t\\t" -> "Messaging" (icon indices and empty columns dropped)"""
    parts = [p for p in s.split('\t') if p and not p.isdigit()]
    return ' | '.join(parts) if parts else s


def summary(d, roots, nodes):
    vis = [c for c in nodes.values() if c.get('visible')]
    out = {'app': d.get('exe') or d.get('process'), 'uid': d.get('uid3'), 'appui': d.get('appui_class')}
    for c in vis:
        if c['class'] == 'CAknTitlePane' or c.get('kind') == 'CAknTitlePane':
            t = c['texts'] or [t for k in c['children'] for t in k.get('texts', [])]
            if t:
                out['title'] = t[0]
    cba = [c for c in vis if c.get('kind') == 'CEikCba' or c['class'] == 'CEikCba']
    stack = [e['ctl'] for e in d.get('stack', [])]
    cba.sort(key=lambda c: stack.index(c['addr']) if c['addr'] in stack else 99)
    if cba:
        # CEikCba's buttons: left, right, then the middle softkey (not drawn on 3rd FP1 phones)
        btns = []
        for b in cba[0]['children']:
            t = [x for k in [b] + b['children'] if k.get('visible') for x in k.get('texts', [])]
            if t and b.get('visible'):
                btns.append((b, t[0]))
        cx = cba[0]['rect'][0] + cba[0]['rect'][2] // 2
        keys = {}
        for b, t in btns:
            # the middle softkey's button is never activated on phones that do not draw it (3rd FP1)
            if not b['flags'] & F_ACTIVE:
                keys.setdefault('middle', t)
            else:
                keys.setdefault('left' if b['rect'][0] + b['rect'][2] // 2 < cx else 'right', t)
        out['softkeys'] = {'left': keys.get('left'), 'right': keys.get('right')}
        if 'middle' in keys:
            out['softkeys']['middle'] = keys['middle']
    focus = [c for c in vis if c['flags'] & F_FOCUS]
    lists = [c for c in vis if 'list' in c and (c['list'].get('items') is not None or c['list'].get('strings'))]
    lst = next((c for c in lists if c in focus), None) or (lists[0] if lists else None)
    if lst:
        lb = lst['list']
        items = [item_text(lst, s) for s in lb.get('items') or lb.get('strings')]
        cur = lb.get('current')
        exact = lb.get('items') is not None
        out['list'] = {'class': lst['class'], 'grid': lb.get('cols') or lst.get('kind') == 'CAknGrid', 'count': len(items) if exact else None, 'current': cur,
                       'selected': items[cur] if exact and cur is not None and 0 <= cur < len(items) else None,
                       'items' if exact else 'strings_heuristic': items}
    menus = [c for c in vis if 'menu' in c]
    if menus:
        m = menus[-1]['menu']
        names = [i['text'] for i in m['items']]
        sel = m.get('selected')
        out['menu'] = {'items': names, 'selected': names[sel] if sel is not None and 0 <= sel < len(names) else None}
    drawn = sorted(((t['rect'][1], t['rect'][0], t['text']) for c in vis for t in c.get('drawn', [])))
    if drawn:
        seen, texts = set(), []
        for _, _, t in drawn:
            if t not in seen:
                seen.add(t)
                texts.append(t)
        out['screen_text'] = texts
    if focus:
        out['focused'] = focus[-1]['class']
        ed = [c for c in focus if 'edit' in c]
        if ed:
            out['editor'] = ed[-1]['edit']
    return out


SHOW_PATHS = False


def print_tree(roots, show_all, max_items):
    def line(c, depth):
        x, y, w, h = c['rect']
        fl = []
        if c['flags'] & F_FOCUS:
            fl.append('focused')
        if c['flags'] & F_DIM:
            fl.append('dimmed')
        if c['flags'] & F_OWNWIN:
            fl.append('window')
        if not c.get('visible'):
            fl.append('hidden')
        kind = ' <%s>' % c['kind'] if c.get('kind') and c['kind'] != c['class'] else ''
        s = '%s%s%s  [%d,%d %dx%d]%s' % ('  ' * depth, c['class'], kind, x, y, w, h,
                                        (' ' + ' '.join(fl)) if fl else '')
        if c.get('texts'):
            s += '  ' + ' '.join('"%s"' % t.replace('\t', '\\t') for t in c['texts'][:4])
        if 'edit' in c:
            s += '  edit=%r' % c['edit']
        own = [ic for ic in c.get('icons', []) if not ic['slot'].startswith('icon[')]
        if own:
            s += '  ' + ' '.join('icon=' + icon_name(ic) for ic in own[:4])
        if c.get('drawn'):
            s += '  drawn: ' + ' '.join('"%s"' % t['text'].replace('\t', ' ') for t in c['drawn'][:6])
        if SHOW_PATHS:
            s += '  {%s}' % c['path']
        print(s)
        if 'list' in c:
            lb = c['list']
            items = lb.get('items')
            print('%s  list: %s, %s items, current %s, top %s (%s)' % (
                '  ' * depth, lb.get('model', '?'), len(items) if items is not None else '?',
                lb.get('current'), lb.get('top'), lb.get('array', '?')))
            for i, t in enumerate((items or [])[:max_items]):
                print('%s   %s %2d %s' % ('  ' * depth, '>' if i == lb.get('current') else ' ', i, item_text(c, t)))
            if items and len(items) > max_items:
                print('%s     ... %d more' % ('  ' * depth, len(items) - max_items))
            if lb.get('strings'):
                print('%s   strings near the item array (heuristic, order not the list\'s): %s' % (
                    '  ' * depth, ', '.join('"%s"' % t.replace('\t', '\\t') for t in lb['strings'][:max_items])))
        if 'menu' in c:
            m = c['menu']
            for i, it in enumerate(m['items']):
                print('%s   %s %2d %s  (cmd %#x%s)' % ('  ' * depth, '>' if i == m.get('selected') else ' ', i,
                                                   it['text'], it['cmd'] or 0, ', submenu' if it['cascade'] else ''))
        for k in c['children']:
            if show_all or k.get('visible'):
                line(k, depth + 1)
    for r in roots:
        if show_all or r.get('visible'):
            line(r, 0)


# ---- the agent backend: the in-app probe (src/n95apps/uiprobe, docs/ui-probe.md)

def unescape(t):
    """uiprobe text: \\\\ \\t \\n and \\uXXXX"""
    out, i = [], 0
    while i < len(t):
        c = t[i]
        if c == '\\' and i + 1 < len(t):
            n = t[i + 1]
            if n == 'u' and i + 6 <= len(t):
                try:
                    out.append(chr(int(t[i + 2:i + 6], 16)))
                    i += 6
                    continue
                except ValueError:
                    pass
            out.append({'t': '\t', 'n': '\n', '\\': '\\'}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def demangle_name(m):
    """Itanium class name ("11CEikListBox", "N9CCoeAppUi6CExtraE") -> "CEikListBox", "CCoeAppUi::CExtra\""""
    def one(i):
        j = i
        while j < len(m) and m[j].isdigit():
            j += 1
        n = int(m[i:j] or 0)
        return m[j:j + n], j + n
    if m.startswith('N'):
        parts, i = [], 1
        while i < len(m) and m[i].isdigit():
            p, i = one(i)
            parts.append(p)
        return '::'.join(parts) or m
    if m[:1].isdigit():
        name, end = one(0)
        return name + ('<..>' if end < len(m) else '')
    return m


def parse_probe(lines):
    """uiprobe's dump lines (format: uiprobe.cpp) -> the dict snapshot() returns: controls with id, parent,
    class, kind, pos, size, flags, texts, list, menu, edit; the control stack"""
    d = {'controls': [], 'stack': [], 'backend': 'agent'}
    by = {}
    for ln in lines:
        f = ln.split('\t')
        tag = f[0]
        try:
            if tag == 'A':
                d.update(exe=unescape(f[1]), uid3='%#010x' % int(f[2], 16), appui_class=demangle_name(f[3]))
            elif tag == 'C':
                c = {'addr': f[1], 'parent': None if f[2] == '-1' else f[2], 'via': None, 'class': demangle_name(f[3]),
                     'kind': None if f[4] == '-' else f[4], 'pos': [int(f[5]), int(f[6])], 'size': [int(f[7]), int(f[8])],
                     'flags': int(f[9], 16), 'win': 0, 'components': [], 'texts': [], 'seq': len(d['controls'])}
                d['controls'].append(c)
                by[f[1]] = c
            elif tag == 'T':
                by[f[1]]['texts'].append(unescape(f[2]).strip())
            elif tag == 'E':
                by[f[1]]['edit'] = unescape(f[2]).replace('\u2029', '\n')
            elif tag == 'L':
                by[f[1]]['list'] = {'count': int(f[2]), 'current': int(f[3]), 'top': int(f[4]), 'bottom': int(f[5]),
                                    'cols': int(f[6]), 'model': demangle_name(f[7])}
            elif tag == 'I':
                by[f[1]]['list'].setdefault('items', []).append(unescape(f[3]))
            elif tag == 'M':
                by[f[1]]['menu'] = {'selected': int(f[2]), 'items': []}
            elif tag == 'N':
                by[f[1]]['menu']['items'].append({'cmd': int(f[3]), 'cascade': int(f[4]), 'flags': int(f[5], 16),
                                                  'text': unescape(f[6]) if len(f) > 6 else ''})
            elif tag == 'S':
                d['stack'] = [{'ctl': x} for x in f[1:] if x]
            elif tag == 'B':
                by[f[1]].setdefault('icons', []).append({
                    'slot': f[2], 'size': [int(f[3]), int(f[4])], 'mode': int(f[5]), 'hash': f[6][2:].rjust(8, '0'),
                    'file': unescape(f[7]), 'bmp': int(f[8]), 'mask': int(f[9])})
            elif tag == 'W':
                d.setdefault('drawn', []).append({'win': None if f[1] == '-1' else f[1], 'x': int(f[2]), 'y': int(f[3]),
                                                  'w': int(f[4]), 'h': int(f[5]), 'text': unescape(f[6]) if len(f) > 6 else ''})
            elif tag == 'G':
                d['guard'] = f[1] if len(f) > 1 else ''
            elif tag == 'X':
                d['truncated'] = f[1] if len(f) > 1 else True
        except (IndexError, KeyError, ValueError):
            d.setdefault('bad_lines', []).append(ln)
    d['objects_seen'] = 0
    return d


def agent_snapshot(base, tid=None, tries=2):
    """ask the probe in the app (devagent `ui`); returns the snapshot dict, or {'error': ...}. A busy app
    (still starting, or redrawing after another dump) can miss the agent's 3 s: asked once more"""
    for n in range(tries):
        d = _agent_snapshot(base, tid)
        if not d.get('error') or 'no reply' not in d['error'] or n == tries - 1:
            return d
        time.sleep(0.5)


def _agent_snapshot(base, tid=None):
    r = subprocess.run(base + ['--json', 'agent', 'ui'] + (['%x' % tid] if tid else []),
                       capture_output=True, text=True, timeout=30)
    try:
        j = json.loads(r.stdout)
    except ValueError:
        return {'error': (r.stdout + r.stderr).strip() or 'agent: no answer'}
    if not j.get('ok', True) or 'items' not in j:
        return {'error': j.get('msg') or j.get('error') or (r.stdout + r.stderr).strip()}
    d = parse_probe([i.get('l', '') for i in j['items']])
    d['tid_hex'] = j.get('tid')
    return d


BACKEND = 'auto'      # the CLI's --agent / --gdb


def capture(base, tid=None, backend=None):
    """One UI snapshot of the focused app (or thread `tid`): {'raw', 'roots', 'nodes', 'summary', 'tid', 'wg'}.
    backend: 'agent' (the in-app probe), 'gdb' (read the frozen phone's memory), 'auto' (agent, then gdb)."""
    backend = backend or BACKEND
    wgname = ''
    d = None
    if backend in ('agent', 'auto'):
        d = agent_snapshot(base, tid)
        if d.get('error') and backend == 'auto':
            err = d['error']
            d = None
    if d is None:
        if tid is None:
            tid, wgname = focused_tid(base)
        d = snapshot(base, tid)
        d['backend'] = 'gdb'
        if backend == 'auto':
            d['agent_error'] = err
    elif tid is None and d.get('tid_hex'):
        tid = int(d['tid_hex'], 16)
    d['wg'] = wgname
    if d.get('error'):
        return {'raw': d, 'tid': tid, 'wg': wgname, 'error': d['error']}
    if not ICON_LABELS:
        load_icon_labels(base)
    roots, nodes = build_tree(d)
    summ = summary(d, roots, nodes)
    summ['backend'] = d.get('backend')
    return {'raw': d, 'roots': roots, 'nodes': nodes, 'summary': summ, 'tid': tid, 'wg': wgname}


def tree_json(roots, show_all=False):
    def clean(c):
        r = {k: v for k, v in c.items() if not k.startswith('_') and k not in ('children', 'via', 'parent', 'pos', 'components', 'seq')}
        if r.get('icons'):
            r['icons'] = [dict(ic, id=icon_id(ic), **({'label': icon_label(ic)} if icon_label(ic) else {})) for ic in r['icons']]
        r['children'] = [clean(k) for k in c['children'] if show_all or k.get('visible')]
        return r
    return [clean(r) for r in roots if show_all or r.get('visible')]


def target_list(summ):
    """what the arrow keys move in: an open menu pane first, else the focused list or grid"""
    if 'menu' in summ:
        m = summ['menu']
        return 'menu', m['items'], m['items'].index(m['selected']) if m.get('selected') in m['items'] else None, False
    if 'list' in summ and summ['list'].get('items') is not None:
        lb = summ['list']
        return lb['class'], lb['items'], lb['current'], lb.get('grid', False)
    return None, None, None, False


def find_item(items, text):
    t = text.casefold()
    for pred in (lambda s: s.casefold() == t, lambda s: s.casefold().startswith(t), lambda s: t in s.casefold()):
        hit = [i for i, s in enumerate(items) if pred(s)]
        if hit:
            return hit[0]
    return None


def press(base, keys, delay=0.15):
    if keys:
        subprocess.run(base + ['key'] + keys + ['--delay', str(delay)], capture_output=True, text=True, timeout=60)


def select(base, text, open_item=False, rounds=6):
    """Move the highlight of the open menu / focused list or grid to the item labelled `text` with the
    arrow keys, checking with a fresh snapshot after each batch (lists can wrap, grids are rows of
    columns: the batch is corrected next round). Returns the final summary; raises on failure."""
    last = None
    busy = 0
    for rnd in range(rounds):
        st = capture(base)
        while st.get('error') and busy < 6:   # an app still starting (GSApp loading its plugins) answers late
            busy += 1
            time.sleep(0.5)
            st = capture(base)
        if st.get('error'):
            raise RuntimeError(st['error'])
        kind, items, cur, grid = target_list(st['summary'])
        if items is None:
            raise RuntimeError('no menu or readable list in focus')
        i = find_item(items, text)
        if i is None:
            raise RuntimeError('%r not among %s items: %s' % (text, kind, items))
        if last is not None and cur == last:
            time.sleep(0.3 * rnd)        # the keys were lost (an app still starting drops input): back off, retry
            st = capture(base)
            kind, items, cur, grid = target_list(st['summary'])
        last = cur
        if cur == i:
            if open_item:
                if rnd:
                    time.sleep(0.3)      # the highlight just moved: the Menu's 3D grid drops OK while it animates
                press(base, ['ok'])
            return st['summary']
        n = i - (cur or 0)
        if grid is not True and grid and grid > 1 and cur is not None:   # columns known (the probe): rows, then columns
            dr, dc = i // grid - cur // grid, i % grid - cur % grid
            press(base, (['down' if dr > 0 else 'up'] * abs(dr) + ['right' if dc > 0 else 'left'] * abs(dc))[:60])
            continue
        fwd, back = ('right', 'left') if grid else ('down', 'up')
        press(base, [fwd if n > 0 else back] * min(abs(n), 60))
    raise RuntimeError('highlight did not reach %r after %d rounds' % (text, rounds))


def wait_for(base, text, timeout=10.0, interval=0.5):
    """until `text` is in the visible UI (title, softkeys, labels, list or menu items)"""
    end = time.time() + timeout
    t = text.casefold()
    while True:
        st = capture(base)
        if not st.get('error'):
            vis = [c for c in st['nodes'].values() if c.get('visible')]
            hay = [x for c in vis for x in c.get('texts', [])]
            for c in vis:
                hay += [item_label(x) for x in c.get('list', {}).get('items') or c.get('list', {}).get('strings') or []]
                hay += [m['text'] for m in c.get('menu', {}).get('items', [])]
            if any(t in h.casefold() for h in hay):
                return st['summary']
        if time.time() > end:
            raise RuntimeError('%r not on screen after %.0f s' % (text, timeout))
        time.sleep(interval)


def set_backend(a):
    global BACKEND
    BACKEND = 'gdb' if a.gdb else 'agent' if a.agent else 'auto'


USAGE = """uidump.py [--session DIR | --instance NAME] [COMMAND]
  (none) | dump [--all] [--json] [--raw] [--tid HEX] [--items N]   the focused app's control tree
  --agent | --gdb             the in-app probe only / read memory through gdb (default: probe, else gdb)
  select TEXT [--open]        highlight the menu/list/grid item labelled TEXT (keys + check); --open presses OK
  wait TEXT [--timeout S]     until TEXT is visible (labels, title, softkeys, list and menu items)"""


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    cmd = 'dump'
    for c in ('dump', 'select', 'wait'):
        if c in argv:
            cmd = c
            argv.remove(c)
            break
    ap = argparse.ArgumentParser(prog='uidump.py', usage=USAGE)
    ap.add_argument('--session', default=None, help='session directory (a device instance is one)')
    ap.add_argument('--instance', default=None, help='device instance name (tools/instance.py list)')
    ap.add_argument('--agent', action='store_true', help='only the in-app probe (no gdb fallback)')
    ap.add_argument('--gdb', action='store_true', help='read the frozen phone\'s memory instead of asking the probe')
    if cmd != 'dump':
        ap.add_argument('text')
        ap.add_argument('--open', action='store_true')
        ap.add_argument('--timeout', type=float, default=10.0)
        a = ap.parse_args(argv)
        base = sydb_base(a)
        set_backend(a)
        try:
            r = select(base, a.text, a.open) if cmd == 'select' else wait_for(base, a.text, a.timeout)
        except RuntimeError as e:
            sys.exit('uidump: %s' % e)
        print(json.dumps(r, indent=1, ensure_ascii=False))
        return
    ap.add_argument('--tid', default=None, help='thread id (hex) of the app thread; default: the focused window group')
    ap.add_argument('--all', action='store_true', help='also hidden controls (other views, invisible panes)')
    ap.add_argument('--json', action='store_true', help='summary + tree as JSON')
    ap.add_argument('--raw', action='store_true', help='the walk result as read (no tree)')
    ap.add_argument('--items', type=int, default=30, help='list items printed per list (text mode)')
    ap.add_argument('--ids', action='store_true', help='print each control\'s path id (stable across restarts)')
    a = ap.parse_args(argv)
    global SHOW_PATHS
    SHOW_PATHS = a.ids
    base = sydb_base(a)
    set_backend(a)
    st = capture(base, int(a.tid, 16) if a.tid else None)
    d, tid = st['raw'], st['tid']
    if a.raw:
        print(json.dumps(d, indent=1, ensure_ascii=False))
        return
    if st.get('error'):
        sys.exit('uidump: ' + st['error'])
    roots, nodes, summ = st['roots'], st['nodes'], st['summary']
    if a.json:
        print(json.dumps({'summary': summ, 'tree': tree_json(roots, a.all)}, indent=1, ensure_ascii=False))
        return
    print('# %s uid %s (thread %#x), %s; %d controls; via %s%s' % (
        d.get('exe') or d.get('process'), d.get('uid3'), tid or 0, d.get('appui_class'), len(nodes),
        'the in-app probe' if d.get('backend') == 'agent' else 'gdb (%d objects read)' % d.get('objects_seen', 0),
        ' (probe: %s)' % d['agent_error'] if d.get('agent_error') else ''))
    if d.get('truncated'):
        print('# cut: %s' % d['truncated'])
    for k in ('title', 'softkeys', 'focused', 'editor', 'menu', 'screen_text'):
        if k in summ:
            print('# %s: %s' % (k, summ[k]))
    if 'list' in summ:
        print('# list: %s, %s items, current %s = %r' % (summ['list']['class'], summ['list']['count'],
                                                          summ['list']['current'], summ['list']['selected']))
    print_tree(roots, a.all, a.items)


if __name__ == '__main__':
    main()
