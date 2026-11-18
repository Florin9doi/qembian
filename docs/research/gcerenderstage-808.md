# gcerenderstage.dll (808 Belle FP2): the WSERV render stage that drives GCE (2026-11-15)

Experiment: take one small, self-contained WSERV binary out of the 808's ROFS1 and document it end to end
with Ghidra + a Ghidra-MCP plugin on `localhost:8089`, to see what a bounded RE pass actually buys for this
project. Target chosen in `docs/research/videocore-device.md`'s neighbourhood: the render stage at the
bottom of the window-server pipeline, i.e. the piece between EwSrv's drawing and `gce.dll`.

Firmware: RM-807 113.010.1508 (Belle FP2), `sys/bin/gcerenderstage.dll`, 5333 bytes in ROFS1 (offset
`0x005c2dcc`), **byte-pair compressed**, code `0x1908` (6408 B) at link address `0x8000`, no data/bss,
1 export, 50 import thunks. Tooling: `tools/rofs.py` -> `tools/e32load.py` -> ARM ELF; Ghidra 12.1.4
(`ARM:LE:32:v8`, image base `0x8000`). Static analysis only, nothing run. `[verified]` = read in the
bytes/decompiler output or the cited header; `[inferred]` = best guess.

Artifacts (git-ignored): `build/roms/nokia-808-rm807/RM-807_113.010.1508_04.01+C00.01/ghidra/`
(`gcerenderstage.elf` to reload, `gcerenderstage.c` headless pseudocode, `README-gcerenderstage.md` the
prep notes). The Ghidra program itself carries 35 function names, 20 plate comments and 3 structures.

## Result in one paragraph

`gcerenderstage.dll` is an **ECom plugin** (uid3 `0x20031CB9`, implementation `0x20031CBA`) whose single
export is `ImplementationGroupProxy` and whose factory `CGceRenderStageFactory` builds a
`CGceRenderStage` when the window server asks for a render stage. The stage implements `CWsRenderStage`
(Begin/End), `MWsDisplayControl`, `MWsContentReadyForComposition` and `MWsScene`, and hands almost
everything to a `CGceWrapper` that owns the `Gce::CGce` compositor object plus a **refcounted surface
information cache** over `RSurfaceManager`. So the frame path in the 808 is
`EwSrv drawing -> CGceRenderStage::Begin/End -> CGceWrapper -> Gce::CGce (gce.dll) -> GCE backend` and the
"is my frame on screen" path is `MWsContentReadyForComposition` (WS type id 37) borrowing the compositor's
own `MContentReadyForComposition` (`0x2002E6D5`). That is the boundary our `src/gcebe` stand-in replaces.

## ECom plugin entry (this was the open question in the prep notes)

Export ordinal 1 (`0x81F0`, Thumb) is not a factory body:

```
*a0 = 1;                    // aTableCount
return (void*)0x91DC;       // TImplementationProxy[], 8 bytes there:
                            //   { TUid 0x20031CBA, TProxyNewLPtr 0x840D }  (0x840C, Thumb)
```

`0x840C` = `CGceRenderStageFactory::NewL`: `AllocZL(0x14)` + ctor, factory object 0x14 bytes
(vtable, `MWsObjectProvider` sub-vtable, `MWsRenderStageFactory` sub-object at +0x10). The factory base
dtor calls `wsgraphicdrawer` ord96, which calls `REComSession::DestroyedImplementation` - independent
confirmation of the ECom path. `[verified: table bytes, export body, ord96 body]`

Who instantiates `0x20031CBA` is still unknown: nothing in this module refers to it, so it is the window
server (`EwSrv.exe`) asking ECom for a render-stage implementation, probably by the
`KMWsRenderStageFactory` object extension. `[inferred]` Worth one grep in `EwSrv.exe` later.

## Classes (all names from the module's own RTTI strings at 0x92F4+)

| class | vtable | size | notes |
|---|---|---|---|
| `CGceRenderStageFactory` | `0x96D4` (+ `0x96FC`, `0x9708` sub-vtables) | 0x14 | `MWsRenderStageFactory`; `CreateFinalRenderStageL` only |
| `CGceRenderStage` | `0x955C` (19 slots) + `0x95B0`/`0x95BC`/`0x95D8`/`0x95F4` | 0x24 | `CWsRenderStage` + 4 extension bases |
| `CGceWrapper` | `0x942C` (13 slots) + `0x9468` | 0x28 | owns `Gce::CGce`, the surface cache, a 0x10-byte helper |
| `CGceWrapperElement` | `0x949C` (21 slots) + `0x9500` | 0x58 | one GCE layer + surface id, `MWsElement` |
| `CSceneElement<CGceWrapperElement>` | `0x961C` | (base of the above) | generic element wrapper |
| `CScene<CSceneElement<CGceWrapperElement>>` | `0x971C` | - | element list / z-order (`MWsScene`) |
| `CSurfaceInfoCache` | `0x9608` | 0x34 | `RArray` of surface entries + `RSurfaceManager` session |

Object layouts recorded as Ghidra structures (field offsets verified from ctor/dtor and delegate bodies):

```
CGceRenderStage (0x24)                     CGceWrapper (0x28)
+0x00 CWsRenderStage vftable                +0x00 MWsScene-ish vftable (13 slots)
+0x04 MWsObjectProvider vftable (ord15)     +0x04 MWsObjectProvider vftable
+0x08 iPimpl (CWsRenderStage, 8 bytes)      +0x08 iScreen
+0x0C MWsDisplayControl sub-object          +0x1C Gce::CGce*         (gce.dll ord1)
+0x10 5-slot sub-object (unresolved)        +0x20 helper object (vtable 0x971C)
+0x14 MWsContentReadyForComposition         +0x24 CSurfaceInfoCache*
+0x18 CGceWrapper*
+0x1C MWsDisplayControl*   <- from the GCE object (Extension 0x10286497)
+0x20 MContentReadyForComposition* <- from the GCE object (Extension 0x2002E6D5)

CSurfaceInfoCache (0x34)
+0x00 vftable   +0x04 RArray<TSurfaceCacheEntry> (granularity 0x40)   +0x24 RSurfaceManager session
```

`CGceRenderStage::ResolveObjectInterface` (`0x82A4`) is the map of that layout:

```
36  KMWsScene                     -> mGceWrapper + 4
37  KMWsContentReadyForComposition -> this + 0x14
0x10286497 KUidWsDisplayControl   -> this + 0x0C
else CWsRenderStage::ResolveObjectInterface (wsgraphicdrawer ord84):
       id 13 -> this ; otherwise Next()->ResolveObjectInterface(id)
```
Ids 13/36/37 come straight from `EWsObjectInterfaceId` in
`windowing/windowserver/inc/Graphics/openwfc/WSGRAPHICDRAWERINTERFACE.H`. `[verified]`

## Frame lifecycle (as far as the code shows)

```
CreateFinalRenderStageL(screen, redraw, screenNumber)     0x8462
  -> CGceRenderStage::NewL(screen, redraw)                0x8244   AllocZL(0x24)
       ctor stamps 5 vtables, BaseConstructL (ord83) allocates the 8-byte CWsRenderStage pimpl
       mGceWrapper = CGceWrapper::NewL()                  0x84CC   AllocZL(0x28)
            ConstructL(screen)                            0x848C
              mGce = gce.dll ord1(screen) = Gce::CGce::NewL
              mSurfaceInfoCache = CSurfaceInfoCache::NewL()  0x8E06  (RSurfaceManager::OpenSession)
       mDisplayControl = mGce->Extension(0x10286497)
       mContentReady   = mGce->Extension(0x2002E6D5)

Begin(dirtyRegion)   0x8300  -> CGceWrapper state at +0x18 (region empty/begin seen)
End(finishedStatus)  0x830A  -> completion path, display-control + content-ready interfaces
```

`End()` is where a frame's completion is decided, and the interesting asymmetry is in
`CSurfaceInfoCache::QuerySurfaceInfo` (`0x8E56`): the top byte of the surface id selects between
**HAL-backed screen surfaces** (`HAL::Get(id[0], 0x1F/0x20, ...)` - the two display dimensions, swapped
when the surface is rotated) and **`RSurfaceManager` surfaces**
(`OpenSurface`/`SurfaceInfo`/`CloseSurface`, `KErrNotSupported` for "small" ids). Whatever we stand in for
must answer both kinds consistently with what `gce.dll` expects. `[verified bodies; the HAL attribute
meanings are inferred]`

The cache is refcounted per surface id: `RegisterSurface` (`0x8ED4`) returns early with a bump of the
entry's refcount when the id is already present, otherwise builds an entry (`TSurfaceId` at +0x00,
`iRefCount = 1` at **+0x10**, a `TBuf8<0x20>` for the surface info at +0x18), queries it, and appends.
`ReleaseSurface` (`0x8F48`) decrements at +0x10 and removes at zero; `GetSurfaceInfo` (`0x8F70`) copies
the blob from **entry+0x18**. Evidence: `+1`/`-1` on `[entry,#0x10]` at `0x8EF4`/`0x8F5E`. `[verified]`

## wsgraphicdrawer.dll ordinals this module depends on (decoded from ROM)

These are Nokia-private (no `.dso`; the S60/Symbian3 SDKs stop at `ws32`), so they were recovered by
decompiling `WsGraphicDrawer.dll` from the same ROM (`tools/ghidra/decompile.sh WsGraphicDrawer.dll`).
Reusable dictionary:

| ord | shape | meaning |
|---|---|---|
| 15 | `return 0` | `MWsObjectProvider::ResolveObjectInterface` base implementation |
| 83 | `AllocZL(8)`, store at `this+8` | `CWsRenderStage::BaseConstructL` |
| 84 | `id==13 -> this`, else `Next()->ResolveObjectInterface(id)` | `CWsRenderStage::ResolveObjectInterface` |
| 87 | stamps base vftable + provider sub-vtable | `CWsRenderStage::CWsRenderStage` |
| 88 / 90 | dtor pair (90 = `~CWsRenderStage`) | `CWsRenderStage` destructor |
| 91 | `AllocZL(0xC)`, stores `(a1,a2)` in the pimpl at `this+0xC` | base-class ctor with pimpl |
| 92 | `return *(*(this+0xC) + 8)` | pimpl getter (here: the `MWsScreen`) |
| 93 / 96 / 94 | stamps vtable + `REComSession::DestroyedImplementation` in the dtor | ECom plugin base class |

`[verified bodies]` Other ordinals used by the module (50, 99, 100) were not decoded yet.


## Session 2: the frame state machine, the extension methods, and who loads it

**`CGceRenderStage::End` answers the hang question.** The stage keeps five flags in `CGceWrapper`
(the object at `+0x18`):

```
+0x0C iComposing     CGce has been told to compose the pending scene, completion not seen yet
+0x10 iBeginSeen     Begin() was called
+0x14 iEndRequested  something changed the scene since Begin() -> the next End() must flush
+0x18 iRegionDirty   the last Begin() had a non-empty region
+0x1C mGce           Gce::CGce*        +0x24 mSurfaceInfoCache
```

```
Begin(region)  0x8300 -> CGceWrapper::Begin      0x88B4  iBeginSeen=1; iRegionDirty=!region->IsEmpty();
                                                          if (iEndRequested) ComposeIfNeeded()
End(status)    0x830A -> if (CGceWrapper::IsFramePending())          // iBeginSeen && iEndRequested
                             CGceWrapper::FlushFrame(status)         // 0x888C: mGce->vt[0x18/4](status)
                         else if (status)
                             *status = KRequestPending; RequestComplete(&status, KErrNone)
                         CGceWrapper::ResetBegin()
```

So **a render pass completes synchronously with `KErrNone` unless the scene changed during it**; only then
does the caller's `TRequestStatus` travel into the compositor (`CGce::vtable[0x18/4]`, the single place that
happens). `iEndRequested` is set by `CGceWrapper::RequestEnd` (`0x8558`), called from **11 sites**, all scene
mutations: element insert/remove/attribute setters, surface register/release, and
`DisplayControlRequest` when a display-control call returns < 1. And `ComposeIfNeeded` (`0x8534`) tells
`CGce` to compose (`vtable[0x58/4]`) as soon as a frame is open with a dirty region, marking
`iComposing`; `NotifyCompositionComplete` (`0x8852`) calls `vtable[0x5C/4]` and clears it.
[verified: disassembly of 0x830A/0x886A/0x888C/0x88B4/0x8534/0x8558/0x8852 + xrefs]

**The 11 extra primary-vtable slots are the extension interfaces, implemented inline.** Each extension
sub-object's vtable holds adjustor thunks (`subs r0,#0xC` / `#0x10` / `#0x14` then `b <primary slot>`) back
into the primary vtable, so the stage has one implementation per method, shared by the sub-objects:

| primary slot | entered from | forwards to |
|---|---|---|
| 0x82F4 / 0x82F8 / 0x82FC | thunks -0x14 / -0xC / -0x4 | `CGceRenderStage::ResolveObjectInterface` (0x82A4) |
| 0x8344 | +0x0C sub-object (`MWsDisplayControl`, vtable 0x95BC) | NotifyOnDisplayChange -> mGceDisplayControl `vt[0x14/4]` |
| 0x8350 | idem | NotifyOnDisplayChangeCancel -> `vt[0x18/4]` |
| 0x835C | idem | NotifyOnConfigChange -> `vt[0x1C/4]` |
| 0x8368 | idem | NotifyOnConfigChangeCancel -> `vt[0x20/4]` |
| 0x8374 | +0x10 sub-object (vtable 0x95D8) | `mGceDisplayControl->vt[0x00/4]` |
| 0x8380 | idem | `vt[0x04/4]` |
| 0x838C | idem | `CGceWrapper::DisplayControlSet` (mGce display control `vt[0x08/4]`) |
| 0x839A | idem | `CGceWrapper::DisplayControlRequest` (`vt[0x0C/4]`, and a result < 1 dirties the frame) |
| 0x83A8 | idem | `vt[0x10/4]` |
| 0x83B4 | primary (no thunk) | `mGceContentReady->vt[0x00/4]` with 5 args |
| 0x83C8 | +0x14 sub-object (`MWsContentReadyForComposition`, vtable 0x95F4) | same call as 0x83B4 |
| 0x83DC | primary | `mGceContentReady->vt[0x04/4]` (NotifyContentReadyCancel) |

`[verified for every thunk and forwarding target; the binding of the sub-vtable slots to the four
MWsDisplayControl method names is by header order and stays inferred]`

**Who loads it, and how it is chosen.** `Private/10009d8f/ecom-0-0.spi` in ROFS1 (the ECom registry,
166624 bytes at offset 0x690E318) contains the plugin group entry:

```
gcerenderstage   interface UID 0x10285A29   implementation 0x20031CBA   name "gcerenderstage"
```

Siblings under the same interface UID: `umauirenderstage` (0x20031CB3 "UMA UI Render Stage",
0x20031CB5 "NULL UI Render Stage"), `clonerenderstage` (0x20031CB6/B8) - so **0x10285A29 is the "render
stage factory" ECom interface** and the window server creates stages through it. [verified: registry bytes]

`sys/bin/EwSrv.exe` (ROFS1, uid3 0x10003B20, 152256 bytes byte-pair compressed, code 0x31EF4) contains
that interface UID once, **a table of {name, name2, TUid} rows including 0x20031CBA**, and a string run
`DISPLAYCONFIG / tcsc / gce / clonesource / openwf / numaui / nullui / platformui / clonesink /
platformcompositor`. So EwSrv selects the render stage by a configured *name* ("gce" for our plugin) and
maps it to the implementation UID through that table, then asks ECom for interface 0x10285A29.
`[verified: decompressed EwSrv image; the exact name->row pairing for 0x20031CBA is inferred]`

This also means **EwSrv.exe is a ROFS binary too** (not XIP) - it is not in `core.json`/`symindex.json`,
so `tools/e32load.py` + `tools/rofs.py --extract` are needed to look at it, exactly like this module.


## Session 3: completion pass (nothing unnamed left)

The whole module is now named and typed in the Ghidra program - no `FUN_*` remains:

- **200 functions**, all named: 6 classes with full method sets, 50 imports renamed, 30 adjustor thunks named
  after their targets (`<Class>::thunk_<Method>_m4/_m12/_m16/_m20`), plus the E32/CRT glue
  (`E32DllEntry`, `E32Dll_StaticInitialiserRun`, `EComProxy_StaticInit`).
- **Structures**: `CGceRenderStage` (0x24), `CGceWrapper` (0x28), `CGceWrapperElement` (0x58),
  `CScene` (0x10), `CSurfaceInfoCache` (0x34), `CGceRenderStageFactory` (0x14), `TSurfaceCacheEntry` (0x38),
  `TImplementationProxy` (8), `__class_type_info8` (8, applied to the nine RTTI records).
- **Enum** `EWsObjectInterfaceId`: KMWsRenderStageFactory 14, KMWsScene 36, KMWsContentReadyForComposition
  37, KUidWsDisplayControl 0x10286497 (from `WSGRAPHICDRAWERINTERFACE.H` / `wsdisplaycontrol.h`).
- **Data labelled**: every vtable (14, with the exact base-class name per sub-vtable), every RTTI record and
  RTTI name string, the ECom `TImplementationProxy` table at 0x91DC, the factory type-id record 0x91E4, the
  two UID literals (0x83E8/0x83EC), the comparator pointer 0x8FA4, and the E32 startup descriptor 0x800C.
- **16 prototypes** applied to the public API (`Begin`, `End`, `ResolveObjectInterface`, `NewL`,
  `CreateFinalRenderStageL`, `RegisterSurface`, `UnregisterSurface`, `SetSceneRotation`,
  `CSurfaceInfoCache::*` ...) - `this` kept explicit as `void*` because Ghidra's ARM has no thiscall.

New facts the naming pass produced:
- `CGceWrapper` **is** the `MWsScene` implementation (its 13-slot vtable is CreateSceneElementL,
  DestroySceneElement, ComposePendingScene x2, InsertSceneElement, RemoveSceneElement, RegisterSurface,
  UnregisterSurface, SetSceneRotation, SceneRotation) - this is what `ResolveObjectInterface(36)` returns.
- `CGceWrapper+0x20` (the "0x10-byte helper" open question) is **`CScene<CSceneElement<CGceWrapperElement>>`**:
  a doubly linked list (count at +4, head at +8, tail at +0xC) of elements linked through +0x50/+0x54.
- `CGceWrapperElement` layout: +8 wrapper, +0xC GCE layer, +0x10 flags, +0x14 alpha/blend, +0x18 enabled,
  +0x1C TSurfaceId, +0x2C destination rect, +0x3C clip mode, +0x40 clip rect, +0x50/+0x54 list links.
- ~~`TSurfaceCacheEntry`: TSurfaceId at 0, TSize at +0x10, refcount at +0x18~~ - **wrong, see the
  corrections section**: refcount is at +0x10, the blob at +0x18.
- ~~All element methods return KErrNotReady (-2) for a null surface and call RequestEnd()~~
  **wrong, see the corrections section**.

Left as-is on purpose (not analysable further without more binaries): the literal pools at 0x83E5, 0x87B0,
0x90AC (constants for the GCE layer contract), the mis-decoded init block at 0x8FE8, and the unnamed GCE-side
vtable offsets - naming those needs `gce.dll` (XIP) and `compositionbackend.dll`.


## Corrections after the 2026-11-15 validation pass

An external audit of this write-up and of the Ghidra program found real errors. They are recorded here
with their evidence and their status in the program; the sections above were edited in place where the
wrong statement was load-bearing. **Treat the annotation pass as "names applied and mostly right", not as
verified understanding.**

| # | Claim in sessions 1-3 | Reality | Status |
|---|---|---|---|
| 1 | the six `RSurfaceManager` imports were named | all six were **rotated**: the thunk order in the E32 import block is 12,14,13,2,3,4, not 2,3,4,12,13,14. Correct: `0x8160 Open(12)`, `0x8178 ctor(14)`, `0x8190 Close(13)`, `0x81A8 OpenSurface(2)`, `0x81B0 SurfaceInfo(3)`, `0x81B8 CloseSurface(4)` | fixed in the program + toolchain (see below), test added |
| 2 | cache entry refcount at +0x18 (session 3 "fixed" it from +0x18 to +0x1C+blob) | **+0x10**: `0x8EF4` does `+1` on `[entry,#0x10]`, `0x8F5E` does `-1` | fixed (struct + comments) |
| 3 | "null surface returns `KErrNotReady` (-2)" | -2 is **KErrGeneral**; KErrNotReady is -18. The code returns -2 | fixed |
| 4 | "every element setter calls `RequestEnd()`" | `SetRotationOrFlip` (`0x8A10`) does not; `SetVisible` (`0x8A38`) only when `iFlags & 1` | fixed |
| 5 | "only literal pools and a few init-pool decodes remain" | 33 addresses were still decoded as **ARM** while the code is Thumb (e.g. `0x8A0C` = `subs r0,r0,#4; b 0x898A` shows as `ldr r1,[r12,r0,lsl #0x1e]!`) | **fixed 2026-11-15** (Ghidra restarted with `GHIDRA_MCP_ALLOW_SCRIPTS=1`): all 36 addresses re-decoded with `TMode=1`, 32 adjustor thunks + `0x840C`/`0x88AC`/`0x8FE8`/`0x9034` are real functions again, stale labels removed; a scan of all 199 functions reports **0 ARM-like bodies** |
| 6 | "200 functions, fully named and typed" | naming coverage, not understanding; and the count needed explaining | see below |

**Why the import error happened (root cause, now fixed).** `tools/e32load.py` and `tools/xip2elf.py`
hard-coded `~/work/S60_3rd_FP1/.../armv5/lib` as the ordinal-name source regardless of the device. S60 3rd
FP1 predates `RSurfaceManager`, so the six ordinals came out as `ord12`/`ord2`/... under the *same* library
name, and `e32load` said nothing. The names were then assigned to thunks by address order, which is wrong
because the import block lists ordinals 12,14,13,2,3,4. Both tools now default `--sdk-lib` to the device
profile's `firmware.sdk_lib` (env `S60_SDK_LIB` still overrides), and `e32load.py` prints every ordinal it
could not name:

```
gcerenderstage.dll.e32: 21 import ordinal(s) could not be named from .../Nokia_Symbian3_SDK_v1.0/.../lib:
    gce.dll             1
    wsgraphicdrawer.dll 15,50,83,84,85,86,87,90,91,92,93,96,99,100
```

`tests/e32load_test.py` pins the mapping (surfacemanager 12/14/13/2/3/4 at those code offsets), the SDK
default, and the warning.

**ARM/Thumb (fixed 2026-11-15).** With the MCP script gate enabled, a script set `TMode = 1` on all 36
affected addresses, cleared and re-disassembled them, and re-created the functions; the stale labels from
the intermediate state were removed. A full scan of the 199 functions now reports **no ARM-like body**
(no `mrc`/`mcr`/`ldc`/`stc`/`halt_baddata`). Two findings came out of doing it properly:

- `0x840C` (`CGceRenderStageFactory::NewL`) is `AllocZL(0x14)` + ctor at `0x83F8`; the ctor calls
  wsgraphicdrawer ord93 (the ECom plug-in base ctor) and stamps vtable `0x96D4` plus the provider sub-vtable
  at +4. `0x842E` (vtable slot 3) is `BaseConstructL`: ord91 allocates the 0xC pimpl that later carries the
  `MWsScreen` into `CreateFinalRenderStageL`.
- the block at `0x8FE8` that earlier notes called "`movs r0,#0x20; blx` pairs, purpose unknown" is the
  module's **exception-cleanup chain**: each pad destroys a local (`CWsRenderStage` dtor, `CBase::~CBase`)
  and calls `__cxa_end_cleanup`, and is reached only by the unwinder - hence no caller. The `0x20` was the
  frame offset Ghidra passed in r0, not an argument. Renamed `ExceptionCleanupPads`.

**Function counts.** `/get_current_program_info` reports `function_count` 215 while `/list_functions`
returns 165: `FunctionManager` counts the 50 imported symbols' stubs in the `<EXTERNAL>` space as well
(`/list_external_locations` = 50), the listing endpoint returns functions in the loaded image. Both numbers
are consistent; neither is evidence of completeness. Before the corrections the pair was 250/200
(200 image functions of which 33 were bogus ARM decodes).

**What is actually verified vs. inferred now.** Verified: object layouts that the code loads/stores directly
(the two structs above, `CScene` links), the six imports, the forwarding targets of the extension methods,
the frame state machine, the E32/ECom entry chain, the class names from RTTI. Inferred and marked as such:
the `MWsDisplayControl`/`MDisplayControlBase` slot-to-method binding, the semantics of most
`CGceWrapperElement` setters (named from body shape, not from a spec), the meaning of the GCE vtable
offsets, and the `CScene`/element interaction order.


## The import index this pass produced

`tools/import_index.py` now builds the missing piece: per firmware, `dll -> ordinal -> name` (from the
device's own SDK) and `module -> [dll, ordinal, thunk offset]` for every XIP and ROFS module. For the 808:
4652 modules, 835175 imports, 741736 named, 93439 left visibly unnamed (no `.dso`), 18 s to build. Some
results that bear on this module:

```
import_index.py at gcerenderstage.dll 0xcc      ->  gce.dll ordinal 1  = <unnamed>
import_index.py dll gce --ordinal 1             ->  1 user(s): gcerenderstage.dll
import_index.py unresolved --dll wsgraphicdrawer -> 60 ordinals, incl. 15,50,83-100 (every one we use)
```

Two corrections to earlier assumptions, both from the index:

1. `gce.dll` **ordinal 1 has exactly one importer in the 808 firmware: `gcerenderstage.dll`.** The N8 note
   (`docs/devices/nokia-n8-rm596/composition.md`) also lists `10286378.dll` as an importer - true for the N8,
   not for this firmware. So on the 808 the whole GCE surface that our `src/gcebe` stand-in must satisfy is
   reached through this one render stage.
2. `wsgraphicdrawer.dll` exports 105 slots, of which our SDK names none beyond the SDK-era ones; the index
   shows 60 ordinals still unnamed *and who imports them* (`ord 1`: `EwSrv.exe` only; `ord 4`: `EwSrv.exe`,
   `alfcrpplugin.dll`; ...). That is the prioritised work list for decoding the WS base-class API from ROM
   bodies - the manual pass in this document covered 15/50/83-100 because this module uses them.


## What this buys the project

1. **The GCE boundary is now named on both sides.** `docs/devices/nokia-n8-rm596/composition.md` covered
   `gce.dll`/`compositionbackend.dll`; this covers the WSERV side. If we ever want Belle's own render stage
   to run against our `src/gcebe` stand-in, this module is the caller contract: `Gce::CGce::NewL(screen)`,
   `Extension(0x10286497)`, `Extension(0x2002E6D5)`, plus the surface-manager and HAL answers above.
2. **Surface bookkeeping is a concrete stand-in requirement** - refcounted ids, a `TSurfaceInfoV01` blob
   per surface, and two different sources of truth (HAL vs `RSurfaceManager`) that must agree.
3. **A cheap regression:** if `gcebe` ever mis-answers `SurfaceInfo`, `CSurfaceInfoCache` will happily
   cache the wrong size for a surface, and every later composition of it is wrong - a plausible class of
   "stale/garbled frame" bug worth looking for before blaming the compositor.

## Open questions / next pass

- Which *name* selects 0x20031CBA in EwSrv's table, and where the name comes from (CenRep key / wsini).
  Following that gives the real "why is the GCE render stage used at all" answer.
- `CGce::vtable[0x18]`, `[0x58]`, `[0x5C]` (composition submit / complete) are the Nokia-private GCE API -
  naming them needs `gce.dll` (XIP, 2 exports, 38584 B) decompiled, plus `compositionbackend.dll` behind it.
- The `+0x10` 5-slot sub-object: RTTI says `MDisplayControlBase`; the Belle build has 5 methods where the
  EPL drop has 2. Its GCE-side targets (vt 0x00/0x04/0x08/0x0C/0x10) are unnamed.
- The 0x10-byte helper at `CGceWrapper+0x20` (vtable 0x971C, CBase dtor only): purpose unknown.
- Ordinals 50/99/100 of `wsgraphicdrawer.dll`, and `gce.dll` ord2.

## How this was produced (reproducible)

```
SYBORG_DEVICE=nokia-808-rm807
DEV=build/roms/nokia-808-rm807/RM-807_113.010.1508_04.01+C00.01
python3 tools/rofs.py $DEV/rofs1.img --extract sys/bin/gcerenderstage.dll $DEV/ghidra/gcerenderstage.dll.e32
python3 -I tools/e32load.py $DEV/ghidra/gcerenderstage.dll.e32 $DEV/ghidra/gcerenderstage.elf
# Ghidra GUI + GhidraMCP 7.0.0 plugin (REST, port 8089): GET /mcp/schema lists all 239 endpoints;
# the ones used here: /list_functions /decompile_function /create_function /rename_function
# /set_comment /create_struct /read_memory /save_program (bulk decompile caps at ~20 functions/call).
tools/ghidra/decompile.sh WsGraphicDrawer.dll      # XIP module, same ROM: the ord dictionary above
```

`e32load.py`/`xip2elf.py` used to default `--sdk-lib` to S60 3rd FP1 no matter which device was loaded, so
the six `surfacemanager` imports of this module came out unnamed - and were then hand-mapped from thunk
*address* order, which is not ordinal order (see corrections). Both tools now default to the device
profile's `firmware.sdk_lib`, and `e32load.py` prints every import ordinal it could not name
(`tests/e32load_test.py` pins the mapping and the warning).
