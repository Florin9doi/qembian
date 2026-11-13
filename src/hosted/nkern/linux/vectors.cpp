// Hosted EKA2 nanokernel, Linux platform: exec call dispatch.
// Dispatch() is ported from e32/nkern/win32/vectors.cpp (Copyright (c) 1998-2009 Nokia Corporation
// and/or its subsidiary(-ies), Eclipse Public License v1.0). Change: an invalid exec number also leaves
// the kernel (the Win32 version returned with iInKernel still raised).
// Entry: the phone's EUSER stubs are `svc #n; bx lr` (n = exec number, bit 23 = fast exec). A seccomp
// filter keyed on the instruction pointer turns every svc in the ROM stub range into SIGSYS; the handler
// runs Dispatch on the thread's own stack with r0-r3 from the signal context as the argument block,
// and returns r0. The intended fast path patches the stubs into direct calls (architecture.md); this
// trap stays as the safety net.

#include "nk_priv.h"

typedef TInt (*TExecHandler)(TInt, TInt, TInt, TInt);
typedef void (*TPreprocessHandler)(TInt*, TUint32);

inline TInt Invoke(TLinAddr aHandler, const TInt* aArgs)
	{
	return (TExecHandler(aHandler))(aArgs[0], aArgs[1], aArgs[2], aArgs[3]);
	}

volatile TUint32 HostedExecTraps;	// execs that arrived through SIGSYS
TInt HostedTraceExecs;				// > 0: print that many trapped execs (number, arguments, result)
void (*HostedExecTraceWho)(char* aBuf, TInt aMax);	// the kernel layer: the current thread's full name
const char* HostedTraceExecsMatch;	// trace only threads whose name contains this (htest XTRACE_THREAD)

static TBool Contains(const char* aH, const char* aN)
	{
	TInt l = strlen(aN);
	for (; *aH; ++aH)
		if (!memcmp(aH, aN, l))
			return ETrue;
	return EFalse;
	}

TLinAddr HostedBTraceOutHandler;			// the kernel layer's BTraceOut exec handler (kplat.cpp)
const TUint8* HostedBTraceFilter;		// its primary filter, a byte per category
TUint32 HostedBTraceFast;				// BTraceOuts answered without entering the kernel (tests)
extern "C" void HostedExecBTrace();		// below
extern "C" void HostedExecBTraceB();
// every patched svc's trampoline: its exec number and its target word, so that the BTraceOut ones can be retargeted
// once the exec number is known (the first BTraceOut through Dispatch); lone svcs (B: resume after the svc) apart
struct STramp { TUint32 iN; TUint32* iTarget; TBool iLone; };
static STramp Tramps[8192];
static TInt TrampCount;
static TInt BTraceExecNo = -1;
static void RecordTramp(TUint32 aN, TUint32* aTarget, TBool aLone)
	{
	if (TrampCount < (TInt)(sizeof(Tramps) / sizeof(Tramps[0])))
		Tramps[TrampCount++] = (STramp){ aN, aTarget, aLone };
	}
static void RetargetBTrace(TInt aN)
	{
	BTraceExecNo = aN;
	for (TInt i = 0; i < TrampCount; ++i)
		if (Tramps[i].iN == (TUint32)aN)
			__atomic_store_n(Tramps[i].iTarget, (TUint32)(Tramps[i].iLone ? &HostedExecBTraceB : &HostedExecBTrace),
			                 __ATOMIC_RELEASE);
	}

/** Executive dispatch: aArgs is an array of up to 4 arguments; returns the exec's result */
EXPORT_C TInt Dispatch(TInt aFunction, TInt* aArgs)
	{
	NThread& me = *static_cast<NThread*>(TheScheduler.iCurrentThread);
	__NK_ASSERT_ALWAYS(!me.iDiverted);
	// fast path: a trace whose category is filtered out (kplat.cpp SBTraceFastInit)
	if (!(aFunction & 0x800000) && HostedBTraceFilter && me.iSlowExecTable)
		{
		const SSlowExecTable* t = (const SSlowExecTable*)((const TUint8*)me.iSlowExecTable - _FOFF(SSlowExecTable,iEntries));
		if (TUint(aFunction) < TUint(t->iSlowExecCount) && (TLinAddr)t->iEntries[aFunction].iFunction == HostedBTraceOutHandler &&
		    !HostedBTraceFilter[((TUint32)aArgs[0] >> 16) & 0xff])
			{
			++HostedBTraceFast;

			// from now on the trampolines of this exec answer it themselves (one word each: atomic for other threads)
			if (BTraceExecNo != aFunction)
				RetargetBTrace(aFunction);
			return EFalse;
			}
		}
	EnterKernel();
	if (aFunction & 0x800000)
		{
		aFunction &= 0x7fffff;
		// fast exec
		const SFastExecTable* table = me.iFastExecTable;
		if (aFunction == 0)
			{
			// special case fast exec call
			NKern::WaitForAnyRequest();
			LeaveKernel();
			return 0;
			}
		if (table && TUint(aFunction) < TUint(table->iFastExecCount))
			{
			NKern::Lock();
			TInt r = Invoke(table->iFunction[aFunction-1], aArgs);
			NKern::Unlock();
			LeaveKernel();
			return r;
			}
		// invalid exec number passed, so ensure we invoke the invalid exec handler
		aFunction = -1;
		}

	// slow exec
	const SSlowExecTable* table = (const SSlowExecTable*)((const TUint8*)me.iSlowExecTable - _FOFF(SSlowExecTable,iEntries));
	if (TUint(aFunction) >= TUint(table->iSlowExecCount))
		{
		TInt r = Invoke(table->iInvalidExecHandler, aArgs);
		LeaveKernel();
		return r;
		}
	const SSlowExecEntry& e = table->iEntries[aFunction];
	if (e.iFlags & KExecFlagClaim)
		NKern::LockSystem();
	if (e.iFlags & KExecFlagPreprocess)
		{
		// replace the first argument with the result of preprocessing
		TPreprocessHandler preprocesser = (TPreprocessHandler)table->iPreprocessHandler;
		preprocesser(aArgs, e.iFlags);
		}
	TInt r = Invoke(e.iFunction, aArgs);
	if (e.iFlags & KExecFlagRelease)
		NKern::UnlockSystem();
	LeaveKernel();
	return r;
	}

// one exec call from user code: trace (htest --xtrace) and dispatch; aArgs = r0..r3 as the stub got them
// htest --env GLSTAT=1: calls per exec number, for the per-second top list (hgl.cpp HostedGlStat)
TUint32 HostedExecCount[2][256];			// [fast][number & 0xff]
TUint32 HostedExecLastLr[2][256];
static TInt ExecCall(TUint32 n, TInt* aArgs, TUint32 lr)
	{
	++HostedExecTraps;
	++HostedExecCount[(n & 0x800000) ? 1 : 0][n & 0xff];
	HostedExecLastLr[(n & 0x800000) ? 1 : 0][n & 0xff] = lr;
	TUint32 a0 = aArgs[0], a1 = aArgs[1], a2 = aArgs[2], a3 = aArgs[3];
	TBool trace = HostedTraceExecs > 0;
	char who[80];
	who[0] = 0;
	if (trace && HostedExecTraceWho)
		{
		HostedExecTraceWho(who, sizeof who);
		if (HostedTraceExecsMatch && !Contains(who, HostedTraceExecsMatch))
			trace = EFalse;
		}
	if (trace)
		{
		--HostedTraceExecs;
		hprintf("EXEC %s %x (%08x %08x %08x %08x) [lr %08x] %s\n", (n & 0x800000) ? "fast" : "slow",
		        n & 0x7fffff, a0, a1, a2, a3, lr, who);
		}
	TInt r = Dispatch((TInt)n, aArgs);
	if (trace)
		hprintf("     -> %08x\n", r);
	return r;
	}

static void PatchLoneOnce(TUint32* aSvc);
static TBool PatchOnTrap;

// Every svc executed in Symbian code (the ROM, RAM-loaded code) traps here unless it was patched into a
// direct call. An ARM svc outside the stub block is patched on its first execution (PatchLoneOnce): only
// instructions that really ran are touched, never data that looks like an svc. Bug 2026-11-12: Belle FP2's
// EUSER has inline svcs outside its stub block (CServer2::RunL's ServerReceive, svc 0x24); they were neither
// trapped nor patched, reached Linux as system calls, and the file server never received a second message.
// Regression: nk_exec (an inline svc outside the stub range).
static void ExecTrap(int, hsiginfo* aInfo, hucontext* aUc)
	{
	TUint32 n;
	TBool thumb = (aUc->mc.cpsr & 0x20) != 0;
	if (thumb)
		n = *(const TUint16*)(aInfo->sys.call_addr - 2) & 0xff;	// Thumb svc #imm8 (trap only)
	else
		n = *(const TUint32*)(aInfo->sys.call_addr - 4) & 0x00ffffff;
	aUc->mc.r[0] = (TUint32)ExecCall(n, (TInt*)&aUc->mc.r[0], aUc->mc.lr);
	if (!thumb && PatchOnTrap)
		PatchLoneOnce((TUint32*)(aInfo->sys.call_addr - 4));
	}

// ---- direct exec calls ----
// EUSER's exec stubs are `svc #n; bx lr` pairs. Each is patched into `ldr pc, [pc, #-4]; .word T`, T a
// trampoline of its own (`movw/movt ip, #n; ldr pc, =HostedExecDirect`): the exec becomes a plain call
// into the kernel's dispatcher on the thread's own stack, with the same arguments (r0-r3) and result (r0)
// as through the SIGSYS trap, without a signal (~34 us per exec under TCG). A stub that does not match is
// left as it is and still traps.
extern "C" TInt HostedExecDirectC(TUint32 aN, TInt* aArgs, TUint32 aLr)
	{
	return ExecCall(aN, aArgs, aLr);
	}

// The stack is realigned to 8 bytes: EUSER's own callers reach a stub with sp only 4-byte aligned (legal
// inside a module), and clang code relies on AAPCS alignment. Bug 2026-11-12: with sp = 4 mod 8 the modem
// core computed `d + 6` as `d | 6` (= d + 2): the IMEI reply came out shifted, the TSY never got an IMEI,
// Etel answered -1 and the DRM server retried for ever (the trap path was aligned by Linux's signal frame).
// Register contract = the trap's: only r0 changes; r1-r3 come back as the argument array holds them after
// the exec (a preprocess handler may rewrite it), r12, lr and the flags as they were. Bug 2026-11-12: the
// first version let the C code clobber r1-r3/r12/flags and start-up stalled after the splash screen.
// Trampoline: push {r0-r3, ip, lr}; movw/movt r0, #n; ldr pc, =HostedExecDirect
extern "C" void HostedExecDirect();
__asm__(
	".text\n .arm\n .balign 4\n .global HostedExecDirect\n HostedExecDirect:\n"
	"	mrs	r1, apsr\n"
	"	push	{r1, r4}\n"				// flags, r4
	"	mov	r4, sp\n"
	"	bic	sp, sp, #7\n"				// AAPCS: 8-byte aligned for the kernel (see below)
	"	add	r1, r4, #8\n"				// the saved r0-r3: the arguments array
	"	ldr	r2, [r4, #28]\n"			// the caller's lr (trace)
	"	bl	HostedExecDirectC\n"
	"	mov	sp, r4\n"
	"	pop	{r1, r4}\n"
	"	msr	apsr_nzcvqg, r1\n"
	"	add	sp, sp, #4\n"				// r0 = the result: drop the saved one
	"	pop	{r1-r3, ip, lr}\n"
	"	bx	lr\n"
	// the same for a lone `svc #n` (not followed by `bx lr`): its trampoline saved the resume address (the
	// instruction after the svc) above lr; continue there
	" .global HostedExecDirectB\n HostedExecDirectB:\n"
	"	mrs	r1, apsr\n"
	"	push	{r1, r4}\n"
	"	mov	r4, sp\n"
	"	bic	sp, sp, #7\n"
	"	add	r1, r4, #8\n"
	"	ldr	r2, [r4, #28]\n"
	"	bl	HostedExecDirectC\n"
	"	mov	sp, r4\n"
	"	pop	{r1, r4}\n"
	"	msr	apsr_nzcvqg, r1\n"
	"	add	sp, sp, #4\n"
	"	pop	{r1-r3, ip, lr}\n"
	"	ldr	pc, [sp], #4\n");
extern "C" void HostedExecDirectB();

// BTraceOut with its category filtered out, answered in the trampoline (2026-11-13: the 808's font and bitmap
// client and server call it ~500,000 times a second; even Dispatch's fast path cost a call into C each). Entered
// as HostedExecDirect (saved r0-r3, ip, lr on the stack, r0 = the exec number); returns EFalse with every other
// register and the flags as they were, or goes on to HostedExecDirect. Regression: kn_efile.
extern "C" void HostedExecBTrace();
__asm__(
	".text\n .arm\n .balign 4\n .global HostedExecBTrace\n HostedExecBTrace:\n"
	"	mrs	r2, apsr\n"
	"	ldr	r1, =HostedBTraceFilter\n"
	"	ldr	r1, [r1]\n"
	"	cmp	r1, #0\n"
	"	beq	1f\n"
	"	ldr	ip, [sp]\n"				// the trace header (the saved r0)
	"	ubfx	ip, ip, #16, #8\n"		// its category (BTrace::ECategoryIndex)
	"	ldrb	ip, [r1, ip]\n"
	"	cmp	ip, #0\n"
	"	bne	1f\n"
	"	ldr	r1, =HostedBTraceFastAsm\n"
	"	ldr	ip, [r1]\n"
	"	add	ip, ip, #1\n"
	"	str	ip, [r1]\n"
	"	msr	apsr_nzcvqg, r2\n"
	"	add	sp, sp, #4\n"				// the saved r0
	"	pop	{r1-r3, ip, lr}\n"
	"	mov	r0, #0\n"				// EFalse
	"	bx	lr\n"
	"1:	msr	apsr_nzcvqg, r2\n"
	"	b	HostedExecDirect\n"
	"	.ltorg\n"
	// the same for a lone svc (its trampoline saved the resume address above lr): resume there
	" .global HostedExecBTraceB\n HostedExecBTraceB:\n"
	"	mrs	r2, apsr\n"
	"	ldr	r1, =HostedBTraceFilter\n"
	"	ldr	r1, [r1]\n"
	"	cmp	r1, #0\n"
	"	beq	2f\n"
	"	ldr	ip, [sp]\n"
	"	ubfx	ip, ip, #16, #8\n"
	"	ldrb	ip, [r1, ip]\n"
	"	cmp	ip, #0\n"
	"	bne	2f\n"
	"	ldr	r1, =HostedBTraceFastAsm\n"
	"	ldr	ip, [r1]\n"
	"	add	ip, ip, #1\n"
	"	str	ip, [r1]\n"
	"	msr	apsr_nzcvqg, r2\n"
	"	add	sp, sp, #4\n"
	"	pop	{r1-r3, ip, lr}\n"
	"	mov	r0, #0\n"
	"	ldr	pc, [sp], #4\n"
	"2:	msr	apsr_nzcvqg, r2\n"
	"	b	HostedExecDirectB\n"
	"	.ltorg\n");
TUint32 HostedBTraceFastAsm;				// BTraceOuts answered in the trampoline (tests)


volatile TUint32 HostedExecStubsPatched;

// trampolines within branch range (+-32 MB) of the ROM's stub block: a page below the ROM (0x80000000), in
// the top of the hosted DLL data reservation (memmodel/linux/mcodeseg.cpp leaves it out)
static const TLinAddr KTrampolineBase = 0x7ff00000u;
static const TLinAddr KSymbianCodeLo = 0x70000000u, KSymbianCodeHi = 0xC0000000u;	// RAM code .. end of the ROM area

static const TUint32 KTrampolineSpare = 0x10000;		// for svcs patched when they first trap
static TUint32* TrampNext;
static TUint32* TrampEnd;

// a lone `svc #n` -> `b T`; T saves the resume address (svc + 4) above lr -> HostedExecDirectB.
// The caller has made the page writable. False when T is out of branch range or the pool is full.
static TBool PatchLone(TUint32* st)
	{
	TUint32* t = TrampNext;
	if (t + 9 > TrampEnd)
		return EFalse;
	TInt off = ((TInt)t - ((TInt)st + 8)) >> 2;
	if (off < -0x800000 || off >= 0x800000)
		return EFalse;
	TUint32 n = st[0] & 0x00ffffff;
	t[0] = 0xe24dd004u;			// sub sp, sp, #4
	t[1] = 0xe92d500fu;			// push {r0-r3, ip, lr}
	t[2] = 0xe59fc010u;			// ldr ip, [pc, #16]   (t[8]: the resume address)
	t[3] = 0xe58dc018u;			// str ip, [sp, #24]
	t[4] = 0xe3000000u | ((n & 0xf000) << 4) | (n & 0xfff);					// movw r0, #lo16
	t[5] = 0xe3400000u | (((n >> 16) & 0xf000) << 4) | ((n >> 16) & 0xfff);	// movt r0, #hi16
	t[6] = 0xe51ff004u;			// ldr pc, [pc, #-4]
	t[7] = (TUint32)((TInt)n == BTraceExecNo ? &HostedExecBTraceB : &HostedExecDirectB);
	RecordTramp(n, &t[7], ETrue);
	t[8] = (TUint32)(st + 1);	// resume
	hsys(SYS_ARM_cacheflush, (TUint32)t, (TUint32)(t + 9), 0);
	TrampNext = t + 9;
	st[0] = 0xea000000u | ((TUint32)off & 0x00ffffffu);	// b T (one word: atomic for other threads)
	hsys(SYS_ARM_cacheflush, (TUint32)st, (TUint32)(st + 1), 0);
	return ETrue;
	}

// an unconditional ARM svc that just trapped: patch it (once; with interrupts masked, the pool is shared)
volatile TUint32 HostedExecSvcsPatchedOnTrap;
static void PatchLoneOnce(TUint32* aSvc)
	{
	TInt irq = NKern::DisableAllInterrupts();
	if ((aSvc[0] & 0xff000000u) == 0xef000000u)
		{
		TLinAddr pg = (TLinAddr)aSvc & ~0xfffu;
		if (hsys(SYS_mprotect, pg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
			{
			if (PatchLone(aSvc))
				++HostedExecSvcsPatchedOnTrap;
			hsys(SYS_mprotect, pg, 0x1000, PROT_READ | PROT_EXEC);
			}
		}
	NKern::RestoreInterrupts(irq);
	}

static TInt PatchStubs(TLinAddr aLo, TLinAddr aHi)
	{
	TUint32 count = (aHi - aLo) / 4;
	TUint32 bytes = (count * 36 + KTrampolineSpare + 0xfff) & ~0xfffu;
	TUint32* tramp = (TUint32*)hmmap((TAny*)KTrampolineBase, bytes,
	                                 PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if ((TLinAddr)tramp != KTrampolineBase)
		return -1;
	TrampEnd = tramp + bytes / 4;
	TLinAddr pg = aLo & ~0xfffu, pgEnd = (aHi + 0xfff) & ~0xfffu;
	if (hsys(SYS_mprotect, pg, pgEnd - pg, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
		return -1;
	TUint32* t = tramp;
	for (TUint32* st = (TUint32*)aLo; (TLinAddr)st < aHi; ++st)
		{
		if ((st[0] & 0xff000000u) != 0xef000000u)
			continue;		// not an unconditional svc (conditional ones keep trapping)
		TUint32 n = st[0] & 0x00ffffff;
		TUint32 movw = 0xe3000000u | ((n & 0xf000) << 4) | (n & 0xfff);					// movw r0, #lo16
		TUint32 movt = 0xe3400000u | (((n >> 16) & 0xf000) << 4) | ((n >> 16) & 0xfff);	// movt r0, #hi16
		if ((TLinAddr)(st + 1) < aHi && st[1] == 0xe12fff1eu)
			{
			// `svc #n; bx lr` -> `ldr pc, [pc, #-4]; .word T`; T: push; n; -> HostedExecDirect (returns to lr)
			t[0] = 0xe92d500fu;			// push {r0-r3, ip, lr}
			t[1] = movw;
			t[2] = movt;
			t[3] = 0xe51ff004u;			// ldr pc, [pc, #-4]
			t[4] = (TUint32)&HostedExecDirect;
			RecordTramp(n, &t[4], EFalse);
			st[0] = 0xe51ff004u;
			st[1] = (TUint32)t;
			t += 5;
			++st;
			}
		else
			{
			TrampNext = t;
			if (!PatchLone(st))
				continue;
			t = TrampNext;
			}
		++HostedExecStubsPatched;
		}
	TrampNext = t;
	hsys(SYS_ARM_cacheflush, (TUint32)tramp, (TUint32)t, 0);
	hsys(SYS_ARM_cacheflush, aLo, aHi, 0);
	hsys(SYS_mprotect, pg, pgEnd - pg, PROT_READ | PROT_EXEC);
	return 0;
	}

/** Route every svc executed in [aLo, aHi) (the ROM's exec stubs) to Dispatch. Call before the threads
	that will run that code are created: the filter is inherited by new threads. */
TInt HostedExecTrapInit(TLinAddr aLo, TLinAddr aHi)
	{
	TInt r = hsigaction(SIGSYS, ExecTrap, SA_NODEFER);
	// the stub block of a phone ROM: trap every svc of Symbian code (RAM-loaded code 0x70000000.., the ROM
	// 0x80000000..), not only the block's; our own image (0xD0000000) keeps making Linux system calls
	TLinAddr lo = aLo, hi = aHi;
	if (aLo >= KSymbianCodeLo && aHi <= KSymbianCodeHi)
		lo = KSymbianCodeLo, hi = KSymbianCodeHi;
	if (r == 0)
		r = hseccomp_trap_range(lo, hi);
	if (r == 0 && !hgetenv_hex("EXEC_TRAP_ONLY", 0))	// htest --env EXEC_TRAP_ONLY=1: the trap path only
		{
		r = PatchStubs(aLo, aHi);
		PatchOnTrap = r == 0;
		}
	return r;
	}
