# P910i: what ends the start-up splash (stall 15)

Static reading only (no runs). [V] = read in the code, [I] = inferred.
Addresses are ROM VAs (eka1/p910i/core/ROM.bin, VA = 0x50000000 + offset). Helper used:
an objdump wrapper that fills in import-thunk names.

## 1. Who ends the splash: QStart kills splash.exe [V]

QStart.exe (0x50739220) is one CActive whose RunL `0x5073A0D4` is a state machine at `this+28`
(jump table 0x5073A0F0) that completes itself after each state (`0x5073A248`):

| state | does |
|---|---|
| 0 | -> 2; TRAP(`0x50739B48`): start `Z:\SYSTEM\PROGRAMS\SPLASH` for the start-up reason (RProcess::Create + Resume, handle at +116); +120 = 1 when it started |
| 2 | `0x50739BFC` (CDictionaryFileStore settings: start-up reason, delete a file) + `0x50739E2C` (font/sysfnt.ini work) -> 5 |
| 5 | while the list at +96/+108 has entries: `0x50739E50` starts the next entry (below); empty -> 6 |
| 6 | -> 7; **`RSemaphore::Wait` on global semaphore "QUARTZSPLASH" (+32) `count(+136)` times** (`0x5073A184` loop), Close; then, if the splash was started, **`RProcess::Kill(0)` on splash** (`0x5073A1AC`) |
| 7 | second list (+76/+88), then done |

"QUARTZSPLASH" is created by QStart while parsing QStart.RSC (`0x5073A3B6`, CreateGlobal, count 0).
+136 is incremented once per list entry that has the "wait for QUARTZSPLASH" flag (`0x50739F66`),
and decremented by the thread-death handler `0x5073A1E4` (an app that dies before signalling is not
waited for) [V code, I flag meaning]. The apps that signal it all carry the string:
Tele.app (0x50A6A99C), Alaunch.app (0x50712700), appicker.app (0x5071A8B4) [V strings]. splash.exe
itself never looks at a semaphore or property: it only shows the logo (window group "SPLASH") and
talks to LinneaStartUpSvr [V strings]. **Nothing but QStart state 6 removes the logo.**

## 2. QStart.RSC list and where QStart waits now [V]

QStart.RSC (0x5073BB60): a header (`SwissA`, `StartupReasonProvider` DLL, reason ids 4..10), seven
SPLASH entries (one per start-up reason), then one start-up list per reason (normal, charging
(`bco`), master reset (`MRapp`), ... ). Entry = {LTEXT path, type byte (0x0D app / 0x12 exe [I]),
flags, LTEXT semaphore name, 05 00 00 00 00 [I timeout/retry]}. The normal list (RSC +0x160):

```
Flipkeypad, LinShell, CntDbLocaleUpdate (CntDbLocaleUpdate_sem), PortHandlerStarter,
QDefaultThemeInit (THEME_SEM), Tele (TELE_1_SEM), QEikStartPhase2 (EikPhase2), SYSTEMAMS,
Alaunch, appicker (TELE_2_SEM), LinneaStartBTHeadsetSupport, LinneaStartSoundHandler,
QSystemSounds, WimSrv, ConSvr, sprot, autolauncher
```

Entry start `0x50739F??..0x5073A030`: start the app (`0x507392F8`: StartApp / StartDocument /
EikDll::StartExeL / RProcess), then, if the entry names a semaphore, find it in the array at +152
(TDesC::Compare loop) and **`RSemaphore::Wait`** on it (`0x50739FFE` -> `0x5073A44C`). The
saved stack top **0x5073A003 is the return address of that Wait** [V]: QStart is blocked on an
entry semaphore, not in a file-server call. Since Applications (Alaunch) and Appicker run, the
semaphore is **TELE_2_SEM** (appicker's entry) [I: confirm that LinneaStartBTHeadsetSupport,
LinneaStartSoundHandler, QSystemSounds, sprot, autolauncher are NOT in the process list].

## 3. Both semaphores come from Tele's start-up engine step 6 [V]

Tele.app start-up engine (docs/research/p910i-teleapp.md; step at +24, observers at +28):
- `0x50998010` RunStep: observers' slot +8 then +12 with the step, step++; new step 2 ->
  signal TELE_1_SEM, **new step 6 -> signal QUARTZSPLASH** (`0x509981D8` OpenGlobal + Signal).
- Standby observer's step handler `0x509A72D4`, **case 6** (`0x509A733A`): window group to the
  back, `CVwsSessionWrapper::SetSystemDefaultView`, then **signal TELE_2_SEM** (`0x509A73AC` ->
  `0x50998204`). (Second TELE_2_SEM path: `0x50A3946E`, another flow, not read.)
- "Phone on" path: `0x50998130` (engine RunL with async flag clear) runs steps 1 and 2 only
  (TELE_1_SEM: QStart went on to EikPhase2, SYSTEMAMS, Alaunch, appicker). Steps 3..6 run only
  from the engine's other RunL `0x50998184` (vtable 0x50A81130 slot 6: four RunSteps), armed by
  `0x50998150` (self-complete), called only from **`0x509A5874`** (an observer sets its state
  +28 = 2, then `engine->StartAsync`).
- `0x509A5874` is called only from two completion callbacks of a Tele-internal state manager
  (singleton `0x50A2EE04`, observers in an RPointerArray at +24, callbacks through vtable slots
  +12 / +24 with (r1, r2, r3 = error); `0x50A2F16E`, `0x50A2F33E`):
  - `0x509A5928(this, r1, r2, err)`: err == 0 and **r1 != 0** -> `0x509A5874` (steps 3..6);
    r1 == 0 -> `0x509A58A0` (other state path, no steps); err == -16 -> re-register, up to 5
    retries, then Panic "TeleApp" -16; other err -> Panic "TeleApp" err.
  - `0x509A59F4(this, r1, r2, err)`: err == 0 and r1 != 0 -> `0x509A5874`.
  So the wait is: **the manager must report a "ready/on" state (r1 = TRUE)**. r1/r2 are the two
  words at `[manager+16]+48` (`0x50A30448` / `0x50A3044C`) [V]. Which phone condition fills them
  (radio on, SIM ready, registration) was not traced (budget) [I: radio-power / phone-state].

Chain: manager state "on" -> Tele steps 3..6 -> QUARTZSPLASH + TELE_2_SEM -> QStart finishes the
list (BT headset, sounds, sprot, autolauncher) -> state 6 waits QUARTZSPLASH from Tele, Alaunch,
appicker -> kills splash.

## 4. GSM side: open question, what to check first

- No unanswered request blocks this except possibly **0x0533** (E3 -> 49): swbp builds it with the
  allocator `0x508A7314` (sig 0x533, one byte at [2]); 0x0532 is its sibling (`0x508A72F4`). No
  direct `bl` to either in swbp: they are reached through a table [V]. A request built by an
  allocator with no reply stub usually gets its answer as a subscribed indication [I]; 0x0533
  is the most likely "switch RF on / phone on" request whose result Tele's manager waits for.
- None of the 28 kept indications is sent today besides 0xC384. The candidate answer is one of
  them carrying a "radio on / phone state" value (0xC389 is already known to be decoded by ereq
  at buf[6] for the EStart path; others not decoded) [I].

## 5. Cheapest faithful fix (one short run, then a reply line)

1. One run with a breakpoint/`ngage_sess break` at **`0x509A5928`** and **`0x509A59F4`** (log r1,
   r2, r3) and at `0x509A5874`, plus one at the swbp send of 0x0533 (`0x508A7314`, log lr): tells
   (a) whether the manager calls back at all, (b) with r1 = 0 (state says "off") or err != 0, and
   (c) who sends 0x0533. If the callback never comes, the manager is waiting for the phone; the lr
   at 0x0533 names the request.
2. Fix on the GSM side: answer 0x0533 with the indication its receiver subscribes to (find it from
   the caller in step 1: its ereq/swbp receive sigsel, as was done for 0xC384/0x4F4E), payload
   "on/ok" (status byte 0). Add it to `src/qemu-ngage/p910i-ose-replies.txt` as `reply 0533 <sig> ...`
   (or as a timed unsolicited indication if it is not a reply).
3. Not faithful but unblocks the UI for testing: signal TELE_2_SEM and QUARTZSPLASH (x3) from
   outside; QStart then kills the splash. Hides telephony being down; use only as a probe.

## 6. Follow-up (after the run: 0x509A5928/0x509A59F4/0x509A5874 never fire) [static]

Run result [V, coordinator]: with "Phone on", none of Tele's three entry points fire in 100 s, and
0x0533 `{18 00 00 00 00 00}` is still sent (from the swbp 0x508B2D9C area, not from 0x508A7314).

How the manager calls back [V]:
- The manager (singleton `0x50A2EE04`) is itself a start-up-engine observer. Its step-2 cases
  (`0x50A2EEEC`, `0x50A2EF50`) load `tele_sim_me_locks.rsc` and create an adapter object
  (`0x50A2FD34`; M-interfaces at +4 / +8, vtables 0x50A7E904 / 0x50A7E924, this-adjust thunks
  0x50A672xx / 0x50A699xx). They also create two workers:
  - +16 = **SIM/ME-lock state machine** `0x50A2F40C` (observer = adapter+4). It keeps a task queue
    (dispatcher `0x50A2F4B0`, jump table 0x50A2F4F8; tasks `0x50A2F6B8`, `0x50A2F6DC`,
    `0x50A2F700`, `0x50A2F724`, `0x50A2F748`, `0x50A2F76C`, `0x50A2F790`, `0x50A2F7B4`; queues
    filled by `0x50A2F5F0` (9 tasks) and `0x50A2F668` (4 tasks)). Current task id at +44, result
    words at +48 / +52 (the r1 / r2 that Tele's observers get).
  - +20 = object `0x50A31C4C` (observer = adapter+8), step 3 calls `0x50A326A0(obj, 1)`.
- Callback chain: adapter+4 slot 2 (`0x50A672FC` -> `0x50A2FD4C`) -> notify `0x50A2F0EC` (observer
  slot +12 = `0x509A5928`); adapter+8 slot 1 (`0x50A69994` -> `0x50A2FD7C`) -> `0x50A2F2BC`
  (observer slot +24 = `0x509A59F4`).
- The lock machine's requests run on an AO (`0x50A2FD90`, vtable 0x50A79F6C, created in
  `0x50A2F44C`). Request starters are `0x50A2FF68`..`0x50A30164`: SetActive, then a virtual call
  on the sub-object at **[ETel singleton + 16] + 0xA4** (vtable slots +12, +24, +36, +48, +60,
  +80, +92). RunL `0x50A2FF04`: iStatus == 0 -> observer slot +8 (r1 = request id at +32); error
  -> observer slot +16 (id, error).
- ETel singleton (slot 4, ctor `0x50996F04`): RTelServer at +12, then at +16 an object built by
  **erigsmapi.dll** (`0x508F2490`: RAdvGsmPhone inside; it embeds interface sub-objects at
  +0xA0 (vtable 0x508F9C7C) and +0xA4 (vtable 0x508F9D70, every slot pure virtual
  0x508F5358)). erigsmapi's requests are RTelSubSessionBase::Get / CancelReq with IPCs
  0x2723..0x2726... (e.g. `0x508F43DC`, `0x508F4420`) -> erigsm.tsy -> ereq -> swbp [V for the
  IPC numbers read, I for the chain].

Not settled (budget): the +0xA4 sub-object's real class (its base vtable is all pure virtual, so the
derived one is installed elsewhere [I]), and so which erigsmapi request (ETel IPC, OSE signal) is
still outstanding. **No reply-table line can be given yet.**

Cheapest next step (one run, logging catches, no hot-path breakpoints):
1. `0x50A2F4B0` (dispatcher: log [r0+44] = task id, and the queue) and the request starters
   `0x50A2FF68`, `0x50A2FF94`, `0x50A2FFC0`, `0x50A3001C`, `0x50A3004C`, `0x50A3007C`, `0x50A300AC`,
   `0x50A300DC`, `0x50A3010C`, `0x50A30138`, `0x50A30164` (log lr and the vtable slot target
   `[[r0+24]+0xA4]` -> `[vt+N]`). The last starter that fires with no `0x50A2FF04` after it is the
   outstanding request.
2. Its target function in erigsmapi gives the ETel IPC (0x27xx). The TSY handler for that IPC gives
   the OSE request (likely the 0x0533 from 0x508B2D9C, or one of the SIM-lock queries) and the
   indication or reply it waits for. That gives the `reply`/`unasked` line.
