# Belle FP2 (Nokia 808): can the EPL window-server source replace the shipped EwSrv/Ws32?

Scope: device `nokia-808-rm807`, firmware `RM-807/113.010.1508/04.01+C00.01`. Static work only
(imports.json, ecom.json, core.json, the ROFS binaries' E32 headers and strings, the ROM's wsini.ini, the
local EPL graphics drop). Written 2026-11-19 for the registry component `wserv` (`undecided`) and task
`wserv-source-delta-assessment`; the owner decides the strategy. Method and table shape follow
`docs/research/belle-qt-avkon-source-assessment.md`. Every number comes from
`build/roms/nokia-808-rm807/RM-807_113.010.1508_04.01+C00.01/{imports.json,ecom.json,core.json}` (SDK names
from `Nokia_Symbian3_SDK_v1.0/epoc32/release/armv5/lib`) unless marked otherwise.

## 1. What the firmware ships (inventory)

"importers" = firmware modules importing the DLL; "used" = distinct ordinals imported by anyone;
"exports" = the E32 header's export count; "modver" = E32 module version.

| binary | where | exports | importers | used | max used | modver | status |
|---|---|---|---|---|---|---|---|
| Ws32.dll (client API) | XIP core | **677** | 389 | 387 | 676 | 10.0 | [verified] |
| WsGraphicDrawer.dll (plugin/drawer base classes) | XIP core | **105** | 13 | 62 | 105 | 10.0 | [verified] |
| EwSrv.exe (the server) | ROFS1, RAM-loaded, 152256 B compressed (0x102822aa), code 0x31ef4, 1139 import thunks, 0 exports | - | - | - | - | 10.0, tools 0.0.1026 | [verified] |
| remotegc.dll | XIP | 21 | - | - | - | - | [verified] |
| gcerenderstage.dll (ECom 0x10285A29, impl 0x20031CBA "gcerenderstage") | ROFS1 | 1 | - | - | - | - | [verified] (docs/research/gcerenderstage-808.md) |
| umauirenderstage.dll (impls 0x20031CB3 "UMA UI Render Stage", 0x20031CB5 "NULL UI Render Stage") | ROFS1 | 1 | - | - | - | 10.0 | [verified] |
| clonerenderstage.dll (impls 0x20031CB7 "Clone Source", 0x20031CB8 "Clone Sink") | ROFS1 | 1 | - | - | - | 10.0 | [verified] |
| sssrenderstage.dll (impl 0x20039F82 "CSssRenderStage") | ROFS1 | 1 | - | - | - | 10.0 | [verified] |
| alfrenderstage.dll (impl 0x200184B6 "CAlfRenderStage") | ROFS1 | 1 | - | - | - | 10.769 | [verified] |
| alfcompositorrs.dll (impl 0x2002BCF9 "CAlfRenderStage") | ROFS1 | 1 | - | - | - | - | [verified] |
| aknextendedfader_arm11.dll (impl 0x2001B299 "AknExtendedFader", a CWsPlugin fader) | XIP | 1 | - | - | - | - | [verified] |
| keyrouter.dll (uid3 0x102872E2, key-event routing plugin, loaded by name "keyrouter.dll") | ROFS1 | 1 | - | - | - | 10.0 | [verified] |
| CRP drawers 10281922.dll, 2000b5e7.dll, 20016bec.dll, alfcrpplugin.dll (CWsGraphicDrawer subclasses) | XIP / ROFS1 | 1 each | - | - | - | - | [verified] |

ECom interface **0x10285A29** (the one WSERV plugin interface: render stages, faders, any CWsPlugin) has 9
implementations in 7 DLLs in this firmware; no other ECom interface is served to EwSrv (EwSrv's own ECom
imports are `CreateImplementationL` with and without resolver params, and `FinalClose`) [verified].

EwSrv.exe's imports (distinct ordinals): euser 464, bitgdi 41, fbscli 30, **wsgraphicdrawer 29**, estor 22,
drtaeabi 21, efsrv 18, gdi 18, dfpaeabi 5, hal 4, scppnwdl 4, ecom 3, ektran 3, libEGL 1 (`eglReleaseThread`)
[verified]. It imports no NGA-only library directly (no surfacemanager, surfaceupdateclient, openwfc,
graphicsresource): every surface is handled through the render-stage plugins, exactly the EPL NGA layering.
IPC: `!Windowserver` is provided by EwSrv.exe and connected to only through Ws32.dll [verified, ipc index].

## 2. What source is local, and its relation to the firmware

`~/work/symbian/oss.FCL.sf.os.graphics`, git `ff133bc5`, tag **PDK_4.0.a-1** (2010-10-18): `windowing/windowserver`
(nga/SERVER + nga/SERVER/openwfc = the NGA server, nga/graphicdrawer, inc/Graphics/openwfc headers, eabi defs
`WS322U.DEF`, `GRAPHICDRAWERU.DEF`, `W32STDGRAPHICU.DEF`) and `windowing/windowserverplugins` (openwfc stdplugin
0x10286378 with the FLICKERBUFFER 0x1028637B / DISPLAY 0x1028637A render stages, cscplugin 0x10286506 TCSC
0x10286507, keyeventrouting = keyrouter.dll uid3 0x102872E2) [verified].

Relation: **related, same lineage, ~1.5 years older**, and from a differently laid-out tree. EwSrv's panic
strings name `Y:/sf/os/graphics/windowing/windowserver/SERVER/{ANIMDLL,CLIENT,EVENT,gc,GROUPWIN,POINTER,REDRAWQ,
registeredsurfacemap,renderstagemanager,ROOTWIN,rotationutils.h,scrdev,screen,ScreenRedraw,WsMemMgr}.cpp`
[verified]: a tree in which the NGA server lives in `SERVER/` directly (the drop still has `nga/SERVER/openwfc/`
and no `rotationutils.h`). So Nokia built Belle from a post-201039 graphics package after the nonnga/nga split
was collapsed [inferred: from the path strings]. Ini keys, class names and the plugin interface enum agree
(section 4), so the delta is incremental, not a rewrite.

## 3. Ordinal compatibility: the EPL defs against what the firmware's importers use

| DLL | EPL def max | firmware exports | used | used > def | who needs the post-def ordinals | names (def = SDK) |
|---|---|---|---|---|---|---|
| ws32.dll | 661 | 677 | 387 | **10** (662-665, 670, 671, 673-676) | **Cone.dll** (663-665, 670, 671, 673-675: 8 ordinals), xn3layoutengine.dll (662), gfxtransadapter.dll (676) | 375/375 equal |
| wsgraphicdrawer.dll | 105 | 105 | 62 | 0 | - | SDK has no library; the def names all 62 (table below) |
| w32stdgraphic.dll | 50 | not in firmware | 0 | - | - | - |

All counts [verified]. The SDK (Symbian^3 v1.0) names ws32 up to 652; the EPL def goes to 661 (653
`RWsSession::SendEffectCommand`, 654 `RWindowBase::SetPurpose`, both used by QtGui; 655-661 = the
RegisterEffect/OverrideEffects/UnregisterEffect/IndicateAppOrientation family, unused by any firmware module);
Belle appended **16** more (662-677), of which 10 are used. Their names are unknown (no Belle .def on disk);
EwSrv's capability strings list the server-side APIs that are new relative to the drop's server strings and are the
likely owners: nothing new there beyond RegisterEffect/OverrideEffects/SendEffectCommand/SetCloseProximityThresholds/
SetHighPressureThresholds, which the def already has, so the 662+ tail is most likely client-side convenience
(Cone's 8 look like one new class or one new group of RWindow/RWsSession members) [inferred].

Consequence: a **ws32.dll built from the drop fails to load Cone.dll** (every Avkon and Qt app), xn3layoutengine
(the home screen) and gfxtransadapter (transitions) [verified: missing ordinals -> KErrNotFound at load].

### wsgraphicdrawer.dll: the plugin ABI, fully named by the EPL def

Of its 62 used ordinals, the render stages use the `CWsRenderStage` set (83 BaseConstructL, 84
ResolveObjectInterface, 85 Next, 86 SetNext, 87 ctor, 90 dtor, 99 typeinfo) and the `CWsPlugin` set (91
BaseConstructL, 92 Env, 93 ctor, 95/96 dtor, 97 PluginName, 100 typeinfo); EwSrv uses the drawer array,
`MWsAnimationScheduler` (24, 25, 105 `Animate(MWsScreen&, TRequestStatus*)`), `TWservCrEvent` ctors and the
share/unshare API; the CRP drawers use `CWsGraphicDrawer` 3-13, 33, 49, `TWsGraphicMsgAnimation`/`BufParser`
[verified]. Export count and def max are both 105: Nokia did not extend this DLL after the drop [verified].

### Plugin object-interface ids (the task's "KMWsRenderStageFactory 14, KMWsScene 36, ...")

The EPL enum `EWsObjectInterfaceId` (WSGRAPHICDRAWERINTERFACE.H) runs 1..37: 13 KCWsRenderStage, 14
KMWsRenderStageFactory, 15 KMWsMemoryRelease, 17 KMWsFader, 18 KMWsPluginManager, 19 KMWsIniFile, 20
KMWsRawEventServer, 22 KMWsTextCursor, 23 KMWsGraphicsContext, 24 KMWsCompositionContext, 25 KMWsEventHandler,
26 KMWsScreenDevice, 28 KMWsUiBuffer, 29 KMWsDebugBar, 30 KMWsDrawableSourceProvider, 32 KMWsWindowTreeObserver,
33 KMWsDrawAnnotationObserver, 34 KMWsWindowVisibilityNotifier, 35 KMWsWindowTree, 36 KMWsScene, 37
KMWsContentReadyForComposition [verified]. The 808's gcerenderstage resolves 13, 36, 37 and the UID-sized id
0x10286497 (KUidWsDisplayControl), and EwSrv's code carries 0x10286497 twice [verified: gce doc +
byte search]; the EwSrv RTTI set contains `MWsWindowTree`, `MWsScreenConfigList`, `MWsMemoryRelease`,
`MWsPluginManager`, `MWsIniFile`, `MWsGraphicDrawerEnvironment` [verified]. Nothing contradicts the EPL
numbering; whether Belle added ids above 37 (Nokia-private extensions the alf/sss stages may resolve) cannot be
seen without decompiling the stages [inferred: unlikely to matter for gcerenderstage, open for alfrenderstage].

## 4. Behavioural deltas visible statically

**Plugin loading.** Both resolve `[section] ID 0x...` via `CreateImplementationL(uid)` or `TYPE` via the
0x10285A29 resolver; both carry a hard-coded name table for sections without ID/TYPE. The tables differ:

| name in RENDERSTAGES/PLUGINS | EPL drop (wspluginmanager.cpp) | Belle EwSrv (strings + UID bytes in code) |
|---|---|---|
| FLICKERBUFFER / DISPLAY | 0x1028637B / 0x1028637A (stdplugin, openwfc) | **absent** (strings and UIDs) |
| TCSC | 0x10286507 | `tcsc` -> 0x10286507 (UID present) |
| default when no RENDERSTAGES | "flickerbuffer display" | **"platformui platformcompositor"** |
| platformui / platformcompositor / openwf / screen | - | present; UIDs in code: 0x20031CBA (gce), 0x20031CB3 (UMA UI), 0x20031CB5 (NULL UI), 0x20031CB7/B8 (clone) |
| nullui / numaui / clonesource / clonesink | - | present |

[verified: strings; the name->UID pairing is inferred from the co-occurring UID literals; `platformcompositor` ->
gcerenderstage 0x20031CBA is the only candidate that implements `CreateFinalRenderStageL`]. The ROM's
`system/data/Wsini.ini` (rofs1, 2668 B) asks for `PLUGINS sssrenderstage alf compositor platformcompositor` and
`RENDERSTAGES sssrenderstage alf compositor platformcompositor` on SCREEN0 and SCREEN1, with `[alf] ID 0x200184B6`,
`[compositor] ID 0x2002BCF9`, `[sssrenderstage] ID 0x20039F82`, plus `KEYCLICKPLUGIN tactileclickplugin.dll`,
`CHANGETRACKING`, `VIRTUALROTATION`, `WINDOWMODE Color16MAP`, `ABSOLUTEFADING`, `S60_KEYROTATOR -90`, `YSHIFTING 8`,
`SUPPRESSEVENTSTIMEAFTERROTATION 450000`, two screen modes (360x640 / 640x360) [verified]. This answers the
gce doc's open question: EwSrv instantiates 0x20031CBA itself, by its built-in name `platformcompositor`.

**Ini keys.** Belle keys absent from the drop: `DISPLAYCONFIG`, `SUPPRESSEVENTSTIMEAFTERROTATION`. Drop keys
absent from Belle: `DEBUGOSB`, `DISPLAY`, `FLICKERBUFFER`, `FRAMECAPTURE`, `EMULATOR_ROTATE_POINTER_COORDS`,
`TCSC` (as a key; the uid stays) [verified]. `VIRTUALROTATION`, `S60_*` and `YSHIFTING` in wsini are not EwSrv
strings; they are read by other components through MWsIniFile or their own parser (YSHIFTING is in the drop's
server as a key but not a Belle string: Nokia moved it) [verified strings; owner inferred].

**Server classes.** 128 RTTI class names in EwSrv; 8 have no declaration anywhere in the drop:
`CDsaMsgQueue`, `CWsAbortDirect`, `CWsActiveScheduler`, `MWsWindowGroupFromIdentifier`,
`TWalkWindowTreeDebugElements`, `TWalkWindowTreeOrderElements`, `TWalkWindowTreeReCalcChildAbs`,
`TWalkWindowTreeUpdateElements` [verified]. The first three are old (nonnga-era) DSA/scheduler classes that the
201039 NGA tree had dropped and Belle kept or re-added; the walker classes are Belle work on the window-element
tree (the alf/compositor stages' element ordering). Drop classes with no Belle RTTI: `CFallbackMap`, `CMessage`,
`CPluginInfo`, `CRedrawSegment`, `CRenderStageManager`, `CShellStarter`, `CWindowElement`, `CWindowElementSet`,
`CWsPassword`, `CWsTop` [verified] - mostly classes without virtuals (no RTTI emitted), so not evidence of
removal, except `CWindowElement`/`CWindowElementSet` (the drop's OpenWF element bookkeeping), which Belle
replaced by the `TWalkWindowTree*Elements` walkers and `CRegisteredSurfaceMap` [inferred].

**Orientation.** Both have `CWsRenderOrienationTracker` and the theme-orientation P&S logging strings
[verified]; Belle adds `BLANKSCREENONROTATION`/`SUPPRESSEVENTSTIMEAFTERROTATION` handling and
`rotationutils.h` [verified strings].

**Key routing.** Both load `keyrouter.dll` (the drop's keyeventrouting plugin, same uid3 0x102872E2 as the
firmware's) by default name, overridable with `KEYROUTERPLUGIN` [verified].

## 5. Verdict per DLL

- **Ws32.dll (client)**: an EPL-201039 build is **not** ordinal-compatible: 10 used ordinals above 661, 8 of them
  imported by Cone.dll [verified]. Names agree on all 375 ordinals both the def and the SDK have.
- **WsGraphicDrawer.dll**: **ordinal-compatible** (105 = 105, all 62 used ordinals named by the def) [verified].
  A rebuild is possible but pointless: it is the plugin base-class library and the firmware's stages already
  match it.
- **EwSrv.exe**: no export ABI to break (0 exports, 1139 imports all from EPL-era libraries) [verified]. The
  drop's NGA server would load the same plugins through the same 0x10285A29 interface and the same
  `CWsRenderStage`/`CWsPlugin` base ABI, **but** its built-in stage table (flickerbuffer/display, stdplugin
  0x10286378, which is not in the firmware) and default chain differ from Belle's (platformui platformcompositor,
  gce/UMA/NULL/clone UIDs), it lacks `DISPLAYCONFIG`, `SUPPRESSEVENTSTIMEAFTERROTATION`, the Belle element
  walkers and whatever the 662+ client ordinals talk to on the server side (new opcodes in the `!Windowserver`
  protocol: unknown count) [verified strings; opcode delta inferred].
- **Render stages / plugins**: gcerenderstage, umaui, clone, sss, alf, alfcompositorrs, aknextendedfader: all
  Nokia, no source; they compile against exactly the drop's `CWsRenderStage`/`CWsPlugin` ABI and interface ids
  [verified for the imports; ids verified for gce only].

## 6. Recommendation for the component `wserv`

**retain** EwSrv.exe, Ws32.dll and WsGraphicDrawer.dll as shipped; keep the EPL drop as the exact reading copy
of the plugin ABI (`WSGRAPHICDRAWERINTERFACE.H`, `WsRenderStage.h`, `WsRenderStageFactory.h`,
`GRAPHICDRAWERU.DEF`), which it is [verified for every ordinal the firmware uses]. Reasons:

1. The seam the port needs is **below** the server: `CreateFinalRenderStageL` -> gcerenderstage -> `gce.dll` ->
   our `src/gcebe`. Nothing the port must change is inside EwSrv; the server already drives whatever final
   stage ECom gives it.
2. `build-source` is ruled out for Ws32.dll (Cone would not load) and would regress the server (section 5).
3. `adapt-source` of the server is possible in ABI terms (same plugin interface, same base library, same ini
   grammar) but buys nothing until a Belle server behaviour must change; if that day comes, the cheapest route
   is still a code patch to the binary (`code-patches.txt`), or `wsini.ini` edits (the chain is
   configuration: a `[platformcompositor] ID 0x...` section can point SCREEN0 at a stand-in stage with no
   server change at all).
4. `replace-interface` applies only to the **render stage**, not to the server: a stand-in ECom plugin for
   0x10285A29 implementing `CreateFinalRenderStageL` + `MWsScene`(36) + `MWsContentReadyForComposition`(37) +
   `MWsDisplayControl`(0x10286497) against the drop's headers would let us remove gce.dll's dependence on the
   VideoCore compositor. That is the `ws-render-stage` contract's job, already partially documented.

Cost driver if the owner wants the source anyway: the 16 post-drop Ws32 exports and their server opcodes
(Cone, xn3layoutengine, gfxtransadapter depend on them) and Belle's element walkers; both need RE of EwSrv.
Not recommended.

## 7. Next bounded question (only if a stand-in render stage is attempted)

Which object-interface ids above 37 (if any) and which `MWsScene`/`MWsElement` methods does Belle's
`alfcompositorrs.dll` (impl 0x2002BCF9, the stage before `platformcompositor`) resolve from its `Next()` stage?
That fixes the minimal surface a stand-in final stage must present to the alf chain. One Ghidra pass over
alfcompositorrs.dll's `ResolveObjectInterface` callers (~a few hours), register against contract
`ws-render-stage`.

## Method

- Ordinal sets: `imports.json` (`modules[*].imports` = [dll, ordinal, offset, section]); SDK names: its `dlls`;
  EPL defs parsed as `symbol @ ordinal`; export counts and module versions from `core.json` (XIP) and the E32
  header words at 0x18/0x20 of the ROFS extractions (`tools/rofs.py --extract`, `tools/e32load.py`).
- ECom: `tools/ecom_index.py iface 0x10285A29`; IPC: `tools/ipc_index.py server '!Windowserver'`.
- Strings: `strings -el` / `strings` on the decompressed EwSrv.exe; UID literals by little-endian byte search.
- No decompilation, no run.
