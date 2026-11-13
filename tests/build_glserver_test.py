#!/usr/bin/env python3
"""scripts/build-glserver.sh rebuilds a broken output. Regression for 2026-11-13: a link interrupted mid-way left a
0-byte build/hosted/glserver/glserver newer than every source; the script took it for up to date and every
`phone.py --gl` failed with "Permission denied". Also: a non-executable output is rebuilt, a good one is kept."""
import os, shutil, stat, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = tempfile.mkdtemp(prefix='glserver-test-')        # never the real output: a phone may be running it
OUT = os.path.join(DIR, 'glserver')
fails = 0


def check(cond, what):
    global fails
    print('  %s %s' % ('ok' if cond else 'FAIL', what))
    fails += 0 if cond else 1


def build():
    return subprocess.run(['bash', os.path.join(ROOT, 'scripts/build-glserver.sh')], capture_output=True, text=True,
                          env=dict(os.environ, GLSERVER_OUT=DIR))


def good():
    return os.path.isfile(OUT) and os.path.getsize(OUT) > 0 and os.access(OUT, os.X_OK)


def main():
    if build().returncode or not good():
        print('build_glserver_test: SKIP (no working build here)')
        return 0
    try:
        open(OUT, 'w').close()                      # the interrupted link: empty and newer than every source
        os.utime(OUT, (time.time() + 60, time.time() + 60))
        r = build()
        check(r.returncode == 0 and good(), 'an empty glserver newer than its sources is rebuilt')
        os.chmod(OUT, os.stat(OUT).st_mode & ~(stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
        r = build()
        check(r.returncode == 0 and good(), 'a non-executable glserver is rebuilt')
        before = os.stat(OUT).st_mtime_ns
        build()
        check(os.stat(OUT).st_mtime_ns == before, 'a good, current glserver is kept')
        check(not [f for f in os.listdir(os.path.dirname(OUT)) if '.tmp.' in f], 'no temporary link output left')
    finally:
        shutil.rmtree(DIR, ignore_errors=True)
    print('build_glserver_test: %s' % ('PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
