/*
 * The OpenVG matrix operations of the host's reference implementation (Khronos RI riApi.cpp / riMath.h,
 * sfopenvg), redone bit for bit for the guest's copy of a context's matrices (vgshadow.cpp): only + and * in the
 * RI's order (IEEE single precision gives the same bits on the host and the guest; build with
 * -ffp-contract=off). vgRotate (sin/cos: two libms) is not here: it makes the copy unknown.
 * Rows as in the RI's Matrix3x3: d[row][col]; an OpenVG matrix m[9] is column-major.
 *
 * KEEP IN SYNC with the host. Why this is duplicated: vgGetMatrix was a host round trip (~20 a frame in the
 * 808's alfredserver, each a blocked wait under emulation); the guest answers it from this copy instead. The
 * host's code stays the reference: the RI's vgLoadIdentity/vgLoadMatrix/vgMultMatrix/vgTranslate/vgScale/vgShear,
 * inputFloat and Matrix3x3::operator* (riApi.cpp, riMath.h in
 * external/oss.FCL.sf.os.graphics/openvg/openvgrefimplementation/sfopenvg/sfopenvg; marked by
 * src/syvg/patches/0015-matrix-keep-in-sync.patch). A change there must be made here too, and the other way
 * round; tests/vgshadow_test.py compares both bit for bit.
 * Original work for qembian. SPDX-License-Identifier: MIT
 */
#ifndef VGSHADOW_MATH_H
#define VGSHADOW_MATH_H

typedef struct { float d[3][3]; } VgsMatrix;

/* RI inputFloat: NaN -> 0, +-inf (and beyond) -> +-the largest float */
static inline float vgsInput(float f)
{
	union { float f; unsigned int i; } p;
	p.f = f;
	if (((p.i >> 23) & 0xff) == 255 && (p.i & 0x7fffff))
		return 0.0f;
	union { unsigned int i; float f; } mx;
	mx.i = 0x7f7fffffu;
	return f < -mx.f ? -mx.f : f > mx.f ? mx.f : f;
}

static inline void vgsSet(VgsMatrix *a, float m00, float m01, float m02, float m10, float m11, float m12,
                          float m20, float m21, float m22)
{
	a->d[0][0] = m00; a->d[0][1] = m01; a->d[0][2] = m02;
	a->d[1][0] = m10; a->d[1][1] = m11; a->d[1][2] = m12;
	a->d[2][0] = m20; a->d[2][1] = m21; a->d[2][2] = m22;
}

static inline void vgsIdentity(VgsMatrix *a)
{
	vgsSet(a, 1, 0, 0, 0, 1, 0, 0, 0, 1);
}

static inline void vgsAffine(VgsMatrix *a)
{
	a->d[2][0] = 0; a->d[2][1] = 0; a->d[2][2] = 1;
}

/* a = a * b (RI operator*: t[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j], left to right) */
static inline void vgsMul(VgsMatrix *a, const VgsMatrix *b)
{
	VgsMatrix t;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			t.d[i][j] = a->d[i][0] * b->d[0][j] + a->d[i][1] * b->d[1][j] + a->d[i][2] * b->d[2][j];
	*a = t;
}

/* an OpenVG m[9] (column-major) as the RI reads it in vgLoadMatrix / vgMultMatrix */
static inline void vgsFromVg(VgsMatrix *a, const float *m)
{
	vgsSet(a, vgsInput(m[0]), vgsInput(m[3]), vgsInput(m[6]),
	          vgsInput(m[1]), vgsInput(m[4]), vgsInput(m[7]),
	          vgsInput(m[2]), vgsInput(m[5]), vgsInput(m[8]));
}

/* vgGetMatrix's output */
static inline void vgsToVg(const VgsMatrix *a, float *m)
{
	m[0] = a->d[0][0]; m[1] = a->d[1][0]; m[2] = a->d[2][0];
	m[3] = a->d[0][1]; m[4] = a->d[1][1]; m[5] = a->d[2][1];
	m[6] = a->d[0][2]; m[7] = a->d[1][2]; m[8] = a->d[2][2];
}

/* the RI's operations on the current matrix; aAffine: the mode is not VG_MATRIX_IMAGE_USER_TO_SURFACE */
static inline void vgsLoad(VgsMatrix *d, const float *m, int aAffine)
{
	vgsFromVg(d, m);
	if (aAffine)
		vgsAffine(d);
}

static inline void vgsMult(VgsMatrix *d, const float *m, int aAffine)
{
	VgsMatrix n;
	vgsFromVg(&n, m);
	if (aAffine)
		vgsAffine(&n);
	vgsMul(d, &n);
	if (aAffine)
		vgsAffine(d);
}

static inline void vgsTranslate(VgsMatrix *d, float tx, float ty, int aAffine)
{
	VgsMatrix n;
	vgsSet(&n, 1, 0, vgsInput(tx), 0, 1, vgsInput(ty), 0, 0, 1);
	vgsMul(d, &n);
	if (aAffine)
		vgsAffine(d);
}

static inline void vgsScale(VgsMatrix *d, float sx, float sy, int aAffine)
{
	VgsMatrix n;
	vgsSet(&n, vgsInput(sx), 0, 0, 0, vgsInput(sy), 0, 0, 0, 1);
	vgsMul(d, &n);
	if (aAffine)
		vgsAffine(d);
}

static inline void vgsShear(VgsMatrix *d, float shx, float shy, int aAffine)
{
	VgsMatrix n;
	vgsSet(&n, 1, vgsInput(shx), 0, vgsInput(shy), 1, 0, 0, 0, 1);
	vgsMul(d, &n);
	if (aAffine)
		vgsAffine(d);
}

#endif
