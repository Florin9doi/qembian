#!/usr/bin/env python3
"""Decode check of the camera model's JPEG encoder (src/qemu-ngage/hw/arm/ngage_cam.c, the 3650/3660 stills):
the file must decode with PIL (libjpeg) at 640x480 and 160x120, show the eight colour bars and the corner square, and
carry libjpeg's standard Huffman tables.
  python3 tests/ngage-cam-jpeg-check.py
"""
import io
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
HELPER = r'''
#include <stdio.h>
#include "ngage_cam.h"
static unsigned char b[400000];
int main(int c, char **v)
{
    unsigned n = ngage_cam_jpeg(b, sizeof b, atoi(v[1]), atoi(v[2]), 0);
    fwrite(b, 1, n, stdout);
    return n ? 0 : 1;
}
'''
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def dhts(d):
    out, i = [], 2
    while i < len(d):
        m, n = d[i + 1], d[i + 2] << 8 | d[i + 3]
        if m == 0xc4:
            out.append(d[i + 4:i + 2 + n])
        if m == 0xda:
            break
        i += 2 + n
    return sorted(out)


def main():
    tmp = pathlib.Path(tempfile.mkdtemp())
    (tmp / 'h.c').write_text('#include <stdlib.h>\n' + HELPER)
    exe = tmp / 'h'
    subprocess.run(['cc', '-O2', '-I', str(ROOT / 'src/qemu-ngage/hw/arm'), '-o', str(exe), str(tmp / 'h.c'),
                    str(ROOT / 'src/qemu-ngage/hw/arm/ngage_cam.c'), '-lm'], check=True)
    std = io.BytesIO()
    Image.new('RGB', (16, 16)).save(std, 'JPEG', quality=75, optimize=False)
    ok = True
    for w, h in ((640, 480), (160, 120)):
        data = subprocess.run([str(exe), str(w), str(h)], capture_output=True, check=True).stdout
        im = Image.open(io.BytesIO(data))
        im.load()
        im = im.convert('RGB')
        bars = [im.getpixel((w * i // 8 + w // 16, h // 2)) for i in range(8)]
        good = im.size == (w, h) and all(max(abs(a - b) for a, b in zip(p, q)) <= 12 for p, q in zip(bars, BARS))
        corner = im.getpixel((h // 8 * 3 // 4, h // 8 * 3 // 4))
        good = good and corner[0] > 200 and corner[1] < 50 and dhts(data) == dhts(std.getvalue())
        print('%dx%d: %d bytes, bars %s, corner %s: %s' % (w, h, len(data), bars, corner, 'ok' if good else 'FAIL'))
        ok = ok and good
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
