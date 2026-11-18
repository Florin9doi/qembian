# P1i crypto.ldd: what dcl.dll asks of it

Sony Ericsson P1i R6E30 (se-p1i). Client: `dcl.dll` (ROFS1 `Sys/Bin/dcl.dll`, loaded at 0xeea19000 on our
boots), which sits on `crys.dll` (ROFS1 `Sys/Bin/crys.dll`, uid3 0x10201cd5). Static reading only. [V] = read
in the code, [I] = inferred.

## Verdict: the driver does no crypto, it hands out hardware register windows

`crypto.ldd` does not do crypto on request. `crys.dll` is a user-mode driver for the crypto hardware, a
CryptoCell-like engine (AES/DES/hash/RNG/PKA register blocks). It programs the engine directly through plain
loads and stores at the addresses the LDD gives it [V]. The LDD's controls only:

- acquire a hardware block: lock/power it [I] and write the user-visible base address of its registers into
  one output word at `a1` [V];
- release it again [V].

`crys.dll` takes each base with `crys_ord36(base, type)`. That export stores the pointer in a global for
the given type [V], and the engine code then uses it. For example, `crys` 0x80002bbc..0x80002c90 polls
`base(type 2) + 0x2c0` for bit 0x20 and writes key and control words at `base + 0x208.., 0x220..0x22c, 0x2c8` [V].
Today the stand-in refuses every control and writes nothing. `dcl.dll` mostly ignores the return code, so it
passes the uninitialised stack word as the base, and the first register access data-aborts. That is the
observed crash after ctl 3, ctl 8, ctl 1.

So the crypto itself cannot be done in the kernel stand-in. Emulating it for real means a QEMU device that
models the engine's registers (heavy, out of scope). The cheap way out is to give `crys.dll` RAM where it
expects registers.

## Control table [V unless marked]

All acquire controls use `a1` = address of **one output TUint32** (the stack word `[sp]`). Its input content
is garbage: the observed "a1=[0, 0x006022f0, 1, ...]" is uninitialised stack. `a2` is unused. The release
controls are `DoControl(n)` with no arguments.

| ctl | kind | a1 | crys type (`ord36`) | released by | callers in dcl.dll | return checked? |
|----:|------|----|---------------------|-------------|--------------------|-----------------|
| 0 | acquire | out: base | 5 (global 0x400014, used by 13 crys sites, main block [I]) | ctl 5 | export_167 and similar (0xeea1ad8a, 0xeea1b3d0, 0xeea1cdb0) | yes, `!= 0` -> free + bail |
| 1 | acquire | out: base | 2 (0x40000c: AES/DES/hash engine regs, offsets to >= 0x2c8) | ctl 6 | export_55/50/98/91/2/167... (12 sites) | only in export_167 |
| 2 | acquire | out: base | 6 (0x400018) | ctl 7 | export_2, export_167 and similar (6 sites) | export_2: yes (`!= 0` -> ctl 6) |
| 3 | acquire | out: base | 3 (0x400008) | ctl 8 | export_130, export_128, export_144, export_141 (start-up: export_130) | no |
| 4 | acquire | out: base | 1 (0x400010) | ctl 9 | export_45, export_40 (2 sites) | no |
| 5 | release ctl 0 | none | | | 90 call sites | no |
| 6 | release ctl 1 | none | | | 57 | no |
| 7 | release ctl 2 | none | | | 38 | no |
| 8 | release ctl 3 | none | | | 24 | no |
| 9 | release ctl 4 | none | | | (wrapper at 0xeea1d88c) | no |

Wrappers in dcl.dll, 0xeea1d880..0xeea1d8c8: Ctl0(a1), Ctl9, Ctl6, Ctl5, Ctl3(a1), Ctl8, Ctl7. ctl 1, 2 and 4
are called inline. The channel is a singleton (`export_32`, opened lazily by `0xeea1d030`) [V].

Types 7 and 8 do not come from the LDD: dcl allocates them itself (`crys_ord38(7)` = 0x6ce words of workspace,
`crys_ord38(8)` = 15 words) [V].

The start-up path (`!APS[10200b15]`, drmclockhandler -> drmcrypto -> dcl) [V]:
- `export_130`: ctl 3 -> `ord36(w,3)` -> `ord37(15)`, which returns 0 without touching hardware (switch case 9)
  -> `crys_ord15(obj+32, 0)` -> ctl 8.
- Next, a function that uses ctl 1: `ord36(w,2)`, then `ord37(13|17)`. `ord37(17)` = 0x80003026 fills
  pointer tables in the type-7 workspace from the type-7 base, with no register access [V]. Then engine
  calls: here the null or garbage base faults.

What the operations are [I]: the same pattern (acquire -> `ord37(sel)` -> engine call -> release) wraps
dcl's hash, cipher and RNG exports. export_2 seeds the RNG (`crys_ord21(ptr, len)`) under ctl 1 + ctl 2.
The LDD control does not tell you which operation runs.

## How crys.dll waits on the hardware [V]

`0x80007fdc wait(reg, mask)`: loop until `(*reg & mask) == mask`, at most 4,000,000 iterations
(literal 0x3d0900). On timeout it returns 0xf00121 (other paths return 0xf01025/0xf01027). Over zeroed RAM
every wait therefore spins about 4 M times and the CRYS call returns an error. It does not crash.

## Proposed stand-in [I, untested]

Controls 0-4 map one shared chunk of 5 pages into the caller's process, once per process, and return
`user base + n * 4 KB`. The register windows are assumed to fit in 4 KB each; for type 2 that is [V] up to
0x2c8, for the rest [I]. Controls 5-9 return `KErrNone`.

The page fill decides what the waits see:
- **0x00 (recommended first):** every engine wait times out, so CRYS returns errors and dcl `User::Leave`s
  with them. The cost is about 4 M loads per wait. The outcome to test: does drmclockhandler survive a Leave
  from drmcrypto?
- **0xff:** waits for "done" bits pass at once, and results are whatever crys last wrote. Hash, random and
  ciphertext come back as deterministic junk and no error is raised. This is the fallback if errors kill
  the APS. It would break any check that compares a stored MAC; the DRM clock would probably just reset [I].

A real hash or RNG in software is not possible at this level, because the work is done by crys.dll's
register sequence and not by an LDD request. To get real results, you would have to replace crys.dll's
exports instead (43 exports; a user-side stand-in DLL), which is not needed for boot.

```cpp
// src/standins/semccrypto/semccrypto.cpp (hook only; the tablestub.h framework as now)
#include "tablestub.h"
#include <kernel/kern_priv.h>

// crypto.ldd hands crys.dll the user address of a hardware register block (ctl 0..4, one out word at a1)
// and releases it (ctl 5..9). H-p1i-crypto-absent: RAM pages stand in for the registers.
const TInt KCryptoWindows = 5;
const TUint KWindowSize = 0x1000;
const TUint8 KFill = 0x00;			// 0x00: waits time out -> CRYS errors; 0xff: waits pass, junk results

struct TMapped { DProcess* iProcess; TUint iId; TLinAddr iUser; };
static TMapped Mapped[8];			// one entry per client process (DRM clock handler, DRM agents)

static TInt UserBase(DThread* aClient, TLinAddr& aUser)
	{
	DProcess* p = aClient->iOwningProcess;
	for (TInt i = 0; i < 8; ++i)
		if (Mapped[i].iProcess == p && Mapped[i].iId == p->iId)
			{ aUser = Mapped[i].iUser; return KErrNone; }
	NKern::ThreadEnterCS();
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = KCryptoWindows * KWindowSize;
	info.iMapAttr = EMapAttrFullyBlocking;			// uncached: crys polls these words
	info.iOwnsMemory = ETrue;
	info.iDestroyedDfc = NULL;
	DChunk* chunk = NULL;
	TLinAddr kaddr = 0;
	TUint32 attr;
	TInt r = Kern::ChunkCreate(info, chunk, kaddr, attr);
	if (r == KErrNone)
		r = Kern::ChunkCommit(chunk, 0, info.iMaxSize);
	if (r == KErrNone)
		{
		memset((TAny*)kaddr, KFill, info.iMaxSize);
		TInt h = Kern::MakeHandleAndOpen(aClient, (DObject*)chunk);	// the client's handle keeps it mapped
		r = h < 0 ? h : KErrNone;
		if (r == KErrNone)
			aUser = (TLinAddr)Kern::ChunkUserBase(chunk, aClient);
		}
	if (chunk)
		Kern::ChunkClose(chunk);
	NKern::ThreadLeaveCS();
	if (r == KErrNone)
		for (TInt i = 0; i < 8; ++i)
			if (!Mapped[i].iProcess || Mapped[i].iId != Mapped[i].iProcess->iId || i == 7)
				{ Mapped[i].iProcess = p; Mapped[i].iId = p->iId; Mapped[i].iUser = aUser; break; }
	return r;
	}

static TInt CtlHook(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32*)
	{
	if (aFn >= 0 && aFn < KCryptoWindows)			// acquire window aFn: out word = its user address
		{
		if (!a1)
			return KErrArgument;
		TLinAddr base = 0;
		TInt r = UserBase(aClient, base);
		TLinAddr win = r == KErrNone ? base + aFn * KWindowSize : 0;
		if (r == KErrNone)
			r = Kern::ThreadRawWrite(aClient, a1, &win, sizeof(win));
		Kern::Printf("SYBSTUB: crypto.ldd window %d at %08x for %O: %d", aFn, win, aClient, r);
		return r;
		}
	if (aFn >= 5 && aFn <= 9)					// release window aFn-5
		return KErrNone;
	return KErrNotSupported;
	}

NO_CONTROLS
TABLE_STUB_LDD_CTLHOOK("crypto.ldd", KCtl, ETrue, CtlHook)
```

Notes:
- The process key uses `iId` as well as the pointer, so that a recycled `DProcess*` does not get a stale
  address. The table is not locked, because the hook runs in the client thread and DRM is one process
  [I]; add a `NKern::FMWait` if that turns out wrong.
- `EMapAttrFullyBlocking` is a choice, not a requirement; `EMapAttrCachedMax` (as in dspcontrolldd) also
  works for RAM.
- Regression tests: one for the hook (ctl 1 writes a non-zero, page-aligned address; a repeat call returns
  the same address; ctl 6 returns 0); a boot check that `!APS[10200b15]` no longer data-aborts in dcl.dll.

## Cheaper alternatives if this still fails

1. Keep `drmclockhandler.aps` out of start-up (P1i start-up list or a code patch), giving up the DRM clock.
2. Patch dcl.dll so that `export_32` (channel getter) makes the callers bail. This needs per-function
   patches, so it is worse than option 1.
3. A user-side stand-in `crys.dll` with software AES/DES/SHA/RNG (43 exports): only if DRM content must work.
