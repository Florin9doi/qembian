// nk_svcwin: exec calls from svc instructions OUTSIDE the ROM's stub block. Regression for bug 2026-11-12:
// Belle FP2's EUSER (the 808) has inline svcs elsewhere in its code (CServer2::RunL's ServerReceive, svc
// 0x24); the seccomp trap covered only the stub block, so they reached Linux as system calls and the file
// server never received its second message. Now every svc in the Symbian code window (0x70000000-0xC0000000)
// traps, and an ARM svc that traps is patched into a direct call on its first execution.
// Layout (all inside the window): a stub block page (one `svc; bx lr` stub) and a function page with an
// inline svc: `str lr, [sp, #-4]!; svc #0x800001; ldr pc, [sp], #4`. Checks: the inline svc dispatches,
// it is patched after its first trap (the word becomes a branch), the patched path returns the same result,
// the stub in the block is patched at start as before.
#include <nk_priv.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern TInt HostedExecTrapInit(TLinAddr aLo, TLinAddr aHi);
extern volatile TUint32 HostedExecSvcsPatchedOnTrap;

static TInt Fast1(TInt a) { return a + 1; }
static TInt Invalid(TInt) { return -5; }
static const struct { TInt iCount; TLinAddr iFn[1]; } FastTable = { 2, { (TLinAddr)&Fast1 } };
static const struct { TInt iCount; TLinAddr iInvalid; TLinAddr iPre; SSlowExecEntry iE[1]; } SlowTable = {
	1, (TLinAddr)&Invalid, 0, { { 0, (TLinAddr)&Invalid } } };

static const TLinAddr KBlock = 0x7f400000u, KFunc = 0x7f401000u;
typedef TInt (*TFn)(TInt);
static NThread Initial, Main;

static void MainFn(TAny*)
	{
	static_cast<NThread*>(NKern::CurrentThread())->iInKernel = 0;		// "user mode"
	volatile TUint32* f = (volatile TUint32*)KFunc;
	TInt r1 = ((TFn)KFunc)(41);
	TUint32 patched = HostedExecSvcsPatchedOnTrap, w = f[1];
	TInt r2 = ((TFn)KFunc)(99);
	TInt r3 = ((TFn)KBlock)(7);
	static_cast<NThread*>(NKern::CurrentThread())->iInKernel = 1;
	HCHECK(r1 == 42, "an inline svc outside the stub block dispatches (41 -> %d)", r1);
	HCHECK(patched == 1 && (w & 0xff000000u) == 0xea000000u, "it was patched into a branch on its first trap (%u, %08x)", patched, w);
	HCHECK(r2 == 100, "the patched path returns the same result (99 -> %d)", r2);
	HCHECK(HostedExecSvcsPatchedOnTrap == 1, "no further patching on the second call (%u)", HostedExecSvcsPatchedOnTrap);
	HCHECK(r3 == 8 && ((TUint32*)KBlock)[0] == 0xe51ff004u, "the stub in the block is patched at start and works (%d)", r3);
	hexit(htest_done());
	}

int main(int, char**)
	{
	LxInit();
	TUint32* blk = (TUint32*)hmmap((TAny*)KBlock, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC,
	                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	HCHECK((TLinAddr)blk == KBlock, "code pages in the Symbian window at %08x", KBlock);
	blk[0] = 0xef800001u;			// svc #0x800001 (fast exec 1)
	blk[1] = 0xe12fff1eu;			// bx lr
	TUint32* fn = (TUint32*)KFunc;
	fn[0] = 0xe52de004u;			// str lr, [sp, #-4]!
	fn[1] = 0xef800001u;			// svc #0x800001
	fn[2] = 0xe49df004u;			// ldr pc, [sp], #4
	hsys(SYS_ARM_cacheflush, KBlock, KBlock + 0x2000, 0);
	hsys(SYS_mprotect, KBlock, 0x2000, PROT_READ | PROT_EXEC);
	HCHECK(HostedExecTrapInit(KBlock, KBlock + 8) == 0, "exec trap: block %08x..%08x, window traps", KBlock, KBlock + 8);
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iTimeslice = -1;
	NTimerQ::Init1(1000);
	NKern::Init(&Initial, info);
	HostedStartSystemTimer(1000);
	memclr(&info, sizeof(info));
	info.iFunction = &MainFn;
	info.iPriority = 25;
	info.iTimeslice = -1;
	info.iFastExecTable = (const SFastExecTable*)&FastTable;
	info.iSlowExecTable = (const SSlowExecTable*)&SlowTable;
	HCHECK(NKern::ThreadCreate(&Main, info) == KErrNone, "thread created");
	NKern::ThreadResume(&Main);
	for (;;)
		NKern::Idle();
	}
