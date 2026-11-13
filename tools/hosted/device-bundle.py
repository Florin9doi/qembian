#!/usr/bin/env python3
"""A hosted EKA2 phone bundle for a real arm64 Linux device (the Pixel 3a under postmarketOS): everything the phone
needs without the test VM, plus the window to show it. The Symbian side runs as a native 32-bit ARM process there,
exactly as in the test VM (docs/hosted-eka2/pixel-3a.md).

    python3 tools/hosted/device-bundle.py [--rom nokia-n95-8gb-rm320] [--out DIR] [--tar FILE.tar]

The bundle (default build/hosted/bundle/<device>/; it contains firmware and drive images: never commit or
publish it):
  run.sh            starts the phone on the device as root (sysctls, /t /rom /seed links, the GL server for phones
                    that compose on the GPU, the phone program); `run.sh view` opens the window (phone.py --attach)
  t/kn_efile        the hosted kernel + board + phone (the htest program, in phone mode), and t/nk_rom.args
  rom/              the composed ROM and what its arguments name (ROFS, the fake modem's reply table)
  seed/             the drives past first boot (SQLite logs checkpointed), sparse
  src/, scripts/    the GL server's sources: built on the device (`run.sh build-gl`, needs gcc, glib, epoxy)
  tools/, models/, platform/   the phone window (tools/hosted/phone.py) and the device profile it reads
Original work for qembian.
"""
import argparse, os, shlex, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools/hosted'))


def copy_sparse(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    subprocess.run(['cp', '--sparse=always', src, dst], check=True)
    subprocess.run(['fallocate', '--dig-holes', dst], check=False)     # zero blocks become holes


def copy_tree(src, dst, ignore=None):
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=ignore, symlinks=True)


RUN_SH = r'''#!/bin/sh
# qembian hosted phone on a real device (docs/hosted-eka2/pixel-3a.md). As root:
#   ./run.sh build-gl    once: build the GL server here (phones that compose on the GPU: GL=1 below)
#   ./run.sh             start the phone (serial log: $RUN/phone.log); Ctrl-C stops it
#   ./run.sh view        the window (another terminal, in the graphical session; touch = mouse)
#   ./run.sh check       what this kernel offers the 32-bit Symbian process
set -e
B=$(cd "$(dirname "$0")" && pwd)
RUN=${RUN:-/tmp/qembian}
DEVICE=@DEVICE@
GL=@GL@
case "$1" in
build-gl)
    cd "$B" && CC=${CC:-cc} bash scripts/build-glserver.sh
    exit ;;
view)
    exec python3 "$B/tools/hosted/phone.py" --rom "$DEVICE" --attach "$RUN/phone.shm" --serial "$RUN/phone.log" ;;
check)
    echo "kernel: $(uname -a)"
    for f in /proc/sys/abi/swp /proc/sys/abi/cp15_barrier /proc/sys/abi/setend; do
        [ -e "$f" ] && echo "$f present" || echo "$f missing (ARMv5/v6 user code needs it; Belle phones may not)"
    done
    zcat /proc/config.gz 2>/dev/null | grep -E '^CONFIG_(COMPAT|SECCOMP_FILTER|ARMV8_DEPRECATED|SWP_EMULATION|CP15_BARRIER_EMULATION|COMPAT_ALIGNMENT_FIXUPS|MEMFD_CREATE|CROSS_MEMORY_ATTACH)=' \
        || echo "no /proc/config.gz: check the kernel config by hand (docs/hosted-eka2/pixel-3a.md)"
    file "$B/t/kn_efile" 2>/dev/null || true
    exit ;;
esac
[ "$(id -u)" = 0 ] || { echo "run.sh: as root (sysctls, /t /rom /seed)"; exit 1; }
echo 1 > /proc/sys/abi/swp 2>/dev/null || true
echo 1 > /proc/sys/abi/cp15_barrier 2>/dev/null || true
# the phone program reads /t/nk_rom.args, /rom/... and /seed/... (the test VM's layout)
for d in t rom seed; do
    if [ -L "/$d" ] || [ ! -e "/$d" ]; then ln -sfn "$B/$d" "/$d"; else echo "run.sh: /$d exists and is not our link"; exit 1; fi
done
mkdir -p "$RUN"
rm -f "$RUN/phone.shm"; truncate -s 4M "$RUN/phone.shm"
# every nanothread on one CPU (the last: on the Pixel 3a a big Cortex-A75 core), helper threads on the others;
# HPIN=ffffffff ./run.sh turns it off (lxhost.cpp)
ENVV="HTEST=1 PHONE=$RUN/phone.shm HPIN=${HPIN:-$(printf %x $(($(nproc) - 1)))}"
if [ "$GL" = 1 ]; then
    [ -x "$B/build/hosted/glserver/glserver" ] || { echo "run.sh: build the GL server first: ./run.sh build-gl"; exit 1; }
    rm -f "$RUN/gl.chan"; truncate -s 64M "$RUN/gl.chan"
    "$B/build/hosted/glserver/glserver" --chan "$RUN/gl.chan" @GLARGS@ > "$RUN/gl.log" 2>&1 &
    GLPID=$!
    trap 'kill $GLPID 2>/dev/null' EXIT
    ENVV="$ENVV GLCHAN=$RUN/gl.chan"
fi
cd "$RUN"
echo "qembian: $DEVICE starting; log $RUN/phone.log; window: $0 view"
env -i $ENVV "$B/t/kn_efile" > "$RUN/phone.log" 2>&1
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', default=os.environ.get('SYBORG_DEVICE', 'nokia-n95-8gb-rm320'))
    ap.add_argument('--out')
    ap.add_argument('--tar', help='also pack the bundle into this tar (sparse)')
    a = ap.parse_args()
    out = a.out or os.path.join(ROOT, 'build/hosted/bundle', a.rom)
    os.makedirs(out, exist_ok=True)
    # 1. the phone program, built for this device (htest's per-device build)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/hosted/htest.py'), '--rom', a.rom, '--build-only'],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('device-bundle: build failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])
    import htest
    exe = os.path.join(htest.OUT, 'dev', a.rom, 't', 'kn_efile')
    if not os.path.exists(exe):
        sys.exit('device-bundle: no %s' % exe)
    share, args = htest.rom_setup(a.rom)
    if not share:
        sys.exit('device-bundle: no composed ROM for %s (tools/device.py build)' % a.rom)
    os.makedirs(os.path.join(out, 't'), exist_ok=True)
    shutil.copy2(exe, os.path.join(out, 't', 'kn_efile'))
    open(os.path.join(out, 't', 'nk_rom.args'), 'w').write(args)
    # 2. what the arguments name under /rom, and the drives
    for line in args.splitlines():
        for w in line.split()[1:]:
            if w.startswith('/rom/'):
                copy_sparse(os.path.join(share, w[5:]), os.path.join(out, 'rom', w[5:]))
            elif w.startswith('/seed/'):
                copy_sparse(os.path.join(htest.SEED_SHARE, w[6:]), os.path.join(out, 'seed', w[6:]))
    # 3. the GL server's sources and the flags the profile gives it (phones that compose on the GPU)
    env = dict(os.environ, SYBORG_DEVICE=a.rom)
    qa = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'qemu-args'], env=env,
                        capture_output=True, text=True).stdout.split()
    gl = any(x.startswith('syborg-gl.identity=') for x in qa)      # the phone has the syborg-gl device
    glargs = []
    for x in qa:
        for prop, opt in (('identity', '--identity'), ('egl-configs', '--egl-configs'),
                          ('gpu-memory-mb', '--gpu-memory-mb'), ('msaa', '--msaa')):
            if x.startswith('syborg-gl.%s=' % prop):
                v = x.split('=', 1)[1]
                if prop in ('identity', 'egl-configs'):         # files: into the bundle
                    copy_sparse(v, os.path.join(out, 'gl', os.path.basename(v)))
                    v = '"$B/gl/%s"' % os.path.basename(v)
                glargs += [opt, v]
    for d in ('src/hosted/glserver', 'src/syvg', 'src/hosted/include/board'):
        copy_tree(os.path.join(ROOT, d), os.path.join(out, d))
    os.makedirs(os.path.join(out, 'src/qemu-syborg/hw/arm'), exist_ok=True)
    hw = os.path.join(ROOT, 'src/qemu-syborg/hw/arm')
    for f in os.listdir(hw):
        if f.startswith('syborg_gl') or f in ('syvg.h', 'vg2_fn.h'):
            shutil.copy2(os.path.join(hw, f), os.path.join(out, 'src/qemu-syborg/hw/arm', f))
    os.makedirs(os.path.join(out, 'scripts'), exist_ok=True)
    for f in ('build-glserver.sh', 'build-syvg.sh'):
        shutil.copy2(os.path.join(ROOT, 'scripts', f), os.path.join(out, 'scripts', f))
    # 4. the window and the device profile it reads (text only)
    ign = shutil.ignore_patterns('__pycache__', '*.pyc', 'ghidra')
    copy_tree(os.path.join(ROOT, 'tools'), os.path.join(out, 'tools'), ignore=ign)
    copy_tree(os.path.join(ROOT, 'platform'), os.path.join(out, 'platform'), ignore=ign)
    copy_tree(os.path.join(ROOT, 'personalities'), os.path.join(out, 'personalities'), ignore=ign)
    model = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'get', 'device.model'], env=env,
                           capture_output=True, text=True).stdout.strip()
    os.makedirs(os.path.join(out, 'models'), exist_ok=True)
    for f in os.listdir(os.path.join(ROOT, 'models')):
        src = os.path.join(ROOT, 'models', f)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(out, 'models', f))
        elif os.path.isdir(src) and (f == model or a.rom.startswith(f)):
            copy_tree(src, os.path.join(out, 'models', f), ignore=ign)
    sh = RUN_SH.replace('@DEVICE@', a.rom).replace('@GL@', '1' if gl else '0') \
               .replace('@GLARGS@', ' '.join(glargs))
    open(os.path.join(out, 'run.sh'), 'w').write(sh)
    os.chmod(os.path.join(out, 'run.sh'), 0o755)
    du = subprocess.run(['du', '-sh', out], capture_output=True, text=True).stdout.split()[0]
    print('device-bundle: %s (%s on disk; GL server %s)' % (out, du, 'needed: run.sh build-gl' if gl else 'not needed'))
    if a.tar:
        subprocess.run(['tar', '--sparse', '-C', os.path.dirname(out), '-cf', a.tar, os.path.basename(out)], check=True)
        print('device-bundle: %s (%s)' % (a.tar, subprocess.run(['du', '-sh', a.tar], capture_output=True,
                                                               text=True).stdout.split()[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
