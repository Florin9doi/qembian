# Nokia 808: can a firmware kernel-side XIP module (LDD) run on our kernel?

Date: 2026-11-19. Device `nokia-808-rm807`, firmware `RM-807/113.010.1508/04.01+C00.01`. Gates the
"keep the phone's LDD, write our PDD" plan (docs/research/808-driver-seams.md,
docs/research/808-registry-review-2026-11-17.md section 4; registry task `sensor-ldd-replacement-pdd`).
Static work only (core.json, imports.json, hybrid-manifest.txt, kernel-side.img, hybrid.img, our EPL
kernel source, our build's def files). No QEMU, no decompilation.

Marks: **[V]** read from the files named, **[I]** inferred.

Short answer: **yes, in place.** The three candidate LDDs have no .data/.bss, their ROM addresses fall in a
window of the hybrid ROM that holds only a pad file, and all but 2 (accelerometer, magnetometer) or 10 (BC02)
of their ekern imports resolve to the same-named export of our kernel. The composer can copy the firmware
bytes to their original addresses and patch two things: the import thunk targets (one word per thunk) and the
DLL reference table entry (one word). Making a RAM-loadable E32 image instead is possible but costs 3x more
and adds relocation-recovery risk; it is the route for modules with data.

## 1. The candidates: addresses, data, imports, ABI gap

### 1.1 Layout in the firmware ROM [V core.json, core-flat.img]

All addresses are ROM linear (base 0x80000000). `header` = TRomImageHeader (the directory's file address).
`code` includes the export directory at its end (`code+codesize` == DllRefTable). The directory `size` is
0x70-0x78 short of the real extent (the composer's comment on DLL reference tables says the same for our files).

| module | header | code (codesize) | textsize | export dir (count) | DllRefTable | .data ROM / RAM (datasize+bss) | entry | uid3 | ROM flags |
|---|---|---|---|---|---|---|---|---|---|
| accelerometer.ldd | 0x802259a0 | 0x80225a18..0x80228e6c (0x3454) | 0x3454 | 0x80228e50 (7) | 0x80228e6c..0x80228e74 (1 entry: 0x8005af88) | **none** (totaldata 0; iDataBssLinearBase 0xc80880e0 unused) | 0x80225a18 | 0x20000586 | 0x1180612b |
| BC02.LDD | 0x801f1a10 | 0x801f1a88..0x801f746c (0x59e4) | 0x59e4 | 0x801f7468 (1) | 0x801f746c..0x801f7474 (1 entry: 0x8005af88) | **none** | 0x801f1a88 | 0x101fb3f8 | 0x1180212b |
| magnetometerldd.ldd | 0x80233140 | 0x802331b8..0x80234714 (0x155c) | 0x155c | 0x802346fc (6) | 0x80234714..0x8023471c (1 entry: 0x8005af88) | **none** | 0x802331b8 | 0x2001e24e | 0x1180612b |
| accelerometer.pdd (for contrast) | 0x8021a460 | 0x8021a4d8..0x80225994 (0xb4bc) | 0xb4bc | 0x8022594c (18) | 0x80225998 | 0x80225994 / **0xc8087fa0** (4 + 0x13c) | 0x8021a4d8 | - | 0x1780212b |
| firmware ekern.exe | 0x8005af88 | 0x8005b000..0x800be60c | | 0x800c258c (1268) | - | 0x800be60c / 0xc8000000 (0x11c8 + 0x3098) | | 0x100041af | |

Bytes to keep for a module = `[header, DllRefTable end)`: accelerometer.ldd 0x802259a0..0x80228e74 (0x34d4),
BC02.LDD 0x801f1a10..0x801f7474 (0x5a64), magnetometerldd.ldd 0x80233140..0x8023471c (0x15dc).

The "fixed data address" concern of the brief does not apply to the three LDDs: **they have no initialised
data and no bss** [V core.json totaldata 0]. It applies to the firmware PDDs (accelerometer.pdd's data lives at
0xc8087fa0, inside the firmware kernel's data/heap window) and to most kernel extensions (83 of the
firmware's kernel-side files have data, RAM range 0xc8000000..0xc8087fa0+ [V]); those cannot be kept in place
without extending our kernel's data window (section 2.3).

Import thunks: every ekern import is a two-word thunk in code, `ldr pc,[pc,#-4]; .word <export address in the
firmware's ekern>` at code+76, +84, ... (8 bytes each) [V imports.json offsets, core-flat.img words]. There is
no import address table outside the code; the thunk word is the only thing to patch.

### 1.2 Imports and the ABI gap

Sources: imports.json (per thunk: ordinal, offset) [V]; names of the ordinals from the Symbian^3 SDK v1.0
`ekern.dso` (the profile's `sdk_lib`) [V]; our kernel's exports from
`build/epocroot/sf/os/kernelhwsrv/kernel/eka/eabi/ekernu.def` (frozen, 1185 ordinals) and the generated
`build/epocroot/epoc32/build/syborgfx/.../_syborgfx_ekern{000a0000}.def` (1202 ordinals, what EKERN.EXE in
hybrid.img really exports: `expcount=1202`, `expdir=0x81fd7464`) [V].

| module | ekern thunks | distinct ordinals | resolve to the same name in our frozen def | **missing in our kernel** (ordinal > 1185) | other DLLs |
|---|---|---|---|---|---|
| accelerometer.ldd | 42 | 40 | 38 | **2**: 1237, 1251 | none |
| BC02.LDD | 72 | 69 | 59 | **10**: 1193, 1201, 1202, 1205, 1206, 1208, 1209, 1210, 1237, 1251 | none |
| magnetometerldd.ldd | 33 | 32 | 30 | **2**: 1237, 1251 | none |

Name check for ordinals <= 1185: for every imported ordinal the SDK name equals our frozen def's name at that
ordinal (0 differences in 40/69/32) [V]. That the *firmware's* ekern has the same function at that ordinal is
[I]: the firmware ekern carries no names; the evidence is that Symbian never renumbered ekern (both the SDK
and EPL defs agree on all 1185), Nokia appended (1268 exports), and the thunk targets of shared ordinals are
identical across the three modules (790 -> 0x8005db54, 383 -> 0x800b2734).

What the three LDDs use (all names [V from the SDK dso]): DLogicalDevice/DLogicalChannel ctor/dtor/SetDfcQ,
TMessageQue, TMessageBase::Complete, Kern::ThreadDesRead/Write, ThreadRawRead/Write, ThreadGetDesLength/
MaxLength/Info, RequestComplete, Alloc/Free/ReAlloc, SafeClose, DObject::SetName, SystemTime, CurrentThread,
DoCurrentThreadHasCapability, NFastMutex, TVersion, TPtr8/TPtrC8/TBufBase8, BTrace::OutFiltered/N/Big,
__aeabi_* helpers; BC02 adds DPowerHandler (ctor/Add/Remove/PowerUpDone/PowerDownDone), TDfc (ctor/Add/Enque),
NTimer::OneShot, NKern::TimerTicks/ThreadEnterCS/LeaveCS/CurrentContext, TDynamicDfcQue, Kern::InfoCopy,
QueryVersionSupported, TDesC8::At/Mid/Ptr, operator new.

The missing ordinals (Nokia additions above 1185; no SDK or EPL name) [V ordinals; names unknown]:

| ordinal | firmware ekern target | used by | note |
|---|---|---|---|
| 1237 | 0x8005f364 (ekern+0x4364, nkern area) | all three + accelerometer.pdd's neighbourhood | [I] small nkern helper; to identify in Ghidra |
| 1251 | 0x800953ac (ekern+0x3a3ac) | all three | [I] Kern:: helper; to identify in Ghidra |
| 1193, 1201, 1202, 1205, 1206, 1208, 1209, 1210 | 0x80064714..0x80065198 (one 0xa84-byte cluster) | BC02 only | [I] one class's methods (a Nokia-added kernel class; DPowerHandler/DFC-queue style), to identify in Ghidra |

**Trap [V]:** our EKERN.EXE exports ordinals 1186..1202 too, but they are elf2e32's unfrozen extras of our
build (1186 DPowerHandler C1, 1187 TThreadCleanup C1, 1188 DBatteryMonitor C1, 1189 DLogicalChannel C1,
1190 DPowerController C1, 1191 DPowerHal C1, 1192 __aeabi_idiv, 1193 __aeabi_uidiv, 1194 __ashldi3,
1195 __ashrdi3, 1196 __div0, 1197 __divdi3, 1198 __divsi3, 1199 __lshrdi3, 1200 __muldi3, 1201 __udivdi3,
1202 __udivsi3). A firmware import of 1193/1201/1202 would bind to `__aeabi_uidiv`/`__udivdi3`/`__udivsi3`
silently. Binding must therefore go **by name through a table**, never "the ordinal exists in our export
directory", and any ordinal above the frozen 1185 must be listed explicitly or refused.

Beyond ordinals, the driver-facing ABI is the vtable layout of DObject, DLogicalDevice, DLogicalChannelBase,
DLogicalChannel, DPowerHandler (an LDD derives from them and the kernel calls its slots). sym92 has that
check (`personalities/sym92/kabi.toml` + `tools/personality/kabi.py`, slot-by-slot against the device's own
ekern) and one known difference (our DLogicalChannel::SendMsg tail slot, handled by patch 0008 for RAM-loaded
drivers). **sym102 has no kabi.toml yet**, `tools/personality/check.py --device nokia-808-rm807` says "nothing
checked" [V]. The firmware's ekern vtables are readable (exported `_ZTV` symbols are in the export table as
for the N95), so the same tool applies once a sym102 kabi.toml exists.

Other header fields: the firmware ROM image flags differ from our build's in bits 0x6100 (0x1180612b vs our
0x1180002b) and the hardware variant is 0x09080004 vs our 0x0b0a0001 [V]. The firmware's user-side XIP files
carry the same values and load on the hybrid today, so the loader's variant/flags checks pass for them [I:
same code path, pcodeseg.cpp XIP branch].

## 2. ROM placement: can the composer keep the firmware module at its original address?

### 2.1 What is where in the hybrid ROM [V hybrid-manifest.txt, kernel-side.img, romcompose.py, model.toml]

- `[kernel] place = "after-firmware"`: all 39 OURS files (EKERN.EXE 0x81e8cf88 .. rm_debug.ldd, then
  ESTARTCOMP.TXT/esock_bt.cmi at 0x823a8000) sit **after** the firmware image. Below the firmware user side
  (`ustart` = 0x804b2b80) the hybrid holds only our TRomHeader + bootstrap + root list (`rootlist` 0x80001e00),
  then `sys/data/romcompose.pad` from 0x80002c00 (dead filler that pushes our files past the firmware), and the
  gcebe blob at 0x80300000. The ROM bytes 0x80002c00..0x804b2b80 of the hybrid are `kernel-side.img`'s pad.
- The three candidates (0x801eb0a0..0x8023471c, PDDs included) overlap **nothing live**: only the pad file [V].
- Our kernel's data: iKernDataAddress 0xc8000000 (same as the firmware's), EKERN.EXE totaldata 0x3684, our
  stand-ins have no data (databsslin 0xc800dff0, totaldata 0). The firmware PDDs' data (0xc8087fa0 for
  accelerometer.pdd, 0xc80108e0 for magnetometerpdd.pdd) lies outside what our bootstrap maps for data
  (`iTotalSvDataSize`) and inside/after our initial stack and kernel heap [I: bootmain.s maps
  data+stack+heap as one block from iKernDataAddress]. So a kept module **must have totaldata 0**, which the
  three LDDs do.

**Collision answer: none.** Code ranges, export directories and DllRefTables of the candidates fall in the pad;
they have no data, so there is no RAM collision either.

### 2.2 What the kernel needs for an XIP module [V kernel source]

- A **directory entry** (name, TRomImageHeader address, size, attributes 0x81) so the loader (the firmware's
  own efile.exe, the hybrid's secondary) finds `Z:\sys\bin\accelerometer.ldd` and sees an XIP image.
  romcompose.py already builds a fresh directory from `entries` (firmware user files, then ours; ours win on a
  name clash) [V romcompose.py main].
- The **TRomImageHeader** as is: iEntryPoint, iCodeAddress, iCodeSize, iTextSize, iExportDir/Count, iUids,
  iFlags, iHardwareVariant, iPriority, iExceptionDescriptor; iDataAddress/iDataBssLinearBase/iTotalDataSize are
  0 for the LDDs. The XIP branch of `DEpocCodeSeg::DoCreate` (memmodel/epoc/pcodeseg.cpp:83-99, 214-238) copies
  these into the code seg; `DMemModelCodeSeg::DoCreateXIP` (flexible) does nothing else. For kernel-side XIP
  modules with data, `DEpocCodeSeg::InitData` memcpy's iDataAddress -> iDataBssLinearBase and clears bss at
  load time (pcodeseg.cpp:323-338); the bootstrap copies only the kernel's own data (bootmain.s "Init
  .data/.bss") and maps iKernDataAddress..+iTotalSvDataSize [V].
- The **DllRefTable** (`iDllRefTable`, right after the export directory): `{flags u16, count u16, entries}`,
  one entry = 0x8005af88 = the firmware ekern's TRomImageHeader [V bytes]. The loader walks it for
  dependencies (putils.cpp:323 `iDepCount`, pcodeseg.cpp:216 `iDllRefTable->iEntry[0]`). In the hybrid,
  0x8005af88 is pad bytes, so the one entry must become **our EKERN.EXE's header, 0x81e8cf88** [V address].
- The **import thunk words**: each `.word` after `ldr pc,[pc,#-4]` holds an absolute address in the firmware's
  ekern. Patch each to our export: `word(our expdir + 4*(ordinal-1))` for ordinals resolved by name, or a
  shim address for the missing ones (section 4). Our kernel is ARM code; `ldr pc` interworks on ARMv6 if an
  export carries the Thumb bit, so either is fine [I].
- The **exception search table**: the composer already merges the firmware's user-window entries with ours;
  a kept module's entry (if its iExceptionDescriptor is non-zero) belongs in the merge [I: same mechanism].
- Nothing else: XIP kernel-side code is not copied, not relocated, not checksummed by the loader [V pcodeseg].

### 2.3 Is "keep selected firmware kernel modules at their original addresses" a composition option?

Yes, cheaply, for modules with `totaldata == 0`. romcompose.py already has the pieces: `kernel_side()` finds
which firmware files are kernel-side, `--blob ADDR=FILE` writes raw bytes into the free gap below `ustart`,
`--file` adds directory entries, and the directory/exception-table are rebuilt. A `--keep-fw NAME` option
would: (1) take the firmware file's `[header, DllRefTable end)` bytes from `core-flat.img`, (2) check the
range is free in our image (pad only) and `totaldata == 0`, (3) patch the DllRefTable entry and each thunk
word through an ordinal-to-symbol map, refusing unmapped ordinals, (4) add the directory entry (dropping our
stand-in of the same name), (5) add the module's exception-table entry. Modules with data would need the data
window extended (our iTotalSvDataSize and the bootstrap's mapping) and their `iDataBssLinearBase` moved,
which means relocating data references inside the code: that is route B's problem, not a placement option.

## 3. The alternative: a RAM-loadable E32 image from the XIP bytes

What exists: `tools/xip2elf.py` (one XIP image -> ELF with export/import/mapping symbols, for analysis) and
`tools/e32load.py` (RAM-loaded E32 -> ELF: Symbian deflate, relocation, import resolution against the XIP
ROM) [V docstrings]. Neither writes an E32 image. Missing for a loadable image:

1. **E32ImageHeaderV writer**: UIDs, iCodeBase = original code address (so unrelocated words are right for
   delta 0), iEntryPoint, iTextSize/iCodeSize, iExportDirOffset/count, flags (KImageImpFmt_ELF,
   EABI, header format V, no paging bits), SID/VID/capabilities from the ROM header, iHeaderCrc, uncompressed.
   About 120 lines.
2. **Import section**: exact, no guessing. imports.json already lists every thunk (ordinal, code offset); ELF
   import format = per DLL a list of code offsets whose word the loader overwrites with the export address.
   The ordinal can be rewritten here, even to another DLL (a shim DLL for 1237/1251): the gap handling is
   free in this route. About 60 lines.
3. **Relocation section**: every 32-bit word in the code that holds an address inside the module's own code
   range (vtables, function-pointer tables, literal-pool addresses of strings/data, export directory words).
   Candidates: aligned words `W` with `code <= W < code_end`. False positives are instruction words and
   constants that happen to fall in a 13-23 KB window at 0x8022xxxx/0x801fxxxx: an ARM instruction word
   0x8022xxxx decodes as a HI-conditional data-processing instruction, rare but real; a missed relocation or a
   false one both corrupt silently. Safe classification needs the code/data map from disassembly ($a/$t/$d;
   xip2elf's mapping symbols are heuristic, Ghidra's listing is reliable). Thumb-bit-carrying words relocate
   fine by addition. About 150 lines plus a Ghidra pass per module.
4. Loading: from ROFS (profile `[[rom.rofs_replace]]`-style add) or from C:\sys\bin via sydb, with
   `User::LoadLogicalDevice`; the loader does not check certificates for drivers, only capabilities [I].

Estimates (one engineer): route A (in place) **1-2 days**: composer option + model key + a ROM-level unit test
(thunks resolve, DllRefTable patched, directory entry present) + the ordinal map; route B (RAM-loadable)
**3-5 days**: the three writers + the per-module relocation classification + a loader-level test, with the
residual risk of a wrong relocation showing up as a random fault later. Route B becomes necessary only for
modules with data or for loading without a ROM rebuild; for a *binding test* route A is cheaper and fully
checkable statically.

## 4. Verdict and plan: first binding test of accelerometer.ldd on our kernel

Route A, with a **kabi shim** for the two unknown ordinals and a **vtable-probe PDD** so the test also yields
the LDD-to-PDD interface the task needs.

1. Identify ordinals 1237 (0x8005f364) and 1251 (0x800953ac) in the firmware's ekern (Ghidra, two
   functions, half a day at most). If they exist in our kernel under another name, map them; if not, stub them.
2. `personalities/sym102/ekern-ordinals.toml` (new): `ordinal = "symbol"` for every ordinal a kept module
   imports, generated from the frozen def for <= 1185 and hand-written above it; plus `kabi.toml` for sym102 on
   the sym92 model (classes DObject, DLogicalDevice, DPhysicalDevice, DLogicalChannelBase, DLogicalChannel,
   DPowerHandler) and run `tools/personality/check.py --device nokia-808-rm807` until it passes or lists the
   differences (the SendMsg tail slot is the known one).
3. `src/standins/kabishim/` (new, OURS kernel extension, ~40 lines): one exported stub per missing ordinal,
   each `Kern::Printf("KABI: fw ekern ordinal %d called from %08x", N, lr); return KErrNotSupported;`.
   The thunks for 1237/1251 point at these; the serial log then says whether and when the LDD calls them.
4. `tools/romcompose.py`: `--keep-fw NAME` (section 2.3; ~100 lines) with the checks (range free, totaldata 0,
   every ordinal mapped, refuse otherwise) and a manifest column `FW-KEPT`. `tools/device.py build` reads
   `[kernel] keep_firmware = ["accelerometer.ldd"]` from the 808 profile and passes the option; the
   `[[standins]] accelerometer` LDD half is dropped from the ROM (its PDD half stays, renamed to the probe).
   Test: `tests/romcompose_keepfw_test.py` (compose a small ROM, assert the thunk words equal our export
   directory entries, the DllRefTable entry is our EKERN.EXE header, the directory lists the firmware file).
5. `src/standins/accelerometer/` PDD -> probe PDD: `DPhysicalDevice` named `Accelerometer.<unit>` whose
   `Validate`/`Info` return KErrNone and whose `Create` returns an object with a 32-slot vtable of
   `Kern::Printf("accel-pdd: slot %d (%08x %08x %08x)")` stubs returning 0. The firmware LDD's DoCreate then
   prints the sequence of PDD calls it makes (the interface the replacement PDD must implement).
6. Boot a test session (`sydb --session build/sydb-xipldd start`, never the owner's instance), let the sensor
   framework open the accelerometer (accssy.dll does so at start-up), and read `sydb log --grep 'KABI|accel-pdd|accelerometer'`.

Success in the serial log: (a) no `KERN-EXEC 3`/kernel fault while `accelerometer.ldd` is loaded (with the
KDLL trace on: "CodeSeg ... accelerometer.ldd" created from the ROM header, run address 0x80225a18); (b) the
first `accel-pdd: slot N` lines, i.e. the firmware LDD's DoCreate ran on our kernel and called into our PDD;
(c) `KABI: fw ekern ordinal 1237/1251 called` lines, or their absence, deciding whether those two need a real
implementation. Failure modes to expect: a wrong vtable slot order (step 2 catches it statically first), a
PDD contract the LDD needs before reporting the channel open (the slot trace shows which).

## 5. Open unknowns

- Names/semantics of ekern ordinals 1237, 1251 (all three LDDs) and the 1193-1210 cluster (BC02): need Ghidra.
- Whether the firmware's DLogicalChannel/DLogicalDevice vtable layouts match ours for sym102 (no kabi.toml yet).
- The LDD-to-PDD interface of accelerometer.pdd (the probe PDD in step 5 is how to learn it cheaply).
- Loader acceptance of the firmware ROM image flags 0x6100 bits on a kernel-side XIP module (user-side files
  with the same bits load today).
- Whether the gcebe blob gap check (`at < oend`) needs the kept files included in `oend` (yes if any kept
  module ever sits above 0x80300000; the candidates do not).
