# N81 8GB (RM-179): missing atomic execs fixed (2026-11-14)

**Boots (2026-11-17):** N81 11.0.045 and the E71 300.21.012 reach the first-boot region query. The loader-heap
corruption below ("two RHeaps on one chunk", E71's LoaderThread USER 45) was our multiple-model
`ReadExportDir` writing count + 1 words (9.2: ordinal 0 first) into the 9.1-style loader's count-word buffer
(efile 0x801620c2: `User::Alloc(iExportDirCount * 4)`, `buf[ordinal - 1]`); it also bound every import one
ordinal off (the AAC decoder's NULL read). Fix: kernel patch
`personalities/sym92l91/patches/0027-sym92l91-export-dir-from-ordinal1.patch`, test
`tests/export_dir_ordinal1_test.py`. The sections below are history.

## Verified root cause and fix (supersedes the historical hypotheses below)

The original FbServ stall is an emulator exec-ABI bug, not a missing bitmap buffer, compression
configuration, ECom plugin, or firmware signal. N81 11.0.045 EUSER uses:

| Address | Operation |
|---|---|
| 0x8012605c | RCriticalSection::Wait: LockedDec(this+4); only skip semaphore wait if OLD value is 1 |
| 0x80134a64 | User::LockedDec wrapper |
| 0x8011dc6c | ARM `svc 0x80000b; bx lr` |
| 0x80126072 | RCriticalSection::Signal: LockedInc(this+4); signal semaphore only if OLD value < 0 |
| 0x80134a5c | User::LockedInc wrapper |
| 0x8011dc64 | ARM `svc 0x80000a; bx lr` |

`personalities/sym92/exec.map` marked F0A/F0B `!invalid` because the N95 did not call them.
sym92l91 shares that map. `Exec92BadFast_*` returns **zero without changing memory**, so Wait
incorrectly falls through to the semaphore and Signal cannot perform the required counter update
or negative-old-value wakeup. The kernel semaphore implementation itself is not the faulty part.

Fix: map F0A/F0B to the existing `LockedInc91`/`LockedDec91` atomic shims (update memory and return
the original value). No FbServ ROM patch, memory-pressure override, or invented signal is needed.

The earlier interpretation of a critical section as simply a count-1 kernel semaphore was wrong:
its user-space counter starts at 1 and controls the fast path. Absence of kernel Signal calls does
not establish that the worker skipped a firmware release. The firmware worker at 0x8087f31e calls
Wait, updates fields, then unconditionally calls Signal at 0x8087f338.

Validation: kernel rebuild succeeded with zero errors. Fresh 11.0.045 boot in
`build/sydb-n81-atomic-review` draws the Nokia logo (`/tmp/n81-atomic-fixed.png`) with the Nokia
FbServ code unmodified, and proceeds past the old wedge. It later encounters
`Startup::MMFControllerProxyServer-a3 Panic KERN-EXEC 3` (15.770 s), Startup panic
`E32USER-CBase 46` (15.782 s), and a kernel exception at 16.883 s, user PC 0x80120418,
DFAR 0x01743144. These are remaining bring-up failures; this is not a claim of boot-to-idle.
A second fresh boot, with tracing disabled (`build/sydb-n81-atomic-review2`), independently draws
the same logo and reaches the same exception PC/DFAR at 12.149 s. Both test sessions are stopped.

The ABI checker also incorrectly counted `!invalid` entries as implemented. Fixed that check and
added `tests/personality_check_test.py`. Driver vtable checks pass; the stricter SWI check now
reports a separate unimplemented Se8 present in N81's EUSER (not established as a cause of the
later crash). Do not describe the full ABI check as passing.

## Historical investigation (2026-11-13; causal claims below are superseded)

Owner report: "the N81 does not even boot, not even the Nokia logo". Status was L0
(`report-devices-2026-11-10.md`: stall at ~9-12 s on a white screen).

Reproduced on 11.0.045 (and 21.0.010 behaves the same): everything answers up to **~8.5 s**, then the
log stops and the machine is **idle** for minutes (`dbg attach` → `NKIdle`), no panic, no thread death.
The ARM1136 alignment fix (`patches/qemu-baseport/0026`) composed in changes nothing.

## The chain [V, KIPC trace build + tools/syb_report.py]

```
8.53s  SplashScreen.exe::Main          -> !AknIconServer     M:0     ← the Nokia logo asks for its icon
8.55s  AknIconSrv.exe::!AknIconServer  -> !Fontbitmapserver  M:43    ← never answered
8.68s  !HWRMServer::!HWRMServer        -> SAServer           M:1
```

`dlopen`/modem/drive activity is not the problem: a KLOCDRV build shows every local-media control and
request completed to the stall (`>DLocalDrive::DoControl … ret 0`); the only unanswered ISI requests
are the pre-existing boot ones (SIM `E2`, CALL `44`, audio `66`).

## Root cause: two fast execs of the 9.2 personality were stubs [V, 2026-11-13]

`F0a`/`F0b` in `personalities/sym92/exec.map` were `!invalid`, i.e. our handlers answered 0 and never
touched the counter they were handed. The N81's EUSER calls exactly those slots for
**`User::LockedInc`/`LockedDec`** (`svc #0x80000a` at `0x8011dc64`, `svc #0x80000b` at `0x8011dc6c`), and
its `RCriticalSection` keeps its counter with them: the firmware's export table
(`build/roms/.../abi/n95-euser-swi.json`) has `RCriticalSection::Wait() -> 0x80000b`,
`RCriticalSection::Signal() -> 0x80000a`, `User::LockedInc/Dec -> 0x80000a/b`. With the counter frozen,
the critical section's wait/signal decisions are wrong: it can be taken and never signalled — the
deadlock described below. Mapping both slots to the existing 9.1 atomics (`@LockedInc91`,
`@LockedDec91`) makes the N81 draw its logo **with no ROM patch** (`code_patches` stays empty), which
proves the cause.

The checker that should have caught it (`tools/personality/check.py`) counted an `!invalid` slot as an
implementation; it now ignores them (`supported_execs()`), so a missing SWI is reported as `MISSING`.
For the N81 that leaves one: `Se8` (`SetFloatingPointMode` by the swi map) — next on the list.
Regression test: `tests/device_py_test.py::ExecMapTest` (also checks, per device, that the SWIs the
`RCriticalSection`/`LockedInc`/`LockedDec` exports use are all mapped and not stubs).

## The deadlock the stubs caused [V]

The M:43 handler in the N81's `FbServ.exe` (Thumb; `xipdis.py core-flat.img core.json FbServ.exe`,
dispatcher `cmp r0,#43` at `FbServ+21e`/`0x8087d506` → `0x8087d8c6`):

```
8087d8e4  blx RMutex::Wait()                              ; the bitmap/server lock
8087d8ee  blx FbsCli!CBitwiseBitmap::CheckBackgroundCompressData()
8087d904  bl   0x8087efa8                                 ; queue the job (RPointerArray + 40-byte record,
                                                          ; then a virtual call on this+0x4080 vtable[0])
8087d91c  blx RMutex::Signal(); return                    ; the message completes here
```

FbServ runs **two threads**: `!Fontbitmapserver` (the server) and `FbsBgCompressThread` (the worker,
created at `FbServ+0x21c0`, `RThread::Create` at `0x8087f420`, priority `0x64`, then `Resume()`).
This is the first two-thread server in our fleet — the N95's FbServ is single-threaded, and on the
N95 the same client's first request is `M:0`, which completes instantly, i.e. **the N95 never takes
this path**.

A semaphore trace build (`device.py build nokia-n81-8gb-rm179 --trace 0x80000010`, the kernel's
`KSEMAPHORE` bit) shows FbServ's one local semaphore (`FbServ.exe::Local-c8035758`):

```
9.708  Semaphore FbServ.exe::Local-c8035758 Wait 0 Timeout 0     ← acquired (count 1 -> 0)
9.883  Semaphore FbServ.exe::Local-c8035758 Wait -1 Timeout 0    ← blocked (count 0 -> -1)
```

and **no `Signal` on it anywhere in the boot**. With `tools/sydb/kwalk.py` the same state is visible
from the other side: both FbServ threads are parked in `ExecHandler::SemaphoreWait → DSemaphore::Wait
→ NKern::Block` (the worker's and the server's supervisor stacks both start with those frames), and
one boot shows the shared semaphore's count at **-2** (both threads waiting). The server thread also
*holds* a `DMutex` with hold count 1 (`iMutexList`), i.e. it is inside the M:43 handler.

So the two threads wait on one counting semaphore of FbServ's own, the first having *acquired then
re-waited*, and the boot's first icon never comes back. `DSemaphore::Wait`/`Signal` in our kernel
(`kernel/eka/kernel/skernel.cpp`) implement the ordinary EPL semantics (count goes negative on
blocking, one `Signal` wakes one waiter), so this is not a broken semaphore primitive: **the firmware
never signals that semaphore in this boot** — its own logic decides that it must wait, and the other
thread is not the one that delivers.

## Why the N81 takes that path at all (next to test)

Background bitmap compression is a *memory-pressure* feature: FbServ only compresses "background"
bitmaps when it believes memory is tight, and it hands the job to `FbsBgCompressThread`. On the real
phone the splash does not normally trigger it. Candidates for what differs here:
1. **Memory figures.** The N81 kernel profile uses `H-ram-168` (168 MB: 128 MB + the unpaged core
   image) and `paged = true`, so the firmware's free-RAM/low-memory view is ours, not the N81's.
   Compare the firmware's own numbers (`HAL::Get(EMemoryRAMFree)` etc.) with the real N81 at idle.
2. A CenRep key or property that enables background compression (FbServ reads CenRep early); the value
   our ROM hands it may differ from the phone's factory image.
3. An error from the ECom service FbServ imports (`ECom.dll`, 4 imports) that the firmware swallows
   on the path that should signal — a swallowed error there turns "compression finished" into nothing.

## Rule: no ROM code patches for this (owner's call, 2026-11-13)

The stall is something *we* are missing, so it gets implemented on our side — kernel, stand-ins,
composition — not patched out of Nokia's `FbServ.exe`. (A temporary experiment that branched the M:43
handler to its error path did draw the splash, which is useful as *evidence* that the wedge is inside
that handler; it is not a fix and has been reverted.)

## Ruled out [V]

- `EComServer -> FbServ: M:4 r:-17004` at 7.000 s (`KEComErrNoInterfaceIdentified`) is for
  `KUidShaperFactoryPlunginInterface = 0x10279726` (the OpenFont shaper factory). That UID does not
  exist anywhere in the RM-179 11.0.045 firmware (core.json + all three ROFS images searched), so the
  lookup fails on a real N81 too: not our bug. It is also the only ECom failure in the whole boot.
- The N81 package has no factory user data (`RM179_11.0.045_002_erase_uda.fpsx` only erases C:), so a
  blank C: is correct for this phone; the missing piece is not on the UDA layer.
- The lookup is not memory-pressure driven in an obvious way and the modem/drives are clean (above).

## Where the two threads are, from live user-stack dumps [V, 2026-11-13]

The catcher already dumps a thread's user registers (from the kernel's `ThreadUserContextTables`) and a
symbolised user-stack scan; a `catch add --at ADDR --thread FbServ --action dump` rule delivers it for a
*live* thread, because the stop happens while that thread is current (so its user memory is mapped).
Armed at `RSemaphore::Wait`, `RCriticalSection::Wait` and `RMutex::Wait`
(`sydb start --paused`, arm, `sydb resume`), the dumps show:

```
FbServ.exe::FbsBgCompressThread        (the worker)
  at    EUser.dll+a51a  RSemaphore::Wait()
  caller EUser.dll+b098 RCriticalSection::Wait()+14
  stack  FbServ.exe+207c  <-  FbServ.exe+2c84  <-  FbServ.exe+2c38      (its job loop)

FbServ.exe::!Fontbitmapserver          (the server, inside the M:43 handler)
  at    RCriticalSection::Wait()  (from FbServ.exe+207c)
  stack  FbServ.exe+20ce  <- FbServ.exe+2686  <- FbServ.exe+1cfc  <- FbServ.exe+682
                                                                  ^ the enqueue call (0x8087efa8)
```

So the sleeping primitive is an **`RCriticalSection`** — a count-1 local semaphore, i.e. *not*
re-entrant — shared by the worker's job loop and FbServ's enqueue path; the M:43 handler takes an
`RMutex` (recursive, a different object) before it, so that pairing is fine. The `KSEMAPHORE` trace
showed the CS taken once (`Wait 0`, 1 -> 0) and then blocked twice (`Wait -1`, then -2) with **no
`Signal` on it at all**, so the CS is entered and never released. The worker's job path is therefore
the leak: it takes the CS, and the release never happens — while `User::Leave`/`LeaveIfError` catches
on both FbServ threads show no leave in the job path (the leaves seen are the server's font/`TFindFile`
and ECom probes, the latter for the shaper UID that the firmware does not contain). So the worker
*returns without signalling*, i.e. its "job done" leg depends on something our environment does not
provide, rather than on an exception.

## Open, separate: the start-up multimedia thread crashes (the real next domino)

Order of events in a KMMU-trace boot [V]:

```
15.086  Exc 1 FAR=0x00000060 FSR=5  R15=0x81293b44 (our kernel), R14=0x80128a15 (EUser)
        thread Startup::MMFControllerProxyServer-a3          ← "magic address" descriptor recoveries, 5 in a row
15.100  Exc 1 FAR=0x00000004 FSR=5  R15=0x77270af8 MmfEnhAacPlusDecoder.dll+0x8af8
        R14=0x772900a3 MmfEnhAacPlusDecoderPlugin.dll+a3      ← NULL+4: a member read off a NULL pointer
15.103  Thread Startup::MMFControllerProxyServer-a3 Panic KERN-EXEC 3
15.121  Thread Startup::startup Panic E32USER-CBase 46       ← consequence: its server thread died
```

The thread is `Startup`'s MMF controller proxy, bringing up the **start-up audio path**:
`RMMFControllerProxy::SendSync` -> `CMMFController::MMFObjectContainerL` -> `AACAudioPlayControllerPlugin.DLL` ->
`AdvancedAudioController.dll` -> `MmfServerBaseclasses_ord112` -> `ECom REComSession::CreateImplementationL` ->
`MmfEnhAacPlusDecoderPlugin.dll` -> the decoder's constructor, which reads `[NULL+4]` (catch dump:
`regs (exception frame)`, `r0=0`, `FAR=4`).

Just before, the *same* thread produced five "magic address" recoveries (`Exc 1` with a small FAR,
`R15` inside our kernel's `IsMagic` path, `R14` in `EUser`'s `RMessagePtr2::Read/Write`). That path is
our own hack: when a client passes something that is not a valid descriptor, the kernel skips the
instruction, sets Z and returns the "address" — i.e. **the customer's call is silently given garbage
instead of `KErrBadDescriptor`**. A client that then trusts the returned buffer ends up with a NULL/
garbage pointer, exactly like the decoder above. `docs/hacks.md` has this as an open TODO ("worth
checking against exec92's IPC argument-flag translation").

### The descriptor path is NOT the bug (instrumented, 2026-11-13) [V]

A temporary `Kern::Printf` in `RMessageK::TMsgArgs::ReadDesHeaders` (kernel `sipc.cpp`, removed again)
printed, for every IPC call, the four argument words, the flags, the descriptor flags and the DACR, plus
the parsed header per descriptor slot. Result:

- the MMF thread's "suspicious" call parses **correctly**:
  `args 00407c88 00000001 801237e1 00407b88 flags 00000807 descflags 00000804` ->
  `slot 0 ptr 00407c88 r 0 type 3 len 42 max 256 data 00407c90` (type 3 = `EBuf`, data inline at
  header+8, i.e. the `"Z:\private\1104F57\MMRProvider..."` text) and
  `slot 3 ptr 00407b88 r 0 type 3 len 4 max 4 data 00407b90`;
- the `Exc 1` "magic address" recoveries are the kernel's **designed** fault-tolerant descriptor
  validation: a client that passes an integer in a descriptor slot gets `r -38` (`KErrBadDescriptor`)
  and the slot is marked undefined - e.g. `slot 1 ptr 00000140 r -38`. Not a bug, and not the cause;
- `dacr 00000001` in every call is the normal state (user pages are not in domain 15 here).

So the earlier "descriptor ABI" hypothesis is **disproved**: the 9.1/9.2 argument convention (args in
r4..r8, loaded by the EUser veneer `ldmne r2,{r4..r8}; svc 0x4c`) is handled correctly by our kernel.

## The fatal fault: two RHeaps on one chunk (instrumented, 2026-11-13) [V]

Temporary `Kern::Printf`s in `ExecHandler::ChunkCreate`/`ChunkAdjust` (kernel `sexec.cpp`, removed again)
plus `catch --at RChunk::Adjust --thread LoaderThread --action dump` narrowed the loader fault to:

- **one** chunk object (`c80253b0`, created once by `efile.exe::LoaderThread`, max 16 MB, unnamed at
  creation, later named `LoaderThread::$HEAP`), **one** thread, and **every** adjust goes through
  `RHeap::Alloc`/`RHeap::ReAlloc` (no direct chunk adjust by the loader);
- the adjust sizes **oscillate between two monotonic series**: `0x34, 0x44, 0x45, 0x46, 0x47, 0x48` and
  `0x33, 0x34, 0x35, 0x36, 0x37, 0x38` [V: the log lines]; every adjust returns 0 (success) [V], so the
  chunk really does shrink to the smaller series' size and decommit the other's pages, and the next
  write into them is the fatal fault (`DFAR=0x01743144`, `DFSR=0x807` = write + page translation fault,
  `PC` in EUser's RHeap code) [V];
- **interpretation [I, not proven]:** two `RHeap` objects on the same chunk, each with its own `iSize`
  (the alternative is one heap whose `iSize` is reset between grows - the two series are exactly
  0x10000 apart, which fits two heaps created with different initial sizes);
- the chunk is created **once**, so the second heap is constructed on the *existing* chunk - the loader
  reuses a chunk it should have replaced (a chunk handle that should have been closed is still valid, or
  the second `RHeap` is constructed on the same chunk after the first was destroyed without the chunk
  being reset).

Next: instrument `HandleClose`/`ChunkClose` for the loader thread and check whether the chunk's handle is
closed between the two heaps; if it is not, the fix is in our handle-close semantics (`RChunk::Close`,
`HandleDuplicate92`), not in the heap.

## Also open: a fatal fault in the efile loader (secondary)

Separately, the loader dies later writing into its own `$HEAP` chunk (base `0x01700000`): see the data
below; it happens *after* the Startup crashes and is probably a consequence of the loader being
mid-load into a process that dies, but it must be checked once the decoder crash is fixed.

With the logo drawn the boot goes on and stops on a **fatal** fault. Owner's datum (`PC 0x80120418`,
`FAR 0x01743144`) reproduced and explained [V, 2026-11-13]:

- `0x80120418` is **EUser heap code** (the nearest-export name `TDes16::Fill` is wrong: Ghidra shows
  `FUN_801203c0` is the RHeap free-list logic), reached from the **`efile.exe::LoaderThread`** (caught at
  `Exc::Fault`, gdb reports that thread).
- User registers at the fault: `r4=0x01700000` (chunk base), `r2=FAR=0x01743144` (the write pointer),
  `r1=r6=0x00010038`; user stack: `RHeap::Alloc` <- `User::Alloc` <- `efile.exe` (loader).
- `DFSR=0x807` = write + *page translation fault*: the page is missing entirely, on a write **inside**
  the chunk: the chunk is `efile.exe::LoaderThread::$HEAP`, base `0x01700000`, and the KMMU trace shows
  it being adjusted 0x36000 -> 0x46000 -> 0x47000 -> 0x36000 -> 0x37000 -> 0x47000 -> 0x48000 ->
  0x37000 -> 0x38000, i.e. it **grows and shrinks (decommits) repeatedly on the same base**, and the
  faulting address 0x43144 lies in pages that an earlier adjust had committed and a later one
  decommitted. So one heap is writing into pages another adjust/commit cycle has taken away — the
  suspicious pairs are the shrink-then-grow sequence and the possibility of two `RHeap`s over one chunk
  (chunk handle mix-up: `HandleDuplicate92`/`ChunkCreate` are our shims) or a chunk-address collision in
  the multiple-model allocator (two chunk objects reporting base 0x01700000 while one is alive).
- The fault is **fatal** rather than a thread panic because `Exc::Dispatch` sees
  `NKern::HeldFastMutex()` true: `TheCurrentThread->iNThread.iHeldFastMutex == &TheScheduler.iLock`
  (owner field = the same thread). A user-mode thread must never hold the scheduler's fast lock, so our
  fast-mutex bookkeeping also leaks somewhere; that turns a recoverable user fault into a stopped
  machine and should be fixed even after the heap bug.

Next measurements (all cheap):
1. Count the chunks claiming base `0x01700000`: walk efile's `DProcess` chunk list at the stop (gdb +
   DWARF; `kwalk.py`'s vtable trick works for `DMemModelChunk` too) and print base/size/name per chunk.
   Two live chunks with that base = address-space collision; one chunk with two `RHeap`s = handle mix-up.
2. Trace the chunk API for the loader thread only (`KPROC`/`KMMU` are on in `hybrid-proc.img`):
   `Exec::ChunkAdjust`, `DMemModelChunk::DoCommit/DoDecommit` for base 0x1700000, to see which adjust
   removes the page that is later written.
3. Then chase the held-fast-mutex leak (a kernel path that returns to user mode while
   `iHeldFastMutex` is set).

## What is still to find (our side)

Narrow the worker's job body: catch `RCriticalSection::Signal()` (N81: `0x80126072`) with the same
`--thread FbServ` dump rule, and a dump rule on `FbServ+0x2686`/`+0x2c38` (the release sites, from the
stacks above) to see which release is expected and where the job path actually diverges. Then the
missing piece is one of: the bitmap's shared compress data (chunk/mapping the worker reads — a
client-allocated buffer our FBServ/AknIcon path may not be setting up), or the queue's release being
skipped because a value our kernel returns differs (the sym92 exec map's `RCriticalSection`/`RSemaphore`
pair is shared with working servers, so a plain primitive bug is unlikely).

