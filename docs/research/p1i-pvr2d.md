# P1i: the window server's PowerVR 2D path, and the cheapest way around it

Question: WSERV panics USER 47 inside `PVR2DIF.dll` when our `services` stand-in refuses every bridge
call. How to give the P1i (se-p1i R6E30, UIQ 3, Symbian 9.1) a working 2D path? Static reading only.
Tags: [V] verified in the firmware's code, [I] inferred.

## Answer: option (c), a 6-function `PVR2DIF.dll` stand-in

PowerVR is not on the drawing path at all. It only backs the optional *hardware bitmap* and
*graphics accelerator* hooks of the screen driver, which the generic Symbian screen driver answers with
"not supported". Replace `PVR2DIF.dll` with a stub that gives those answers. Leave `ScDv.dll`, BitGdi,
FbsCli, PVR2D and services_um as they are.

### Who uses PVR2DIF

- The only static importer of `PVR2DIF.dll` in the firmware is SE's screen driver `ScDv.dll` (ROFS1,
  146 KB of code, 43 exports = the 9.1 SCDV ABI) [V: import tables of every E32 image in z:\sys\bin].
  No module loads PVR2DIF or PVR2D by name. The only UTF-16 "PVR2DIF" strings are inside PVR2D and
  PVR2DIF themselves, probably panic categories [V].
- ScDv touches PVR2DIF only through six ARM veneers, one per exported function. ScDv has no internal
  calls into PVR2DIF, so it does all its drawing in software and finds the screen through HAL (it imports
  hal.dll) [V: no other references to the thunks or veneers]:

| ScDv export | Function (SCDVU.DEF) | → PVR2DIF ord |
|---|---|---|
| 4 | `RHardwareBitmap::SetAsScreenReference(TInt)` | 4 |
| 5 | `RHardwareBitmap::Create(TDisplayMode, TSize, TUid)` | 1 (the stack argument is copied, then `bl`) |
| 6 | `RHardwareBitmap::Destroy()` | 2 |
| 8 | `CHardwareGraphicsAccelerator::GenericCapabilities()` | 10 |
| 9 | `CHardwareGraphicsAccelerator::NewL(RHardwareBitmap)` | 11 |
| 10 | `RHardwareBitmap::GetInfo(TAcceleratedBitmapInfo&) const` | 3 |

- Callers of these ScDv exports [V: SE BitGdi.dll and FbsCli.dll, names from the SDK .dso]:
  - BitGdi imports ScDv 4, 9, 10. In `CFbsScreenDevice` construction (BitGdi export 55, `0x2a25c` from
    load base) it does `if (hw.SetAsScreenReference(-(screen+1)) == KErrNone) TRAP(iAccel = CHardwareGraphicsAccelerator::NewL(hw))`.
    Any non-zero result skips the accelerator. Export 53 returns the handle that SetAsScreenReference
    wrote (0 when it failed). The other NewL call (export 10, the bitmap device) runs only when
    `CFbsBitmap::HardwareBitmapHandle()` != 0 [V].
  - FbsCli imports ScDv 5, 6, 10 (`CFbsBitmap::CreateHardwareBitmap` and its info and destroy calls).
    Failure is an ordinary return code [I: standard 9.1 code; not traced further].
- So the panic comes from the first call, `SetAsScreenReference` (PVR2DIF ord 4), at the window
  server's screen-device construction [I: the most likely place; PVR2DIF ord 4 calls PVR2D ords 10, 5
  and 11, and PVR2D calls services_um].
- `Resource/powervr.ini` holds GLES settings only (ParameterBufferSize, ForceExternalZBuffer, WindowSystem
  per process). `System/Data/Wsini.ini` has no PowerVR switch. No configuration turns PVR2DIF off [V].

### What the stub must return

These are the answers of the generic EPL screen driver (`screendriver/sgeneric/accelbitmap.cpp`,
`smint/AccelHard.cpp`). The calling convention is ARM EABI. `RHardwareBitmap` is `{ TInt iHandle; }`;
`this` is in r0.

| PVR2DIF ord | Meaning | Arguments | Must return |
|---|---|---|---|
| 1 | `RHardwareBitmap::Create` | r0 this, r1 TDisplayMode, r2/r3 TSize (w,h by value), [sp] TUid | `KErrNotSupported` (-5), `iHandle` left at 0 |
| 2 | `RHardwareBitmap::Destroy` | r0 this | nothing |
| 3 | `RHardwareBitmap::GetInfo` | r0 this (const), r1 `TAcceleratedBitmapInfo&` | `iHandle == 0 ? KErrBadHandle (-8) : KErrNotSupported (-5)` |
| 4 | `RHardwareBitmap::SetAsScreenReference` | r0 this, r1 screen as `-(n+1)` | `KErrNotSupported` (-5); do not write `iHandle` |
| 10 | `CHardwareGraphicsAccelerator::GenericCapabilities` | none (static) | `NULL` |
| 11 | `CHardwareGraphicsAccelerator::NewL(RHardwareBitmap)` | r0 = handle (static) | never reached once ord 4 and ord 1 fail. Generic code leaves `KErrNotSupported`; returning `NULL` is equivalent at both BitGdi call sites (TRAPped, result stored in a zeroed member) [I] |
| 5–9, 12–15 | not imported by anyone | – | return -5 (keep the ordinals so the export count stays 15) |

The image header should copy PVR2DIF's own [V]: uid1 0x10000079, uid2 0, uid3 0, vid 0x7000000f,
caps 0x000ffffe (All -TCB), flags 0x1200002b, 15 exports. With ord 11 returning NULL, the stub imports
nothing. Otherwise it needs only `User::Leave` from euser (9.1 ordinal from the profile's sdk_lib).

### Proposed stand-in

```c
/* src/<somewhere>/pvr2dif_stub.c -> PVR2DIF.dll (H-p1i-pvr2dif-stub). The generic screen driver's answers:
   no hardware bitmaps, no graphics accelerator. ScDv/BitGdi then draw in software as on a reference board. */
#define KErrNotSupported (-5)
#define KErrBadHandle    (-8)
typedef struct { int iHandle; } RHardwareBitmap;

int  ord1_Create(RHardwareBitmap *self, int mode, int w, int h /*, TUid on stack */) { return KErrNotSupported; }
void ord2_Destroy(RHardwareBitmap *self) {}
int  ord3_GetInfo(const RHardwareBitmap *self, void *info) { return self->iHandle ? KErrNotSupported : KErrBadHandle; }
int  ord4_SetAsScreenReference(RHardwareBitmap *self, int screen) { return KErrNotSupported; }
const void *ord10_GenericCapabilities(void) { return 0; }
void *ord11_AcceleratorNewL(int handle) { return 0; }   /* unreachable; see table */
int  ord_unused(void) { return KErrNotSupported; }       /* ords 5-9, 12-15 */
```

Wiring: build it the way `src/khr/build.sh` builds its E32 DLLs (GCC 14 symbianelf plus elf2e32 with a
15-entry .def). Add to `models/se-p1i/model.toml`:

```toml
[[rom.rofs_replace]]
image = 1
path = "sys/bin/PVR2DIF.dll"
built = "pvr2dif/PVR2DIF.dll"
hack = "H-p1i-pvr2dif-stub"
```

Then add a `docs/hacks.md` row. Regression test (`tests/`): parse the built image and check 15
exports, the caps and uids above, and that ords 1, 3 (with a zero handle) and 4 return -5, -8 and -5.
The return values can be checked by disassembling the stub. Keep the `pvrservices` LDD stand-in:
`libgles_cm.dll` still imports services_um, but with the stub nothing in WSERV/FBSERV loads PVR2D or
services_um any more [V: importers].

Effort: about 2–4 hours (stub, build script, profile entry, hack note, test). Risk: after the stub,
the next stop is whatever ScDv needs from the display HAL (screen address, mode, pitch) on our board
[I]. That is the ordinary display stand-in work every device needs, and it is not PowerVR.

## Options (a) and (b), sized for comparison

- (b) Redirect PVR2D.dll (9 ordinals that PVR2DIF imports: 1, 2, 3, 5, 7, 10, 11, 12, 13) to a software
  PVR2D: device enumeration, a context, wrapping screen memory and bitmaps, blit, and a blit-complete
  query. You would also have to reproduce what PVR2DIF expects from each (memory info layouts,
  `PVR2DBLTINFO`). The gain over (c) is nothing: those calls only serve hardware bitmaps and the
  accelerator, which the UI does not need. About 2–4 days. Not recommended.
- (a) A `services` LDD stand-in that fakes the PowerVR services bridge (connect, device enumeration,
  device memory heaps and allocation, mapping into the client, sync objects, 2D submit) under PVR2D and
  services_um. This is the heaviest option: services_um's bridge is broad, and the fake device would
  need memory-mapping semantics. About 1–2 weeks. Worth it only for GLES on the real
  `libgles_cm.dll`, and the project's route there is sygl replacement anyway (`[gl] impl`).
