# P1i SecurityApp: what start-up waits for

Sony Ericsson P1i R6E30 (`se-p1i`). Static reading of `Sys/Bin/SecurityApp.exe` and `Sys/Bin/Security.dll`
(ROFS1, loaded with `tools/e32load.py` at link base 0x8000, disassembled as Thumb after adding a `$t`
mapping symbol) and of OpaClient's ICopsManager proxy in the core XIP image. [V] = read in the
disassembly, [I] = inferred. Research budget reached; the open end is marked.
(The serial log of the stalled run was overwritten by a new session, so the call sequence below is the
one from the brief.)

## 1. How SecurityApp tells SysStart it is done

No P&S key and no process rendezvous with a status: **`RThread::Rendezvous(KErrNone)`** in SecurityApp's
main thread (helper at 0xb1d8, called once, guarded by a flag) [V]. Two paths reach it, both in
`CPhoneUiMediator` (RTTI `16CPhoneUiMediator`, vtable 0xeefc) [V]:

| path | code | what else |
|---|---|---|
| `MSecurityPhoneObserver` slot 0 (mixin at +4, thunk 0xacba -> 0xac92): "all clear" | 0xac92 | cancels an active object, Rendezvous, then signals the global semaphore **`SECURITY_ALL_CLEAR_SEM`** twice (created at 0xb144) [V] |
| observer slots 6 / 7 (0xad3c, 0xad5e) -> 0xa9b6 | 0xa9b6 | if `iPhone->vfunc(+0x38)` returns 0: TeleFramework #99, then opens **`POWER_ALL_CLEAR_SEM`** and `Wait()`s on it, then Rendezvous [V]. Slot 7 also cancels; slot 6 with arg 0 stores an error code first [V] |

The `MSecurityPhoneObserver` callbacks come from `Security.dll` (the "security phone": RTTI
`CPhoneFactoryImpl`, `CPhoneLockFactory`, `CSimLockFactory`, `CCopsManagerFactory`, `CTsyEventListener`,
`CTsyCodeAdapter`) [V names; I which class calls which slot]. So start-up continues when Security.dll
decides the lock state; SecurityApp itself makes no OPA calls except OpaClient #121 [V].

## 2. ICopsManager in Security.dll

GUID table at Security.dll 0xa888 [V]: ISystemControl {991baf67}, ISimApplication {10bf3803},
ISimManager {0303fc15}, **IPhoneLock {a24977c1-8ffc-eb41-b74d-642cb0d87107}** (+0x48),
**ISimLock {c3e250a5-9ed1-2943-83e8-0383aea38f60}** (+0x58), ICopsManager {7bf1607a} (+0x68), CLSID
{6192b8a8} (+0x78).

- `CCopsManagerFactory` (0x8746): CreateInstance(CLSID {6192b8a8}, IID ICopsManager) through the
  factory manager, completion 0x87c8 checks status >= 0, the IID and method 4, sets ready and hands the
  ICopsManager to its `MCopsManagerFactoryObserver` slot 0 [V]. Its destructor (0x8790) calls
  `Release()` (vtable +0x14) on the ICopsManager [V] -> very likely the BridgeServer **type 10** for
  object 4219 seen on the log = Release, which needs no answer [I].
- Phone-lock class (ctor 0x884c, OpaClient #26 with the IPhoneLock IID -> +0x14): calls
  **ICopsManager vtable +0x30 = m5** `(this+0x14, request observer this+8)` (0x8900) [V call, I slot ->
  m5 by the proxy below]. Completion (0x8912 / 0x8920): sets done (+0xc = 1) and calls its observer
  (+0x18) slot 0 with the +0x14 buffer [V].
- SIM-lock class (ctor 0x8944, OpaClient #283 with the ISimLock IID -> +0x14): calls ICopsManager
  vtable +0x2c = **m4** (0x89f0), same completion pattern (0x8a02 / 0x8a10) [V call, I slot -> m4].
  **m4 is not on the log**: the SIM-lock check never started.
- Third class (0x8a20): OpaClient #46 (ISimManager IID), #115 (ISimApplication IID), #271
  (+0x08 GUID {207a5453-1401-3247-82b7-d2c34649e466}, not in our tables) [V].
- ETel (RMobilePhone, ETELMM ordinals by the SF def file): #494 **NotifySecurityEvent** (0x8ff6),
  #490 VerifySecurityCode, #484 ChangeSecurityCode, #474 AbortSecurityCode [V ordinals; I that the 9.1
  ordinals equal the SF ones]; SemcEtelApi #19/#84/#95 (names unknown).

## 3. ICopsManager proxy (OpaClient, vtable 0xf810ce50, IID {7bf1607a})

| m | proxy | input | output reserve | reply parse (0xf80cdb7e) |
|---|---|---|---|---|
| 3 | 0xf80cd9a2 | `{3}` (4) | 4+4+4 = 12 | status, then word K -> binds the caller's holder to K (f80b6e20 + holder slot 0x1c) [V] |
| 4 | 0xf80cda3e | `{4}` (4) | 4+4+4 = 12 | same as m3: **returns an interface** [V] |
| 5 | 0xf80cdad8 | `{5}` (4) | **4+4+15 = 23** | status (word 0); if >= 0: **15 raw bytes** read into the caller's buffer (f80bb29e(stream, buf, 15, fixed=1)) [V]; no bind |

So the 23-byte answer to m5 is:

```
bytes 0..3   status (int32, 0 = OK; negative = error, the 15 bytes are not read)      [V]
bytes 4..18  15-byte result copied verbatim into Security.dll's buffer (this+0x14)    [V]
bytes 19..22 unused (reserve)                                                          [I]
```

The meaning of the 15 bytes is not read yet [I]: the buffer is made by OpaClient #26 with the
IPhoneLock IID, so it is the phone-lock state/info (enabled flag, lock status, maybe a remaining-attempts
count). All zero is the most plausible "phone lock off, not locked" [I].

## 4. Why it idles (best guess) and what to change

The m5 reply (all zero) completes, the CopsManager factory is released (type 10), and the next step
never goes out: neither ICopsManager m4 (SIM lock) nor any IPhoneLock call. Candidates, cheapest first:

1. **Generated out-interface for ICopsManager m4 is wrong** [I, strong]: `syborg_ccpu_outif.inc` says
   m4 -> ICopsManager (own IID); Security.dll's SIM-lock class prepares an **ISimLock** holder for it.
   Change it (in `ccpu_opa_outif_fix`):
   ```c
   /* ICopsManager m4 -> ISimLock {c3e250a5-9ed1-2943-83e8-0383aea38f60}: Security.dll's CSimLockFactory
      (0x8930) prepares an ISimLock holder (OpaClient #283) for it */
   {{0x7a,0x60,0xf1,0x7b,0x16,0x61,0x45,0xa2,0xa8,0x0f,0x33,0x54,0xbe,0x49,0x35,0x9c}, 4,
    {0xa5,0x50,0xe2,0xc3,0xd1,0x9e,0x43,0x29,0x83,0xe8,0x03,0x83,0xae,0xa3,0x8f,0x60}},
   ```
   This only matters once m4 is called, but it is wrong either way.
2. **The 15 bytes**: if all-zero means "unknown / not ready" the phone factory may wait for an
   ICBPhoneLock-style callback. Cheap experiment: answer m5 with a nonzero byte 4 (e.g. 1 = "off"/"ready")
   via `ccpu_opa_values` (word 1 = 0x00000001) and see whether m4 or an IPhoneLock call follows [I].
   Alternative experiment: `ccpu_opa_fail` m5 -> 0x8000ffff (not found): Security.dll's error path in
   the completion (status < 0) may take "no phone lock" and move on [I].
3. **The wait is on ETel, not the CCPU** [I]: Security.dll uses `RMobilePhone::NotifySecurityEvent`
   and SemcEtelApi; with no SIM the TSY must report something (e.g. SIM absent / no lock). Check on the
   stalled phone which SecurityApp requests are outstanding: `sydb strace SecurityApp` (ETel IPCs) and
   `sydb dbg` breakpoints on SecurityApp 0xac92 / 0xad3c / 0xad5e (link base 0x8000 + its code-seg base)
   to see whether any observer slot is ever reached.

Not needed: a reply to type 10 (Release) [I]; a P&S key for SysStart [V: only Rendezvous].
