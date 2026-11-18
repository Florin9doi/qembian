# Nokia 6600 (NHL-10): why Autolock is in front on a normal start

Static reading of `eka1/nhl10/output/rom/ROM.bin` (VA = 0x50000000 + offset). Import names come from
the module/ordinal of each stub's target, named with the Series 60 1.2 ARMI `.lib` files (ordinals match
for Sysagt, SecUi, CommonEngine, ApGrfx, Ws32, avkon, EikCore, sysutil) [V for module+ordinal, I for
names]. Nothing was run. [V] = read in the code; [I] = inferred.

## Autolock.app (code 0x50a30e34, Thumb)

Objects: AppUi (`this`), model at AppUi+0x80, call-state observer at AppUi+0xb4, "locked" flag at
AppUi+0xb8, key-capture handles at AppUi+0xa0/0xa4/0xa8, RSystemAgent at AppUi+0xbc.

| What | Address | Behaviour |
|---|---|---|
| AppUi ConstructL tail | 0x50a31006..0x50a3103c | connect SA, then `TApaTask::SendToBackground` (starts hidden) [V] |
| Model ConstructL | 0x50a31bbc | SA connect; SharedData observer on 0x100058EC "LockTime" (0x50a31e88); SA 0x100052F2 observer (0x50a31a40); activity manager (0x50a31c30); then **SetLockedL(0)** (0x50a31c08) [V] |
| SetLockedL(b) | 0x50a31db4 | `RSAVarChangeNotify::NotifySaVarChangeL(0x100052F2, b)` and SharedData 0x100058EC "AutoLock" = b (0x50a31f04) [V] |
| Activity manager start | 0x50a31c30 | timeout = LockTime x 60 s if LockTime != 0, else 60000; inactive callback 0x50a31d51 [V]; the manager is in DLL 0x50f78xxx (CUserActivityManager-like) [I] |
| Inactive callback | 0x50a31d50 | ActivateAutolock only if LockTime != 0 [V] |
| SA 0x100052F2 observer RunL | 0x50a31b28 | ActivateAutolock when the new value == 1 [V] |
| **ActivateAutolock** | 0x50a31d6c | requires AppUi+0xa8 == 0, `!CEikAppUi::IsForeground()`, and `SysStartup::State() == 203`; then SetLockedL(1), capture keys (0x50a313d8), LockL (0x50a31368) [V] |
| LockL | 0x50a31368 | if SA 0x100052CB (current call) != 0: defer (call observer 0x50a32024 re-locks when call state returns to 0); else locked=1 and `TApaTask::BringToForeground` [V] |
| HandleForegroundEventL | 0x50a31120 | foreground and not locked: SendToBackground; foreground and locked: capture keys. Background and locked: if call state == 0 and SIM status 0x100052E9 != 4: BringToForeground again [V] |
| HandleCommandL 300 (unlock) | 0x50a31204 | `CSecurityHandler(RAdvGsmPhone)`, `AskSecCodeInAutoLockL`; on success locked=0, SetLockedL(0), SendToBackground [V] |

Only Autolock writes SA 0x100052F2; the constant also appears in SecUi, Phone.app and Ncnlist, which only
read it (SecUi 0x50eb70d0 GetState; Phone 0x50bea9f4 GetState) [V for SecUi/Phone, I for Ncnlist].
SharedData "AutoLock" (0x100058EC) is read by avkon, AknNotifyPlugin and Phone.app (0x50bc9f9a) [V refs].

So Autolock locks at start-up only if [V]:
1. SharedData 0x100058EC `LockTime` != 0 and the activity manager reports inactivity (timeout LockTime
   minutes), or
2. someone sets SA 0x100052F2 = 1, or
3. a deferred LockL from an active call;
and in cases 1 and 2 only once `SysStartup::State() == 203`.

`SysStartup::State()` (sysutil 0x50a1d954) = SharedData 0x10005943 key `state.val`, default 200 if the
key or file is missing [V]. 203 is the "start-up finished, normal" value [I]. With a factory start (105)
Starter restores the SharedData defaults (LockTime = 0 [I]); on a kept flash the values persist.

## Unlock path, and why no code query appears (SecUi 0x50eb6ed4, AskSecCodeInAutoLockL)

[V] It reads SharedData 0x100058EC `LockTime`:
- `LockTime == 0` (or the read fails): **no query is shown**; it calls etelmm ordinal 0x50eb9b9c with
  (phone, status, 0, 2) = `RMobilePhone::SetLockSetting(ELockPhoneDevice, ELockSetDisabled)` [I names]
  and waits in a CActiveSchedulerWait (0x50eb8e18/0x50eb8eac).
- `LockTime != 0`: code query (0x50eb5f00, 5..10 digits), then etelmm 0x50eb9b8c with type 4 =
  `VerifySecurityCode(ESecurityCodePhonePassword, ...)` [I], wait.
- Results: KErrNone -> unlocked; -39 (KErrAbort) -> stays locked; KErrAccessDenied/-21, KErrGsm0707*
  (0xffffec50, 0xffffeeb5) / -22 notes, others via text resolver; then recursion (asks again) [V].

So a lock screen with no query on LSK, keys dead, means: LockTime == 0 and the TSY request for
SetLockSetting(phone device, disabled) is never completed (the nested wait swallows everything) [I].

## What to change / check

1. Check the kept C: SharedData files: `C:\System\SharedData\100058EC.ini` (`LockTime`, `AutoLock`) and
   `10005943.ini` (`state.val`). If `AutoLock` = 1 there, the phone was saved while locked [I].
2. In the session, watch SA sets of 0x100052F2 (sysagt NotifySaVarChangeL caller): if Autolock itself
   sets it to 1 (0x50a31dd6 with r2=1), find which trigger (0x50a31b3c = SA observer, 0x50a31d5e =
   inactivity) by breakpoints on 0x50a31d6c [I plan].
3. Press LSK on the lock screen and list the new ISI requests: the unanswered one is the SEC request
   behind SetLockSetting/VerifySecurityCode; answer it with OK, and unlocking works [I].
4. Autolock's start-up lock is avoided if `LockTime` is 0 and nobody sets SA 0x100052F2 = 1, or if
   `state.val` != 203 (no activation at all; workaround only, it breaks real autolock) [V logic].
