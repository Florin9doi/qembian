# EKA1 vs EKA2: kernel sizes, and what reimplementing EKA1 would cost (2026-11-18)

The owner asked for a theoretical comparison. Could the EKA1 kernel be rebuilt from RE with AI tools, the way
EKA2 is open (EPL)? The examples: the N-Gage QD (RH-29 V 04.10, Symbian 6.1, EKA1) and the N95 8GB
(RM-320 35.0.001, Symbian 9.2, EKA2). [V] = measured from the ROMs/sources in this repo, [I] = estimate.
Nothing here is a plan of record. We do not need it: the original EKA1 kernel already runs on our `ngage` board.

## 1. Binary sizes [V]

N-Gage numbers come from `eka1/analysis/files.tsv` (the reconstructed ROM). N95 numbers come from the core ROM's
`core-files.txt` (`tools/device.py paths nokia-n95-8gb-rm320 dir`).

| Module | N-Gage QD, EKA1 | N95 8GB, EKA2 | EKA2 / EKA1 |
|---|---:|---:|---:|
| `ekern.exe` (the kernel) | 161,900 | 228,052 | 1.41 |
| `EUser.dll` (user library) | 214,976 | 206,192 | 0.96 |
| `EFile.exe` (file server; EKA2 also holds the loader) | 65,432 | 132,984 | 2.03 |
| `EFSrv.dll` (file server client) | 25,988 | 32,784 | 1.26 |
| `EwSrv.exe` (window server) | 127,856 | 155,192 | 1.21 |
| Kernel side: kernel + extensions + LDDs + PDDs | ~307 KB (18 modules) | 3.05 MB (65 modules) | ~10 |
| ... of which the kernel and its basic extensions/media | ~307 KB | ~0.5 MB [I: without camera, WLAN, NIPS, crash monitor, GPU] | ~1.6 |
| ROM | 14.9 MB, 1060 files | 59.9 MB core (2719 files) + ROFS | 4+ |

Functions in `ekern.exe`, counted by prologue (ARM `stmfd sp!,{..,lr}` + Thumb `push {..,lr}`, so leaf
functions are not counted) [V count, I as a function total]: EKA1 **1,056**, N95 EKA2 **1,417**. With leaf
functions that is about 1,400 vs 1,900 [I].

Interfaces [V]:
- EKA1: 237 executive calls (SWI table at 0x503210DC, 233 non-null), 506 kernel exports by ordinal (the API
  that EKA1 kernel-side drivers link against). `eka1/analysis/ghidra-notes.md`, `kernel-api-map.tsv`.
- EKA2 (N95): 265 executive calls (28 fast + 237 slow) (`abi/swi-n95-vs-ours.txt`). Kernel-side drivers
  link against ekern exports with the EKA2 DDK layout.
- Already named on the EKA1 side: 193 kernel entries with evidence (`kernel-identifications.tsv`: the exec
  calls by behaviour, a few exports). EKA2L1 maps 48 EKA1 exec calls by name (`svc_register_funcs_v6`).

## 2. Source sizes (EKA2, the open EPL tree) [V]

`build/epocroot/sf/os/kernelhwsrv/kernel/eka`, lines of .cpp/.h/.inl/.cia/.s, comments included:

| Part | Lines |
|---|---:|
| nanokernel `nkern` (unicore) | 16,241 |
| kernel `kernel` (objects, IPC, threads, chunks, drivers framework, power, debug) | 67,045 |
| memory model, multiple (the N95's) + the shared epoc part | 11,449 + ~14,500 |
| `klib` | 3,801 |
| **N95-style kernel core** | **~113,000** |
| `euser` (user library) | 86,208 |
| `common` (shared by kernel and euser) | 30,224 |
| public headers `include` | 158,113 |

That builds to 228 KB (urel, Nokia) or ~420 KB (our udeb with traces). So 1 KB of kernel binary is about
500 source lines with comments, or ~250 lines of code [I].

**EKA1 estimate** [I]: 162 KB of kernel at the same ratio is ~80,000 source lines (~40,000 lines of code),
about 1,400 functions. EKA1 is a smaller, older design: no nanokernel, a kernel server thread instead of
EKA2's lock-free fast paths, a simpler memory model (the moving model only, like our N73 build) and no
SMP, no demand paging, no platform security.

## 3. Reimplementing EKA1: three ways [I unless marked]

What we have today [V]: the original EKA1 kernel runs unmodified on the `ngage` QEMU board
(`src/qemu-ngage`, 3,665 lines of C, docs/eka1-ngage.md). It boots to the menu and runs games from the card.
So a reimplementation would buy other things: hosting (no CPU emulation of the kernel, like hosted EKA2),
one kernel for every EKA1 phone without a board model each, source-level debugging, and the fun of it.

| Way | What is written | Size | Hard part | Verdict |
|---|---|---|---|---|
| A. HLE of the user ABI (EKA2L1's way) | the 233 exec calls + the kernel's IPC server protocol, in a host program | EKA2L1's kernel HLE is ~21,000 lines for EKA1 + EKA2 together [V] | ROM kernel-side drivers (LDD/PDD, extensions) cannot run: every device the phone's code opens needs an HLE stand-in | cheapest for running apps/games; nothing of the original kernel side survives |
| B. Matching decompilation of `ekern.exe` | ~1,400 functions back to C/C++ that rebuild to the same behaviour (byte match optional) | ~40k lines of code | object layouts that drivers read directly (DThread, DChunk, DLogicalChannel offsets); the 506 exports' signatures (no names in the ROM, no EKA1 `ekern.lib` here) | feasible with agents: small binary, an oracle to compare against, functions are independent units |
| C. EPL EKA2 with an "EKA1 personality" | the open EKA2 kernel exporting the 237 EKA1 exec calls and the 506 EKA1 kernel exports, with EKA1 object layouts | personality layer of maybe 10-20k lines | EKA1 and EKA2 differ deeply: IPC v1 (RMessage, 4 args), the kernel server, loader in the kernel, DLL model and the driver ABI. Our sym91/92 personalities only bridge 9.x to 9.x | most work for the least fidelity; not recommended |

Why B is plausible with AI tooling now [I]:
- **Size.** About 1,400 functions, around 40k lines, is a medium decompilation project. EKA2 is 1.4x the
  binary and 2-3x the concepts.
- **An oracle.** The `ngage` board runs the original kernel, so every rebuilt function can be differentially
  tested. Run the same boot or game with the original and the rebuilt function swapped in (a code patch on the
  ROM, the way `code-patches.txt` patches firmware), and compare exec-call traces, memory and the serial log.
  `ngage_sess.py` already gives gdb, breakpoints and traces. This is the step that makes AI-written code
  trustworthy.
- **Reference designs.** EKA2's EPL source (same company, same naming, many shared concepts: DObject, handles,
  chunks, TRequestStatus) helps naming and structure. The 6.1 SDK headers give user-side classes and layouts.
  EKA2L1 names 48 exec calls.
- **Agent throughput** [I, a guess]: an agent decompiling, naming and differentially testing 30-60
  functions a day puts `ekern.exe` at 1-2 months of agent time. `Nokia.dsy` (the N-Gage variant, 29 KB), the
  extensions and drivers (~145 KB) add about as much again if they must be rebuilt too. In practice they would
  keep running as original ROM code on a rebuilt kernel (the same split as our EKA2 work: our kernel, their
  drivers).

Risks [I]: the export ABI (506 ordinals without names or prototypes: every one inferred from callers). The
assembler fast paths (context switch, exception vectors, IPC copy) are better hand-written than decompiled.
The licensing of a decompiled Nokia binary: a matching decompilation is a derivative of Nokia's code and could
not be published like the EPL tree. A clean-room path would have one agent write specs from the binary and
another write code from the specs only.

## 4. Bottom line

- EKA1's kernel is small: 162 KB and ~1,400 functions against EKA2's 228 KB and ~1,900. The whole EKA1 kernel
  side is ~0.3 MB.
- For running N-Gage software, the original kernel on the `ngage` board already works. Way A (HLE) is the
  cheap route for hosting apps without a CPU model.
- If we ever try it for fun, way B (decompile `ekern.exe` with agents and verify each function against the
  original on the `ngage` board) is the realistic one: weeks to a few months of agent time, not years. Start
  with the exec calls (233 entry points already located and 193 identified), then the 506 exports by their
  callers in the ROM's drivers.
