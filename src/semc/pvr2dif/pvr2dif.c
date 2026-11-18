/* PVR2DIF.dll replacement for the Sony Ericsson P1i (H-p1i-pvr2dif-stub, docs/hacks.md): the answers of the
 * generic EPL screen driver (screendriver/sgeneric accelbitmap.cpp, AccelHard.cpp): no hardware bitmaps, no
 * graphics accelerator. The real PVR2DIF goes PVR2D -> services_um -> the PowerVR kernel driver, which is
 * not in our kernel; its only user, SE's ScDv.dll, then draws in software, as on a reference board
 * (docs/research/p1i-pvr2d.md). Bug 2026-11-17: the window server panicked USER 47 at its first bridge call.
 * Exports by ordinal (pvr2dif.def): 1 Create, 2 Destroy, 3 GetInfo, 4 SetAsScreenReference,
 * 10 GenericCapabilities, 11 accelerator NewL; 5-9 and 12-15 are imported by nobody. No imports. */
#define EXPORT __attribute__((visibility("default")))
#define KErrNotSupported (-5)
#define KErrBadHandle    (-8)

typedef struct { int iHandle; } RHardwareBitmap;

EXPORT int pvr2dif_create(RHardwareBitmap *self, int mode, int w, int h)
	{ (void)self; (void)mode; (void)w; (void)h; return KErrNotSupported; }
EXPORT void pvr2dif_destroy(RHardwareBitmap *self) { (void)self; }
EXPORT int pvr2dif_getinfo(const RHardwareBitmap *self, void *info)
	{ (void)info; return self->iHandle ? KErrNotSupported : KErrBadHandle; }
EXPORT int pvr2dif_set_as_screen_reference(RHardwareBitmap *self, int screen)
	{ (void)self; (void)screen; return KErrNotSupported; }
EXPORT const void *pvr2dif_generic_capabilities(void) { return 0; }
EXPORT void *pvr2dif_accelerator_newl(int handle) { (void)handle; return 0; }
int pvr2dif_unused(void) { return KErrNotSupported; }
int pvr2dif_dll_body(int reason) { (void)reason; return 0; }
EXPORT int pvr2dif_unused5(void) { return pvr2dif_unused(); }
EXPORT int pvr2dif_unused6(void) { return pvr2dif_unused(); }
EXPORT int pvr2dif_unused7(void) { return pvr2dif_unused(); }
EXPORT int pvr2dif_unused8(void) { return pvr2dif_unused(); }
EXPORT int pvr2dif_unused9(void) { return pvr2dif_unused(); }
EXPORT int pvr2dif_unused12(void) { return pvr2dif_unused(); }
EXPORT int pvr2dif_unused13(void) { return pvr2dif_unused(); }
EXPORT int pvr2dif_unused14(void) { return pvr2dif_unused(); }
EXPORT int pvr2dif_unused15(void) { return pvr2dif_unused(); }
