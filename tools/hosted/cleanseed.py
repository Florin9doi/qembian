#!/usr/bin/env python3
"""A clean copy of a device's drive seed for the hosted kernel: every SQLite write-ahead log (*-wal) on the
FAT drive images is checkpointed into its database and removed, as a clean shutdown leaves a phone.

    python3 tools/hosted/cleanseed.py SEED_DIR OUT_DIR

Why (docs/hacks.md, Hosted EKA2): the seeds are taken from a running QEMU phone, so databases still carry
logs; hosted boots never write their drives back, so every boot replays the same logs, and on the hosted
kernel the 808's SQL server reports such databases corrupt (KSqlErrCorrupt -321: tzserver and wmdrmserver
die, start-up resets the phone) while QEMU replays them fine. Open: why the replay differs. A log without its
database (the seed's tz user data) is deleted: the server creates the database again.
Needs mtools (mdir, mcopy, mdel) and Python's sqlite3. Images are copied sparse; the seed is not touched.
"""
import os, shutil, sqlite3, subprocess, sys, tempfile


def run(*a):
    return subprocess.run(a, capture_output=True, text=True)


def esc(path):
    """an mtools path with its glob characters taken literally ('[' opens a character class)"""
    return path.replace('[', '[[]')


def exists(img, path):
    return run('mdir', '-b', '-i', img, esc(path)).returncode == 0


def wal_files(img):
    r = run('mdir', '-/', '-b', '-a', '-i', img, '::')
    return [l.strip() for l in r.stdout.splitlines() if l.strip().lower().endswith('-wal')]


def clean(img):
    done = []
    for wal in wal_files(img):
        main = wal[:-4]
        tmp = tempfile.mkdtemp()
        db, dbw = os.path.join(tmp, 'x.db'), os.path.join(tmp, 'x.db-wal')
        have_main = exists(img, main)
        if have_main:
            if run('mcopy', '-n', '-i', img, esc(main), db).returncode or \
               run('mcopy', '-n', '-i', img, esc(wal), dbw).returncode:
                sys.exit('cleanseed: could not read %s and its log' % main)
            con = sqlite3.connect(db)
            con.execute('pragma wal_checkpoint(TRUNCATE)')
            con.close()                          # the last connection: the log is checkpointed and deleted
            if run('mcopy', '-o', '-i', img, db, main).returncode:      # a destination is literal
                sys.exit('cleanseed: could not write back %s' % main)
            done.append('%s: checkpointed' % main)
        else:
            done.append('%s: no database, log removed' % main)
        if run('mdel', '-i', img, esc(wal)).returncode:
            sys.exit('cleanseed: could not remove %s' % wal)
        shutil.rmtree(tmp)
    return done


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    seed, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    for name in sorted(os.listdir(seed)):
        src, dst = os.path.join(seed, name), os.path.join(out, name)
        if name.endswith('.img'):
            subprocess.run(['cp', '--sparse=always', src, dst], check=True)
            for line in clean(dst):
                print('%s %s' % (name, line))
        elif os.path.isfile(src):
            shutil.copyfile(src, dst)


if __name__ == '__main__':
    main()
