# NeoSymbian: what their work offers qembian (review 2026-11-12)

NeoSymbian (github.com/NeoSymbian) ported EKA2 natively to AArch64 and runs it on QEMU `virt` and the Pixel
3a XL, with a current-QEMU Syborg board for the 32-bit kernel. Read-only review of three clones:
`symbian-qemu` (their Syborg board on QEMU 11.1.2), `kernel` (their kernelhwsrv fork, branch `main`),
`docs`. Tags: [verified] = read in their code or docs, [inferred] = our reasoning. Licences are as their
files state them [verified for the files named]; confirm before copying code.

## Their direction, in short

- **Native arm64 kernel**: AArch64 **ILP32** (`-mabi=ilp32 -march=armv8.2-a -mgeneral-regs-only`, aarch64
  GCC 14), the **SMP nanokernel** (`nkernsmp`) with GICv3, the **flexible** memory model; new code in
  `kernel/eka/{nkernsmp,kernel,common}/arm64` (~4.1k lines) and `baseport/arm64` [verified].
- **User side**: not phone firmware. The 2010 open-source Symbian user side, rebuilt with GCC 14 (armv5te
  soft-float) and running at **AArch32 EL0**, plus Rust (`e32-rs`; `neoui` replaces the window server)
  [verified]. So their kernel also needs CPUs that still execute 32-bit ARM.
- **Drivers**: no Linux compatibility layer. Native EKA2 kernel drivers in C++, one per Qualcomm block
  (`baseport/arm64/sdm670_assp/`, 48 files: RPMh, SPMI, SMEM/GLINK/QMI, SDHCI, DWC3, DSI, WCN3990 Wi-Fi,
  modem/ADSP loaders, ...), each written from the hardware protocol as Linux's drivers describe it (their
  comments say "read, not copied"). Kernel C++ owns MMIO, interrupts, DMA and buffers; user-side Rust owns
  protocols and policy (telephony: QRTR, QMI, rmtfs). The remote processors' own firmware (modem, ADSP,
  Wi-Fi) does the heavy work [verified: `docs/driver-support.md`, the driver headers].
- **Compared with us**: we run unmodified Nokia firmware (QEMU, and the hosted kernel on Linux, which uses
  Linux's drivers instead of writing new ones per SoC). The overlap is the Syborg board and AArch32-on-ARMv8
  fix-ups. Nothing in their work changes `docs/hosted-eka2/future.md`; it adds references (below).

## Takeaways, ranked

| # | What | Theirs | Ours | Licence of their code | Effort |
|---|---|---|---|---|---|
| 1 | qtest for the board's devices | `symbian-qemu/qemu/tests/qtest/syborg-test.c` | `src/qemu-syborg` tests | MIT: code with attribution | 1-2 days |
| 2 | hostfs device (a host folder as a drive; the EPL guest side `svphostfs` is in our baseport) | `qemu/hw/arm/syborg_hostfs.c` | `src/qemu-syborg/hw/arm/` | MIT | 1-2 days |
| 3 | Kernel raw-event queue 40 -> 256 (below) | `kernel/eka/kernel/sinit.cpp:46` | `patches/` (kernelhwsrv) | EPL | hours |
| 4 | AArch32 instructions ARMv8 dropped, emulated: SWP/SWPB, misaligned LDRD/STRD/LDM/STM, VFP short vectors | `kernel/eka/kernel/arm64/a64e_a32fix.cpp` | check against Linux's compat emulation for the hosted kernel; a future KVM/native path | EPL-2.0 | hours (audit) |
| 5 | **Template for hosted SMP** (section "Item 5") | `nkernsmp/arm64/a64s_ncsched.cpp`, `docs/ekern-backend.md` | `src/hosted/nkern/` (future.md section 5) | Apache-2.0 (derived from Phorepoc's x86-64 x32 port) | days to weeks |
| 6 | **Smaller items** (section "Item 6") | various | various | various | hours each |

### Item 3: the raw-event queue and our touch driver

`KMaxEventQueue` is 40 in the EPL kernel (`kernel/eka/kernel/sinit.cpp:48`); NeoSymbian raised it to 256
because a busy UI lost touch releases. We found the same mechanism earlier (docs/research/touch-driver-design.md
R1) and fixed the consequence in the exyin stand-in instead: a down or up that does not fit is parked and
retried every 2 ms, only moves superseded by a newer move are dropped. A larger queue would still mean fewer
dropped moves (smoother drags when the window server lags, e.g. on the Android host) and no retry delay on
presses/releases. Decide by measurement: exyin logs `SYBSTUB: exyin press N events M ms, K overflows` per
press; overflows > 0 during normal drags = take the patch (with a regression test that floods the queue).

### Item 5: a template for hosted SMP

future.md section 5 needs a Linux platform layer for `nkernsmp`. NeoSymbian's port shows the shape:
- The generic `nkernsmp` sources compile unchanged against a **pure C++ backend** (no `.cia` files): the
  per-CPU scheduler, IPIs, idle and the timer glue live in one file, `a64s_ncsched.cpp` (~1,070 lines),
  derived from Phorepoc's x86-64 `-mx32` port (`x64s_ncsched.cpp`, Apache-2.0) [verified].
- **Method** (`docs/ekern-backend.md`): link the neutral kernel, take the census of undefined symbols, and
  implement exactly those in the backend; repeat until it links. The same works for a hosted backend: our
  `nkern/linux` would get an SMP sibling where each Symbian CPU is a host thread (or a set of host threads,
  one runnable per CPU), IPIs are futex wakes or signals, and the per-CPU timers are host timers.
- Their fork fixes one generic SMP scheduler bug we would inherit: `NKern::JoinGroup` keeps the joiner at
  the head of the ready list when `pC->iPriority >= aGroup->iPriority` (upstream uses `>`); ours is still
  upstream (`kernel/eka/nkernsmp/nkern.cpp:2544`). Take it with the SMP work.
- Licence: Apache-2.0 for their backend, EPL for the generic files: code usable with attribution, per file.
- Order: after the single-host-thread and multi-process decisions (future.md 3 and 4), which change what a
  "CPU" is in the hosted model.

### Item 6: smaller items (hours each, when needed)

| Item | Where (theirs) | When it matters for us |
|---|---|---|
| GCC 14 flags for the EPL kernel sources: `-std=gnu++03 -fno-lifetime-dse -fno-strict-aliasing -fno-delete-null-pointer-checks -fpermissive` | `baseport/arm64/tools/build-ekern.sh:29-31` | compare with our kernel and hosted (clang) flags when an optimisation-level bug appears; idea only (our scodeseg.cpp -O0 hack is a case of this class) |
| SMP `NKern::JoinGroup` ready-list fix | `kernel/eka/nkernsmp/nkern.cpp` | with item 5 (nkernsmp) |
| Loader: import words masked with `& 0xffff` (images from a modern elf2e32 carry an addend in the top half) | `userlibandfileserver/fileserver/sfile/sf_lepoc.cpp:794` | if we load images built by our patched elf2e32_next and an import resolves wrong [inferred] |
| `InitialiseEntropyBuffers` ignores a failed `Kern::Alloc` (their KRN-005, documented, not fixed) | `kernel/eka/kernel/srandombuff.cpp` | low-memory boots; a two-line check in our patches |
| Unmodelled board regions as `create_unimplemented_device` so `-d unimp` logs accesses instead of reading 0 silently | `qemu/hw/arm/syborg.c` | bring-up of a new device: catches a driver touching a device we do not model; the hosted board's I/O bus could log the same |

## Not useful to us

Their Pixel 3a drivers (we use Linux's on the hosted path), `run-syborg` (sydb covers snapshots and
scripted runs), `syborg-view.py` (GPL-2.0-only; sydb's window covers it), their interrupt controller and
timers (same semantics as ours, no bug we lack).
