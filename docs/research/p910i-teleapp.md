# P910i stall 14: Tele.app panics "TeleApp" -1 after "Phone on"

Static reading only (no runs), 2026-11-18. [V] = read in the code or a dump, [I] = inferred.
Tools used: objdump on `eka1/p910i/core/ROM.bin`, an import-thunk resolver (thunk slot -> export
directory of the DLL in the ROM -> ordinal, names from `analysis/user-exports.tsv`), the panic dump
`build/ngage/sess-p910/crash-1/state.json`.

## 1. Which step fails

- The start-up engine (object with step counter at +24, observer array at +28, "async" flag at +32):
  - `0x50997F8C` RunStep: TRAP(`0x50998010`); on a leave -> `0x50997FCC` = `User::Panic("TeleApp", leave code)`. The leave code is the TRAP result, not iStatus [V].
  - `0x50998010` calls every observer's vtable slot +8 with (step), then every observer's slot +12 with (step), then step++; new step 2 signals `TELE_1_SEM`, new step 6 signals `QUARTZSPLASH` (strings at 0x50A6A9B8 / 0x50A6A998) [V].
  - `0x50998130`: clears the async flag; if step == 1, calls RunStep twice (steps 1 and 2) synchronously [V].
- The panic stack in crash-1 (`sp+070 = 0x5099814B`, `sp+078 = 0x50A09623`) is the return address of the **second** RunStep call in `0x50998130`, called from Tele.app `0x50A0961E` [V]. So after "Phone on" the engine runs steps 1 and 2 in one go, and **step 2 leaves with -1** [V].
- The observers' step handlers switch on the step (`cmp r1,#6` + jump table). Handlers with work for step 2 [V, jump tables decoded]:

| handler | step-2 case | does |
|---|---|---|
| 0x509A724C (Standby) | 0x509A7280 | load resource `tele_Standby.rsc` |
| 0x509B2460 (AOC) | 0x509B2494 | load `tele_aoc.rsc`, then 0x509B0F28, 0x50996EA8, 0x509B3F60, 0x509B1124 (CallMeter.ini store, see below) |
| 0x509B68D8 (Bluetooth) | 0x509B690C | load `tele_bluetooth.rsc`, then 0x509B5C7C, 0x509B5990, 0x509B561C, 0x509B61B0, 0x509B6164, 0x509B59FC |
| 0x50A269E4 (Service menus) | 0x50A26A18 | load `tele_servicemenus.rsc`, 0x50A26CD0, 0x50A2792C |
| 0x50A26A38 | 0x50A26A6C | 0x50A2B2EC (reaches **appcli.dll@9**), 0x50A2DE44 |
| 0x50A26A88 / 0x50A26AF0 | 0x50A26ABC / 0x50A26B24 | virtual calls only |
| 0x50A2EEB8 (SIM/ME locks) | 0x50A2EEEC | load `tele_sim_me_locks.rsc`, 0x50A2FD34, 0x50A2EE38, 0x50A2F40C (reaches **settingcli.dll** 1, 8, 16, 19, 25, 26, 28, 30, 43) |
| 0x50A2EF1C / 0x50A2EF70 / 0x50A2EFB0 | ... | pointer-array bookkeeping |

- The CMdaServer / MediaClient stale frames do NOT belong to step 2: the only Tele.app call of
  `CMdaServer::NewL` (0x50A357FE) is in the Standby observer's **step 5** case (0x509A7290: app-list
  icons, an image view with CMdaServer + CMdaBitmapScaler + CWsBitmap, alarm client, settingcli,
  eventsvrcli objects) [V]. The two MediaClientAudio NewLs (0x50A28808 ord 1, 0x50A288C6 ord 3) are
  TRAPped and only show a -4 note [V]. Stale frames are left over from earlier activity [I].

## 2. What returns -1: what is ruled out, what is left

Ruled out [V]:
- Resource files. Step-2 resource loader `0x50997DF4`: path = `<drive of the app>` + `\system\apps\tele\` + name, `BaflUtils::NearestLanguageFile`, then `CEikonEnv::AddResourceFileL`-like (EikCore import, the ported name says AddLibraryL [I]). All five files exist in Z: `System/Apps/Tele/` (Tele_Standby, Tele_aoc, Tele_bluetooth, Tele_servicemenus, Tele_sim_me_locks .RSC) [V files.tsv]. A missing-file leave would be repeatable, not timing-dependent.
- `C:\System\Apps\Tele\CallMeter.ini` (AOC, 0x509B17EC: `SetSessionPath("\System\Apps\Tele\")`, `CFileStore::OpenL`): it is not on the factory C: [V mdir], OpenL would leave -1, but the caller 0x509B10DE TRAPs it and on any error writes a new one (0x509B18A0: `MkDirAll` + `CFileStore::ReplaceL`) [V]. Not the panic.
- Media server: not in step 2 (above).

Left [I]:
- The -1 did not show up in the run with a conditional breakpoint on `User::Leave` (where the panic also disappeared). Every leave goes through `User::Leave` 0x5004B184 (`LeaveIfError` calls it, checked) [V], so the leave is real but **timing-dependent**. The classic timing-dependent KErrNotFound is `RSessionBase::CreateSession` to a server that is **not yet registered** (server process still starting). [I]
- Step-2 code that connects to servers: `appcli.dll@9` (observer 0x50A26A38 via 0x50A2B334) and `settingcli.dll` (SIM/ME locks via 0x50A2F40C, 0x50A2E90C...). settingsvr is up since stall 9 (13-process list), so **appcli.dll's server is the first suspect**, settingcli second; the deeper Bluetooth/AOC calls (0x509B5C7C..., 0x509B0F28...) were not walked (budget) [I].
- A second way to get -1 with timing: a `RProperty`/`RFs`-style Find on something another process creates during the same seconds (e.g. a file or a P&S/shared-data key written by a starting server) [I].

## 3. Cheapest next step / fix

1. One run, no breakpoints on the Phone thread's hot path: a catch on `RSessionBase::CreateSession`
   returning -1 (EUser export, find by name in user-exports.tsv; break on its return path with
   `r0 == -1` and thread == Phone) or on the Exec that creates the session. The server name in the
   argument names the missing server. Also catch `User::Leave` with r0 == -1 as a *catch* (freeze at
   first hit) rather than a breakpoint that stops and resumes.
2. If it is a not-yet-started server (appcli's server, or settingsvr restarting): the fix is on our
   side only if that server is held up by us, e.g. waiting for an OSE/GSM reply after "Phone on"
   (check unanswered OSE requests in the 2 s window first, as in stalls 10/12/13), or a device the
   server opens at start-up. The phone itself would have the server up before Tele reaches step 2;
   the order is driven by the starter, so look at which server process is created after Phone on
   and what it is blocked on.
3. If it is a missing setting/file instead, add it to the C: image build (flash=) with the value
   the factory phone has.

Tooling note [I]: `ngage_sess.py dump` saves SDRAM at 0x0A000000 / 16 MB (N-Gage layout). The
P910i crash-1 dump does not contain the Phone thread's stack (its return addresses are not found
in sdram.bin or isram.bin), so P910i dumps miss the phone's RAM; the dump base/size should come
from the board model.
