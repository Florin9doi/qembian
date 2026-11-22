# UI tree from memory: the gdb reader (`sydb ui --gdb`)

Internals. For using `sydb ui` start with [ui-automation.md](ui-automation.md).

`sydb ui` now asks the app itself first: the in-app probe, [ui-probe.md](ui-probe.md). This page
describes the second backend, used with `--gdb`, or automatically when an app does not answer the
probe (started before the probe was installed, or not an Eikon app). It needs nothing on the phone.

A `uiautomator dump` for unmodified S60 apps. It gives the title, softkeys, the open menu and its
selection, the focused list or grid with its items and current index, editor text, and the control tree
(class, screen rectangle, texts). Nothing is injected into the phone and no guest code runs: the phone is
frozen for ~0.3 s while gdb reads the app's own CONE/Avkon objects. One dump takes 0.6-0.8 s.

```
sydb ui [--all] [--json] [--raw] [--tid HEX] [--items N]   tree + summary (hidden controls with --all)
sydb ui select TEXT [--open]      move the highlight of the open menu / focused list / grid to TEXT
                                  (arrow keys, checked with a fresh dump after each batch); --open = OK
sydb ui wait TEXT [--timeout S]   until TEXT is visible (labels, title, softkeys, list and menu items)
```
MCP: `ui_tree` (summary + tree, ~5 KB of JSON), `ui_select`, `ui_wait`. Code: `tools/sydb/uidump.py`,
tests: `tests/uidump_test.py`.

Example (N95 8GB, Menu with Options open):
```
# Z:\sys\bin\menu2.exe uid 0x101f4cd2 (thread 0x154), CMenuAppUi; 52 controls, 2592 objects read
# title: Menu
# softkeys: {'left': 'Select', 'right': 'Cancel'}
# focused: CEikMenuPane
# list: CXn3DGrid, 12 items, current 4 = 'Gallery'
CViewControl  [0,54 240x241] window
  CXnGridAdapter  [1,55 238x239]
    CXn3DGrid <CAknGrid>  [1,55 238x239]
      list: CAknGridM, 12 items, current 4, top 0 (CDesC16ArrayFlat)
       >  4 Gallery ...
CEikMenuPane  [0,128 240x167] focused window
   >  2 Change Menu view  (cmd 0x1776)
      7 Memory details  (cmd 0x177b, submenu) ...
```
A flow is a few commands, each checked against the dump, not a screenshot:
`ui select Tools --open; ui wait Settings; ui select Settings --open; ui wait General` (4 s).

## How it reads the app

1. **Which app:** the agent's `wg` gives the focused window group's thread id. The thread's `DThread` is
   found by scanning the kernel heap for the DThread vtable (as `kwalk.py`). Its `DMemModelProcess` gives
   the process's page directory (`iLocalPageDir`).
2. **Another process's memory:** the phone is stopped in whatever thread was running. User addresses
   of the app are translated by walking its page directory and page tables (section, small and large
   pages), and physical memory is read through QEMU's gdbstub (`maint packet Qqemu.PhyMemMode:1`).
   ROM and kernel addresses are global and read directly.
3. **CCoeEnv:** CONE keeps `CCoeEnv::Static()` in DLL TLS, which on EKA2 is the kernel's
   `DThread::iTls` (`STls {handle, uid, ptr}`). The pointer whose class is a CCoeEnv is the app's
   environment, and its first members include the AppUi (`CCoeAppUi`: `iStack` +0xc = control stack).
4. **Class names without symbols:** the ROM's vtables carry C++ RTTI: `vtable[-1]` is the typeinfo
   (`14CCarouselAppUi`, `9CEikonEnv`), `vtable[-2]` the offset to the complete object (used for
   interface pointers such as `MDesCArray*`), and `__si_`/`__vmi_class_type_info` give the base classes.
   So "is a CEikListBox" holds for Nokia's own subclasses (`CXn3DGrid`, `CAknSettingStyleListBox`) with
   no per-firmware class table.
5. **Which objects:** breadth-first over the heap cells reachable from the env, the AppUi and the stack
   entries (a cell is a pointer whose RHeap length word at -4 is sane). It passes through up to 4 typed
   non-control objects, and through raw cells (array buffers) only straight from a typed object. It
   stops at env/AppUi hubs and is capped at 6000 objects. Every object whose class derives from
   CCoeControl is kept.
6. **Tree:** a control's parent is CONE's own (`CCoeControl::Parent()`: the per-control data store
   `iData`, key 0; the component `CCoeControlArray` is key 2). A control with no CONE parent that does
   not own a window hangs under the control whose memory pointed to it. A window-owning one is a root
   (menu panes, dialogs, CBAs). Screen rectangles: a control's position is relative to the window it
   draws in (its nearest window-owning ancestor), and a window's position is relative to its parent window.
7. **Semantics** (adapters): `CEikListBox`/`CAknGrid` view (current/top/bottom) and model.
   `CTextListBoxModel`'s item array is read when it is a `CDesC16ArrayFlat` (HBufC*) or a `CPtrC16Array`.
   For an app's own `MDesCArray` (Messaging's `CMceListItemArray`, Settings' `CGSListBoxItemTextArray`)
   the text is built by code, so the tool collects the descriptor-headed strings (TBuf/TBufC/HBufC)
   up to 3 hops from the array object. These come as `strings_heuristic`: in memory order, usually the
   list order, and they may include hidden rows. `CEikMenuPane` gives items (command id, cascade, text)
   and the selection. `CEikEdwin` gives its text (`CPlainText` flat or segmented buffer). Any control
   shows the HBufC strings among its members (labels, title).

## Offsets (N95 8GB RM-320, S60 3rd FP1 / 9.2), from the ROM's own accessors

Read with `tools/syb_sym.py fn MODULE ADDR` (Ghidra) on exported accessors. The SDK header's
`CCoeControl` layout is 4 bytes off from the real one.

| What | Offset | Accessor |
|---|---|---|
| CCoeControl position, size | +0x14, +0x1c | `Position()`, `Size()` |
| CCoeControl flags | +0x24: 0x4 owns window, 0x8 focused, 0x10 activated, 0x20 invisible, 0x40 dimmed, 0x1000 blank | `OwnsWindow()`, `IsFocused()`, `IsActivated()`, `IsVisible()`, `IsDimmed()`, `IsBlank()` |
| CCoeControl window, data store | +0x28, +0x2c (store: 64-bit nibble map key->slot, slots at +8; key 0 parent, 2 component array) | `DrawableWindow()`, `Parent()`, `CountComponentControls()` |
| CCoeControlArray | RArray<{ctl, id}> at +0xc | `Count()`, `At()` |
| CEikListBox view, model | +0x60, +0x68 | `View()`, `Model()` |
| CListBoxView top, bottom, current | +0x14, +0x18, +0x20 | `TopItemIndex()`, `BottomItemIndex()`, `CurrentItemIndex()` |
| CTextListBoxModel item array | +8 | `ItemTextArray()` |
| CEikMenuPane items, selected | +0x70 (CArrayPtrFlat of items, item data +0xc: cmd, cascade, flags, TBuf text), +0x80 | `NumberOfItemsInPane()`, `ItemDataByIndexL()`, `SelectedItem()` |
| CEikEdwin text | +0x70 -> CPlainText, byte store +0x10 | `Text()`, `CPlainText::DocumentLength()` |
| CArrayFixBase | count +4, entry length +0xc, CBufBase +0x14 (CBufFlat data +0x10) | |

Softkeys: the CBA highest on the control stack. Its activated buttons are left/right by which half
they are in. A labelled button that is not activated is the middle softkey, which 3rd FP1 phones
do not draw.

## Limits and next steps

- Checked on the N95 8GB (RM-320): Menu grid and Options menu, Messaging, Settings (list, tabs,
  setting list), the Bluetooth name query (dialog, editor), the screensaver. Other 9.x firmware should
  mostly work (RTTI and CONE's data store are the same), but the offsets in the table are per
  release: check them with `syb_sym.py fn` before trusting a new firmware. The 5th-edition/Symbian^3
  CONE has not been tried.
- Multiple memory model only (ARMv6 page tables). Not the moving model (SE P1i, `syborgmv`) or hosted EKA2.
- The thread scan covers the kernel heap at 0xc8000000-0xc9200000 (enough on the N95). The tool
  attaches and detaches the session's gdb, so breakpoints set in it are dropped.
- Not read yet: listbox arrays in segmented buffers (`CDesC16ArraySeg`), item rectangles inside lists
  and grids (index and top only), the status pane's server-side panes (signal, battery, clock live in
  EikSrv), non-CONE UIs (games, Flash, Java, OpenGL apps), and text drawn straight to the window
  without a member string.
- `select` refuses lists known only heuristically, so a wrong order never leads to a wrong press. An
  opt-in that uses the harvested order is a small step: it matched the screen in Call settings.
- Next steps: a `ui tap/press` by control (keys found by focus order), events (`wait` for a class or a
  focused control), and calling `MdcaPoint` inside the app for dynamic arrays (a debugger "function
  call" on the app's UI thread at a safe point such as `CActiveScheduler::WaitForAnyRequest`).
