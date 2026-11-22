#!/usr/bin/env python3
"""A stand-in modem on the syborg-modem link (u16 LE length + PhoNet message): logs what the phone sends.

  python3 tools/cmt/linkspy.py SOCKET [--log FILE] [--answer-mtc]

Listens on the unix socket SOCKET; start the phone with
  SYDB_QEMU_EXTRA="-chardev socket,id=cmt,path=SOCKET -global syborg-modem.link=cmt"
Prints one line per message: direction, resource, transaction, message id, receiver/sender objects, bytes.
--answer-mtc answers MTC_STATE_QUERY_REQ (15 02) with NORMAL, to see what the phone asks next.
Used to test the link plumbing and to list the boot queries the modem firmware must answer
(docs/research/n95-cmt-emulation.md).
"""
import argparse, os, socket, struct, sys, time


def fmt(d, m):
    if len(m) < 10:
        return f'{d} short {m.hex()}'
    return (f'{d} res={m[3]:02x} tid={m[8]:02x} id={m[9]:02x} robj={m[6]:02x} sobj={m[7]:02x} '
            f'rdev={m[1]:02x} sdev={m[2]:02x} len={len(m)} data={m[10:].hex()}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('socket')
    ap.add_argument('--log')
    ap.add_argument('--answer-mtc', action='store_true')
    a = ap.parse_args()
    if os.path.exists(a.socket):
        os.unlink(a.socket)
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(a.socket)
    srv.listen(1)
    out = open(a.log, 'a', buffering=1) if a.log else sys.stdout
    conn, _ = srv.accept()
    t0 = time.time()
    buf = b''
    while True:
        chunk = conn.recv(65536)
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 2:
            n = struct.unpack_from('<H', buf)[0]
            if len(buf) < 2 + n:
                break
            m, buf = buf[2:2 + n], buf[2 + n:]
            print(f'{time.time() - t0:8.3f} ' + fmt('APE->', m), file=out)
            if a.answer_mtc and len(m) >= 10 and m[3] == 0x15 and m[9] == 0x02:
                r = bytes([0x00, 0x6c, 0x00, 0x15, 8, 0, m[7], 0x00, m[8], 0x65, 0x01, 0x01, 0, 0])
                conn.sendall(struct.pack('<H', len(r)) + r)
                print(f'{time.time() - t0:8.3f} ' + fmt('<-CMT', r), file=out)


if __name__ == '__main__':
    main()
