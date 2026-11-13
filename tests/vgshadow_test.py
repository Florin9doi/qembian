#!/usr/bin/env python3
"""The guest's copy of OpenVG matrices (src/khr/vgshadow_math.h) against the host's reference implementation:
random sequences of load / mult / translate / scale / shear in affine and image modes, with NaN, +-inf, -0,
denormals and huge values, compared bit for bit after every step with the RI's own Matrix3x3 (build/syvg/ri,
scripts/build-syvg.sh), done as riApi.cpp does it. Regression for 2026-11-13: vgGetMatrix cost a host round trip
(~20 a frame in the 808's alfredserver); the guest answers it from its copy, which must equal the host's."""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RI = os.path.join(ROOT, 'build/syvg/ri')

PROG = r'''
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <VG/openvg.h>
#include "riMath.h"
#include "vgshadow_math.h"
using namespace OpenVGRI;
static unsigned s = 12345;
static unsigned rnd() { s = s * 1103515245u + 12345u; return s >> 1; }
static float rf()
{
	switch (rnd() % 12) {
	case 0: { union { unsigned i; float f; } u; u.i = 0x7fc00000u | (rnd() & 0xff); return u.f; }  /* NaN */
	case 1: return (rnd() & 1) ? 1.0f / 0.0f : -1.0f / 0.0f;
	case 2: return -0.0f;
	case 3: { union { unsigned i; float f; } u; u.i = rnd() & 0x7fffff; return u.f; }            /* denormal */
	case 4: return (rnd() & 1 ? 1 : -1) * 3.0e38f;
	default: return ((int)(rnd() % 200001) - 100000) / 997.0f;
	}
}
static Matrix3x3 riFrom(const float *m)
{
	return Matrix3x3(inputFloat(m[0]), inputFloat(m[3]), inputFloat(m[6]),
	                 inputFloat(m[1]), inputFloat(m[4]), inputFloat(m[7]),
	                 inputFloat(m[2]), inputFloat(m[5]), inputFloat(m[8]));
}
static int same(const Matrix3x3 &a, const VgsMatrix &b)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			float x = a[i][j], y = b.d[i][j];
			if (memcmp(&x, &y, 4) && !(x != x && y != y))
				return 0;
		}
	return 1;
}
int main()
{
	long steps = 0, fails = 0;
	for (int seq = 0; seq < 20000; seq++) {
		int affine = rnd() % 4 != 0;
		Matrix3x3 d; VgsMatrix g; vgsIdentity(&g);
		for (int k = 0; k < 12; k++) {
			float m[9], x = rf(), y = rf();
			for (int i = 0; i < 9; i++) m[i] = rf();
			switch (rnd() % 6) {
			case 0: d = riFrom(m); if (affine) d[2].set(0, 0, 1); vgsLoad(&g, m, affine); break;
			case 1: { Matrix3x3 n = riFrom(m); if (affine) n[2].set(0, 0, 1); d *= n; if (affine) d[2].set(0, 0, 1);
			          vgsMult(&g, m, affine); break; }
			case 2: { Matrix3x3 n(1, 0, inputFloat(x), 0, 1, inputFloat(y), 0, 0, 1); d *= n; if (affine) d[2].set(0, 0, 1);
			          vgsTranslate(&g, x, y, affine); break; }
			case 3: { Matrix3x3 n(inputFloat(x), 0, 0, 0, inputFloat(y), 0, 0, 0, 1); d *= n; if (affine) d[2].set(0, 0, 1);
			          vgsScale(&g, x, y, affine); break; }
			case 4: { Matrix3x3 n(1, inputFloat(x), 0, inputFloat(y), 1, 0, 0, 0, 1); d *= n; if (affine) d[2].set(0, 0, 1);
			          vgsShear(&g, x, y, affine); break; }
			case 5: d.identity(); vgsIdentity(&g); break;
			}
			steps++;
			if (!same(d, g) && fails++ < 5)
				printf("mismatch at sequence %d step %d\n", seq, k);
			float out[9]; vgsToVg(&g, out);
			if (out[0] != g.d[0][0] && out[0] == out[0]) fails++;
		}
	}
	printf("vgshadow: %ld steps, %ld mismatches\n", steps, fails);
	return fails != 0;
}
'''

# inputFloat lives in riApi.cpp; the test brings the RI's definition (riApi.cpp:187) as it is
INPUT = r'''
namespace OpenVGRI { RIfloat inputFloat(VGfloat f) { if(RI_ISNAN(f)) return 0.0f; return RI_CLAMP(f, -RI_FLOAT_MAX, RI_FLOAT_MAX); } }
'''


def main():
    if not os.path.isdir(RI):
        print('vgshadow_test: SKIP (build/syvg/ri missing: scripts/build-syvg.sh)')
        return 0
    d = tempfile.mkdtemp()
    src = os.path.join(d, 't.cpp')
    open(src, 'w').write(PROG.replace('using namespace OpenVGRI;', INPUT + 'using namespace OpenVGRI;'))
    exe = os.path.join(d, 't')
    inc = ['-I' + RI, '-I' + os.path.join(RI, 'include'), '-I' + os.path.join(ROOT, 'build/syvg/inc'),
           '-I' + os.path.join(ROOT, 'build/syvg/inc/VG'), '-I' + os.path.join(ROOT, 'src/khr'),
           '-I' + os.path.join(ROOT, 'src/syvg')]
    # the host's RI flags (scripts/build-syvg.sh): -O2, float maths as compiled there
    r = subprocess.run(['g++', '-O2', '-std=gnu++11', '-D__OPENVGHEADERS_USE_VG_1_1', '-DNDEBUG', '-include',
                        os.path.join(ROOT, 'src/syvg/syvg_shim.h')] + inc + ['-o', exe, src],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-3000:])
        print('vgshadow_test: FAIL (build)')
        return 1
    r = subprocess.run([exe], capture_output=True, text=True)
    print(r.stdout.strip())
    print('vgshadow_test: %s' % ('PASS' if r.returncode == 0 else 'FAIL'))
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
