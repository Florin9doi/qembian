# N-Gage first boot is too slow (factory-fresh flash): investigation notes

Status: **QD: fine now** (owner, 2026-11-26: ~2 min for a fresh boot, under a minute for a kept flash). **NEM-4:
the "freeze" was a different bug, fixed:** euart1.pdd's endless UART transmitter wait (H-nem4-uart-txempty,
docs/devices/nokia-ngage-nem4/bring-up.md "Status"); a fresh NEM-4 now reaches the Time query in ~4 min. The notes
below are the earlier (2026-11-26 morning) investigation of slow flash writes.

Original status: **open** (2026-11-26). Expected: a factory-fresh N-Gage QD (`-M ngage` on HEAD f40e28d, RH-29 V 04.10)
reaches SysAp in 1-2 min (docs/eka1-ngage.md says ~40 s to the logo, ~95 s to the UI, measured 2026-11-11).
Observed: after 5-10 min it is still at the start-up stage with a white screen. A kept flash (second boot) is
not affected. The N-Gage NEM-4 port (stashed, see the end) behaves the same, so this is a board problem.

Marks: [V] verified (run or code), [I] inferred.

## Reproduce

```sh
# factory-fresh QD, session tools (baseline tree, eka1/output + eka1/extracted)
S="python3 eka1/tools/ngage_sess.py -s agent-qdfresh"
rm -f build/ngage/phones/qdfresh-agent.flash
$S start --model ngage --flash build/ngage/phones/qdfresh-agent.flash
$S objects | head -12; wc -l build/ngage/sess-agent-qdfresh/serial      # SysAp prints "CSysApAppUi..."
```

Timeline [V, 2026-11-26]: t=30 s EKern/EFile/EStart; t=55 s + EwSrv, FbServ, Starter, SharedDataServer, DosServer;
then nothing new for minutes (kernel debug output stays at 3 lines: `TRtcQueue::Add: 2/4`, `auxstart 105`). The
NEM-4 under the same conditions crawled on to SplashScreen (NOKIA logo), EInfoServer, randsvr, RfsServer, Sysagx
after ~10 min.

## Not a recent regression [V]

The same fresh boot on QEMU built from HEAD (7da861e) and from 1976d48 (2026-11-22): 3 serial lines after
150-180 s, no SysAp. So it predates the 3650/3660/6680/7650-camera work. Commits not yet tried: anything before
1976d48, in particular 45e20ab (2026-11-12, which brought H-ngage-idle) and be411f8/7a4cb86 (2026-11-11/12).
**Next cheap step: bisect further back** (build a worktree's QEMU incrementally, see "Tools" below).

## What the CPU does [V]

- PC samples (pause/cont, `regs`): mostly FIQ mode in medint.pdd (QD 0x503808A4..0x50380908), else the null thread
  (0x8040719C) asleep in the NOS stub's idle WFI (0x5149EA38, CPSR 0x6000001B, H-ngage-idle). So the system is idle,
  blocked on flash I/O, not looping.
- Flash traffic (`-trace pflash_data_write -trace pflash_io_write`): from t≈33 s, chip 1 (`ngage.flash1`) offset
  0x400000 upward is programmed **one halfword per program cycle** (0x40, data, then 0xFF), plus block erases
  (0x20/0xD0) and 0x60/0x03 configuration writes; each offset shows up ~4 times over the run. Rate: **~140
  halfwords/s**, i.e. ~29 KB written in 150 s. Median gap between data writes 4.6 ms (spread 1-20 ms, not
  quantised to the 15.6 ms tick).
- `-d int` + timestamps: **one FIQ per halfword**, FIQs arriving 1-10 ms apart in host time, although timer 3 is
  programmed for 0x1FE ticks = 14.7 us (periodic). Also ~130 000 SVC (executive-call) exceptions and ~3000 IRQs
  in 50 s (who makes the SVCs is not known yet).

## How medint writes flash [V static, QD addresses]

- Thread side 0x503815FC..0x50381644: starts a run: timer 3 (0x0C120A60) mode 2 (periodic), count 0x1FE, status
  cleared, FIQ source 3 unmasked (helpers 0x50382CFC/D08/D14/D20 = ekern imports).
- FIQ handler 0x50380890: per FIQ reads the flash status (bit 7 ready, bit 4 error), writes 0xFF, then 0x40 + the
  next halfword, returns (`movs pc, lr`). At the end of the buffer: calls 0x50382CB4 -> import slot 0x50342080 =
  ekern 0x5033F304 = **Kern::Add(TDfc&)** (export 7), stops timer 3 (+0x04 = 0), masks FIQ source 3, and leaves
  through `ldr pc, [ip, #36]` = 0x50321B1C, ekern's FIQ exit.
- Kern::Add only links the DFC into the queue anchored at **0x80006BB8** (no flag).
- ekern's FIQ exit 0x50321B1C: calls the hook at `*(0x80005E6C + 0x18)` if set, then `*(0x80005E6C + 0x1C)`; if
  that returns non-zero and the FIQ interrupted neither IRQ nor SVC mode, it switches to mode 0x1B, calls
  `+0x20`, and then runs a pending reschedule (flag 0x80000378 -> 0x50322018) or the DFC queue (-> 0x5032202C).
  With +0x1C null it is a plain return, so a DFC queued from FIQ waits for the next IRQ.
- The table at 0x80005E6C (9 words) is set only through **ekern export 390** (0x50322404, validates the pairs
  +4/+8, +0x10/+0x14, +0x1C/+0x20, copies 0x24 bytes; 0x503223D0 reads it back). Nothing in the ROM's files calls
  it: on the phone the (scrambled) NOS presumably does, and our NOS stand-in never does. At run time the table is
  all zero [V].
- The null thread (0x5032BB24): calls the idle hook (variant slot 3, method 18, r1 = 13: our stub), then, with the
  tickless global 0x800004AC = 0, ekern's empty ASIC idle 0x5031DC78, and loops. It never runs DFCs itself.

## Hypotheses tested

| Hypothesis | Test | Result |
|---|---|---|
| H-ngage-idle's WFI makes the FIQ-queued DFC wait for the tick | patch the stub's `mcr` to a NOP through gdb before start | **invalid test**: gdb's write to the ROM-mode flash went to the pflash device as a command; the machine printed nothing. Redo by rebuilding the stub instead |
| The missing post-FIQ hook (+0x1C/+0x20) delays medint's completion DFC | gdb at t=20 s: `*(0x80005E88) = 0x5031F8AC` (ekern "return 1"), `*(0x80005E8C) = 0x5031E698` ("return 0") (scratchpad hook_test.sh) | **no change** (still 3 serial lines at 300 s). Probably still worth giving the stub (the NOS installs it on the phone [I]), but it is not the cause |
| Completions are tick-bound | gap histogram | no: gaps are spread 1-20 ms, median 4.6 ms |

## Leading suspicion (not proven)

The time is lost **between FIQs** (14.7 us programmed, ms observed), on the host side. Candidates:
1. **QEMU cost per program cycle**: every 0x40 switches the CFI01 chip out of ROMD (read-array) mode and 0xFF back
   in, i.e. two memory-map rebuilds and TLB flushes per halfword. With thousands of halfwords this may dominate.
   Check with a host profile (below), or by timing a loop of program cycles.
2. Timer 3 expiry delivered late (main-loop timer latency while the vCPU sleeps in WFI, or FIQs masked for long
   periods by the ~2600 SVC/s).
3. The SVC storm itself: some thread is polling. Find it (`ngage_sess.py objects`/`stack`, or a breakpoint on the
   SWI vector with a count) and why (a timeout the RFS/DBMS code hits because writes are slow?).

The 2026-11-11 measurement (95 s) predates H-ngage-idle (2026-11-12): bisecting across 45e20ab tells quickly
whether the idle WFI is involved.

## Tools for the next session

- Host profiling: `perf` is blocked (`perf_event_paranoid` = 4) and ptrace is limited to descendants
  (`ptrace_scope` = 1): run QEMU **under gdb** and send it SIGINT from outside every 0.4 s (`handle SIGINT stop
  nopass`; command file: `run`, then N x (`thread apply all bt 14`; `continue`)). Find QEMU's PID as gdb's child
  (`pgrep -P $GDB`); never `pgrep -f` with a pattern that also matches your own shell. The first attempt produced
  no samples: check that gdb's `run` really started (look at the .txt output) before relying on it.
- Timestamped traces: `-d int -trace pflash_data_write -D /dev/stderr 2>&1 >/dev/null | python3 -c '...prefix
  time.monotonic()...'` (the log backend has no timestamps).
- Monitor sockets: a unix socket path under the scratchpad is too long (AF_UNIX limit): use `-monitor
  tcp:127.0.0.1:PORT,server,nowait`. No `socat` on this host: talk to it from Python.
- Writing guest memory: gdb writes to RAM work (e.g. 0x80005E6C at t=20 s); writes into the flash/ROM region do
  not (they hit the pflash command interface).
- A/B against old commits: `git worktree add --detach DIR COMMIT`, symlink `downloads/`, run its
  `scripts/build-qemu-ngage.sh` (first build ~10 min, then incremental), run that binary on
  `eka1/output/rom/ROM.bin` with `-M ngage,ppm=...,isi=...,flash=NEW` and wait for "SysAp" in the serial file.

## Related work (staged with this note)

The first N-Gage (NEM-4 V 4.03) is `-M ngage`, the QD is `-M ngageqd` (`--model ngageqd` in the tools); the QD's
extraction is in `eka1/rh29/`, the NEM-4's in `eka1/nem4/` (flash files `eka1/nem4_rom/`, git-ignored). NEM-4 reached
the NOKIA logo on its first boot, then the same slow flash phase: docs/devices/nokia-ngage-nem4/bring-up.md. The
"Reproduce" commands above were run before the rename: with this layout use `--model ngageqd` for the QD, and
`eka1/rh29/output/rom/ROM.bin`, `eka1/rh29/extracted/RH2941.026`, `-M ngageqd` in direct QEMU runs.
