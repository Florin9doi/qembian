// nk_lxinit: the host thread record at process start.
// Regression for the bug of 2026-11-12: TPIDRURO (the register the hosted layer keeps its per-thread
// record in) is not zero when a 32-bit process starts on arm64 Linux; LxInit took a nonzero value for
// "already initialised" and skipped the set-up (silent crash at kernel boot). Checks: whatever
// TPIDRURO held, LxInit makes LxSelf() the main thread's record; a second LxInit keeps it; threads
// created afterwards get their own records; the user's TPIDRURW is left alone.
#include <nk_priv.h>

static volatile TInt Ok;
static SLxThread* Seen;
static TInt Thread(TAny*)
	{
	Seen = LxSelf();
	Ok = 1;
	return 0;
	}

// stays alive until released (its CPU set is read while it runs)
static volatile TInt Release;
static TInt Waiting(TAny*)
	{
	Ok = 1;
	while (!Release) hsys(SYS_sched_yield);
	return 0;
	}

int main(int, char**)
	{
	TUint32 before;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(before));
	htest_note("TPIDRURO at process start: %08x", before);
	hsys(SYS_ARM_set_tls, 0x1e3a2740);		// a stale value, as observed
	__asm__ volatile("mcr p15, 0, %0, c13, c0, 2" :: "r"(0x5eed));	// the user side's register
	// bug 2026-11-12 (kn_modem): code asking LxSelf() before LxInit got the stale value
	HCHECK(LxSelf() == NULL, "LxSelf() is NULL before LxInit, whatever TPIDRURO holds");
	LxInit();
	SLxThread* me = LxSelf();
	HCHECK(me && (TUint32)me != 0x1e3a2740 && me->iTid == hgettid(), "LxInit set up the main record over a stale TPIDRURO (%p)", me);
	LxInit();
	HCHECK(LxSelf() == me, "a second LxInit keeps it");
	TUint32 rw;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(rw));
	HCHECK(rw == 0x5eed, "TPIDRURW (the user side's) untouched (%x)", rw);
	SLxThread* t = LxCreateThread(&Thread, 0, ETrue, 0);
	while (!Ok) hsys(SYS_sched_yield);
	HCHECK(Seen == t && Seen != me, "a new thread sees its own record");
	// htest --env HPIN=<cpu> (2026-11-13): nanothreads on one CPU (hand-overs without cross-CPU wake-ups), the host
	// helper threads (hthread) on the others
	extern TBool LxPinTo(TUint32 aCpu);
	TUint32 all = 0;
	hsys(242 /* sched_getaffinity */, 0, sizeof all, &all);
	if (all & (all - 1))
		{
		TUint32 cpu = __builtin_ctz(all);
		HCHECK(LxPinTo(cpu), "pinning to CPU %u of %08x", cpu, all);
		static TUint32 fake[64];					// a nanothread's record only needs the pointer
		Ok = 0;
		SLxThread* n = LxCreateThread(&Waiting, 0, ETrue, (NThread*)fake);
		while (!Ok) hsys(SYS_sched_yield);
		TUint32 m = 0;
		hsys(242, n->iTid, sizeof m, &m);
		Release = 1;
		HCHECK(m == (1u << cpu), "a nanothread runs on CPU %u only (%08x)", cpu, m);
		static volatile TInt hctid;
		static TUint8 hstack[0x4000];
		static volatile TInt hdone;
		hdone = 0;
		TInt tid = hthread([](void*) -> int { while (!hdone) hsys(SYS_sched_yield); return 0; }, 0, hstack, sizeof hstack, &hctid);
		m = 0;
		hsys(242, tid, sizeof m, &m);
		hdone = 1;
		hthread_join(&hctid);
		HCHECK(m == (all & ~(1u << cpu)), "a host helper thread runs on the other CPUs (%08x)", m);
		}
	else
		htest_note("one CPU: pinning not tested");
	return htest_done();
	}
