# Hosted EKA2: when correct ROM code computes wrong results

How the 808's "corrupt databases" were found (2026-11-12), and what to check first on the next device or
firmware. The full triage method for crashes and hangs is the `symbian-debugging` skill; this page is about
one failure class that is specific to the hosted kernel: **ROM code that works on QEMU gives wrong data on the
hosted kernel**, with no crash at the place where it goes wrong.

## The case: SQLite on the 808

Symptom: the 808 (Belle FP2) booted on the hosted kernel, but its SQL server answered KSqlErrCorrupt (-321)
for every database, even one it had just created; tzserver and wmdrmserver died, and start-up reset the phone.
The same drives booted fine on QEMU.

What it was not (each ruled out with a measurement, about a day of work):
- the data path: the bytes the server read matched the files (`IPCSUM` CRCs against host copies);
- the drive overlay, the seed's WAL logs, the CPU model, the heap clamp, the RAM size, IPC copy chunking, the
  user stack's fill pattern, the ROM's 64-bit helpers (`nk_aeabi`).

What found it: **looking at the stack and the arguments at the failing call**. A probe on SQLite's corruption
report (`sqlite3CorruptError`) gave the caller (`sqlite3PagerAcquire` with page 0). The page number came from
the schema table, and the schema text came from SQLite's own `printf("%lld", 3)`, which had written
`12884901891` = `0x0000000300000003`: the 64-bit argument was read from the wrong pair of words.

The cause: AAPCS puts a 64-bit variadic argument on an 8-byte boundary, **relative to an 8-aligned sp**. Every
user thread on the hosted kernel started with sp = 4 mod 8 on ARMv6K phones, so the caller and `va_arg`
disagreed about the padding word. `HostedEnterUser` (kplat.cpp) had always skipped one word below the
user-side thread data. The phone's kernel (`ckernel.cia`) skips that word only when there is *no* thread
data block. On the N95 (ARMv5, no block) the real kernel leaves sp 4 mod 8 too, so copying the N95's layout
was right for the N95 and wrong for the 808. Regression check: kn_efile, "user threads entered with sp mod 8
as the phone's kernel leaves it".

The lessons:
1. A wrong number made of two copies of a 32-bit value, a 64-bit value with its halves swapped or shifted, or
   `d + 6` computed as `d | 6` (the N95 IMEI bug, roadmap.md) points at the **calling convention**, not at the
   data. Check the ABI before following the data path.
2. Look at the registers and the stack at the first wrong result first. Diffing data paths is the expensive
   second step.
3. "The same as the reference phone" is the target, not "aligned" or "clean". Compare with the phone's own
   kernel source (`kernel/eka/kernel/arm/ckernel.cia`, `nkern/arm/*.cia`), branch by branch, for every CPU
   flavour (`__USERSIDE_THREAD_DATA__`, `__CPU_HAS_CP15_THREAD_ID_REG`, flexible or multiple memory model).

## ABI checklist for a new device or CPU flavour

The hosted kernel replaces the ARM kernel's assembler entry points with its own (`kernel/linux/kplat.cpp`,
`nkern/linux/*`). Each one must leave user mode exactly as the phone's kernel does. Before debugging data on a
new device, check these:

| What | Phone's kernel | Hosted | Check |
|---|---|---|---|
| user sp at thread entry, mod 8 | `ckernel.cia` `DThread::EpocThreadFunction` (with and without user-side thread data) | `HostedEnterUser` | kn_efile counter `HostedUserSpMisaligned` |
| TLocalThreadData size and ID slot | personality `KernLocalThreadDataSize` / `IdOffset` (sym102: 0x38) | the same values | `t_tls`, the device's personality |
| TPIDRURW / TPIDRURO | per CPU flavour (`__CPU_HAS_CP15_THREAD_ID_REG`: arm1176, arm11mp, cortex) | `device_cpu` in htest.py | `t_tls` |
| sp alignment in exec calls | the SVC handler keeps the caller's sp | trampolines realign to 8 (vectors.cpp) | roadmap.md "direct path must realign" |
| registers changed by an exec call | r0 only (and flags) | r0 only | `nk_exec` |
| 64-bit helpers (`__aeabi_ldivmod` …) | ROM's drtaeabi | run as they are | `nk_aeabi --env AEABI=...` |
| unaligned loads and stores, SWP, CLZ encodings | ARMv5/v6 rules | host fix-ups and emulation | `t_unaligned`, `t_swp` |

When a device adds a CPU flavour or a personality, add a row here and a check to the test that covers it.

## Tools (htest --env, all hex where a number)

| Tool | Use | Output |
|---|---|---|
| `SYBCODE:` lines | every RAM-loaded code segment's run address, printed at load | `grep SYBCODE` in the console log; then `tools/rofs.py --extract` + `tools/e32load.py --base CODE` to disassemble |
| `PROBE_CODE=name:offset` | one-shot probe in RAM code (`sqlsrv.exe:11074`); Thumb is handled | `PROBE ... hit`: lr, sp, r0-r11, 256 stack bytes |
| `PROBE_CODE=name:offset:rN=V` | conditional probe, stays armed (e.g. only when r0 = the bad page number) | the same, on every match |
| `PROBES=addr,...` | one-shot probes at ROM (XIP) addresses, bit 0 = Thumb | the same |
| `IPCSUM=procname` | CRC32 of every IPC write into that process: compare with host copies of the files | `IPCSUM dest len crc` |
| `NVDELTA=1` | the run's writes to C: as a delta, `tools/hosted/nvdelta.py` applies or lists them | `/out/nvmem0.delta` |
| `XTRACE_THREAD=name` + `--xtrace N` | trapped exec calls of matching threads; `tools/hosted/execnames.py --personality sym102` names them | `XTRACE` lines |
| `--trace 80000800` | KIPC: every message (MsgAc/MsgCo), to find who waits on whom | kernel trace |
| `PROBE_MS=ea60` | when kn_efile's status probe runs (deaths, sessions, servers) | `note` lines |
| `nk_aeabi` | runs the ROM's 64-bit helpers against C | test result |

A probe's stack dump answers the alignment question at once: the stack words of a varargs call show
where the caller put a 64-bit value and which slot the callee reads.

## Order of work for "works on QEMU, wrong on hosted"

1. Find the first wrong result: the error code, then the thread and the module (SYBCODE) that returns it.
2. Probe the function that reports the error, and read the registers and stack there. Is a value garbage, a
   duplicate word, or off by one word? If so, go to the ABI checklist before anything else.
3. Only then diff the data path (IPCSUM, NVDELTA) and the environment (seeds, RAM, chunks).
4. Fix it as the phone's kernel does it, add the check to the test that runs on the affected device, and add a
   row to the checklist above.
