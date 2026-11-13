#!/usr/bin/env python3
"""Install the user's Symbian 9.2 installserver mod into an offline C: FAT image.

No ROM rebuild, SIS installation, quarantine exploit or guest date change needed.
"""
import argparse
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parent.parent
SHA256 = '280b68c451c0c9c5c65e84fcaef78a6802f2c59b56fcbe95f00635c1ce807cf0'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive', type=Path, default=Path.home() / 'Downloads/Symbian Hack.zip')
    ap.add_argument('--drive', type=Path, help="the C: image (default: the N95's primary instance's, tools/instance.py)")
    ap.add_argument('--instance', help='the instance whose C: it goes into')
    args = ap.parse_args()
    if not args.drive:
        import sys
        sys.path.insert(0, str(ROOT / 'tools'))
        import instance as instmod
        inst = instmod.read(args.instance) if args.instance else instmod.primary_for('nokia-n95-8gb-rm320')
        if not inst:
            ap.error('no N95 instance: --instance NAME or --drive IMG')
        args.drive = Path(inst['_dir']) / 'drives' / 'nvmemory.img'
    drive = args.drive.resolve(strict=True)
    for tool in ('mdir', 'mmd', 'mcopy', 'cp'):
        if not shutil.which(tool):
            ap.error(f'{tool} is required')
    # Run with host process visibility: sandboxed /proc can hide interactive QEMU.
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            if 'qemu' not in (proc / 'comm').read_text():
                continue
            cmd = (proc / 'cmdline').read_bytes().split(b'\0')
            for arg in cmd:
                for part in os.fsdecode(arg).split(','):
                    if part.startswith('file='):
                        path = Path(part[5:])
                        if not path.is_absolute():
                            path = (proc / 'cwd').resolve() / path
                        if path.resolve() == drive:
                            ap.error(f'{drive} is in use by QEMU PID {proc.name}; close it first')
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            ap.error(f'cannot inspect {proc}; run with host process visibility')
    with zipfile.ZipFile(args.archive) as outer:
        names = [n for n in outer.namelist() if n.endswith('/installserver92.zip')]
        if len(names) != 1:
            ap.error('expected exactly one nested installserver92.zip')
        with zipfile.ZipFile(io.BytesIO(outer.read(names[0]))) as inner:
            exe = inner.read('installserver.exe')
    if hashlib.sha256(exe).hexdigest() != SHA256:
        ap.error('unexpected installserver92.exe hash; inspect this archive version first')
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')

    def mtool(name, *params, check=True):
        return subprocess.run([name, '-i', str(drive), *params], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=check)

    mtool('mdir', '::/')  # reject unformatted/wrong images before taking a backup
    backup = drive.with_name(drive.name + '.before-installserver-' + time.strftime('%Y%m%d-%H%M%S'))
    if backup.exists():
        ap.error(f'backup already exists: {backup}')
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(drive), str(backup)], check=True)
    print(f'Backup: {backup}', flush=True)
    for directory in ('::/sys', '::/sys/bin'):
        if mtool('mdir', directory, check=False).returncode:
            mtool('mmd', directory)
    with tempfile.TemporaryDirectory(prefix='n95-installserver-') as tmp:
        source = Path(tmp) / 'installserver.exe'
        source.write_bytes(exe)
        mtool('mcopy', '-o', str(source), '::/sys/bin/installserver.exe')
        result = Path(tmp) / 'readback.exe'
        mtool('mcopy', '::/sys/bin/installserver.exe', str(result))
        if result.read_bytes() != exe:
            raise SystemExit(f'readback failed; restore {backup} before booting')
    print(f'Installed and verified C:\\sys\\bin\\installserver.exe ({len(exe)} bytes)')
    print(f'SHA-256: {SHA256}')


if __name__ == '__main__':
    main()
