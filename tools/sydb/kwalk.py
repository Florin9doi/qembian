#!/usr/bin/env python3
"""Who waits where: every thread of a stopped qembian phone, with the kernel call it is blocked in.

  python3 tools/sydb/kwalk.py [--session DIR | --instance NAME] [--name REGEX] [--all]

Runs gdb-python inside the session's persistent gdb (docs/sydb.md "Debugger"): finds the DThread
objects by their runtime vtable (anchored on TheScheduler.iCurrentThread - offsetof(iNThread), so no
heap range guessing), then for each thread reads NThreadBase::iSavedSP and symbolises the kernel
return addresses on its supervisor stack. Prints one line per thread:

  <id> <name>  wait=<object>   <kernel frames>

`wait=` is NThreadBase::iWaitObj (a DSemaphore/DFastMutex/DThread/0 for message waits); the frames
name the blocking kernel call (`DSemaphore::Wait`, `NKern::WaitForAnyRequest`, `DThread::Die`, ...).
Uses the offsets of this kernel (EPL kernelhwsrv) and the DWARF the session already has loaded.

Why: `tools/syb_report.py` finds the *IPC* request a thread is blocked on; this finds threads blocked
*without* a pending request (semaphores, mutexes, message queues, timers), which the report cannot see
(docs/tooling-ideas.md).
"""
import argparse, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

GDB_CODE = r'''
import gdb
inf = gdb.selected_inferior()
NAME_RE = %(name_re)r
USER = %(user)d

def rd(a, n=4):
    try:
        return int.from_bytes(inf.read_memory(a, n).tobytes(), 'little')
    except Exception:
        return None

def sym(a):
    try:
        s = gdb.execute('info symbol 0x%%x' %% a, to_string=True).strip()
    except Exception:
        return ''
    return s.split(' in section')[0] if ' in section' in s else ''

def tname(base):
    p = rd(base + 20)                       # DObject::iName
    if not p:
        return '?'
    try:
        n = (rd(p) or 0) & 0xfffffff         # HBuf8 length: the name is not NUL-terminated ("Main#", "Standby mode023")
        return inf.read_memory(p + 8, min(n, 80)).tobytes().decode('latin-1')
    except Exception:
        return '?'


# user memory of another process (multiple memory model): walk its page directory, read physical memory
# through QEMU's gdbstub (as uidump.py)
def pword(pa):
    gdb.execute('maint packet Qqemu.PhyMemMode:1', to_string=True)
    try:
        return int.from_bytes(inf.read_memory(pa, 4).tobytes(), 'little')
    except Exception:
        return None
    finally:
        gdb.execute('maint packet Qqemu.PhyMemMode:0', to_string=True)

def uread(pd, va):
    if va >= 0x80000000:
        return rd(va)
    pde = pword(pd + (va >> 20) * 4)
    if pde is None:
        return None
    if pde & 3 == 2:
        return pword((pde & 0xfff00000) | (va & 0xffffc))
    if pde & 3 == 1:
        pte = pword((pde & 0xfffffc00) + ((va >> 12) & 0xff) * 4)
        if pte is None:
            return None
        if pte & 2:
            return pword((pte & 0xfffff000) | (va & 0xffc))
        if pte & 3 == 1:
            return pword((pte & 0xffff0000) | (va & 0xfffc))
    return None

cur = int(gdb.parse_and_eval('(unsigned)TheScheduler.iCurrentThread'))
off = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iNThread')) - 0x10000000
anchor = cur - off
vptr = rd(anchor)                            # runtime "vtable for DThread"
out = []
for chunk in range(0xc8000000, 0xc9000000, 0x40000):
    try:
        mem = inf.read_memory(chunk, 0x40000).tobytes()
    except Exception:
        continue
    for i in range(0, len(mem), 4):
        if int.from_bytes(mem[i:i + 4], 'little') == vptr:
            out.append(chunk + i)
PROC_OFF = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iOwningProcess')) - 0x10000000
WAIT_OFF = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iNThread.iWaitObj')) - 0x10000000
SP_OFF = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iNThread.iSavedSP')) - 0x10000000
SS_OFF = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iSupervisorStack')) - 0x10000000
SSZ_OFF = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iSupervisorStackSize')) - 0x10000000
US_OFF = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iUserStackRunAddress')) - 0x10000000
USZ_OFF = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iUserStackSize')) - 0x10000000
USER_TOP = 20
PD_OFF = int(gdb.parse_and_eval('(unsigned)&((DMemModelProcess*)0x10000000)->iLocalPageDir')) - 0x10000000
ID_OFF = int(gdb.parse_and_eval('(unsigned)&((DThread*)0x10000000)->iId')) - 0x10000000
print('THREADS %%d %%x' %% (len(out), vptr))
for base in sorted(out):
    name = tname(base)
    pname = tname(rd(base + PROC_OFF) or 0) if rd(base + PROC_OFF) else '?'
    name = pname.split('[')[0] + '::' + name    # "Process::Thread", so unnamed server threads can be found
    if NAME_RE and not re.search(NAME_RE, name, re.I):
        continue
    wait = rd(base + WAIT_OFF) or 0
    sp = rd(base + SP_OFF) or 0
    frames = []
    for k in range(0, 200, 4):
        w = rd(sp + k) if sp else None
        if w and 0x80000000 <= w < 0x82000000:
            s = sym(w)
            if s and ('Kern' in s or 'Exec' in s or 'DThread' in s or 'DSemaphore' in s or 'DMutex' in s
                      or 'DMsgQueue' in s or 'DTimer' in s):
                if s not in frames:
                    frames.append(s)
        if len(frames) >= 4:
            break
    print('T %%d %%s wait=%%08x %%s' %% (rd(base + ID_OFF) or -1, name, wait, ' <- '.join(frames)))
    if USER:
        # the user side: the top of the supervisor stack holds the registers saved on the exec call's entry
        # (user r13/r14 and the return address among them); the user stack is scanned from that r13 up
        ss, sz = rd(base + SS_OFF) or 0, rd(base + SSZ_OFF) or 0
        top = [rd(ss + sz - 4 * k) for k in range(USER_TOP, 0, -1)] if ss and sz else []
        print('U top ' + ' '.join('%%08x' %% (w or 0) for w in top))
        us, usz = rd(base + US_OFF) or 0, rd(base + USZ_OFF) or 0
        usp = next((w for w in top if w and us <= w < us + usz), 0)
        codes = []
        a = usp
        pd = rd((rd(base + PROC_OFF) or 0) + PD_OFF) or 0
        while usp and pd and a < us + usz and len(codes) < USER:
            w = uread(pd, a)
            if w and (0x70000000 <= w < 0x7fff0000 or 0x80000000 <= w < 0x90000000) and (w & 1 or w & 3 == 0):
                codes.append('%%08x@%%x' %% (w, a - usp))
            a += 4
        print('U sp=%%08x stack=%%08x+%%x %%s' %% (usp, us, usz, ' '.join(codes)))
'''
GDB_CODE = GDB_CODE.replace('import gdb\n', 'import gdb, re\n', 1)


def main():
    ap = argparse.ArgumentParser(prog='kwalk.py')
    ap.add_argument('--session', default=None, help='session directory (a device instance is one)')
    ap.add_argument('--instance', default=None, help='device instance name (tools/instance.py list)')
    ap.add_argument('--name', default=None, help='only threads whose name matches this regex')
    ap.add_argument('--all', action='store_true', help='include the kernel DFC/idle/housekeeping threads')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--user', type=int, default=0, metavar='N',
                    help='also the user side: the registers on top of the supervisor stack and the first N code '
                         'addresses on the user stack from the saved r13 (symbolise with tools/syb_sym.py)')
    a = ap.parse_args()

    base = [sys.executable, os.path.join(ROOT, 'tools/sydb/sydb.py')]
    if a.instance:
        base += ['--instance', a.instance]
    elif a.session:
        base += ['--session', a.session]
    code = GDB_CODE % {'name_re': a.name or '', 'user': a.user}
    open(os.path.join('/tmp', 'kwalk_code.py'), 'w').write(code)
    r = subprocess.run(base + ['dbg', 'attach'], capture_output=True, text=True)
    subprocess.run(base + ['dbg', 'interrupt'], capture_output=True, text=True)
    r = subprocess.run(base + ['dbg', 'cmd', "python exec(open('/tmp/kwalk_code.py').read())"],
                       capture_output=True, text=True)
    text = (r.stdout or '') + (r.stderr or '')
    subprocess.run(base + ['dbg', 'detach'], capture_output=True, text=True)
    if 'THREADS' not in text:
        sys.exit(text.strip() or 'no output from gdb')

    skip = re.compile(r'^(Null#?|Supervisor|DfcThread|DefragDFC|DShBufThread|TimerThread|PointDFC|'
                      r'NVMem|DriveThread|LoaderThreadK)', re.I)
    rows = []
    last = None                         # the row the following "U" lines belong to (None: thread skipped)
    for line in text.splitlines():
        if line.startswith('U '):
            if last is not None:
                last.setdefault('user', []).append(line[2:])
            continue
        if line.startswith('T '):
            # thread names may contain spaces ("Standby mode"): split on " wait="
            m = re.match(r'T (-?\d+) (.*?) (wait=.*)$', line)
            if not m:
                continue
            tid, name, rest = m.groups()
            wait = rest.split('wait=')[1].split()[0] if 'wait=' in rest else ''
            frames = rest.split('wait=')[1][len(wait):].strip() if 'wait=' in rest else ''
            last = None
            if not a.all and skip.search(name.split('::')[-1]):
                continue
            last = {'id': int(tid), 'name': name, 'wait': wait, 'frames': frames}
            rows.append(last)
    if a.json:
        print(json.dumps(rows, indent=1))
        return
    for row in rows:
        print('%4d  %-30s wait=%s  %s' % (row['id'], row['name'], row['wait'], row['frames']))
        for u in row.get('user', []):
            print('      ' + u)
    print('(%d threads)' % len(rows))


if __name__ == '__main__':
    main()
