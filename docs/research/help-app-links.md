# Help app links: what a help "Open X" link does, and how to do it directly

Question: the S60 Help app (`cshelp.exe`, UID 0x10005234) shows XHTML help with links that open phone
apps or one view of an app. Can we turn those into a table an agent can execute without walking the UI?
Answer: yes. The links are plain URLs in the firmware's help zips, in two forms, and each maps to one
apparc or view-server call. `tools/helplinks.py DEVICE` extracts them to JSON.

Static reading only: the 5800 (RM-356 31.0.101) ROFS, `cshelp.exe` disassembled after `tools/e32load.py`,
and the extractor run over 16 firmwares. Nothing was booted.

## 1. Where the content lives, and its format

- `z:\resource\xhtml\<lang>\` [verified, 5800/N79/5730/N97/5230/N86/E72/N8 Belle/701/808]:
  `index.xml` lists the modules (`<collection id="0x10005951" navtitle="Bluetooth">`). Each module has a
  directory `0x<uid>\` holding `contents.zip`, `index.xml` (topics: `<topicref href="X.html" navtitle=..>`),
  `keywords.xml` and `meta.xml` (title, date). `<lang>` is the two-digit language code (01 = UK English).
  Modules installed separately use the same layout under `z:\resource\applicationhelps\<lang>\0x<uid>\`
  (5800: Share online 0x2000BB53, Search 0x20000907). Both are in ROFS, not in the XIP core.
- `contents.zip` holds one XHTML page per topic, for example `BT_HLP_MAIN.html`. Help-internal links are
  relative (`../0x0112233/GLOS_HLP_AP.html#d608e29723`). Links into a phone app are `app://...` [verified].
  Some pages carry `FeatureID`/class `KFeatureIdPenSupport` blocks that the viewer hides by feature (touch or
  keys), but the links are the same on touch and non-touch phones.
- The viewer is `cshelp.exe` with `HelpEngine.dll`, which uses ezip, xmlframework and hlpmodel. It loads
  `BrowserEngine.dll` at run time (RLibrary) and implements `MBrCtlLinkResolver`/`MBrCtlSpecialLoadObserver`,
  so the browser control hands it every `app://` URL [verified: strings and imports].
- Older firmwares have only legacy `z:\resource\help\*.hNN` (CHlpModel stores, no app links) [verified for
  N95 8GB 35.0.001, N81 8GB, E71, N73: no `resource/xhtml/<lang>` at all]. The XHTML help with app links
  starts with S60 3.2 (N79, 5730, E72, N86). So the N95 has **no** table.
- N8 013.016 and 025.007 (Anna, Russia) have 267 XHTML pages in `xhtml/01` but **no** `app://` links (only
  `http:`). N8 111.040 (Belle Refresh), 701 and 808 have them again (35–37, `cmd` links only) [verified].

## 2. Link syntax and what cshelp does with it

cshelp sorts a URL by its prefix (`app://vsa@localhost/` = 1, `app://cmd@localhost/` = 2), then counts
`/` in the whole URL: **4 = plain form, >= 6 = form with a message, anything else does nothing**. It reads
fields 1–3 up to the next `/`, and field 4 is the rest of the URL, so the message may contain `/`. A number
with `0x` is read as hex (`TLex::Val(TUint, EHex)`), otherwise as decimal (`Val(TInt)`). All of this is
[verified] from the cshelp.exe 5800 disassembly (functions at link addresses 0xabfa, 0xaa30, 0xaa7a, 0xa674,
0xa878, 0xa7aa).

| URL | Call made by cshelp |
|---|---|
| `app://vsa@localhost/<appUid>/<viewUid>` | `CCoeAppUi::ActivateViewL(TVwsViewId(appUid, viewUid))` |
| `app://vsa@localhost/<appUid>/<viewUid>/<msgUid>/<msg>` | `ActivateViewL(TVwsViewId(app, view), TUid msgUid, CnvUtfConverter::ConvertFromUnicodeToUtf8L(msg))` |
| `app://cmd@localhost/<Exe.exe>/<appUid>` | `TApaTaskList(ws).FindApp(appUid)`: if it exists, `BringToForeground()`; else `CApaCommandLine` with `SetExecutableNameL(Exe.exe)`, `SetCommandL(EApaCommandRun = 2)`, then `RApaLsSession::StartApp` |
| `app://cmd@localhost/<Exe.exe>/<appUid>/<msgUid>/<msg>` | as above, plus `TApaTask::SendMessage(msgUid, utf8(msg))`. If the app had to be started, a timer polls `FindApp` (up to 20 tries) and sends the message once the task exists |

- [verified] The view switch is made through cshelp's own `CCoeAppUi`. The view server then brings the
  target app forward, and it starts the app if it is not running (standard view-server behaviour,
  [inferred] for these targets). From an exe with no CONE you get the same result with
  `CVwsSessionWrapper::NewL()` + `ActivateView(TVwsViewId, TUid msgId, const TDesC8& msg)` (viewcli.dll; it is
  in the 5800, N95 and N8 core ROMs, and `viewcli.dso` is in the FP1 SDK the agent builds against).
- [verified] The Symbian^4 Qt help (`~/work/symbian/oss.FCL.sf.app.helps`, HelpContentsView.cpp) keeps the
  comment `app://cmd@localhost/APP_NAME/APP_UID`. That is the same syntax, although that version launches
  the app by UID only.
- [verified] One firmware link carries a message. N97: `app://cmd@localhost/matrixmenu.exe/0x101f4cd2/0x10003a39/mm://root/musicsuite#0`
  ("Music": Menu gets message uid 0x10003a39 with `mm://root/musicsuite#0`, which opens the music folder).
  All other links on the 16 firmwares are plain.
- Firmware typo: the 5800 RealPlayer link is `MediaPlayer.exe/0x1005A3E`, with one digit missing. FindApp
  fails, and cshelp falls back to starting `MediaPlayer.exe`. Executors should prefer the exe name for
  `cmd` links (StartApp by exe, as cshelp does) and use the UID only to find a running task.

## 3. Sample: 5800 XpressMusic (RM-356 31.0.101), English, 51 distinct links

`python3 tools/helplinks.py nokia-5800-rm356 --unique`. View links (`vsa`, all 9):

| Link text (help module) | App UID | View UID |
|---|---|---|
| Bluetooth (Bluetooth) | 0x10005951 | 0x1020745C |
| Paired devices (Bluetooth) | 0x10005951 | 0x1020745D |
| Blocked devices (Bluetooth) | 0x10005951 | 0x20002780 |
| General (Themes) | 0x10005A32 | 0 |
| Wallpaper (Themes) | 0x10005A32 | 3 |
| Home screen (Themes) | 0x10005A32 | 0x102750A7 |
| Themes (Themes) | 0x10005A32 | 0x102750AA |
| GPS data (GPS data) | 0x101F85A0 | 1 |
| USB (USB) | 0x102068E2 | 1 |

App links (`cmd`), a selection: Messaging `Mce.exe` 0x100058C5; Settings `GSApp.exe` 0x100058EC (155 uses,
always the main view: the help never deep-links into a Settings plugin); Contacts `phonebook2.exe`
0x101F4CCE; Calendar `calendar.exe` 0x10005901; Clock `ClockApp.exe` 0x10005903; Camera `cameraapp.exe`
0x101F857A; Gallery `MediaGallery2.exe` 0x101F8599; Music player `mpx.exe` 0x102072C3; Web `BrowserNG.exe`
0x10008D39; Log `Logs.exe` 0x101F4CD5; File mgr. `FileManager.exe` 0x101F84EB; Profiles `ProfileApp.exe`
0x100058F8; Call/Video mailbox `vm.exe` 0x100058F5; Sync `NSmlDSSync.exe` 0x101F6DE4; Device updates
`NSmlDMSync.exe` 0x101F6DE5; Connection manager `ConnectionMonitorUi.exe` 0x101F84D0; Location `lcapp.exe`
0x10283139; Radio `TubeFMRadio.exe` 0x10207A89; Download! `ncd.exe` 0x20008014; App. update
`iaupdate.exe` 0x2000F85A; Cell broadcast `cbsuiApp.exe` 0x101F4CD3; plus 20 more in the JSON.

Counts per firmware (distinct url+text, of which vsa): 5800 51/9, 5230 51/9, N97 61/8, N79 77/13, 5730 76/13,
E72 73/13, N86 72/13, N8 Belle 37/0, 701 35/0, 808 35/0; N95 8GB, N81 8GB, E71, N73, N8 013/025: 0.

How much this buys: the table reaches apps and only a few deep views (Bluetooth sub-views, Themes pages,
GPS data, USB). The help does not link to individual settings pages, so "open setting X" still needs
navigation or a view id taken from somewhere else (for example the GS plugin UIDs).

## 4. Tool and sydb proposal

- `tools/helplinks.py [DEVICE] [--lang 01] [--unique] [--all-links] [-o OUT]` (written, unit tests in
  `tests/helplinks_test.py`). It reads the ROFS images (and the XIP core, if help were ever there) through
  `tools/device.py paths`. Each entry has `kind` (cmd|vsa), `exe`, `app_uid`, `view_uid`, `msg_uid`,
  `msg`, `text`, `page`, `page_title`, `module`, `module_title`, and `invalid` when cshelp would ignore the
  URL.
- Executing an entry from sydb:
  - `cmd` without a message: the existing `sydb launch 0x<uid>` (devagent `CmdLaunch`: `StartApp` with
    `EApaCommandRun`) is enough. Better: launch by the entry's exe name, which survives the RealPlayer typo,
    and bring an existing task forward first (`TApaTaskList::FindApp` + `BringToForeground`) as cshelp does.
  - `vsa`, and `cmd` with a message: add an agent command, for example `sydb view APP VIEW [MSGUID MSG]`.
    In devagent: `CVwsSessionWrapper* v = CVwsSessionWrapper::NewL(); v->ActivateView(TVwsViewId(TUid::Uid(app),
    TUid::Uid(view)), TUid::Uid(msgUid), msg8); delete v;` (link `viewcli.dso`, which is in the FP1 SDK lib
    dir). The `cmd`+message form is `TApaTask::SendMessage(msgUid, msg8)` after FindApp (the agent already
    has a `RWsSession`/apgrfx path for `launch`). Wait for the task's window group with a short poll, not a
    fixed sleep.
  - A higher-level `sydb help-open "Paired devices"` could look the text up in the device's
    `helplinks.py` JSON (cache it in the build dir) and pick launch or view.
- Caveats [inferred]: `ActivateView` from a non-foreground, non-CONE exe needs no capability on 9.x. The
  view server may refuse it while a modal dialog or keyguard is up, so callers should check
  `sydb`'s keyguard state first. View UIDs differ between platform versions (the table is per firmware,
  never shared across a family).
