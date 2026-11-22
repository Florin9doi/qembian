# Belle FP2 (Nokia 808): can the shipped Qt and the Avkon/Uikon/CONE stack be rebuilt from source?

Scope: device `nokia-808-rm807`, firmware `RM-807/113.010.1508/04.01+C00.01`. Static work only
(imports.json, the DLLs' E32 headers and strings, the local source drops and SDK export libraries).
Written 2026-11-19 for the registry components `qt` and `avkon` (both `undecided`); the owner decides the
strategy. Every number below comes from
`build/roms/nokia-808-rm807/RM-807_113.010.1508_04.01+C00.01/imports.json` (schema 3, ordinal names from
`Nokia_Symbian3_SDK_v1.0/epoc32/release/armv5/lib`) unless marked otherwise.

## 1. What the firmware ships

| fact | value | status |
|---|---|---|
| Qt version | **4.8.1**: string `4.8.1` in QtCore and QtGui; E32 module version `4.2049` = 4.(0x801) = 4.8.1 in QtCore, QtGui, QtDeclarative, QtWebKit | [verified] |
| Qt build key | `Symbian full-config`, `buildkey`; `qt_lcnsprod=OpenSource` (an open-source-licensed build, not the commercial one) | [verified] |
| Qt export counts (E32 header, `tools/e32load.py`) | QtCore 4172, QtGui 12823, QtDeclarative 1990, QtWebKit 855, JavascriptCore (QtScript's engine) see table | [verified] |
| Avkon stack module version | `10.799` (avkon.dll, eikcoctl.dll); tools version 2.4.0; all in rofs1 except cone/form/egul/bafl/ws32/fbscli/gdi (XIP core) | [verified] |
| uikon.dll | does not exist in this firmware (0 importers); Uikon is eikcore.dll/eiksrv.dll/eiksrvc.dll | [verified] |
| WebkitEngine.dll | Nokia's S60 browser engine (BrowserNG), not QtWebKit: 2860 exports, module version 18.789, libxml2 2.7.6 inside; no SDK library names it | [verified] |

## 2. Inventory: importers and ordinals used firmware-wide

"importers" = modules in the firmware that import the DLL; "used" = distinct ordinals imported by anyone;
"> SDK" = used ordinals that the Symbian^3 SDK v1.0 export library (Qt 4.7.2 era) does not name, i.e.
ordinals added after that SDK (none of the unnamed ordinals is below the SDK's maximum: the ordinal
tables are append-only, as frozen EABI .def files are). "no lib" = the SDK has no library for the DLL.

### Qt 4.8.1 and QtMobility

| DLL | importers | used | max used | exports | > SDK (4.7.2) | who uses the post-SDK ordinals |
|---|---|---|---|---|---|---|
| qtcore.dll | 229 | 1791 | 4170 | 4172 | 56 (3656..4170) | 183 modules, first QtNetwork, QtGui, QtDeclarative, qsymbianbearer |
| qtgui.dll | 105 | 2187 | 12809 | 12823 | 138 (11823..12809) | 21 modules: QtOpenGL, QtOpenVG, peninputserver, cameraapp, nfclauncherserver, QtDeclarative |
| qtnetwork.dll | 25 | 204 | 1165 | - | 74 (999..1165) | 10: QtBearer, qsymbianbearer, webupload |
| qtdeclarative.dll | 73 | 177 | 1919 | 1990 | 4 | qmlparticlesplugin, qmlgesturesplugin |
| qtscript.dll | 2 | 177 | 443 | - | 15 | QtDeclarative only |
| javascriptcore.dll | 4 | 428 | 849 | - | 43 | WebkitEngine, webkittraverseplugin |
| qtwebkit.dll | 2 | 116 | 716 | 855 | 0 | - |
| qtsql.dll / qtxml.dll / qtxmlpatterns.dll | 6 / 7 / 1 | 62 / 87 / 15 | 442 / 457 / 233 | - | 0 | - |
| qtopengl.dll / qtopenvg.dll / qtsvg.dll | 7 / 1 / 8 | 56 / 3 / 11 | 661 / 184 / 102 | - | no lib / 0 / 0 | - |
| qtmultimediakit.dll | 8 | 422 | 1674 | - | 20 | the ecam/mmf engine plugins |
| qtcontacts / qtlocation / qtsensors / qtsysteminfo | 14 / 5 / 6 / 9 | 453 / 532 / 198 / 95 | 965 / 1800 / 342 / 230 | - | 9 / 4 / 21 / 30 | their own backend/declarative plugins |
| qtfeedback / qtorganizer / qtconnectivity / qtgallery | 5 / 3 / 1 / 1 | 43 / 365 / 87 / 86 | 85 / 864 / 568 / 482 | - | no lib | - |
| qtmessaging / qtpublishsubscribe / qtserviceframework / qtversit | 2 / 9 / 19 / 9 | 94 / 16 / 37 / 48 | 392 / 70 / 131 / 157 | - | 0 | - |
| qtversitorganizer / qtmsapi / qtnvgdecoder / qtuiresourceserver | 1 each | 8 / 9 / 4 / 1 | 21 / 123 / 13 / 14 | - | no lib | - |
| webkitengine.dll (S60 browser, not Qt) | 3 | 150 | 153 | 2860 | no lib | browserng.exe and plugins |

All counts [verified].

### Avkon / Uikon / CONE and the OS-side libraries they sit on

"EPL def" = the maximum ordinal in the EABI .def file of the local EPL drop
`~/work/symbian/oss.FCL.sf.mw.classicui` (git `dcea8997`, tag **201039_2, 2010-10-08**); "> def" = used
ordinals above it, i.e. added by Nokia between that drop and Belle FP2 (2012). "names" = for every used
ordinal that the def has, the def's symbol name equals the SDK library's name (append-only check).

| DLL | importers | used | max used | exports | EPL def max | > def (who) | names |
|---|---|---|---|---|---|---|---|
| avkon.dll | 720 | 2078 | 4742 | 4742 | 4577 | **88**, from 162 modules (SecUi 28, mpx/logs/webupload indicator plugins, musui, eikcoctl, akncapserver, videotelui, lcdui, fmradio, ...) | 1990/1990 equal |
| cone.dll | 821 | 398 | 746 | - | 740 | 6 (741 eikcoctl; 742-745 **QtGui**; 746 avkon) | 392/392 equal |
| eikcoctl.dll | 415 | 955 | 2067 | 2067 | 2006 | 38, from 47 modules (avkon 19, eikdlg 11, aknhlist 10, freestyleemailui, Pbk2UIControls, mce, browserng, ...) | 917/917 equal |
| eikcore.dll (Uikon) | 522 | 231 | 594 | - | 594 | 0 | 231/231 equal |
| eikctl.dll | 131 | 327 | 704 | - | 700 | 3 (avkon, eikcoctl, avkonfep) | 324/324 equal |
| eikdlg.dll | 188 | 149 | 296 | - | 296 | 0 | 149/149 equal |
| eiksrv.dll / eiksrvc.dll (Uikon) | 38 / 4 | 53 / 11 | 166 / 29 | - | 181 / 29 | 0 | - |
| eikcdlg.dll | 1 | 4 | 15 | - | 25 | 0 | - |
| aknnotify.dll | 152 | 74 | 154 | - | 151 | 3 (sysap.exe only) | - |
| aknskins.dll (sf.mw.uiresources) | 329 | 105 | 191 | - | 189 | 2 | - |
| aknicon.dll (sf.mw.uiresources) | 204 | 35 | 47 | - | 47 | 0 | - |
| fontutils.dll (sf.mw.uiresources) | 12 | 27 | 29 | - | 27 | 2 | - |
| form.dll (sf.os.textandloc) | 37 | 135 | 321 | - | 321 | 0 | - |
| bafl.dll (sf.os.ossrv) | 1200 | 206 | 475 | - | 475 | 0 | - |
| egul.dll / gdi.dll | 203 / 187 | 71 / 213 | 262 / 397 | - | not checked | - (0 beyond the SDK) | - |
| ws32.dll / fbscli.dll (wserv, fbserv: other components) | 389 / 333 | 387 / 147 | 676 / 284 | - | not checked | 12 / 2 beyond the SDK | - |

All counts [verified]; the "names" column is the cross-check that the EPL def and the SDK library agree on
every ordinal the firmware uses that both have: 0 differences over 4003 ordinals [verified].

## 3. What source is local, and its relation to the firmware

| component | local source | relation | notes |
|---|---|---|---|
| Qt | **none**. Local Qt material is binaries only: `Nokia_Symbian3_SDK_v1.0` (Qt **4.7.2** headers + .dso, `QT_VERSION_STR "4.7.2"`), `~/work/meego-apps/sdk/QtSDK` (desktop Qt 4.8.1 build, Symbian_Anna sis: `Qt-4.7.403-for-Anna.sis`, `QtWebKit-4.8.1-for-Anna.sis`, QtMobility 1.2.1, Belle: QtQuickComponents 1.1 only). No `QtCoreu.def` anywhere on disk | unknown (expected: related) | The firmware's Qt is an open-source-licensed 4.8.1 "Symbian full-config" build [verified]. The public qt/qt 4.8.1 tree carries the Symbian port and the frozen `src/s60installs/eabi/*u.def` files [inferred: from memory of the tree, not checked here]. Whether Nokia's Belle FP2 4.8.1 added exports beyond the public 4.8.1 defs (candidates: QtGui 11823..12809 used by QtOpenGL/QtOpenVG/peninputserver, QtNetwork 999..1165 used by the bearer plugins) is **not established**: it needs the public def files, which are not local. |
| Avkon/Uikon/CONE | `~/work/symbian/oss.FCL.sf.mw.classicui` at 201039_2 (2010-10-08): uifw/AvKon (avkon), uifw/EikStd (eikcoctl, eikdlg, eikcdlg), uifw/eikctl, commonuisupport/uikon (eikcore, eiksrv, eiksrvc), lafagnosticuifoundation/cone, plus `sf.mw.uiresources` (aknskins, aknicon, fontutils), `sf.os.textandloc` (form), `sf.os.ossrv` (bafl). The registry seed names `oss.FCL.sf.os.ui`; the actual drop is `sf.mw.classicui` | related: same lineage, **two years older** (Symbian^3 PDK 2010.39 vs Belle FP2 2012), names agree for every ordinal both have | Nokia appended 88 avkon, 38 eikcoctl, 6 cone, 3 eikctl, 3 aknnotify, 2 aknskins, 2 fontutils exports after the drop, and the firmware uses all of those listed. For eikcore, eikdlg, eiksrv(c), eikcdlg, aknicon, form, bafl the firmware uses nothing beyond the drop's def. |
| WebkitEngine (S60 browser) | `~/work/symbian/oss.FCL.sf.mw.web` (201039): builds `browserengine.dll` (def max 52) and `webkitutils.dll` (def max 56) | different library (firmware: WebkitEngine.dll, 2860 exports, 153 used) | not a Qt question; belongs to a browser component if one is ever opened |

## 4. Ordinal-compatibility verdict per DLL

The test: a DLL built from a source tree keeps ordinal compatibility with the firmware's importers if and
only if its frozen .def covers every ordinal they import, with the same meaning.

- **Qt (all modules)**: the SDK's 4.7.2 libraries name every used ordinal up to their own maximum and
  nothing used lies below it unnamed, so the tables are append-only from 4.7.2 to the shipped 4.8.1
  [verified]. A public **4.8.1** build would therefore serve every ordinal the public 4.8.1 defs have; the
  post-4.7.2 ordinals (QtCore 56, QtGui 138, QtNetwork 74, QtDeclarative 4, QtScript 15, QtMultimediaKit 20,
  QtSensors 21, QtSystemInfo 30, QtContacts 9, QtLocation 4, JavascriptCore 43) are either public 4.8.1 /
  QtMobility 1.2.x additions or Nokia-private Belle additions; which, is unknown without the public defs
  [inferred: most are public 4.8/1.2 additions; the QtGui tail used only by QtOpenGL/QtOpenVG/peninputserver
  is the likeliest Nokia-private part]. A **4.7.x** build is ruled out: 183 modules would fail to load.
  QtWebKit/QtSql/QtXml/QtXmlPatterns/QtSvg/QtOpenVG and the small QtMobility modules use nothing beyond
  the SDK and are compatible with any 4.7.2+/1.2+ build [verified].
- **avkon.dll**: an EPL-201039 build is **not** compatible: 88 used ordinals above 4577, needed by 162
  firmware modules (and not the obscure ones: SecUi, akncapserver, eikcoctl, lcdui, musui) [verified].
- **eikcoctl.dll**: not compatible (38 missing, needed by avkon.dll itself and 46 others) [verified].
- **cone.dll**: not compatible as is: 6 missing, and 4 of them (742-745) are imported by **QtGui**, 746 by
  avkon [verified]: every Qt app and every Avkon app would fail to load against an EPL cone.
- **eikctl.dll, aknnotify.dll, aknskins.dll, fontutils.dll**: 2-3 missing each, used by avkon/eikcoctl/
  avkonfep, sysap, respectively [verified].
- **eikcore.dll, eikdlg.dll, eiksrv.dll, eiksrvc.dll, eikcdlg.dll, aknicon.dll, form.dll, bafl.dll**:
  ordinal-compatible with the EPL drop (nothing used beyond the def, names equal) [verified]. Behaviour
  parity is a separate question that this static check cannot answer.

## 5. Recommendation per component

**qt: retain** (keep the shipped binaries). Reasons: the Qt in the ROM is an open-source-licensed 4.8.1
build whose ordinal tables are append-only from 4.7.2 and are fully covered by the DLLs we have; nothing we
need to change for the port is *inside* Qt (EGL/OpenVG/OpenGL ES, the graphics system, bearer, sensors and
the modem all sit underneath it and are already replaced at their own interfaces); and no Qt source tree is
local. Building 4.8.1 from the public tree would reproduce at best the same DLLs, and at worst DLLs missing
Nokia's post-4.8.1 exports (QtGui tail, QtNetwork tail: unknown until the public defs are compared). If a Qt
module ever has to change, the path is `adapt-source` of public 4.8.1 for that one module, gated on the
def comparison below; it is not needed now. The registry seed's "4.7.x-belle?" should read "4.8.1
(open-source build, Symbian full-config)".

**avkon: retain** the whole stack as shipped, with the EPL drop as the reading copy (its names are exact for
every ordinal both have). `build-source` from the local 201039 drop is ruled out for avkon, eikcoctl, cone,
eikctl, aknnotify, aknskins, fontutils (section 4), and would not be Belle anyway (two years of UI changes:
the Belle status bar, pop-ups, transitions). The ordinal-clean members (eikcore, eikdlg, eiksrv, eiksrvc,
eikcdlg, aknicon, form, bafl) *could* be rebuilt, but there is no reason to: nothing the port needs lives in
them. `adapt-source` only if a specific Belle behaviour inside one DLL must change, and then as a patch to
the binary first (code-patches.txt) unless the DLL is in the ordinal-clean list. `replace-interface` does
not apply: these are in-process libraries with 400-1200 importers each, not servers with a protocol.

Neither component needs a research task beyond this page; the "229 modules import QtCore" fan-in is a
dependency fact, not work.

## 6. Next bounded question (only if someone wants to change a Qt or Avkon DLL)

Compare the firmware's export counts (QtCore 4172, QtGui 12823, QtDeclarative 1990, QtWebKit 855) and the
post-4.7.2 ordinal lists above with the public `qt/qt` **v4.8.1** `src/s60installs/eabi/*u.def` files (and
QtMobility 1.2.1's). Count only: if the public defs reach the firmware's maxima, the shipped Qt is the public
4.8.1 and `adapt-source` is open for any module; if not, the difference is Nokia-private and `retain` is the
only option for that module. Half an hour with the trees at hand; needs a download, so it was not done here.
Nothing of the kind exists for Avkon: no Belle-era classicui source was published.

## Method

- `tools/rofs.py rofs1.img --extract sys/bin/X` + `tools/e32load.py` (decompression, export counts), E32
  header fields parsed from the extracted images, `strings` on the decompressed code.
- Ordinal sets from `imports.json` (`modules[*].imports` = [dll, ordinal, offset, section]); the SDK names from
  `imports.json` `dlls`; EPL def files parsed as `symbol @ ordinal`.
- No decompilation, no run.
