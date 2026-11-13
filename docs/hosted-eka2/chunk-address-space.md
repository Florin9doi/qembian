# Hosted EKA2: chunk address space (design review, 2026-11-12)

A design review written by an agent, read-only, no runs. [V] = read in the cited file, [I] = inferred.
`MM/` = `build/epocroot/sf/os/kernelhwsrv/kernel/eka/memmodel/epoc/`.

## The problem

All Symbian processes live in one Linux address space. User-visible chunks are placed in one window
[0x01000000, 0x70000000), 1.75 GB, and each chunk reserves its whole maximum (`DWin32Chunk::DoCreate`).
Local heaps are clamped to 16 MB. On the 808, at ~150 s with ~130 heaps, the window holds 1.69 GB reserved
and 49 MB committed; new processes fail with KErrNoMemory (welcome2, widgetui, mdsserver: STDLIBS-INIT -4).

## The answer

Real Symbian also reserves a chunk's whole maximum at creation, **per process**. Our fault is that every
process's local reservations share one window. On a phone the 808's 34 MB heaps overlap at the same
addresses in separate address spaces.

| Model | Max reserved at creation? | Per process or global | Same address for local chunks of different processes? | Base stable? |
|---|---|---|---|---|
| Moving | yes, in the process's data section [V MM/moving/mprocess.cpp:378-389]; the kernel "home" address holds only the committed range and may move [V moving/mchunk.cpp:266-337] | run address per process | yes [V moving/mprocess.cpp:351] | user-visible run address fixed; the kernel asks the process (`DataSectionBase`) |
| Multiple | yes, from a linear-section allocator [V multiple/mchunk.cpp:79,147] | local chunks per process (`iLocalSection`), global chunks in one shared section [V multiple/arm/xkernel.cpp:114-116] | yes | fixed; the kernel reaches other processes through per-thread aliases [V multiple/arm/xmmu.cpp:2586] |
| Flexible | yes, one mapping of the whole memory object per process [V flexible/mchunk.cpp:218] | per address space, [0x00400000, 0x7f000000) [V flexible/mmu/maddressspace.cpp:160] | yes; even a global chunk may sit at different addresses per process [V flexible/mprocess.cpp:556-577] | fixed while mapped [I] |
| Win32 emulator | yes, `MEM_RESERVE` [V emul/win32/mchunk.cpp:107] | global | no | fixed |
| hosted (now) | yes, local heaps clamped to 16 MB | global | no | fixed |

What user code relies on:
- **A stable base while the heap grows.** RHeap grows in place with `Adjust` [V heap_hybrid.cpp:1838,1913].
- **Addresses below 0x80000000** (`(TInt)this` handles, e.g. FbServ).
- **The maximum it asked for.** RHeap lays itself out from `MaxSize()`, so a clamp shows up as early
  KErrNoMemory in big-heap apps [I].
- Global chunks at one address in every process are **not** promised by the flexible model. Keeping one
  address is a stricter, safe choice.

Constraints of ours:
- **One Symbian thread runs at a time** [V src/hosted/README.md]. A per-process view switched on process
  switch is sound now; under SMP it is not.
- **EXE data is copied on switch, not remapped.** Remapping cost ~6% of start-up under TCG
  [V mutils.cpp:204-242, roadmap.md].
- **The kernel reaches other processes' static data through `MM::CurrentAddress` / `DllDataAddress`**
  [V mutils.cpp:257-309].

## Options

| Option | Faithful? | Effort [I] | Speed | Risk |
|---|---|---|---|---|
| **(a) Per-process local window** (multiple-model layout). Local chunks in [0x01000000, 0x40000000), per-process addresses, memfd-backed, mapped only while their process runs. Global/shared/shbuf chunks in a shared window [0x40000000, 0x70000000). The kernel reaches a non-running process's chunk through an alias or pread/pwrite | yes (multiple model; a subset of flexible); no clamp | 6-10 days | 2-5 syscalls per switch between user processes | kernel code that dereferences another process's local pointer directly; disconnected chunks need per-run maps; not SMP |
| (a2) as (a), with parked anonymous memory moved by mremap on switch | yes | 7-12 days | 2 mremaps per committed area per switch | bookkeeping |
| (b) reserve less, grow in place | no: a taken neighbour fails `Adjust` below the maximum | 2-3 days | 0 | random out-of-memory failures, depending on order |
| (c) committed size plus a margin | no, as (b) | 1-2 days | 0 | as (b) |
| (d) one Linux process per Symbian process (future.md section 4) | most faithful, SMP-ready | 4-8 weeks | a host context switch per process switch; IPC through a syscall | large |
| (e) lower the clamp / move code above 0x80000000 | no | <1 day | 0 | only postpones the failure |

## Recommendation

(a) now, keeping (d) as the SMP path; (a)'s per-process allocator and memfd backing carry over to it. Steps:
1. `lxwin32.{h,cpp}`: split the window into local and shared; `VirtualReserveUser` serves the shared one.
2. `DWin32Process`: a per-process local-address allocator (64 KB units) and the list of mapped local chunks.
3. `mchunk.cpp`:
   - local EUserData chunks get the owner's addresses and a sparse memfd of `iMaxSize`;
   - commit and decommit map only when the owner is running, and decommit punches holes;
   - remove `KLocalChunkMax`.
4. `mutils.cpp`:
   - `DoProcessSwitch` unmaps the old process's local range and maps the new one's committed runs;
   - `MM::CurrentAddress` resolves a non-running process's local chunk to a kernel alias.
   - Check every RawRead/RawWrite caller goes through it.
5. Tests: a new `kn_localwin` with:
   - two processes at the same local address with separate contents over 1000 switches;
   - 200 processes with 34 MB maxima;
   - one global chunk seen everywhere;
   - kernel raw reads and writes into a non-running process's heap;
   - decommitted pages faulting;
   - mmaps per switch reported.

   Regressions: 808 `kn_efile` without STDLIBS-INIT -4; N95 start-up times unchanged.

## A second review (another model, same day) agrees on (a) and adds

- [V] The hybrid heap can place its initial heap in the **middle** of its maximum range and asserts contiguous
  growth (`kernel/eka/common/heap_hybrid.cpp:1431,1819,3182,3207`). Sparse high-offset commits happen at once,
  so (c) "committed size plus a margin" fails immediately, not rarely.
- [I] With overlapping local addresses, every path that reaches user memory asynchronously must carry the
  owning process and a reference to the backing:
  - the GL helper thread's callbacks (`hgl.cpp`: raw virtual addresses through self-`process_vm_readv`);
  - DFCs.

  "The current process" is no longer a valid substitute.
- [I] Interrupt paths must use pinned access or defer the copy; building a scratch mapping while holding the
  interrupt lock can deadlock with the suspended thread.
- [I] Effort estimate 10-20 days. No maximum-sized host alias per chunk: map only the live view and bounded
  scratch ranges.

## Built (2026-11-12): switch on conflict

Option (a) with one change: chunks are not unmapped on every process switch.
- Reservation is per process and logical.
- Committed pages stay mapped until another process's chunk needs the same addresses.
- A switch remaps only the incoming process's displaced chunks.

Details:
- New chunks get addresses no other process uses while possible, else where the fewest pages are mapped.
- The kernel copies directly when both sides are mapped where they belong (kernel locked), else through the
  process's memfd.
- The window is [0x01000000, 0x3f000000). It sits below the multiple model's ROM DLL data, which starts at
  0x3ffed000 on the N95.

Measured:
- N95 start-up unchanged (SplashScreen 3.67 s, EikSrvs 4.16 s): no address is shared there.
- 808 at 15 s: 19 displacements and 4 file copies; at 150 s: ~12k displacements.
- The 808 reaches state 110.

Tests: kn_efile "local window", on both phones:
- two processes at one address;
- switch displacement and remap;
- copies through the file;
- decommitted pages refused;
- 92-96 heaps of 34 MB.

Open: frequent displacement between two busy heaps (each an mmap); the 2-mapping cost per commit/decommit.
