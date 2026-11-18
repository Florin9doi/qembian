#!/usr/bin/env python3
"""Nokia 9500 Communicator (RA-2) flash files -> their images (untrusted input: run with python3 -I).

  ra2_extract.py FILE... -o OUT        (RA2n6270.c01 .l18 .u18)

Format [V on RA-002 v.06.27, 2026-11-18; docs/devices/nokia-9500-ra2/bring-up.md]:
  0xA2, u32 BE header length, header (4 zero bytes here), then records until the end of the file:
    +0  0x28
    +1  4 bytes, 0 in every record seen
    +5  2 bytes [I: checksum or id]
    +7  1 byte  [I: 01 data, 02/04 descriptor]
    +8  u32 BE  data length (0 for descriptors)
    +12 3 bytes 0
    +15 1 byte  [I: header checksum]
    +16 data
  A data block is either raw (a ROFS / ROFx image, a FAT image with OEM name "EPOC") or starts with a
  sub-header: magic 56 34 12 99 (u32 LE 0x99123456), then u32 LE fields
    +4 [I crc], +8 [I crc], +12 uncompressed size, +16 compression (1 = raw deflate, 0 = stored),
    +20 [I crc], +24 stored size, +28 load address, +32 load address again,
    +36 a NUL-terminated tag ("001C24070606.27(00)": its 4th character names the image: C core, T,
    L loader, D, S, V variant, E, U), then the data (raw deflate when compressed).
  A record with magic 58 34 12 99 carries text ("001T CRC Disabled"...).
Every record is written as rNN-<offset>.bin; compressed ones also as rNN-<tag>.img (inflated), and a
manifest.json lists them. Nothing is executed or interpreted beyond this.
"""
import argparse, json, os, struct, sys, zlib

SUB = 0x99123456
TEXT = 0x99123458


class FormatError(ValueError):
    pass


def records(data):
    """[(offset, header bytes, payload)] of one flash file"""
    if len(data) < 5 or data[0] != 0xA2:
        raise FormatError('not a Phoenix A2 container')
    hl = struct.unpack_from('>I', data, 1)[0]
    p = 5 + hl
    out = []
    while p < len(data):
        if p + 16 > len(data):
            raise FormatError('truncated record header at 0x%x' % p)
        h = data[p:p + 16]
        if h[0] != 0x28:
            raise FormatError('record type 0x%02x at 0x%x (only 0x28 known)' % (h[0], p))
        ln = struct.unpack_from('>I', h, 8)[0]
        if p + 16 + ln > len(data):
            raise FormatError('record at 0x%x: length 0x%x runs past the end' % (p, ln))
        out.append((p, h, data[p + 16:p + 16 + ln]))
        p += 16 + ln
    return out


def sub_header(payload):
    """the 0x99123456 sub-header as a dict, or None"""
    if len(payload) < 40 or struct.unpack_from('<I', payload, 0)[0] != SUB:
        return None
    f = struct.unpack_from('<8I', payload, 4)
    end = payload.find(b'\0', 36)
    if end < 0:
        raise FormatError('sub-header tag not terminated')
    tag = payload[36:end].decode('ascii', 'replace')
    return {'size': f[2], 'compression': f[3], 'stored': f[5], 'load': f[6], 'load2': f[7],
            'tag': tag, 'data_offset': end + 1}


def unpack(payload, sh):
    body = payload[sh['data_offset']:]
    if sh['compression'] == 1:
        z = zlib.decompressobj(-15)
        out = z.decompress(body) + z.flush()
        if not z.eof:
            raise FormatError('deflate stream of %s not finished' % sh['tag'])
    elif sh['compression'] == 0:
        out = body[:sh['size']]
    else:
        raise FormatError('compression %d of %s unknown' % (sh['compression'], sh['tag']))
    if len(out) != sh['size']:
        raise FormatError('%s: %d bytes unpacked, header says %d' % (sh['tag'], len(out), sh['size']))
    return out


def kind(payload):
    if payload[:4] in (b'ROFS', b'ROFx'):
        return payload[:4].decode()
    if payload[3:7] == b'EPOC' or payload[3:11] == b'EPOC\0\0\0\0':
        return 'FAT'
    if len(payload) >= 4 and struct.unpack_from('<I', payload, 0)[0] == TEXT:
        return 'text'
    return 'raw'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args(argv)
    man = []
    for f in a.files:
        data = open(f, 'rb').read()
        name = os.path.basename(f)
        od = os.path.join(a.out, name)
        os.makedirs(od, exist_ok=True)
        for i, (off, h, payload) in enumerate(records(data)):
            e = {'file': name, 'index': i, 'offset': off, 'header': h.hex(), 'length': len(payload)}
            if payload:
                open(os.path.join(od, 'r%02d-%07x.bin' % (i, off)), 'wb').write(payload)
            sh = sub_header(payload)
            if sh:
                e.update(sh)
                img = unpack(payload, sh)
                tagname = ''.join(c if c.isalnum() else '_' for c in sh['tag'])
                fn = 'r%02d-%s.img' % (i, tagname)
                open(os.path.join(od, fn), 'wb').write(img)
                e['image'] = fn
            elif payload:
                e['kind'] = kind(payload)
            man.append(e)
            print('%-14s r%02d @%07x len %8x %s' % (name, i, off, len(payload),
                  ('%s %s -> %x at %08x' % (sh['tag'], 'deflate' if sh['compression'] else 'stored',
                   sh['size'], sh['load'])) if sh else e.get('kind', 'descriptor')))
    json.dump(man, open(os.path.join(a.out, 'manifest.json'), 'w'), indent=1)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except FormatError as e:
        sys.exit('error: %s' % e)
