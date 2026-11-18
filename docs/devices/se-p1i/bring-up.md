# Sony Ericsson P1i (UIQ 3.0, Symbian 9.1): bring-up log

Profile `models/se-p1i` (family `semc-uiq3`, personality `sym91r`, firmware R6E30). Firmware formats and the
choice of this phone: [docs/semc-firmware.md](../../semc-firmware.md). [V] = checked on a run or in the
firmware, [I] = inferred.

## How to rebuild and boot

```sh
tools/fw/store.py import se_roms/P1/*.rar          # roms/SE-P1i/R6E30 (once)
tools/fw/inventory.py --device se-p1i               # core-flat.img, rofs1/2/3.img, uda-c.img (container "semc")
tools/device.py build se-p1i [--kernel]             # syborgmv kernel, stand-ins, hybrid.img + .rofs
SYBORG_DEVICE=se-p1i sydb --session build/sydb-p1i start --fresh --catch-panics --timeout 60
```
No agent: SE's EStart and SysStart start everything by `z:` path, so the sydb boot hook has no name to take
(sydb says so and boots without it; `ps`/`wg`/`strace` are unavailable, logs and catches work).

## Layout [V]

- Core XIP (BbyoneSW) at 0xF8000000, moving model, 7 fixed processes; our 25 kernel-side files after the
  firmware's, its 78 dropped (`device-compose.txt`).
- Z: = two medrofs partitions (ROFS flash header v2): drive 8 = ROFS1 + the ROFx at ROFS1's iMaxImageSize,
  drive 9 = the customer ROFS (CDA162024/70). `models/se-p1i/ESTART.TXT` replaces Z:\sys\data\ESTART.TXT.
- C: = the phone's 160 MB FAT32 user drive (uda-c.img, from EPOC_Trade_Russia).

## Boot log

1. **0.40 s FAULT, data abort at 0x8 in DRTAEABI `__cxa_allocate_exception`** [V]: the first leave (a C++
   throw in RVCT builds) found NULL exception globals: `UserSvr::DllTls`. The P1i EUSER (RVCT, Thumb) passes
   the 9.2 TLS arguments, the N73's (GCCE) does not; sym91's `DllSetTls91` shim stored r1 = 0 as the
   pointer. Fix: personality `sym91r` (personalities/sym91r/README.md, kernel patch 0026, id 9).
2. **0.46 s EStart panic ESTART_3 -5** [V]: `HAL::Get(EMachineUid)`. SE's Hal.dll serves attributes 0..7
   through the `HALKE` LDD (control 0). Fix: stand-in `src/standins/halke` (get/set/end-of-restore), values
   from Z:\resource\apps\ProductInfo.ini declared in the profile (`[[standins]] config`). EStart then
   restores HAL.DAT through controls 1/2 [V].
3. **0.87 s DebugRouter.EXE panics -1 in E32Main** [V]: SE's debug-log router (USERCOMDEB, EUSBC, ACM::3,
   debugrouter.ini); only itself dies; boot goes on. Left as is.
4. **c32exe `CBridgeServer` panic `CDecmLinkMgr 13429`** (r2 = -29, KErrCommsLineFail) [V]: `BServer.dll`,
   SE's BridgeServer: the remote-object RPC to the CCPU (`CCCPU_RemoteInvokeSession`,
   `RemoteCreateInstance`, `QueryInterface`, `MarshalInterfacePtr`, thread ids) over `CAIFMUX`
   (`caifmux.csy` -> `bridgedrv.ldd`, `AT*SECAIF=`). Nothing answers on the CCPU link.
5. **bridgedrv stand-in** (`src/standins/bridgedrv`, link layer from a static read of caifmux.csy: packet LDD
   "BridgeDRV" units 1/2, request 0 read / 1 write, CAIF control channel ENUM/LINK_SETUP/MODEM_SET/
   LINK_DESTROY answered, `AT*SECAIF=` answered OK) [V]: the -29 panic is gone. Unit 2 gets ENUM and two
   LINK_SETUPs (channels 1, 2); BServer then writes `01 00 0d000000 c814706d c814706d` on link 1, which the
   stand-in drops (H-p1i-ccpu-silent). Unit 1 gets ENUM only, then ~18 s later DoControl(3). Start-up stalls
   with C32Start's state 101f75b6/102045dd = 32 (the comms root server waits for its modules) [I: for
   BServer's first OPA exchange].

6. **RemoteCreateThreadId answered** (type 13 -> 14, [ccpu-link.md](ccpu-link.md)) [V]: BServer gets its
   CCPU thread id. Control 3 (caifmux idle hint) returns KErrNone. Then **`aps.exe` (SE AppSvr, OPA router)
   panics `APS-9` 0x8000FFE3** at 30.8 s (an OPA error). Next: the OPA calls APS makes, answered by a
   host-side CCPU model.

7. **APS-9 0x8000FFE3 explained** (2026-11-13) [V]: 0x8000FFE3 is -29 in BServer's TInt-to-OPA encoding
   (`err & 0x8000FFFF`, f81a4b0c). startuphandler.aps -> OpaClient -> BClientApi export 5
   (`RSubSessionBase::CreateSubSession(BServer, 6)`) got -29 because the BServer session had flagged the
   bridge failed (`+0x13c`): its CProxyShell observer (f81a39c2) treats word 1 of the type-14 reply as the
   CCPU's error, and our stand-in sent 1 ("thread id"). Fix: word 1 = 0. Regression test
   `tests/caif_peer_test.c` (the replies moved into `src/standins/_shared/caifpeer.h`).
8. **BServer link race** [V]: on 2 boots in 7 BServer never sent type 13 and panicked `CDecmLinkMgr 13433`
   (-33) after 22 s: the stand-in answered LINK_SETUP inside caifmux's write, before BServer watched DSR.
   Fix: 2 ms reply latency (H-p1i-ccpu-latency); 9 boots in 9 linked since.
9. **First OPA requests answered** [V]: BServer then sends type 4 (`01000000 <IID> <n>`, one per
   interface it registers) and type 0 (`<CLSID> <IID>`, CreateInstance). Replies {5|1, status 0,
   request id, value} pass the layout checks (handlers f81a4fc8, f81a5644), but every CreateInstance completes
   0x80030000 to APS: the type-1 value must be a proxy BServer already holds (ccpu-link.md "Next").
10. **Start-up animation** [V]: APS's DspBootThread loads `DspBootLdd.ldd` and starts
    `system_animation_player.exe` (ROFS1), which loads `ExtSoundLdd.ldd`, `ExtSoundPdd.pdd`,
    `DspControlLdd.ldd`, `MPEG4Dec.ldd` (panics "Could not load SoundLDD" -1 without the first; APS-9 -1
    follows). APS and EStart also open `GPIODRIVER`. Stand-ins `gpiodriver`, `dspbootldd`, `dspcontrolldd`,
    `extsoundldd`, `extsoundpdd`, `mpeg4dec` (H-p1i-gpio, -dsp-absent, -sound-silent, -video-absent): the
    player now plays nothing, ends, and exits; no panic in start-up but DebugRouter's.
11. **Where it stops now** (2.7 s) [V]: black screen; C32Start state 32; `QDefaultFileInit.exe` kills a
    thread with -11 (not investigated); APS has five asynchronous BServer requests (function 3) pending and
    nothing waits synchronously (trace report). The next step is the CCPU side: register proxies and
    answer CreateInstance, then the CCPU-initiated messages APS waits for [I].

12. **Host-side CCPU** (2026-11-13, night) [V]: `src/qemu-syborg/hw/arm/syborg_ccpu.c` in the modem core,
    `bridgedrv` forwards (ccpu-link.md "The fake CCPU"). CreateInstance works (our type 4 registers a BServer
    proxy, word 1 = the app's proxy), six CCPU objects exist, their calls (type 8) are answered with
    success and no outputs. No panic but DebugRouter's; still a black screen at 2.7 s: next is naming the
    OPA interfaces and methods and the callbacks APS waits for.

13. **LCDCTRL** [V]: APS loads `lcd_ctrl` after the OPA calls (KErrNotFound before); stand-in `lcdctrl`
    (H-p1i-lcdctrl). Processes now: EStart, domainSrv, DebugRouter (dead), sess, aps, c32start, c32exe,
    centralrepositorysrv, EComServer, EventSvr, system_animation_player, QDefaultFileInit (exits 0),
    baksrvs; no window server, no SysStart. APS waits after a call whose 12 output bytes our CCPU does not
    return (ccpu-link.md "Call outputs").

Seen but not needed yet: Hal.dll also opens `LCDCTRL` (lcd_ctrl.ldd, controls 27 and 36) and `GPIODRIVER`
(control 3); EStart opens a GPIODRIVER channel (one control without arguments).

14. **OPA output sizes** (2026-11-17, cb780fd) [V]: the CCPU answers every call with the caller's output size
    (docs/research/p1i-opa-methods.md, table `syborg_ccpu_opa.inc`). Start-up goes one step further: after
    IParameterStorageManager m3 APS creates IRegistryManager (object 4102) and stops there.
15. **What start-up waits for** (docs/research/p1i-startup-callbacks.md) [V chain, I last link]: CStarter (APS)
    loads LCDCTRL, then queues a parameter read in ccpustorage's CCcpuParameterControl, which is sent only when
    the control is "ready". Every call/CreateInstance reply is followed by APS's BServer request 3 (one-way
    `{0x1F9, 8, 0}`, `{callback key, value}`), which **BServer refuses locally with 0x8000FFFA (-6)** [V: IPC
    trace, 2026-11-17]; nothing reaches the CCPU. IParameterStorageManager m3 word 1 = 1 (ccpu_opa_values) did not
    change that. Next: why BServer refuses request 3 (BServer.dll's handler for function 3 / op 0x1F9).
16. **Interfaces returned by calls; fragmented messages** (2026-11-17) [V]: op 0x1F9 is "bind subsession to the
    returned interface" (docs/research/p1i-bserver-fn3.md): calls whose output word 1 is an interface now make the
    CCPU register a new object (type 4 with the call's P) and reply `{0, proxy key, 0}`. And a data packet with
    byte 1 = 1 is a fragment (more follows), not flow control: the CCPU reassembles. Start-up then goes on:
    IAudioDevice m21 (`/ifs/settings/...`), IParameterStorage reads (m11 then m5, variable output) feeding LCDCTRL
    ctl 12 (values 1..5); it stops after the read of parameter 0x6d5. Next: what the m5 reads must return.
17. **QueryInterface; where start-up waits now** (2026-11-17) [V]: BServer type 2 `{2, P, req, IID, obj}` is
    QueryInterface (IBatteryDevice asked for IUsbChargingControl); the CCPU registers an object for the IID and
    answers `{3, status, req, proxy}` (was unanswered). CStarter (aps.exe, docs/research/p1i-cstarter-params.md)
    now runs states 11 and 12 (catches at f81d4192, f81d41cc, f81d3e9c hit; state 13 f81d41d6 and the property
    set f81d3248 that releases EStart are not reached). State 12 (f81d3e9c) waits with RProcess::Logon while
    `system_animation_player` is EExitPending: the player is still alive, silent after 1.217 s (its MPEG4DECDRV
    requests 0..3 pending, EXT_SOUND request 0 cancelled). Next: why the player does not end (what its RunL waits
    for after the sound cancel: the H-p1i-video-absent end-of-stream path, item 10), e.g. a catch on its
    User::WaitForAnyRequest returns or an IPC trace of the player alone.
18. **SysStart and the window server** (2026-11-17) [V]: two stand-in fixes take start-up from CStarter state 12 to
    SysStart. (a) The generic stub kept no pending requests, so the animation player's `DoCancel(n)` (it names
    the request number, declared per driver: `iCancelByNumber`, MPEG4DECDRV) completed nothing and its
    `CActive::Cancel` waited forever; CStarter waited for the player's exit. (b) DSP_CONTROL_DRV controls 0/1
    `{out addr, w1, in size, w3}` return a shared chunk mapped into the client: mmmgmtserver reads
    DspCodecModule.bin into it (was address 0: FileRead -38, mmmgmtserver died, WSERV-INTERNAL 13). Now SysStart
    runs tmrsrv, SysAgt2Svr, FbServ, EwSrv, mmmgmtserver, BT servers. Next stop: semc.tsy `TSY_MErr SM:2 -6`
    after ICallManager m3 (an interface-returning call answered with zeros), then mmmgmtserver USER 21 and
    WSERV USER 47.
19. **All interface-returning calls; parameter values; the PowerVR wall** (2026-11-17) [V]:
    - `syborg_ccpu_outif.inc`: all 85 OpaClient methods that return an interface (generated, docs/research/
      p1i-bserver-fn3.md section 6); returned IIDs are partly guessed, so a call on such an object whose method
      is not in its guessed interface takes its output size from the interfaces with that method and input length
      when they agree (logged).
    - IParameterStorage m5 reads return `{0, 4, value}` (m11 size 4): M3SAudioPolicyMgr took byte 0 of 7 audio
      parameters and the empty replies panicked mmmgmtserver and semc.tsy with USER 21. mmmgmtserver now lives.
    - **Stop:** the window server loads `services` (PowerVR services, Imagination's kernel half is not in our
      kernel ROM): BitGdi/FbsCli draw through PVR2DIF -> PVR2D -> services_um. A no-device `services` stand-in
      (H-p1i-pvr-absent, refuses every bridge call) does not give a software fallback: WSERV panics USER 47 after
      the first bridge call (ctl 16). The P1i needs a 2D PowerVR path (decision pending, see the report).
    - Also open: semc.tsy `TSY_MErr SM:1 -2` (SimManager state 1).
20. **First-boot screen** (2026-11-17) [V]: the P1i shows its "Language" selection (first-boot wizard), correctly
    drawn. On the way:
    - fake CCPU: 1024 tracked objects (64 overflowed: later objects lost their IID, semc.tsy's ISystemControlManager
      m4 got no outputs, TSY_MErr SM:1 -2); ISystemControlManager m4 returns IRadioControl; IPhonebookManager m4
      fails "not ready" (no SIM; a phonebook object made the client walk entries forever, 20000+ objects);
      docs/research/p1i-semc-tsy-sim.md.
    - crypto.ldd stand-in (H-p1i-crypto-absent) and the DRM clock plug-in left out (H-p1i-no-drmclock).
    - LCD HAL `lcd_semc_p1i`: SE's ScDv writes 240-pixel lines into 1024-byte rows (patch 0027,
      SYBORG_FB_LINE_PIXELS=256); with 960 the UI was smeared.
21. **Language chosen, power menu, start-up stall** (2026-11-17) [V]: touch works (exyin). After "UK English" the
    phone restarts its start-up; SysStart needs a working USB client (EPL usbcc + eusbc on a null controller,
    H-usbc-null). It then shows "Memory corrupt D:" and the Power menu (Phone on / Flight mode). After Flight mode a
    progress bar stays full: IAccessoryControl m7 was polled forever (now "not found"); still open: c32's port
    handler (porthandlersvr.dll) panics E32USER-CBase 40 (an active object destroyed on its error path, USB bearer).
22. **Port handler fixed; SecurityApp waits** (2026-11-17) [V]: c32's port handler needs PDD "EUART" (H-p1i-euart-alias)
    and the USB personality start (RUsb::TryStart) needs enough endpoints (the null controller now offers 15 each way;
    with fewer it completed -5 and the port handler panicked !PortHandler 17). No panics left in start-up. After Flight
    mode the splash's progress bar stays full: SysStart started SECURITYAPP.exe (93 s) and waits for it; SecurityApp
    makes its OPA calls (ISecurity-like interfaces, ICopsManager m5) and then idles, waiting for an event the CCPU never
    sends [I]. Next: what SecurityApp waits for (a CCPU callback with SIM / security state).
23. **SecurityApp: lock objects, then a CCPU callback** (2026-11-17) [V]: ICopsManager m3/m4 return ISimLock /
    IPhoneLock objects (Security.dll's SIM-lock and phone-lock classes, proxy slots +0x2c/+0x30); the generated table
    bound them as ICopsManager (m5 then answered 23 bytes where ISimLock m5 has 12). Fixed in `ccpu_opa_outif_fix`
    (test `test_cops_manager_returns_lock_objects`). SecurityApp still waits: it registered an **ICBSimLockStatus**
    callback {23b8d43b} (type 4) and the fake CCPU never calls callbacks. The ETel side is fine (only notifications
    pending). Next: CCPU->app callbacks (docs/research/p1i-ccpu-callbacks.md).
24. **CCPU->app callbacks; Flight mode completes; SysStart reaches CLUI and the Phone app** (2026-11-17) [V]:
    - The fake CCPU now calls registered ICB* objects (BridgeServer type 8 `{8, the app's session word, req, the
      app's target word, n, method, args}` on the calls' link; `ccpu_callbacks` in syborg_ccpu.c). The target is
      the registration whose handle a call carried (word 1 of the args), else the latest one of that interface.
    - ISimLock m5 -> ICBSimLockStatus m3 (0, 0): SecurityApp's SIM-lock check passes (its handler acts only on a == 0).
    - BServer's own reply to such a call corrupted its heap (H-p1i-bserver-callback-reply).
    - ETel reports no SIM (NotifySecurityEvent completes with ENoICCFound), so SecurityApp shows its kind-0 "no SIM"
      state: CPhoneUiMediator slot 6 -> TeleFramework #99 (waits for two start-up images) -> waits on
      POWER_ALL_CLEAR_SEM, then RThread::Rendezvous. The Phone app's Telelib waits on the same semaphore.
    - powerapp signals it when the power-mode change completes: startuphandler -> BAPhandler.aps -> IRadioControl m6
      (subscribe its ICBRadioControl handle; output word 1 = request id) -> m4 -> callback ICBRadioControl m3 (a = 0)
      -> m7(request id) -> its completion = success -> powerapp's callback -> 3 x Signal. Our m6 answered id 0, so
      BAPhandler skipped m7. Now `ccpu_opa_values` gives id 1 and the CCPU sends the callback after m4.
    - Result: SecurityApp rendezvouses, SysStart starts CLUI.exe and Tele.EXE (Phone). Still on the splash: SecurityApp
      then dies in ZebraMMFDevSoundPlugin.dll (+0x30e1, memmove into a buffer whose Ptr() is 0) and the splash stays.
      Next: what the splash and the Phone app wait for.
25. **Start-up completes: the "No SIM card" screen** (2026-11-17) [V]: EXT_SOUND_DRV control 3 now hands out a play buffer
    (a shared chunk in the client's process, size = control 2's last word). Before, SecurityApp's key tone memmoved
    into address 0 and SecurityApp died KERN-EXEC 3. The Memory Stick (D:) image starts out as an empty FAT32 volume
    (`format = "fat"`), so the "Memory corrupt D:" note is gone. After Language, Yes, Flight mode and Select, the
    phone shows SecurityApp's "No SIM card" note, then its "Insert SIM card" screen (Call = emergency call, Help).
    That is where a P1i without a SIM stays. One command: `python3 tools/sydb/sydb.py phone p1i --fresh`.
    Next for a home screen: a virtual SIM behind the CCPU (ISimManager, SIM status callbacks; ETel
    NotifySecurityEvent then reports no ENoICCFound).
26. **Home screen: a minimal SIM** (2026-11-17) [V]: a P1i without a SIM stays on "Insert SIM card" (as the real
    phone; reviews of the P990 say there is no SIM-less mode). The fake CCPU now presents one SIM application with
    no PIN and no network (docs/research/p1i-minimal-sim.md):
    - ISimManager m5: word 1 = 1 application.
    - ISimManager m3: type 1 and label "S".
    - ISimManager m6 returns an ISimApplication object (semc.tsy QIs ISimApplication and IGsmSimApplication).
    - ISimApplication m6(PIN n): state 0, 3 tries, 10 unblock tries. All zeros was "SIM card blocked".
    After Flight mode the UIQ standby ("Sony Ericsson", Today, launcher bar) shows. Open: the Phone app
    (Tele.EXE) panics TeleApp -1 ("Application closed" note over the home screen).
27. **Paused here (2026-11-17)** [V unless marked]:
    - Phone app TeleApp -1: the aps.ini edit (H-p1i-no-drmclock) blanked drmclockhandler in place, the APS loader
      stopped there and never loaded asrhandler.aps; the ASR server died -1 and the Phone app's voice-answer plugin
      with it. Now the edit keeps asrhandler (under test: the Setup wizard appears).
    - asrhandler then read `/afs/system/voice...` from the CCPU file system in a 34000-call loop: IFileSystemManager
      m8 now fails not found (H-p1i-no-ccpu-files).
    - Open: c32's port handler panics `!PortHandler 4`: porthandlersvr.dll 0x83ce TRAPs its set-up 0x831c, which leaves
      -1 in 0xa8c8 -> 0xa79c -> 0xc23a -> 0xc1e4: `RSocketServ::Connect` + `RHCIDirectAccess::Open` (Bluetooth HCI)
      [V call chain]: `RHCIDirectAccess::Open` fails -1 [V catch], then `RSdp::Connect` -1 [V]: the Bluetooth stack
      never starts. bt.prt loads erihci.dll (`bt.bt.esk`: `hcidllfilename= erihci.dll, port= 6`), which opens the LDD
      BTDRV / `btdrv.ldd`; our hybrid has no btdrv.ldd, and the phone's needs SE's UART layer (ecust.dll, GPIODRIVER)
      (docs/research/p1i-bluetooth.md).
    - Tried and dropped: NOPing the leaves after the Bluetooth connects in porthandlersvr.dll (0xc202 HCI, 0xa7b2 SDP,
      0xa7fc BT accessory, 0xa810 SettingCli, 0xa846 HID) gets past !PortHandler 4, but the port handler then uses the
      unopened HID session through bluetooth.dll and dies KERN-EXEC 0; `BtBearer.ph` = 0 changes nothing.
    - Options: (a) a Bluetooth controller: a `btdrv.ldd` stand-in (factory BtDrvDRV, requests 0 read / 1 write a frame,
      controls 0..10, H4-like framing) forwarding to a host-side HCI responder (Command Complete status 0 + fixed
      parameters for the init set; erihci also sends vendor commands 0xFC0F/0xFC22/0xFC2B for its patch download and
      asks the CCPU for the hardware type), est. 1-2 days; (b) live with the !PortHandler 4 note (the phone runs on).
