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
