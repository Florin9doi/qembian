# Tooling ideas

Missing tools that would have saved time, written down after long debugging loops. Reviewed
periodically by the project owner; implemented items move to "Done" with a pointer to the tool.

Format: one entry per idea: **what it is**, *which loop it would have shortened* (with a date), and
a rough cost.

## Open

- **Exec-call argument audit for the 9.2 personality**: list every 9.2 exec call whose argument
  types differ from ours (by value vs by reference, extra/missing args), by comparing the N95
  EUSER stubs' register set-up with our `execs.txt`. *2026-11-05: `TimerAt` passing `TTime&` cost
  the boot 140 s and was only found by stack-walking a spinning thread.* Cost: one day.
- **Undefined-instruction catcher**: report every UNDEF the guest takes with the instruction word
  and the module, so emulator decoding gaps (like the non-canonical `CLZ`) show up at once.
  *2026-11-05: the AknIconServer crash was first seen as a kernel fault with a confusing "far".*
  Cost: small (kernel trace in the undef handler or a QEMU TCG plugin).
- **DOS event log**: log every DosServer event raised (id, value, raising DSY) in the serial log, next
  to the `SYBPROP:` lines, so the ISI reply → DSY → DOS event → sae key chain shows in one run.
  *2026-11-05: the "no SIM" fix needed reading NokiaDSY and sae by hand to see that our 0x1C meant
  "card present" and that SEC cause 0x16 is ignored.* Cost: small (a `syb_gdb brk` preset on
  DosSrv's raise function, or a kernel-side hook on the DosServer session).

- **ISI reply-layout extractor**: for a client handler of a response, decode the ISIMSG field
  accessors it calls (`#165`/`#255`/... take a field id that maps to a fixed offset, `#303` finds a
  subblock) into "offset → accepted values", so a fake-modem reply can be written in one go.
  *2026-11-05: the RTC status reply `19 0B` took four boot-and-probe rounds because the field ids
  (0 → [10], 18 → sb+2) looked like raw offsets.* Cost: small (table of the ISIMSG accessor
  switch statements + a disassembly walk in `tools/isi.py`).

- **UI state probe**: one command that reports the foreground window group/app, whether the
  keyguard/screensaver/autolock is active (Avkon/SysAp P&S keys) and the last screen-update time
  (DisplayDriver ctl 9), so "screen frozen" vs "still locked" is decided from data, not from
  screenshots taken during a slow redraw. *2026-11-06: the keyguard investigation took 7 boots
  of key/screenshot guessing; the answer came from the code and the absence of ctl 9 lines.*
  Cost: small (extend `SYBPROP:` to the Avkon/SysAp categories + a `syb_report` section).

## Done
- `tools/syb_gdb.py` fixes (2026-11-05): stack reads clipped at the end of the supervisor stack (they
  failed silently, so `--stack` and kernel-symbol probes looked broken; the kernel .sym mapping was
  fine), user call site + 9.2 exec name at kernel-mode stops (replaces the "KERN-EXEC call-site
  logger" idea), probe address confirmation and gdb output on "no hits".
- `tools/syb_run.py` + `tools/syb_report.py` (2026-11-05): one-command boot/screenshots/keys and a
  structured report incl. the "likely blocked" IPC wait list.
- `tools/syb_gdb.py` (2026-11-05): leave tracer with module filter, breakpoints with kernel
  expressions (kernel DWARF loaded), current-thread names, symbolised sampling.
- `tools/isi.py` + `platform/isi/replies-bb5.txt + models/<id>/isi-replies.txt` (2026-11-05): ISI names, unanswered-request
  summary, reply table as data.

- `tools/syb_sym.py` (2026-11-05): address → module/export/function/literal, symbol search,
  callers of an export across all modules, string search with references, word dumps,
  per-function decompile.

## Ask the modem log first (2026-11-06)
The "frozen idle" hunt spent many runs on screensaver/keyguard timing before looking at unanswered
modem requests. Idea: `syb_run` report section "unanswered ISI requests" (resource, msg id, first
time, count, sending channel owner) so a missing fake-modem reply shows up on the first run.

## Log app error notes (2026-11-06)
"Carousel: Memory full" needed a manual hunt: a conditional breakpoint `User::Leave` with
`$r0 == -4`, then breakpoints on Hitchcock's EGL error paths. Idea: `sydb errnotes`, which logs every
Eikon error note as an event (app, error code, leave site). Breakpoints on `CEikonEnv::HandleError`
/ `CTextResolver::ResolveError`, or `User::Leave` with an error filter, plus the caller from LR. A
cheaper first step is `sydb dbg leaves CODE --thread T` (conditional Leave breakpoint and
backtrace as one command).

## Freeze and snapshot on panic ("fork on exception"), 2026-11-06
Hunting Carousel's USER-EXEC 3 (an integer divide by zero in Hitchcock that only happens with
some timing) took many runs: every breakpoint or watchpoint changed the timing and the bug went
away. Symbian has no fork(), but three things would give the same effect:
1. **Freeze on panic/exception** (kernel side, fits the phase-2 debug LDD): a DKernelEventHandler
   for EEventKillThread / EEventHwExc that, for a watched process, suspends the thread before it
   dies, prints registers, about 1 KB of stack with symbolised return addresses (SYBCODE/XIP map)
   and the faulting module, and raises a sydb event. Then `sydb dbg attach` sees the live state.
   It also catches User::Panic / RaiseException (USER-EXEC 3) through EEventKillThread.
2. **Snapshot on panic** (host side): the daemon sees `thread.panic` for the watched process,
   stops QEMU (QMP `stop`) and saves a snapshot (`savevm panic-N`; needs a qcow2 overlay for
   the drives). The moment can then be reloaded and inspected with gdb as often as needed,
   like a forked copy.
3. **Record/replay** for races: QEMU `-icount shift=auto,rr=record` and then `rr=replay` makes a
   failing run deterministic, so breakpoints no longer change the outcome.
1 is the cheapest and most useful first step; 2 builds on the daemon events we already have.
**Done 2026-11-06** as `sydb catch` + `sydb snap`/`fork` (docs/sydb.md "Catch, snapshots and
forks"), host side through gdb on cold paths (User::Panic, DThread::Die) plus QMP migration, with
no kernel change. Record/replay (3) is still open.

- **Disassemble a RAM-loaded (ROFS) DLL** (2026-11-07, 5730 NOKIATSY): `tools/e32load.py` does not read
  byte-pair compressed images. Workaround used: `elf2e32_next --dump=hc --e32input=X`, then turn the
  hex code dump into a binary and `objdump -b binary -m arm -M force-thumb` it (no import names).
  Wanted: byte-pair support in e32load.py, or `tools/fw/probe.py code NAME` doing the extract +
  decompress + ELF with import names in one step.
- **Find what start-up waits on**: the manual loop was syb_report on a log cut before the shutdown,
  then requests completed with -33/-3 to sysstart, then the next ISCPEER line. A `syb_report.py
  --until T --who sysstart` view (open requests of one client at time T, with the modem messages they
  caused) would make it one command.

- **Byte-pair compressed E32 images** (`0x102822AA`): `tools/e32load.py` reads only deflate/none, so
  ROFS executables of Symbian^3 (N8) cannot be loaded or symbolised; the composition research needed
  a throwaway decompressor. Add byte-pair decoding to e32load (and so to syb_sym for ROFS code).
- **Ghidra `fn` on N8 core DLLs** (`syb_sym.py fn gce.dll ...`) produced no `ghidra/*.c`; check the
  headless import for the N8 core image.
- **elf2e32_next `--libpath` with several entries looped forever** (it never advanced past the first);
  fixed in patches/elf2e32_next/0002-libpath-search.patch for the container build. The host build in
  ~/work/elf2e32_next is unpatched, so src/khr/build.sh links every import library into one directory.
- **`sydb wait-screen [--changed] [--stable MS] [--timeout S]`**: block until the screen changes and is
  stable, print the elapsed time. Timing UI steps (tap, then wait) was done with a scratch script that
  polled screenshot hashes; scripted flows also raced the first-boot wizard without it.
- **TLB/flush statistics on demand** (QEMU HMP command or QOM property): ASID context hits/misses,
  domain drops, large-page full flushes per index. Today they are printed to stderr every 5 s with
  `SYBORG_ASID_TLB_STATS=1`, and the large-page counter does not exist yet (docs/perf.md).

- 2026-11-09 (N8 Settings delay): `tools/ipc_latency.py` added (slow and open IPC requests from a KIPC-traced
  serial.log). Still missing: a way to keep the KIPC bit on without `sydb strace` (a `sydb trace ipc on|off`),
  catch thread filters that work for kernel-side breakpoints (thread names resolve to '?'), and a
  "who is this thread blocked on" query (wait object of a thread, from the kernel's NThread state).

- 2026-11-09 (5800 Settings root cause): IPC tracing needs message/session IDs,
  stable thread identity across renames, and notification-vs-synchronous labeling.
  `ipc_latency.py` currently pairs FIFO by client name/opcode, so cross-server
  collisions and gaps can produce misleading durations. A modem audit should flag
  mismatched service identifiers even when `deliver` exists. A request-semaphore
  wait alone must not be displayed as proof of an idle app. See the review in
  `docs/devices/nokia-n8-rm596/settings-delay.md`.

- 2026-11-10 (701/808 bring-up): a waiting thread's *user* call stack. In the flexible model gdb reads
  only the current process, so a thread blocked in User::WaitForRequest had to be caught with a hardware
  breakpoint inside EUser's WaitForRequest loop conditioned on its NThread (TheScheduler.iCurrentThread,
  0xC8000C28 on the syborgfx udeb kernel), then woken. A `sydb ustack THREAD` (switch to the thread's
  address space, dump and symbolise its stack from the saved user SP) would have made it one command.
  `tools/sydb/gdb/threads-blocked.gdb`'s "nstate" reads NThreadBase+77, which on this kernel is
  iTimer.iState (always 0); only its "nwait" (+40) is right. Also missing: `sydb mem` (free/total RAM
  pages; read here from TheMmu.iRamPageAllocator by hand) and a pending-sync-IPC list per thread
  (the scratch pairing script paired file-server messages wrongly).

- **ngage_sess: catches filtered by thread, and a cheap "who returned this error".** (2026-11-12, N-Gage
  Bluetooth) Finding where a -11 came from took many boots: a conditional catch on User::Leave or
  LeaveIfError is evaluated on every hit (~0.5-1 s each), which makes boot crawl, and the condition
  can't name a thread. Wanted: `--catch 'VA=LABEL?thread~SocketServer'` evaluated in the daemon from the
  current-thread word without a full register read, and an "exec return value" trace (kernel exec
  handler results != 0 per thread) to see failing kernel calls without breakpoints.

- **A BridgeServer/OPA decoder for `SYBCAIF:` lines, and failures that cannot be filtered away.**
  (2026-11-13, P1i APS-9 / CreateInstance) Each BServer message was decoded by hand from hex (type, proxy,
  request id, GUIDs) and matched to its reply by eye; a `tools/caif.py log SERIAL` (like `tools/isi.py log`:
  request/reply pairs, unanswered requests, GUIDs named from OpaClient's interface table) would have shown
  "every CreateInstance completed 0x80030000" at once. Two time sinks of the session: `device.py build`
  reports a stand-in compile failure as one "failed" line and keeps the previous binary in the ROM (two
  boots tested an old stand-in), and the trace report's "likely blocked" list is empty when the stall is
  an asynchronous wait (APS's five BServer requests), so it needs a "pending since boot, never completed"
  view per client.

- **`sydb strace` silently ends at `--max 200`; the N8's screen needs `frame.seq`, not `screenshot`.**
  (2026-11-13, N8 `*#0000#` hang) Two hours went to tracing the wrong thing: `sydb strace MATCH --secs S`
  defaults to `--max 200` events, and when the cap is hit it clears the kernel KIPC bit, so the trace
  stops after a few seconds *exactly when the app under investigation starts*. The default should be
  large (or none, with `--max` only for a bounded sample), and the end of a run should say why it
  stopped ("hit --max 200"). Likewise `sydb screenshot` returns a black picture on a host-composed phone
  (N8/701/808): the picture is only in `<session>/frame.seq` (magic `FSEQ`, seq at 4, w/h/on at 8, pixels
  at 4096, geometry from `hmp xp /17wx 0xc0005000`). A `sydb screenshot` that decodes the frame sequence
  when `syborg-gl` composes on the host (as `sydb phone` does) would have shown the spinner/black window
  at once. Scratch decoder: `/tmp/n8shot.py` during the session — worth making it a tool.

- **Who is blocked *without* an IPC request: `tools/sydb/kwalk.py` (added 2026-11-13).** The trace
  report's "likely blocked" list only covers IPC, so a thread parked in `RSemaphore::Wait`, a mutex or
  a message queue is invisible — which cost an hour on the N81 splash stall (FbServ's two threads
  deadlocked on one of its own semaphores, no IPC pending at all). `kwalk.py` walks the kernel's
  `DThread` objects (runtime vtable, anchored on `TheScheduler.iCurrentThread`) and prints, per
  thread, the kernel call it is blocked in, read from `NThreadBase::iSavedSP`. Two gaps left:
  `sydb catch --kind any --action dump` never fires for a *killed* process (`RProcess::Kill` does not
  reach `DThread::Die`), so there is still no user-stack dump of a live blocked thread; and the
  thread walk only covers the low 16 MB of kernel RAM (later processes' DThreads were not found by the
  vtable scan — find the heap anchor, e.g. `DProcess::iThreadQ` of each process, instead of scanning).
  Also: the kernel's `KSEMAPHORE` trace bit (`--trace 0x80000010`) logs every semaphore Wait/Signal
  with its count and is the fastest way to see a lost signal — worth a line in docs/tools.md.

- **An ABI checker that counted stubs as implementations.** (2026-11-13, N81 splash stall)
  `tools/personality/check.py` compared the firmware EUSER's used SWIs against `personalities/*/exec.map`
  and treated an `!invalid` slot as present, so `F0a`/`F0b` (the N81 calls them for
  `User::LockedInc/LockedDec`, i.e. every `RCriticalSection`) looked implemented while our handlers were
  no-op stubs returning 0. Lesson for the family: a checker must distinguish "mapped" from
  "deliberately not implemented", and a review of skipped slots is worth more than the count it prints.
  Fixed (`supported_execs()` skips `!invalid`); `tests/device_py_test.py::ExecMapTest` keeps it honest.
- **`kwalk.py` on the moving memory model (2026-11-17).** On the SE P1i (syborgmv) `kwalk.py` finds 0 threads
  (its DThread vtable anchor assumes the multiple model's layout). Needed: find threads through the kernel's
  object container (K::Containers[EThread]) instead, so it works for every memory model.
- **sydb fork/restore and gdb (2026-11-17, SE P1i).** (1) `fork`/`restore` take the device from `$SYBORG_DEVICE`, not
  from the snapshot's session, so a P1i snapshot restored as an N95 (RAM size mismatch). (2) A fork resumed only by
  gdb (`dbg continue`) keeps its block devices inactive; the first flash write asserts in QEMU
  (`bdrv_co_write_req_prepare: !(bs->open_flags & BDRV_O_INACTIVE)`). Workaround: `qmp cont` first, with gdb
  `set breakpoint always-inserted on`. (3) The daemon dropped the connection twice during `dbg wait` after a
  breakpoint hit in a fork. (4) A `dbg attach` after a session restart can talk to a stale gdb: breakpoints set,
  never hit. Wanted: fork/restore read the device from the snapshot, resume through QMP, re-attach gdb per QEMU pid.
- **ngage_sess (2026-11-18, Nokia 6600 bring-up):** (1) `break VA=~LABEL` on a running machine is accepted
  but the logging catch only arms after a `pause`/`cont`; it looked as if the code never ran. Wanted: arm
  at once, or say so. (2) No `key` op: scripted presses go through QMP by hand, and the 6600's ekeyb
  ignores presses shorter than about a second (debounce). Wanted: `ngage_sess key CODE [--hold S]`.
  (3) `stack` printed stale words; it now marks the saved SP (DThread+0x158).

## From the P910i bring-up (2026-11-18, eka1/tools/ngage_sess.py)

- **Breakpoint budget:** with more than about six breakpoints/catches none fired, silently. The daemon should
  check each `Z1` reply and refuse (or warn) past the limit.
- **Conditional logging catches:** `VA=~LABEL?EXPR` ignores EXPR and logs every hit (about 1 s each): apply the
  condition before logging.
- **`x` on another process's run address** fails while that process is not mapped; translate through the owning
  chunk's home address (DChunk +0x28) automatically, as `stack` does.
- **An OSE request decoder:** a tool that takes a signal number and prints its swbp.dll stub, the awaited reply
  signals and the decoder's reads (done by hand with grep over a listing, see docs/devices/se-p910i/how-it-was-done.md
  s. 3), and a `p910i-gsm:` log reader that lists UNANSWERED requests per run.
- **Leave finder:** a one-shot "who left with code N" (the CPU-state trace on User::Leave's first block, filtered
  and symbolised) instead of grepping qemu.log by hand.
