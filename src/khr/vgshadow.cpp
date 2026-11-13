/*
 * The guest's copy of OpenVG matrix state: vgGetMatrix answered without a host round trip (2026-11-13: ~20 a frame in
 * the 808's alfredserver, each a blocked wait for the host). The host stays authoritative: every call still goes to
 * it (queued, as before); the copy only follows them. Arithmetic: vgshadow_math.h (KEEP IN SYNC with the host's RI,
 * see there).
 *
 * Per thread, not per EGL context: the thread's copy is forgotten whenever it calls eglMakeCurrent, eglBindAPI,
 * eglReleaseThread or eglDestroyContext (egl2.inl), so EGL's context and API rules need no model here. After that
 * the first vgGetMatrix asks the host for the matrix mode and the matrix (2 round trips) and keeps the answer.
 * Unknown, and asked again from the host: after vgRotate (sin/cos), the mode set through vgSetf/vgSetiv/vgSetfv,
 * and anything with a pointer the host would refuse (NULL, misaligned: the host sets the error, nothing changes).
 *
 * Build with -DKHR_VGSHADOW_CHECK (KHR_VGSHADOW_CHECK=1 src/khr/build.sh): every answer from the copy is also asked
 * from the host and a difference is logged ("VGSHADOW mismatch"), for a verification run.
 * Original work for qembian. SPDX-License-Identifier: MIT
 */
#include <e32std.h>
#include <e32debug.h>
#include <VG/openvg.h>
#include "vgshadow_math.h"
#include "../syvg/vg2_fn.h"

namespace OpenVGRI
{
void OSAcquireMutex(void);
void OSReleaseMutex(void);
}

extern "C" unsigned int khrVgCall(int aFn, const unsigned int *aArgs, int aCount);

namespace
{
const TInt KThreads = 16;
const TInt KModes = 5;              /* VG_MATRIX_PATH_USER_TO_SURFACE (0x1400) .. VG_MATRIX_GLYPH_USER_TO_SURFACE */

struct SShadow {
	TUint iThread;                  /* thread id; 0: free */
	TUint iUse;                     /* last use (eviction) */
	TInt iMode;                     /* 0..4, -1 unknown */
	TBool iValid[KModes];
	VgsMatrix iM[KModes];
};

SShadow gShadows[KThreads];
TUint gTick;

/* the calling thread's copy, made if needed (evicting the least recently used); under the VG mutex */
SShadow *Mine(TBool aMake)
{
	TUint id = (TUint)RThread().Id();
	SShadow *free = NULL, *old = &gShadows[0];
	for (TInt i = 0; i < KThreads; i++) {
		SShadow &s = gShadows[i];
		if (s.iThread == id) {
			s.iUse = ++gTick;
			return &s;
		}
		if (!s.iThread && !free)
			free = &s;
		if (s.iUse < old->iUse)
			old = &s;
	}
	if (!aMake)
		return NULL;
	SShadow *s = free ? free : old;
	s->iThread = id;
	s->iUse = ++gTick;
	s->iMode = -1;
	for (TInt m = 0; m < KModes; m++)
		s->iValid[m] = EFalse;
	return s;
}

inline TBool Aligned(const void *p) { return p && !((TUint)p & 3); }

/* the current matrix, if both the mode and that matrix are known */
VgsMatrix *Current(SShadow *s)
{
	return s && s->iMode >= 0 && s->iValid[s->iMode] ? &s->iM[s->iMode] : NULL;
}

inline int Affine(SShadow *s) { return s->iMode != VG_MATRIX_IMAGE_USER_TO_SURFACE - VG_MATRIX_PATH_USER_TO_SURFACE; }

class TLock
{
public:
	TLock() { OpenVGRI::OSAcquireMutex(); }
	~TLock() { OpenVGRI::OSReleaseMutex(); }
};
}

extern "C" {

void khrVgShadowForget(void)
{
	TLock l;
	SShadow *s = Mine(EFalse);
	if (s)
		s->iThread = 0;
}

void khrVgShadowSeti(VGParamType aType, VGint aValue)
{
	if (aType != VG_MATRIX_MODE)
		return;
	TLock l;
	SShadow *s = Mine(ETrue);
	if (aValue >= VG_MATRIX_PATH_USER_TO_SURFACE && aValue <= VG_MATRIX_GLYPH_USER_TO_SURFACE)
		s->iMode = aValue - VG_MATRIX_PATH_USER_TO_SURFACE;      /* else the host refuses it: unchanged */
}

/* vgSetf / vgSetiv / vgSetfv: their conversions are not modelled; the mode becomes unknown */
void khrVgShadowSetOther(VGParamType aType)
{
	if (aType != VG_MATRIX_MODE)
		return;
	TLock l;
	SShadow *s = Mine(EFalse);
	if (s)
		s->iMode = -1;
}

void khrVgShadowLoadIdentity(void)
{
	TLock l;
	SShadow *s = Mine(EFalse);
	if (s && s->iMode >= 0) {
		vgsIdentity(&s->iM[s->iMode]);
		s->iValid[s->iMode] = ETrue;
	}
}

void khrVgShadowLoadMatrix(const VGfloat *m)
{
	TLock l;
	SShadow *s = Mine(EFalse);
	if (!s || s->iMode < 0)
		return;
	if (!Aligned(m))
		return;                     /* the host refuses it: unchanged */
	vgsLoad(&s->iM[s->iMode], m, Affine(s));
	s->iValid[s->iMode] = ETrue;
}

void khrVgShadowMultMatrix(const VGfloat *m)
{
	TLock l;
	VgsMatrix *d = Current(Mine(EFalse));
	if (d && Aligned(m))
		vgsMult(d, m, Affine(Mine(EFalse)));
}

void khrVgShadowTranslate(VGfloat x, VGfloat y)
{
	TLock l;
	SShadow *s = Mine(EFalse);
	VgsMatrix *d = Current(s);
	if (d)
		vgsTranslate(d, x, y, Affine(s));
}

void khrVgShadowScale(VGfloat x, VGfloat y)
{
	TLock l;
	SShadow *s = Mine(EFalse);
	VgsMatrix *d = Current(s);
	if (d)
		vgsScale(d, x, y, Affine(s));
}

void khrVgShadowShear(VGfloat x, VGfloat y)
{
	TLock l;
	SShadow *s = Mine(EFalse);
	VgsMatrix *d = Current(s);
	if (d)
		vgsShear(d, x, y, Affine(s));
}

void khrVgShadowRotate(void)
{
	TLock l;
	SShadow *s = Mine(EFalse);
	if (s && s->iMode >= 0)
		s->iValid[s->iMode] = EFalse;
}

/* vgGetMatrix: 1 when answered from the copy (m written); else the caller asks the host and calls GotMatrix */
int khrVgShadowGetMatrix(VGfloat *m)
{
	if (!Aligned(m))
		return 0;                   /* the host sets the error */
	TLock l;
	SShadow *s = Mine(ETrue);
	if (s->iMode < 0) {
		/* the mode from the host once (vgGeti(VG_MATRIX_MODE)); the matrix follows from the caller's call */
		unsigned int a[1] = { (unsigned int)VG_MATRIX_MODE };
		VGint mode = (VGint)khrVgCall(VG2_FN_vgGeti, a, 1);
		if (mode >= VG_MATRIX_PATH_USER_TO_SURFACE && mode <= VG_MATRIX_GLYPH_USER_TO_SURFACE)
			s->iMode = mode - VG_MATRIX_PATH_USER_TO_SURFACE;
	}
	VgsMatrix *d = Current(s);
	if (!d)
		return 0;
	vgsToVg(d, m);
#ifdef KHR_VGSHADOW_CHECK
	VGfloat h[9];
	unsigned int a[1] = { (unsigned int)h };
	khrVgCall(VG2_FN_vgGetMatrix, a, 1);
	if (Mem::Compare((const TUint8 *)h, 36, (const TUint8 *)m, 36))
		RDebug::Printf("VGSHADOW mismatch mode %d: guest %08x %08x %08x %08x %08x %08x host %08x %08x %08x %08x "
		               "%08x %08x", s->iMode, ((TUint *)m)[0], ((TUint *)m)[1], ((TUint *)m)[3], ((TUint *)m)[4],
		               ((TUint *)m)[6], ((TUint *)m)[7], ((TUint *)h)[0], ((TUint *)h)[1], ((TUint *)h)[3],
		               ((TUint *)h)[4], ((TUint *)h)[6], ((TUint *)h)[7]);
#endif
	return 1;
}

/* the host's answer to vgGetMatrix: the current matrix is known from now on */
void khrVgShadowGotMatrix(const VGfloat *m)
{
	TLock l;
	SShadow *s = Mine(EFalse);
	if (!s || s->iMode < 0 || !Aligned(m))
		return;
	VgsMatrix *d = &s->iM[s->iMode];
	vgsSet(d, m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]);
	s->iValid[s->iMode] = ETrue;
}

}
