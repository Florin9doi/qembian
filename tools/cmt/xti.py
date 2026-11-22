#!/usr/bin/env python3
"""Decode the modem's trace port (XTI class, 0x401ef000) from the n95-cmt probe's ordered write log.

  python3 tools/cmt/xti.py [build/n95-cmt/probe/access.log.stream]

Input lines (map "stream" directive, syborg_cmtprobe.c): "address size value pc". A message opens with the
byte 0xc7 (0xc3: a plain string, no format) and closes with the next byte 0 that ends its last field. The format string comes first
(NUL-terminated, written 4 bytes at a time), then the arguments in format order: %s = a NUL-terminated
string, %c = one byte, %lf/%f = 8 bytes (a double, high word first: FPA order), every other conversion = 4 bytes (little-endian). NOLO and MCUSW use
the same layout (MCUSW addresses successive words at +8, +0x10, ...; only the order matters here).
Prints "pc  text" per message; undecodable bytes are shown in hex.
"""
import os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(hh|h|ll|l|L|z)?([diouxXcsfFeEgGp%])')


def messages(lines):
    """Yield (pc, bytes) per message."""
    cur, pc = None, None
    for ln in lines:
        f = ln.split()
        if len(f) < 4:
            continue
        size, v = int(f[1]), int(f[2], 16)
        b = v.to_bytes(8, 'little')[:size]
        if size == 1 and 0xc0 <= v <= 0xcf:   # a message start: 0xc7 printf-style, 0xc3 a plain string
            if cur:
                yield pc, bytes(cur)
            cur, pc = bytearray(), f[3]
            plain = v == 0xc3
            if plain:
                cur += b'\x00'                # marks "no format": render() prints the text as is
            continue
        if cur is None:
            continue
        cur += b
    if cur:
        yield pc, bytes(cur)


def render(data):
    if data[:1] == b'\x00':                  # 0xc3 message: one plain string
        return data[1:].split(b'\0')[0].decode('latin1')
    end = data.find(b'\0')
    if end < 0:
        return data.hex()
    fmt = data[:end].decode('latin1')
    p = end + 1
    out = []

    def take(n):
        nonlocal p
        chunk = data[p:p + n]; p += n
        return chunk

    def sub(m):
        nonlocal p
        conv, lm = m.group(2), m.group(1)
        if conv == '%':
            return '%'
        if conv == 's':
            e = data.find(b'\0', p)
            if e < 0:
                s = data[p:]; p = len(data); return s.decode('latin1')
            s = data[p:e]; p = e + 1
            return s.decode('latin1')
        if conv in 'fFeEgG':
            c = take(8)
            # FPA word order (old ARM ABI): the high word first
            return m.group(0).replace('l', '') % struct.unpack('<d', c[4:] + c[:4])[0] if len(c) == 8 else '?'
        if conv == 'c':
            c = take(1)
            return chr(c[0]) if c else '?'
        c = take(4)
        if len(c) < 4:
            return '?'
        val = struct.unpack('<i' if conv in 'di' else '<I', c)[0]
        spec = re.sub(r'(hh|h|ll|l|L|z)', '', m.group(0))
        return spec % val if conv != 'p' else hex(val)

    text = SPEC.sub(sub, fmt)
    rest = data[p:].rstrip(b'\0')
    return text + (f'  [+{rest.hex()}]' if rest else '')


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'n95-cmt', 'probe', 'access.log.stream')
    for pc, data in messages(open(path)):
        print(f'{pc}  {render(data)}')


if __name__ == '__main__':
    main()
