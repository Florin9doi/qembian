# UI as data: `sydb ui` (the uiautomator of qembian)

`sydb ui` tells you what is on the phone's screen as data rather than pixels: the foreground app, its
title, the softkeys, the open menu and its selection, the focused list or grid with its items and
highlight, the editor's text, every text drawn on screen, the pictures (by icon file and id), and the
control tree with screen rectangles and ids that stay the same after a restart. `sydb ui select` and
`sydb ui wait` drive the phone on top of it. Each step is checked against the data, not against a
screenshot.

It works on unmodified S60 apps. Checked on the N95 8GB (S60 3rd FP1): Menu, Options menus,
Messaging, Settings and its plugin views, Calculator, Clock, Bluetooth dialogs, the idle screen, the
screensaver, akncapserver. Other platforms: see [Platforms](#platforms).

| Backend | How | Speed | Docs |
|---|---|---|---|
| **probe** (default) | `uiprobe.dll`, an ECom plugin that every Eikon app loads, walks the app's controls through their own API on the UI thread and answers through the agent | ~0.15-0.25 s, phone keeps running | [ui-probe.md](ui-probe.md) |
| **gdb** (`--gdb`, and the fallback when an app does not answer) | gdb freezes the phone and reads the app's objects from memory (page tables, RTTI, CONE's data store) | ~0.7 s | [ui-dump.md](ui-dump.md) |

## Quick start

```
python3 tools/sydb/sydb.py phone n95                 # the phone (a window); then, in another terminal:
S="python3 tools/sydb/sydb.py --instance n95"
$S ui                                                # summary + tree of the focused app
$S ui --json                                         # the same as JSON (summary + tree)
$S ui --ids                                          # + each control's path id
$S ui --all                                          # + hidden controls (other views, closed panes)
$S ui select Messaging --open                        # highlight "Messaging" in the menu/list/grid, press OK
$S ui wait Inbox --timeout 10                        # until "Inbox" is on screen
$S ui --gdb                                          # the memory reader instead of the probe
$S agent ui                                          # the probe's raw lines (format: ui-probe.md)
```
Install: `sydb install` (phone stopped), `sydb start --seed ...`, or `sydb update-agent` on a running phone.
`update-agent` reaches only apps started afterwards: restart the phone (`sydb phone NAME --restart`) to
put the new probe into every app.

Example (the idle screen):
```
# Z:\sys\bin\Idle.exe uid 0x101fd64c (thread 0x97), CIdleAppUi; 69 controls; via the in-app probe
# title: Offline
# softkeys: {'left': 'Messaging', 'right': 'Camera'}
# focused: CShortcutPluginGrid
# screen_text: ['Offline', 'Fr 11/01/2008', 'Search: Internet and My content', 'No calendar entries today', ...]
# list: CShortcutPluginGrid, 7 items, current 0 = 'Contacts'
CIdleMainView  [0,54 240x266] focused window
  CIdleControl  [0,54 240x241] focused
    CAIPluginContainerControl  [0,124 240x225] focused window  icon=240x225:b7e57d33
      CShortcutPlugin  [0,124 240x37] focused
        CShortcutPluginGrid <CAknGrid>  [4,126 232x32] focused
          list: CAknGridM, 7 items, current 0, top 0
           >  0 Contacts
              1 Messaging ...
      CCalendarPlugin  [0,182 240x21]
        CPluginItem  [0,182 240x21]
          CEikLabel  [23,185 212x17]  drawn: "No calendar entries today"
```
Tree lines: `class <kind it is a subclass of>  [x,y wxh]  flags  "texts"  edit='...'  icon=...  drawn: "..."  {path}`.
Flags: `focused`, `dimmed`, `window` (owns a window), `hidden`.

## For AI agents (MCP and scripts)

MCP tools (server `tools/sydb/mcp.py`): `ui_tree` (summary + tree, ~5 KB; `tree=false` for the summary
only), `ui_select {text, open}`, `ui_wait {text, timeout}`. Python: `tools/sydb/uidump.py` (`capture()`,
`select()`, `wait_for()`).

- **Where am I / what is selected:** `ui_tree tree=false`. Use the summary's `app`, `title`, `focused`,
  `list.selected`, `menu.selected` and `softkeys`. Prefer it to a screenshot: it is exact and small.
- **Navigate:** `ui_select` + `ui_wait`, one step at a time. `ui_select` presses arrow keys, checks the
  highlight after each batch, retries when the app was busy, and settles before OK. In a grid whose
  columns are known it moves by rows and columns.
- **Read the screen:** `screen_text` is every text drawn on screen, top to bottom (labels, list rows,
  clocks, displays). `editor` is the focused editor's text. Use it to check what was typed: digits
  go to the FEP as keypad keys (`sydb text 95` gave "Wj").
- **What a button or icon is:** `icons[].label` / `icon=...` (see Identifiers). An unlabelled picture
  still has a stable `id`: compare ids, or add a label.
- **Still take a screenshot** for pixels that are not text or icons (games, camera, video, the Menu's
  3D effects), and when the dump says it fell back to gdb and something looks wrong.

### Summary keys

| Key | Meaning |
|---|---|
| `app`, `uid`, `appui`, `backend` | the exe, its UID3, its AppUi class, `agent` (probe) or `gdb` |
| `title` | the title pane text |
| `softkeys` | `{left, right, middle}`: the CBA highest on the control stack; middle = the middle softkey, which 3rd FP1 phones do not draw |
| `focused` | class of the focused control |
| `editor` | text of the focused editor |
| `menu` | the open Options menu: `{items, selected}` |
| `list` | the focused list or grid: `{class, grid (columns or true), count, current, selected, items}`; `strings_heuristic` instead of `items` from the gdb reader when the rows are built by code |
| `screen_text` | every drawn text on screen, top to bottom, deduplicated (probe only) |

### Node fields (JSON `tree`)

`class`, `kind` (the Avkon/Eikon base it is, e.g. `CAknGrid`), `rect` (screen x, y, w, h), `flags` (4 window,
8 focused, 0x10 activated, 0x20 invisible, 0x40 dimmed), `visible`, `path`, `texts` (label/title strings),
`drawn` (`[{text, rect}]`), `icons` (`[{id, label, slot, size, mode, hash, file, bmp, mask}]`), `list`
(`{count, current, top, bottom, cols, model, items}`), `menu` (`{selected, items: [{text, cmd, cascade,
flags}]}`; `cmd` is the command id), `edit`, `children`.

## Identifiers that survive restarts

- **Control path:** `/CCalcContainer/CCalcFuncmapSubPane/CCalcCommandButton[0]`. These are class names
  from the root, with `[n]` among siblings of the same class. Use them to name a control in a test or
  a prompt. A different layout (another view, a dialog open) gives different paths.
- **Picture id:**
  - An Avkon icon is `File.mif#id`, e.g. `Calcsoft.mif#16414`, `phonebook_aif.mif#16384`. This is
    the icon's source, read from Avkon's own icon record.
  - Any other bitmap is `WxH:hash`, the FNV-1a hash of its pixels: same picture, size and theme give
    the same hash.
  - Dimmed and pressed states are separate pictures.
- **Picture label:** the readable name, from the first of these that has one:
  1. `tools/sydb/icon_names.json`: hand-made, `"File.mif#id"` or a hash → label. Add entries when you
     identify an icon on screen; the Calculator's buttons are there.
  2. The SDK's `avkon.mbg`: Avkon's own icons → `qgn_...` names (read from `$S60SDK`, never copied).
  3. App icons `<exe>_aif.mif` → that app's caption, from the agent's app list (cached in
     `<session>/ui-apps.json`; delete the file after installing apps).

  List and grid items that are only icon indices (the idle shortcuts) are named by their icons' labels.
- **Command ids:** menu items carry the app's command id (`cmd`), the same on every run of that firmware.

## Recipes

```
U="python3 tools/sydb/uidump.py --instance n95"
# open Settings > Phone > Call and read the "Automatic redial" value
$U select Tools --open && $U wait Settings && $U select Settings --open && $U wait General \
  && $U select Phone --open && $U wait "Call divert" && $U select Call --open && $U wait "Automatic redial" \
  && $U --json | python3 -c "import json,sys; print(json.load(sys.stdin)['summary']['list']['items'])"
# is the keypad locked?  (akncapserver has the focus, softkey "Unlock")
$U --json | python3 -c "import json,sys; s=json.load(sys.stdin)['summary']; print(s['app'], s.get('softkeys'))"
# the Calculator's display after typing
python3 tools/sydb/sydb.py --instance n95 key 1 2; $U --json | python3 -c "import json,sys; print(json.load(sys.stdin)['summary']['screen_text'])"
```

## Troubleshooting

| You see | Why | Do |
|---|---|---|
| `via gdb` in the header, `(probe: no reply ...)` | the app has no probe (started before it was installed), is not an Eikon app, or was busy for 3 s (an app loading plugins) | restart the phone after installing; ask again; `--agent` shows the probe's error |
| `focused` is akncapserver with softkey "Unlock" | the keypad is locked | `sydb unlock`, `sydb keepawake unlocked` |
| the screensaver app | the phone went idle | `sydb key back`, or `keepawake on` |
| `select` says the item is not among the items | the list is the wrong one (a dialog has the focus), or the text differs | read `ui` first; the match is exact, then prefix, then substring, case-insensitive |
| `X fault in control N` in `agent ui` | the probe met a bad object and skipped it (the app is fine) | report the class: it needs an adapter fix |
| a picture without a file | not an Avkon icon (skin graphics, an app's own bitmaps) | use its `WxH:hash` id, or add a label |
| `UIPROBE: fault ... left to the app` in the log | a fault outside the probe's guard | a probe bug: catch it (`sydb catch add --kind panic --thread APP`) and report |

## Platforms

The probe is built once against the S60 3rd FP1 SDK and loaded by every Avkon app. `sydb install` puts
it only on phones whose profile says `platform.ui` = `s60-*` or `symbian3`. A phone without Avkon (UIQ)
would fail to load it, and a failed library load makes every app's `CEikonEnv` construction leave.

| Platform (`platform.ui`) | Phones here | Probe | gdb reader |
|---|---|---|---|
| S60 3.1 / 9.2 | N95 8GB RM-320 | **works** (tests/uiprobe_smoke.py, 13 checks) | works (offsets read from this ROM) |
| S60 3.1 / 9.2 | N95 RM-159, N81, E90 | not tried | offsets to check |
| S60 3.0 / 9.1 | N73, N73 ME, E61 | not tried: imports must exist in 9.1 (see 1) | offsets to check |
| S60 3.2 / 9.3 | 5730, 6720, N79, N86, E52, E71, E72, E5-00 | not tried | offsets to check |
| S60 5.0 / 9.4 | 5800, 5230, N97 (touch) | not tried; `select` uses keys, taps by rectangle not done | offsets to check |
| Symbian^3 | N8 (all firmware), 701, 808, E6 | not tried; internals 3-5 most likely to differ | offsets to check; flexible memory model: page walk to check |
| UIQ 3.0 | P1i, P990i | **not installed** (no Avkon); a Qikon variant is separate work | moving memory model: not supported |
| hosted EKA2 (N95 on Linux) | - | not tried (no agent bridge there yet) | no QEMU gdb |
| EKA1 phones | N-Gage, 6600, ... | not applicable (no sydb) | - |

**Sweep, per device:** `python3 tests/uiprobe_smoke.py --device ID [--timeout S]`. It uses a throw-away
session (seeded when `build/seeds/ID` exists, else factory-fresh, so a first-boot wizard may have the
focus), never your phones. It checks: the boot, that the probe loads, the fault guard, a dump of the
focused app with text, and that nothing died. Then try `sydb --session build/sydb-uiprobe-smoke-ID ui`
by hand on a few apps (`--keep` leaves the session up). Record the result in the table above.

**What the probe assumes, and how to check it on a new platform.** Each item is either API (and fails
loudly) or an internal (and degrades: only that part goes missing).

1. **Imports exist (fatal if not):** the probe links 9.2 SDK import libraries: Cone, EikCore,
   eikcoctl, Avkon, fbscli, egul, eikctl, ws32, euser, ecom. If one of its ~50 imported ordinals
   (`arm-none-symbianelf-nm -D --undefined-only build/n95apps/obj-uiprobe/uiprobe.elf`) does not exist
   on the target, the DLL fails to load and every app dies: the smoke test catches this at boot. Check
   with `tools/import_index.py` / `tools/syb_sym.py mod DLL` on that device's ROM. The likely
   candidates on 9.1 are newer exports such as `CEikMenuPane::ItemDataByIndexL`. Fix by building a
   variant against that release's SDK.
2. **Library interface:** `CEikonEnv` loads ECom interface 0x10004cc1 (`CEikLibrary2`). Check:
   `tools/syb_sym.py callers REComSession::ListImplementationsL --sites`, the EikCore site, and the UID
   it passes. Capabilities: All-TCB, as CONE (`PlatSec ... UIPROBE` in the log means wrong).
3. **Control stack** `CCoeAppUi` +0xc, checked by its RTTI name. A mismatch only loses that pass; the
   window tree and heap passes still find the controls.
4. **Heap:** UREL cell header of 4 bytes (heap pass, member scan), `RHeap::EWalk`.
5. **Avkon icon record** (AknIcon.dll): `CAknBitmap` +0x1c → record: +0x8 file name, +0xc bitmap id,
   +0xe mask id, +0x14 flags, +0x18 back to the bitmap. The back pointer is checked: on a mismatch
   pictures keep their hash and lose `file#id`. Re-read with `syb_sym.py fn AknIcon.dll` on
   `AknIconUtils::IsMifIcon` / `SetSize`, as in ui-probe.md.
6. **Fault guard:** our kernel's exception frame (the same on every device) and the EXE re-entry stub
   restoring r0-r15 from it (`sydb dbg cmd 'x/34i <exe entry>'`). Each dump's `G recovered` line proves it.
7. **RTTI in the ROM's vtables** (RVCT builds): class names show instead of `?`.
8. **Drawn text:** CWindowGc's `DrawText`/`DrawTextVertical`/`Activate` slots come from the SDK
   headers. A release whose `CWindowGc` vtable differs would record nothing: no `drawn:` anywhere.
   Symbian^3 may also draw through other paths (NGA, Alf), which shows the same way.

For the gdb reader, every offset in [ui-dump.md](ui-dump.md)'s table is per release: re-read them with
`syb_sym.py fn` on the accessors named there before trusting it on another firmware.

## Files

| File | What |
|---|---|
| `src/n95apps/uiprobe/` | the probe: `uiprobe.cpp`, ECom `.rss`, entry stub + fault landing (`startup.s`), `build.sh` |
| `src/n95apps/devagent/devagent.cpp` | `ui` command, P&S properties 0x7569/0x756a |
| `tools/sydb/uidump.py` | both backends, tree, summary, labels, `select`/`wait`, CLI |
| `tools/sydb/icon_names.json` | hand-made picture labels |
| `tools/sydb/sydb.py`, `mcp.py` | `sydb ui`, install/update/uninstall of the probe, MCP `ui_tree`/`ui_select`/`ui_wait` |
| `tests/uidump_test.py` | host-side unit tests (no phone) |
| `tests/uiprobe_smoke.py` | on-phone test and the platform sweep |
