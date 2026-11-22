#!/usr/bin/env python3
"""On-phone test of the in-app UI probe (src/n95apps/uiprobe, docs/ui-probe.md) and `sydb ui`:

  python3 tests/uiprobe_smoke.py [--device ID] [--timeout S] [--keep]
      ~45 s on the N95: a throw-away session build/sydb-uiprobe-smoke-<device> (seeded from build/seeds/<id>
      when there is one, else factory-fresh). Any other device gets the generic checks only (boot, probe
      loads, fault guard, a dump with visible controls, nothing died): the platform sweep of
      docs/ui-automation.md "Platforms".

Boots a seeded N95 with the probe installed and checks one regression per bug found while building it:
  - uiprobe with capability All could not load (CONE lacks TCB): akncapserver died at boot (-46)
  - a leave in CEikStatusPaneBase::ControlL aborted akncapserver (USER-EXEC 3)
  - the fault guard's frame writes were dropped by GCC (UB): the app faulted forever, ViewSrv 11
  - the Menu's grid view sits behind a window whose handle is a bare CCoeControl (heap pass), the
    typeinfo name is odd-aligned, a never-shown menu pane crashed in CountComponentControls
  - BtUi's controls have their CCoeControl base at an offset (KERN-EXEC 3 in CONE)
  - Settings' heap held text that looked like a code pointer (KERN-EXEC 3): now caught by the guard
  - app-built list text (Call settings, Messaging) comes from MdcaPoint, exact and in order
  - text drawn without a member string (idle plugins, the Calculator display) comes from the gc capture;
    pictures are named by Avkon icon file#id (the Calculator's buttons, the idle shortcut grid)
"""
import json, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N95 = 'nokia-n95-8gb-rm320'
ap_dev = sys.argv[sys.argv.index('--device') + 1] if '--device' in sys.argv else N95
TIMEOUT = sys.argv[sys.argv.index('--timeout') + 1] if '--timeout' in sys.argv else '30'
os.environ['SYBORG_DEVICE'] = ap_dev
SESSION = os.path.join(ROOT, 'build', 'sydb-uiprobe-smoke' + ('' if ap_dev == N95 else '-' + ap_dev))
SEED = os.path.join(ROOT, 'build', 'seeds', ap_dev)
SYDB = [sys.executable, os.path.join(ROOT, 'tools/sydb/sydb.py'), '--session', SESSION]
UI = [sys.executable, os.path.join(ROOT, 'tools/sydb/uidump.py'), '--session', SESSION, '--agent']
KNOWN_DEATHS = re.compile(r'adatserver|Camera')     # die without the probe too (emulator issues)
fails = []


def run(cmd, timeout=60):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def check(name, ok, info=''):
    print('%s  %s%s' % ('ok  ' if ok else 'FAIL', name, '' if ok else ': ' + info[-400:]))
    if not ok:
        fails.append(name)


def ui(*args):
    rc, out = run(UI + list(args))
    return json.loads(out) if rc == 0 and out.strip().startswith('{') else {'error': out.strip()}


def summary():
    rc, out = run(UI + ['--json'])
    try:
        return json.loads(out)['summary']
    except ValueError:
        return {'error': out.strip()}


def main():
    for b in ('src/n95apps/uiprobe/build.sh', 'src/n95apps/devagent/build.sh'):
        rc, out = run(['bash', os.path.join(ROOT, b)], timeout=300)
        if rc:
            sys.exit('build failed: %s\n%s' % (b, out[-2000:]))
    run(SYDB + ['stop'])
    start = ['start', '--seed', SEED] if os.path.isdir(SEED) else ['start', '--fresh']
    rc, out = run(SYDB + start + ['--timeout', TIMEOUT], timeout=int(TIMEOUT) + 90)
    check('boots to idle with the probe in every Eikon app', 'phone idle' in out, out)
    rc, log = run(SYDB + ['log', '--grep', 'PlatSec.*UIPROBE|UIPROBE.DLL'])
    check('probe loads', 'PlatSec' not in log and 'UIPROBE.DLL' in log, log)
    run(SYDB + ['keepawake', 'unlocked'])

    rc, out = run(SYDB + ['--json', 'agent', 'ui'])
    lines = [i['l'] for i in json.loads(out).get('items', [])] if out.strip().startswith('{') else []
    check('fault guard recovers a test fault', 'G\trecovered' in lines, out)
    s = summary()
    check('focused app dumps: visible controls with text', bool(s.get('screen_text') or s.get('softkeys')), json.dumps(s))
    if ap_dev != N95:
        finish()
        return

    rc, out = run(SYDB + ['--json', 'threads', 'akncapserver'])
    tid = None
    try:
        tid = next(t['tid'] for t in json.loads(out)['items'] if t['name'].endswith('::akncapserver'))
    except (ValueError, KeyError, StopIteration):
        pass
    rc, out = run(SYDB + ['agent', 'ui', tid[2:] if tid else '0'])
    check('akncapserver dumps (no ControlL leave)', rc == 0 and 'CAknKeyLockControl' in out, out)

    s = summary()
    check('idle: drawn text (no member strings) and icon-named shortcuts',
          'No calendar entries today' in (s.get('screen_text') or []) and (s.get('list') or {}).get('selected') == 'Contacts',
          json.dumps(s))
    run(SYDB + ['launch', 'Calculator'])
    ui('wait', 'Options')
    run(SYDB + ['key', '1', '2'])
    time.sleep(0.5)
    rc, out = run(UI)
    check('Calculator: display drawn, buttons named by icon id', 'drawn: "12"' in out and 'Calcsoft.mif#16414"+"' in out, out)
    run(SYDB + ['key', 'menu'])
    ui('wait', 'Gallery', '--timeout', '10')      # the first open after boot fills the grid late
    s = summary()
    lst = s.get('list') or {}
    check('Menu grid: 12 items, current Gallery, 3 columns',
          lst.get('count') == 12 and lst.get('selected') == 'Gallery' and lst.get('grid') == 3, json.dumps(s))
    run(SYDB + ['key', 'lsk'])
    time.sleep(1)
    s = ui('select', 'Memory details')
    check('Options menu select', (s.get('menu') or {}).get('selected') == 'Memory details', json.dumps(s))
    run(SYDB + ['key', 'rsk'])
    time.sleep(0.5)
    steps = [('select', 'Tools', '--open'), ('wait', 'Settings'), ('select', 'Settings', '--open'), ('wait', 'General'),
             ('select', 'Phone', '--open'), ('wait', 'Call divert'), ('select', 'Call', '--open'),
             ('wait', 'Automatic redial'), ('select', 'Automatic redial')]
    for st in steps:
        r = ui(*st)
        if 'error' in r:
            check('flow step %s' % ' '.join(st), False, r['error'])
            break
    s = summary()
    check('Call settings: app-built rows, exact (14) and selectable',
          (s.get('list') or {}).get('count') == 14 and (s.get('list') or {}).get('selected') == 'Automatic redial | Off',
          json.dumps(s))
    run(SYDB + ['launch', 'Messaging'])
    ui('wait', 'Inbox')
    s = summary()
    check('Messaging list exact', (s.get('list') or {}).get('items', [])[:3] == ['New message', 'Inbox', 'My folders'],
          json.dumps(s))
    run(SYDB + ['launch', 'Bluetooth'])
    ui('wait', "phone's name", '--timeout', '8')
    run(SYDB + ['text', 'Qembian'])
    time.sleep(0.5)
    s = summary()
    check('Bluetooth query: editor text (CCoeControl at a base offset)', s.get('editor') == 'Qembian', json.dumps(s))
    finish()


def finish():
    rc, log = run(SYDB + ['log', '--grep', r'Panic|UIPROBE: fault'])
    bad = [l for l in log.splitlines() if re.search(r'Panic|UIPROBE', l) and not KNOWN_DEATHS.search(l)]
    check('no app died, no fault left to an app', not bad, '\n'.join(bad))
    if '--keep' not in sys.argv:
        run(SYDB + ['stop'])
    print('%d failed' % len(fails) if fails else 'all passed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
