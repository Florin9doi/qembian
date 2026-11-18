# Nokia 9500 (RA-2): why CuiWs.exe leaves with KErrNotFound at start-up

Static reading of `eka1/ra2/core/ROM.bin` (VA = 0x50000000 + offset). No emulator run.
[V] = read in the code at the given address, [I] = inferred.
Symptom: `CuiWs` E32Main (0x50B728C4) TRAPs StartL (0x50B72880), gets -1 and panics "CuiWs" 201.

## Call tree of StartL, with every call that can return an error

Import thunks: `ldr r3,[pc,#4]; ldr r3,[r3]; bx r3` (literal at +8), and for 4-argument calls
`push {r6}; ldr r6,[pc,#8]; ...` (literal at +12). Ordinals resolved through each DLL's
TRomImageHeader export directory (header word 0x40) [V].

| step | address | what it is | can it give -1? |
|---|---|---|---|
| 1 | 0x50B72888 | CuiUtilities ord 141 (450, 0): RProcess::SetPriority + RThread::SetPriority (0x50B495B4) | no [V] |
| 2 | 0x50B728AA | CCuiWsEnv (0x34, vtable 0x50B74FDC) ConstructL = vtable[15] 0x50B72874 -> EikCore ord 303 (this, 5) | not read [I: CEikonEnv/CCoeEnv-style base; would need WServ too] |
| 3a | 0x50B7273C | CuiUtilities ord 110 -> 0x50B48C04: Dll::SetTls only | no [V] |
| 3b | 0x50B72742 | CuiUtilities ord 25 -> ConstructL 0x50B487FC: **two RSystemAgent::Connect** (Sysagt ord 20) + LeaveIfError | yes, see below [V] |
| 3c | 0x50B72746 | CuiUtilities ord 24 -> ConstructL 0x50B47698: **RSharedDataClient::Connect** (Commonengine ord 10, "SharedDataServer"), one more RSystemAgent::Connect, Sysagt ord 2 read | yes [V] |
| 3d | 0x50B7274A | 0x50B6F2D4: object 0x3C, DSClient ord 59/55/58/52, CuiUtilities ord 157/143 (UID 0x101F8DE5, "AppFrameworkAccess"), ord 4 | not read [I] |
| 3e | 0x50B72750 | 0x50B719BC -> ConstructL 0x50B71B1C: **RWsSession::Connect** (Ws32 ord 43) + LeaveIfError, RFs::Connect(4) (EFSrv ord 18) + LeaveIfError, then 0x50B70F9C -> 0x50B711CC (cover LCD) | yes [V] |
| 3f | 0x50B7275C/62 | CuiUtilities ord 108 / ord 152 | not read |
| 3g | 0x50B7276A | CServer::StartL("CuiWs") (EUser ord 1102) | -11 if a 2nd CuiWs, not -1 [V] |
| 4 | 0x50B728B6 | CActiveScheduler::Start | (leaves come from RunL) |

Cover LCD open at 0x50B711CC [V]: `User::LoadLogicalDevice("CuiLcd")`, `User::LoadPhysicalDevice("CuiLcdPdd")`,
`RBusLogicalChannel::DoCreate("CuiLcd", TVersion(0,0,0), unit -1, pdd NULL, info NULL)`, each
LeaveIfError, then DoSvControl(8). Kernel side is consistent: LDD ctor (0x50B3EBB0) iParseMask 2
(physical device allowed, no unit), PDD Install names itself "CuiLcd.Lilly2" (0x50B4133C, literal
0x50B423E8), PDD Validate returns 0 (0x50B41348) [V]. So this path would not give -1 unless a
file were missing (both are in the ROM) [I]. **Since the cover LCD driver sees no traffic, the leave
happens before 0x50B711CC**, i.e. in 3b-3e [I].

## Candidates for the -1, most likely first

1. **RWsSession::Connect -> server "Windowserver" not running** (step 3e, 0x50B71B22).
   Ws32 ord 43 = 0x5033A118 -> 0x5033A078: `CreateSession("Windowserver" @0x5033C038, ver, -1)` then
   LeaveIfError; there is no start-the-server retry [V]. RSessionBase::CreateSession returns
   KErrNotFound when no server of that name exists [V: EUser semantics]. The inner display is not on
   yet, which suggests EwSrv (`System/Libs/EwSrv.exe`, 0x5030DDF0) has not started or not yet
   registered its server when CuiStarter launches CuiWs [I]. On the real phone EwSrv is up long
   before the cover UI [I].
2. **RSystemAgent::Connect -> "SystemAgent"** (3b/3c). Sysagt ord 20 = 0x501C9EE8: CreateSession;
   on -1/-15 it starts `Sysagx` once (0x501C9E24: RProcess::Create, Logon, Resume, wait) and retries;
   the second -1 is returned as is [V]. -1 therefore means Sysagx.exe started but did not create
   the "SystemAgent" server before signalling, or RProcess::Create could not find it [I].
3. **RSharedDataClient::Connect -> "SharedDataServer"** (3c, Commonengine 0x507860E0): same
   start-and-retry-twice pattern; returns -1 only if the started server never appears [V/I].

## Cheapest way to tell which (one boot, no code change)

Breakpoints (or a catch) on the LeaveIfError right after each connect, read r0:
- 0x5033A0D4 (Ws32, after CreateSession "Windowserver") - expected hit with r0 = -1 [I]
- 0x50B48806 / 0x50B48812 (CuiUtilities, after RSystemAgent::Connect)
- 0x50B476B4 (after SharedData connect), 0x50B476D6 (3rd SysAgt connect)
- 0x50B71B26 (CuiWs, after RWsSession::Connect), 0x50B71B34 (after RFs::Connect)
Also: is EwSrv in the process list when CuiWs dies?

## Fix, depending on the answer

- Windowserver: EwSrv must be running before CuiWs. Find out why EwSrv is not up (it is the same
  blocker as "inner display not on": its start-up probably waits on the inner display driver
  VideoDriver.dll / a HAL attribute, see docs/research/ra2-inner-display.md) [I]. As a stop-gap,
  delay CuiWs (CuiStarter) until "Windowserver" exists - but the real fix is EwSrv starting.
- SystemAgent / SharedDataServer: make Sysagx.exe / SharedDataServer.exe start (check their own
  deaths in the log); no stand-in answer can replace a missing user-side server.
- Not the LCD stand-in: nothing in CuiWs reaches the CuiLcd channel before the leave [I].
