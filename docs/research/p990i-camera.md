# P990i R6E28: CamServer "CamSrv:start err" -1, and what a camera mock needs

Research note (2026-11-17), static reading only, no boot. Tags: [verified] = read in the code /
data; [inferred] = best guess. Addresses: CamServer.exe and intergraphapi.dll as loaded by
`tools/e32load.py` (code base 0x8000, CamServer `.data` 0x400000). CamServer is Thumb,
intergraphapi.dll is ARM.

## 1. The cause is not the camera: YUV2RGB.LDD is missing

CamServer main thread (0x8180): `CTrapCleanup`, scheduler, `TRAP(server = NewL())`, and if NewL
leaves it panics `CamSrv:start err` with the leave code (0x8208; literal 0x23170) [verified].
When the main thread panics the process dies, so the second thread
(`CamerasRenderServerThread`) shows the same exit [inferred: process-wide exit].

`CCameraServer::ConstructL` (0x80d0) [verified]:
1. `CServer2::StartL("!OnboardCameraServer")`.
2. 0xc732: starts the render server thread (0xc366: `RThread::Create(... "CamerasRenderServerThread")`,
   rendezvous), then `RSessionBase::CreateSession(renderServer, ver, 2)` and `User::LeaveIfError`.
3. Builds active objects (0xb4fa, 0xb5ee, 0xb784), two sensor objects (0x97c2, called in a loop for
   index 0 and 1) and a CTimer (0xbc24). None of these opens a driver. The sensor ConstructL (0x96f8)
   only builds active objects and does `RProperty::Attach` (result ignored).

So the -1 comes from step 2. Creating a render session builds the render engine (0xc7ea -> 0xc75c
-> 0xd32e -> 0xd300) [verified chain; reached via the session vtable [inferred]], whose ConstructL
does `RFbsSession::Connect` + `LeaveIfError`, then `intergraphapi` ordinal 3 and ordinal 39 [verified].
intergraphapi's set-up (0x90f8, called from 0x81b4/0xa674/0xa730) does [verified]:

```
r = User::LoadLogicalDevice(_L("YUV2RGB"));     // literal 0xb580
if (r != KErrNone && r != KErrAlreadyExists) User::Leave(r);
eglGetDisplay(0) / eglInitialize(...)           // leaves eglGetError() on failure
```

`YUV2RGB.LDD` (XIP 0xf83b1090 in the firmware, `core-files.txt`) is a kernel-side file and is not in
our composed ROM (not in `hybrid-manifest.txt`) [verified], so LoadLogicalDevice returns KErrNotFound
(-1) -> the session's CreateL leaves -1 -> CreateSession returns -1 -> ConstructL leaves -1 ->
`CamSrv:start err -1` [inferred chain, every step read; not run].

The camera driver (camdrv.ldd + PDDs) is opened lazily, per sensor, when a client reserves a
camera, not at start-up (see 3).

## 2. YUV2RGB LDD protocol (the one stand-in needed for start-up)

| item | value |
|---|---|
| file name loaded | `YUV2RGB` (-> `YUV2RGB.LDD`) [verified]; return 0 or -11 accepted |
| factory (channel) name | `YUV2RGBDRV` (literal 0xb91c, `DoCreate` at 0xa930) [verified] |
| DoCreate | unit `KNullUnit` (-1), `TVersion(1,0,1)`, no PDD name, no info [verified] -> no PDD needed |

Controls, issued only when a conversion is set up (0xa8dc, from 0x9f94), not at start-up [verified]:

| fn | args | when | must return |
|---|---|---|---|
| 0 | none | right after DoCreate; non-zero -> channel closed, conversion path fails | KErrNone [verified] |
| 0x43 | a1 = &TInt (client zeroes it first, at obj+12), a2 = &local TInt | after 0 | 0, else abort; a1 probably gets a buffer/chunk value [inferred] |
| 3 | a1 = 0, a2 = 1 if width <= 512 else 0 | | 0, else abort [verified] |
| 4 | a1 = TPtr8 of 48 bytes (colour matrix, chosen by format 0x10000/0x20000/0x40000/0x80000) | when the format changes | 0 [verified] |
| 2 | a1 = width (TInt, by value) | last step | ignored [verified] |
| 1 | none | close (0xa8ac, 0xaa90) | ignored [verified] |

Widths over 1024 are refused by the client itself (0xa97c). For start-up only the load matters;
for the conversion path the cheapest stand-in returns KErrNone for 0/3/4/2/1. Control 0x43 likely
hands out a buffer (the conversion then writes/reads through it) [inferred]; returning
KErrNotSupported for 0x43 makes the client close the channel and fail that conversion cleanly
(0xa9b4) [verified branch], which is the safe choice until a camera viewfinder is wanted.

## 3. camdrv.ldd / DMsoc*.pdd (lazy, per reserved sensor)

Sensor table (CamServer `.data`, used copy at 0x400050, 20-byte entries; +8/+12 filled by a static
initialiser) [verified offsets, names inferred from use]:

| idx | +0 unit | +4 sensor id (8-bit) | +8 PDD file | +12 PDD factory name | +16 |
|---|---|---|---|---|---|
| 0 | 3 | `MI-SOC2010-J` | `DMsoc2010.pdd` | `camdrv.ldd.Msoc2010` | 0 (main 2 MP) [inferred] |
| 1 | 4 | `MI-SOC366` | `DMsoc366.pdd` | `camdrv.ldd.Msoc366` | 1 (front) [inferred] |

Open (0x9898, from 0x82fe on reserve) [verified]:
1. idx >= 2 -> KErrNotSupported.
2. `User::LoadPhysicalDevice(entry+8)`; anything but 0 / -11 -> return that error.
3. `User::LoadLogicalDevice("camdrv.ldd")`; anything but 0 / -11 -> return that error.
4. `RBusLogicalChannel::DoCreate("camdrv.ldd", TVersion(1,0,0x431), unit = entry+0 (3 or 4),
   aDriver = NULL, aInfo = NULL, EOwnerProcess)`; non-zero -> return it. With no PDD name the
   kernel tries every PDD named `camdrv.ldd.*` and calls its Validate(unit, ver) [kernel behaviour].
   So the LDD factory name must be exactly `camdrv.ldd` (it is also what `FreeLogicalDevice` uses,
   0x997c) and a PDD factory name must be `camdrv.ldd.<x>` (FreePhysicalDevice uses entry+12)
   [verified].
5. `DoControl(2, &TBuf8<96>)` (capabilities; result ignored, 96 bytes copied to the sensor object,
   +56) [verified]. Contents not decoded.

Error results from this open come back to the reserving client; CamServer does not panic on them
(0x8302: error returned, no Leave) [verified]. Later traffic seen in the code (not decoded, not needed
for "no camera"): DoControl 20 (a1 = 0/1 from cenrep), 22 (a1 = TUint16[30] list out, a2 = &count),
DoControl(n, a1, a2) and DoRequest sets at 0xac34-0xaea4 and 0xaee6-0xaf6c [verified call sites].

Answer for "camera absent without panicking": do nothing. Without DMsoc*.pdd the reserve gets
KErrNotFound (-1) from LoadPhysicalDevice and CamServer keeps running [inferred from 0x98b4/0x8302].
If a cleaner code is wanted, a PDD `camdrv.ldd.Msoc2010` whose Validate returns KErrNotSupported plus
an LDD `camdrv.ldd` give KErrNotSupported (-5) from DoCreate. A camera that never delivers frames
would need the 96-byte capability block and the request set decoded: out of scope here.

## 4. ECamSemcPlugin / CallControl with "no camera"

Not examined (budget). If the Phone app still dies (E32USER-CBase 63) once CamServer starts, look at
the reserve error path next [inferred].

## Proposed stand-ins

`src/standins/yuv2rgbldd/yuv2rgbldd.cpp` (tablestub style; tablestub accepts any version and no PDD):

```cpp
// Sony Ericsson P990i YUV2RGB.LDD stand-in: "YUV2RGBDRV", hardware YUV->RGB of intergraphapi.dll
// (CamServer render engine). Without the file LoadLogicalDevice fails -1 and CamServer panics
// "CamSrv:start err" -1 (docs/research/p990i-camera.md). No converter: 0x43 refused, so the client
// closes the channel and that conversion fails cleanly; the rest succeed.
#include "tablestub.h"
static const SStubCtl KCtl[] = {
	{ 0x43, EReturn, KErrNotSupported, 0, 0 },
	{ 0,    EReturn, KErrNone, 0, 0 },
	{ 1,    EReturn, KErrNone, 0, 0 },
	{ 2,    EReturn, KErrNone, 0, 0 },
	{ 3,    EReturn, KErrNone, 0, 0 },
	{ 4,    EReturn, KErrNone, 0, 0 },
};
TABLE_STUB_LDD("YUV2RGBDRV", KCtl, ETrue)
```

`.mmp` as `src/standins/dspcontrolldd/dspcontrolldd.mmp` with `linkas YUV2RGB.LDD` (file name the
client loads), plus in `models/se-p990i/model.toml`:

```toml
[[standins]]
name = "yuv2rgbldd"
kind = "ldd"
family = "semc"
role = "YUV2RGBDRV: intergraphapi YUV->RGB (CamServer render engine); load only matters, no converter"
```

Optional (cleaner "no camera"): `camdrv` LDD stand-in named `camdrv.ldd` with no controls, which
needs a PDD (`iParseMask = KDeviceAllowPhysicalDevice | KDeviceAllowUnit`), and one PDD
`camdrv.ldd.Msoc2010` (file `DMsoc2010.pdd`) whose `Validate()` returns KErrNotSupported. Not
needed for start-up.
