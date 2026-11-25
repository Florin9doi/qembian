#!/usr/bin/env python3
"""Boot check for the ngage board's phones: the ROM still boots to its first UI screen, no fault.

Run after every change to the ngage board (src/qemu-ngage/): both phones share it.
  --model ngage (default): N-Gage QD; reference = the time query on the idle wallpaper.
  --model nokia7650: Nokia 7650; reference = the first-boot time query.
  --model nokia6600: Nokia 6600; reference = the first-boot time query. It also covers the 6600
    bring-up stalls (docs/devices/nokia-6600-nhl10/bring-up.md), e.g. regression isa_frame_heap
    (SelfTestServer faulted on a frame queued from another thread's heap): any fatal/abort catch
    fails the check.
Boots a throw-away session (agent-<model>chk) on a copy of a kept flash file, so C: needs no
format, takes a screenshot every 2 s and passes when the screen matches the model's reference
closely enough. References are firmware on screen: build/evidence/<model>/boot-ref-timequery.png
(git-ignored); --save-ref writes it from this run.

  python3 tests/ngage-boot-check.py [--model nokia6600] [--flash FILE] [--secs 90] [--save-ref]
  --joystick (6600/7650): after the time query, OK twice (time, date), Menu, joystick right: the
    screen must change. Regression "joystick_dead" (owner, 2026-11-18: no joystick navigation).
  --idle (6600/7650): the restart path (a kept flash): the idle screen must come up painted (its
    middle, the wallpaper, has many colours; the broken case leaves it white). A start-up time query,
    if shown (fresh flash, or a board without H-nhl10-rtc), is answered with OK twice. Then the menu
    must open.
    Regression "locked_after_restart" (owner, 2026-11-18: the 6600 hung on its home screen when not
    --fresh: "Locked" in the title, the idle area unpainted). Cause: the view server killed Phone.app
    (ViewSrv 11) while the start-up time query, shown on every boot because the clock is never
    valid, stayed up; answering it at once avoids it, which is what this check does.
  --camera (7650): after the time query, OK twice, Camera (left softkey): the viewfinder must show the board's
    test pattern (ngage_cam.c: eight colour bars), the slide closed (qom-set slide-closed) must stop it ("Open
    cover") and opening it must bring it back. Regressions "viewfinder_colours_wrong" (the viewfinder is 0x0RGB
    4444, not YUV: white came out cyan) and "camera_open_cover" (the slide line read closed: "Open cover" for ever).
"""
import argparse, pathlib, shutil, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[1]
FLASH = {'ngage': ROOT / 'build/ngage/phones/test.flash',
         'nokia6600': ROOT / 'build/ngage/phones/nhl10-agent.flash',
         'nokia7650': ROOT / 'build/ngage/phones/nhl2-agent.flash'}
EVIDENCE = {'ngage': 'ngage', 'nokia6600': 'nhl10', 'nokia7650': 'nhl2'}


def load(p):
    from PIL import Image
    return Image.open(p).convert('RGB')


def diff(a, b):
    """Mean absolute difference per channel, 0..255."""
    from PIL import ImageChops, ImageStat
    if a.size != b.size:
        return 255.0
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3


def key(sess, code, hold):
    """one press through QMP, held 'hold' seconds (ekeyb debounces short presses)"""
    sys.path.insert(0, str(ROOT / 'eka1/tools'))
    import ngage_sess as ns
    q = ns.Qmp(sess[-1])
    for down in (True, False):
        q.cmd('input-send-event', events=[{'type': 'key', 'data': {
            'down': down, 'key': {'type': 'qcode', 'data': code}}}])
        time.sleep(hold)


def colours(p, box=None):
    from PIL import Image
    im = Image.open(p).convert('RGB')
    if box:
        im = im.crop(box)
    return len(im.getcolors(1 << 20) or [])


IDLE_BOX = (0, 60, 176, 180)        # the idle screen's wallpaper area (176x208 panel)


def restart_idle(sess, tmp, ref, secs):
    """Wait for a painted idle screen; answer a time query (OK twice) if one shows. Returns the
    wallpaper-area colour count of the last shot and how much the screen changed with Menu."""
    p, end, n = tmp / 'idle.png', time.time() + secs, -1
    while time.time() < end:
        time.sleep(3)
        subprocess.run(sess + ['shot', str(p)], capture_output=True)
        if not p.exists():
            continue
        if ref.exists() and diff(load(p), load(ref)) <= 6.0:
            key(sess, 'f1', 1.0)
            key(sess, 'f1', 1.0)
            continue
        n = colours(p, IDLE_BOX)
        if n > 100:
            break
    key(sess, 'f5', 0.4)
    time.sleep(4)
    m = tmp / 'menu.png'
    subprocess.run(sess + ['shot', str(m)], capture_output=True)
    return n, diff(load(p), load(m)) if m.exists() else -1.0


def to_idle(sess, tmp):
    """OK (time), OK (date); then wait until two shots 3 s apart agree. Returns the idle shot."""
    key(sess, 'f1', 1.0)
    key(sess, 'f1', 1.0)
    # the idle screen comes up (the 6600 takes longer)
    prev, end, p = None, time.time() + 90, tmp / 'idle.png'
    while time.time() < end:
        time.sleep(3)
        subprocess.run(sess + ['shot', str(p)], capture_output=True)
        img = load(p)
        if prev is not None and diff(prev, img) < 0.5:
            break
        prev = img
    return p


def joystick(sess, tmp, idle_done=False):
    """OK (time), OK (date), Menu (short: a long press is the task switcher), right"""
    if not idle_done:
        to_idle(sess, tmp)
    key(sess, 'f5', 0.4)
    time.sleep(3)
    a, b = tmp / 'menu-a.png', tmp / 'menu-b.png'
    subprocess.run(sess + ['shot', str(a)], capture_output=True)
    key(sess, 'right', 0.4)
    time.sleep(2)
    subprocess.run(sess + ['shot', str(b)], capture_output=True)
    return diff(load(a), load(b))


VF_BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
           (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def bars_seen(p):
    """how many of the viewfinder's eight bars (160x120 at x 8, y 52; row 100) have the right colour"""
    im = load(p)
    return sum(max(abs(c - e) for c, e in zip(im.getpixel((18 + 20 * i, 100)), want)) <= 40
               for i, want in enumerate(VF_BARS))


def slide(sess, closed):
    sys.path.insert(0, str(ROOT / 'eka1/tools'))
    import ngage_sess as ns
    ns.Qmp(sess[-1]).cmd('qom-set', path='/machine', property='slide-closed', value=closed)


def camera(sess, tmp):
    """Camera from the idle screen; returns the right viewfinder bars: open, with the slide closed, reopened"""
    to_idle(sess, tmp)
    key(sess, 'f1', 0.6)
    res = []
    for step in ('open', 'closed', 'reopened'):
        if step == 'closed':
            slide(sess, True)
        elif step == 'reopened':
            slide(sess, False)
        n, end = 0, time.time() + 25
        while time.time() < end:
            time.sleep(2)
            p = tmp / ('camera-%s.png' % step)
            subprocess.run(sess + ['shot', str(p)], capture_output=True)
            n = bars_seen(p)
            if (n == 8) == (step != 'closed'):
                break
        res.append(n)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--model', default='ngage', choices=sorted(FLASH))
    ap.add_argument('--flash', help="a kept flash file to copy (default: the model's test phone)")
    ap.add_argument('--secs', type=float, default=90)
    ap.add_argument('--limit', type=float, default=6.0, help='mean difference that counts as a match')
    ap.add_argument('--save-ref', action='store_true')
    ap.add_argument('--joystick', action='store_true', help='also check joystick navigation in the menu')
    ap.add_argument('--idle', action='store_true', help='also check that the idle screen comes up drawn')
    ap.add_argument('--camera', action='store_true', help='7650: viewfinder test pattern and the slide')
    a = ap.parse_args()
    joy = 0.0
    SESS = [sys.executable, str(ROOT / 'eka1/tools/ngage_sess.py'), '-s',
            'agent-ngchk' if a.model == 'ngage' else 'agent-%schk' % a.model]
    REF = ROOT / 'build/evidence' / EVIDENCE[a.model] / 'boot-ref-timequery.png'
    a.flash = a.flash or str(FLASH[a.model])
    if a.idle:
        return idle_main(a, SESS, REF)
    if not a.save_ref and not REF.exists():
        sys.exit('no reference %s: run once with --save-ref on a known-good board' % REF)
    (ROOT / 'build/ngage').mkdir(parents=True, exist_ok=True)
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ngchk-', dir=ROOT / 'build/ngage'))
    fl = tmp / 'phone.flash'
    if pathlib.Path(a.flash).exists():
        shutil.copy(a.flash, fl)
    subprocess.run(SESS + ['stop'], capture_output=True)
    subprocess.run(SESS + ['start', '--model', a.model, '--flash', str(fl)], check=True,
                   capture_output=True)
    best, shot = 255.0, tmp / 'shot.png'
    try:
        end = time.time() + a.secs
        frozen = None
        while time.time() < end:
            time.sleep(2)
            st = subprocess.run(SESS + ['status'], capture_output=True, text=True).stdout
            if '"frozen"' in st:
                frozen = st
                break
            subprocess.run(SESS + ['shot', str(shot)], capture_output=True)
            if not shot.exists():
                continue
            if a.save_ref:
                continue
            best = min(best, diff(load(shot), load(REF)))
            if best <= a.limit:
                break
        if a.save_ref:
            REF.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(shot, REF)
            print('reference saved:', REF)
            return 0
        if a.joystick and best <= a.limit and not frozen:
            joy = joystick(SESS, tmp)
        cam = None
        if a.camera and best <= a.limit and not frozen:
            cam = camera(SESS, tmp)
    finally:
        subprocess.run(SESS + ['stop'], capture_output=True)
    if frozen:
        print('boot check (%s): FAIL, a catch froze the machine:\n%s' % (a.model, frozen))
        return 1
    ok = best <= a.limit
    if a.camera:
        cok = cam is not None and cam[0] == 8 and cam[1] < 4 and cam[2] == 8
        print('%s camera check: %s (viewfinder bars right: %s of 8 open, %s closed, %s reopened; shots in %s)'
              % (a.model, 'PASS' if cok else 'FAIL', *(cam or ('-', '-', '-')), tmp))
        ok = ok and cok
    if a.joystick:
        print('%s joystick check: %s (menu before/after right differ by %.1f)'
              % (a.model, 'PASS' if joy > 1.0 else 'FAIL', joy))
        ok = ok and joy > 1.0
    print('%s boot check: %s (best mean difference %.1f, limit %.1f; last shot %s)'
          % (a.model, 'PASS' if ok else 'FAIL', best, a.limit, shot))
    return 0 if ok else 1


def idle_main(a, SESS, REF):
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='ngchk-', dir=ROOT / 'build/ngage'))
    fl = tmp / 'phone.flash'
    if pathlib.Path(a.flash).exists():
        shutil.copy(a.flash, fl)
    subprocess.run(SESS + ['stop'], capture_output=True)
    subprocess.run(SESS + ['start', '--model', a.model, '--flash', str(fl)], check=True,
                   capture_output=True)
    try:
        n, m = restart_idle(SESS, tmp, REF, a.secs)
        st = subprocess.run(SESS + ['status'], capture_output=True, text=True).stdout
    finally:
        subprocess.run(SESS + ['stop'], capture_output=True)
    ok = n > 100 and m > 5.0 and '"frozen"' not in st
    print('%s idle check: %s (wallpaper area: %d colours, need > 100; Menu changed the screen by %.1f, '
          'need > 5; shots in %s)' % (a.model, 'PASS' if ok else 'FAIL', n, m, tmp))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
