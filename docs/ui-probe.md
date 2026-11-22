# uiprobe: the UI tree from inside the app (`sydb ui`, default backend)

Internals. For using `sydb ui` (commands, output, recipes, platforms) start with
[ui-automation.md](ui-automation.md).

`sydb ui` asks the foreground app itself for its UI. A small DLL inside every Eikon/Avkon app walks the
app's controls through their own API, on the app's UI thread, between two events, and sends a compact
text dump back through the agent. One dump takes about 0.12-0.25 s and the phone keeps running. Commands,
output and the summary are the same as with the memory reader ([ui-dump.md](ui-dump.md), now `--gdb`).

```
sydb ui [--all] [--json]          tree + summary (probe; falls back to the gdb reader if the app does not answer)
sydb ui --agent | --gdb           only the probe | only the memory reader
sydb ui select TEXT [--open]      highlight TEXT in the open menu / focused list / grid (keys, checked)
sydb ui wait TEXT [--timeout S]   until TEXT is visible
sydb agent ui [TID] [MS]          the raw dump lines (format below)
tests/uiprobe_smoke.py [--device ID]   on-phone test (throw-away session, ~45 s on the N95); the platform sweep
```

## How the probe gets into every app

`CEikonEnv` loads every ECom implementation of the Uikon library interface `CEikLibrary2`
(0x10004cc1; read from EikCore, `CEikonEnv` construction) and calls their `InitializeL()` /
`PostAppUiInitializeL()`. On the N95 only Avkon's `AknInit` implements it. `uiprobe.dll` (C:\sys\bin) +
`uiprobe.rsc` (C:\resource\plugins) is a second one, so every Eikon app and Eikon-based server loads
it. There is no kernel change, loader change or injection, and the phone's binaries are untouched.
`sydb install` / `start --seed` put it on C:, and `update-agent` updates it for apps started afterwards.

Once the active scheduler exists, the plugin adds one active object to the UI thread. It waits on a
P&S property in the agent's category (devagent's SID 0xE0001DB7, defined by the agent at start with
pass policies):

| Key | Type | Who sets it | Content |
|---|---|---|---|
| 0x7569 | byte array | agent (`ui`) | `"<tid hex> <seq>"` |
| 0x756a | large byte array (64 KB) | the probe in thread `<tid>` | `"<seq>\n"` + dump lines |

Every probe wakes, only the one whose thread id matches works, and the agent returns the lines as list
items. The target is the focused window group's thread unless one is given.

## What it reads (all on the UI thread)

- **Controls:** three passes, each control once:
  1. the app's window tree (`RootWin().Child()` / `NextSibling()`): CONE creates every window with the
     owning `CCoeControl*` as its client handle;
  2. the status pane's window group (`CEikStatusPaneBase::WindowGroup()`);
  3. the control stack (`CCoeAppUi::iStack`, +0xc, checked by its RTTI name: no public accessor in 9.2);
  4. then every live control in the heap (`RHeap::DebugFunction(EWalk)`: allocated cells whose object
     is a `CCoeControl`), placed under `Parent()`. This finds the Menu's grid view, which sits behind a
     window whose client handle is a bare `CCoeControl` without the window.

  Children come from `CountComponentControls()` / `ComponentControl()`; position from `Position()`
  (the host adds window positions; `PositionRelativeToScreen()` faulted in Ws32 on a Menu control);
  flags from `OwnsWindow`, `IsFocused`, `IsActivated`, `IsVisible`, `IsDimmed`.
- **Class names:** from the vtable's RTTI (`vtable[-1]`). "Is a" checks and base offsets come from
  `__si`/`__vmi_class_type_info`. A class whose `CCoeControl` base is not its first base (BtUi) gets
  its offset added.
- **Semantics:**
  - list boxes and grids: `Model()->NumberOfItems()`, `CurrentItemIndex()`, `TopItemIndex()`,
    `BottomItemIndex()`, grid `NumberOfColsInView()`, and for text models `ItemText(i)`, which also
    gives the rows apps build in code (Messaging, Call settings: exact and in order, so `select`
    works on them);
  - menu panes: `NumberOfItemsInPane()`, `ItemDataByIndexL(i)` (index checked, so it cannot leave),
    `SelectedItem()`;
  - labels: `CEikLabel::Text()`; title pane: `CAknTitlePane::Text()`; editors: `CEikEdwin::GetText()`.
- **Pictures** (`B` lines, shown as `icon=`): `CEikImage::Bitmap()`, the icon arrays of grids and lists
  (`ItemDrawer()->FormattedCellData()` / `ColumnData()->IconArray()`), and `CFbsBitmap`/`CGulIcon`
  pointers among a control's own members (tabs, the Calculator's buttons), each checked under the guard.
  - **Identity that survives restarts:** Avkon's icon record, read from AknIcon.dll's own code: the
    `CAknBitmap` at +0x1c points to it; +0x8 file name, +0xc bitmap id, +0xe mask id, +0x18 back to the
    bitmap; flag 1 = `z:\resource\apps\` stripped, flag 8 = extension stripped, ids from 0x4000 are MIF.
    That gives `Calcsoft.mif#16414`, `phonebook_aif.mif#16384`. For other pictures, an FNV-1a hash of
    the pixels (`240x225:b7e57d33`: the same picture at the same size and theme).
  - **Labels** (host): `tools/sydb/icon_names.json` (hand-made, `"File.mif#id"` or a hash → label), the
    SDK's `avkon.mbg` (Avkon ids → `qgn_` names, read at run time), and app icons `<exe>_aif.mif` → the
    app's caption (agent `apps`, cached in `<session>/ui-apps.json`). Items that are only icon indices
    (the idle screen's shortcut grid: "0".."6") are named by their icons: "Contacts", "Bluetooth"...
- **Drawn text** (`W` lines, `drawn:` in the tree, `screen_text` in the summary): text a control draws
  without keeping it as a string (screensaver clock, Calculator display, idle-screen plugins).
  - **Capture:** during the dump, the app's own `CWindowGc` (`CCoeEnv::SystemGc()`) gets a heap copy of
    its vtable whose `DrawText` (2), `DrawTextVertical` (2) and `Activate` entries record, then call the
    real ones. The slot numbers come from C++ member pointers (the SDK layout), not constants.
  - **Trigger and placement:** each visible top-level window redraws once (`DrawNow()`), then the
    vtable goes back (also after a fault). The host puts each text on the deepest visible control under it.
  - **Not caught:** text drawn into an off-screen bitmap (`CFbsBitGc`), or with another gc.
- **Path ids** (`--ids`, `path` in JSON): class names from the root with `[n]` among same-class siblings
  (`/CIdleMainView/CIdleControl/CAIPluginContainerControl/CShortcutPlugin/CShortcutPluginGrid`), the same
  after a restart.
- **Controls that are not activated:** node only, no virtual calls (a menu pane never opened crashed
  in its own `CountComponentControls`). Everything on screen is activated.

## Safety: it must never kill the app

- **No leaves:** the DLL is built without exception tables (GCC, no unwinder for RVCT's), so a leave
  through it aborts the app. It calls only non-leaving functions, or leaving ones in a way that
  cannot leave. `CEikStatusPaneBase::ControlL` left in akncapserver although `PaneCapabilities` said
  present, so it is not used.
- **Fault guard:** while a dump runs, a user exception handler (`User::SetExceptionHandler`,
  `KExceptionFault`) is installed.
  - **On a fault:** the handler finds the kernel's exception frame on the stack ([type, code, FAR,
    FSR, CPSR, r0-r15], 21 words, saved SP = frame + 84). It points the saved PC at
    `uiprobe_fault_land`, with the current guard's buffer in r0. EUSER's exception entry restores
    r0-r15 from that frame, and the guard's setjmp returns 1.
  - **Scope:** guards sit around each control, each heap cell check (inside the heap walk, with the
    heap locked: the callback neither allocates nor calls the heap) and the whole dump. A bad object
    becomes `X fault in control N` instead of a dead app.
  - **Self-check:** each dump starts with a test fault, and the `G recovered` line proves the guard works.
  - **Trap:** the frame pointer must be laundered (`asm volatile`). Writing through a pointer derived
    from a local's address is UB, and GCC dropped the frame writes, so the app faulted forever until
    ViewSrv killed it.
- **Capabilities:** All-TCB, as CONE. A DLL needs every capability of the process it loads into and
  may link only to DLLs that have all of its own. With All it could not load (CONE lacks TCB), and
  akncapserver died at boot (-46).
- **No writable static data:** state lives on the stack and heap; `Dll::Tls` comes from our entry stub
  (`startup.s`, as src/khr).

## Line format

```
G  recovered                                           fault guard self-test
A  exe  sid  appui-class
C  id  parent  class  kind  x  y  w  h  flags          x, y = Position(); flags 4 window, 8 focused,
                                                       0x10 activated, 0x20 invisible, 0x40 dimmed
S  id...                                               control stack, highest priority first
T  id  text   |   E  id  text                          label/title text   |   editor contents
L  id  count  current  top  bottom  cols  model        list box / grid
I  id  index  text                                     list item (ItemText)
M  id  selected  count   |   N  id  index  cmd  cascade  flags  text   menu pane and its items
B  id  slot  w  h  mode  hash  file  bmp  mask          a picture (slot: image | icon[i] | m[offset])
W  id  x  y  w  h  text                                text drawn in control id's window (window coords)
X  why                                                 cut (buffer full) or a control faulted
```
Text is escaped `\\ \t \n`, and `\uXXXX` outside printable ASCII.

## Limits and next steps

- **Eikon apps only:** apps started before the probe was installed answer only after a restart. Games,
  Java and Flash have no CONE tree.
- **A busy app answers late:** an app answers only when its UI thread is idle. One loading plugins
  (GSApp start) can miss the agent's 3 s, and `select` and `wait` retry.
- **Kept internals:** the control stack offset (+0xc, RTTI-checked) and the UREL heap cell header
  (4 bytes). Everything else is API. Other 9.x firmware should work as is; Symbian^3's CONE is untried.
- **Not yet:**
  - a `ui press CONTROL` (focus by control);
  - pointer taps by rectangle on touch phones;
  - status-pane icons drawn by other processes (signal, battery);
  - item rectangles inside lists.
