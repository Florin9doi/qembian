// kn_efile: the phone's user side starts on the hosted kernel.
// State 2026-11-12 (the frontier this test pins): the EXSTART extension creates the ROM header's secondary
// process, Nokia's efile.exe (unmodified ROM code), which mounts the drives (ROFS Z: from the board's
// flash window, NVMemory C:/E: from the seed images, mapped private) and starts EStart.exe; EStart loads
// the ISC stack (our stand-ins as built-in kernel modules), starts domainSrv, SecEnvInit, EInfoServer, the
// central repository and LanguageSelection, which waits for modem replies (the board has no fake modem
// yet: requests stay unanswered). Every exec call from EUSER's stubs is trapped into the kernel's own exec
// tables. A probe (a timer DFC on a kernel DFC queue, 15 s, htest --env PROBE_MS=hex) lists the processes
// and threads and checks the milestones; a kernel fault before it ends the test through the fault hook.
// SKIP without a ROM.
#define __INCLUDE_NTHREADBASE_DEFINES__
#include <kernel/kern_priv.h>
#include "romload.h"
#include "memmodel.h"
#include <e32rom.h>

extern void HostedUseTestVariant();
extern TInt HostedAddExtension(TInt (*aEntry)(TInt));
typedef void (*THostedCtor)();
extern TInt HostedAddExtension(TInt (*aEntry)(TInt), THostedCtor* aCtors, THostedCtor* aCtorsEnd);
extern "C" THostedCtor __start_kmodinit_Exstart[] __attribute__((weak)), __stop_kmodinit_Exstart[] __attribute__((weak));
extern void HostedSetDebugMask(TInt aIndex, TUint32 aMask);
extern void HostedBootEpoc(TLinAddr aRomHeader);
extern void HostedSetKernelConfig(TUint32 aSet);
extern TInt HostedExecTrapInit(TLinAddr aLo, TLinAddr aHi);
extern volatile TUint32 HostedExecTraps;
extern volatile TUint32 HostedExeDataSwitches;
extern TInt HostedTraceExecs;
TInt Exstart_KernelModuleEntry(TInt aReason);

static void Name(DObject* aObj, char* aOut, TInt aMax)
	{
	TBuf8<128> n;
	aObj->FullName(n);
	TInt l = Min(n.Length(), aMax - 1);
	memcpy(aOut, n.Ptr(), l);
	aOut[l] = 0;
	}

static const char* strstr_(const char* h, const char* n);
#define strstr strstr_
static TBool WasSeen(const char* aPart);
static void LocalWindowTest();
static void PhysicalCommitTest();
extern TLinAddr HostedPhysicalToLinear(TPhysAddr aPhys, TUint32 aSize);
static TBool HasPart(const char* aName, const char* aPart);
static char Seen[96][32];
static TUint32 SeenMs[96];			// when it started (ms of host time since the test started)
static TInt SeenCount;
static TUint64 T0;
extern const TUint32* HostedFbPixels(TInt& aWidth, TInt& aHeight, TInt& aPitch);
// servers with outstanding messages (who waits on whom): delivered-not-received count and, per session, messages
// not completed yet, with the session's owner (the probe, and on request in phone mode: PhoneDiag)
static void ServersReport()
	{
	DObjectCon& servers = *K::Containers[EServer];
	servers.Wait();
	for (TInt i = 0; i < servers.Count(); ++i)
		{
		DServer* sv = (DServer*)servers[i];
		TInt delivered = 0;
		for (SDblQueLink* l = sv->iDeliveredQ.First(); l != &sv->iDeliveredQ.iA; l = l->iNext)
			++delivered;
		for (SDblQueLink* l = sv->iSessionQ.First(); l != &sv->iSessionQ.iA; l = l->iNext)
			{
			DSession* ss = _LOFF(l, DSession, iServerLink);
			if (ss->iMsgCount <= 0)
				continue;
			char sn[64], on[96];
			Name(sv, sn, sizeof sn);
			if (ss->Owner())
				Name(ss->Owner(), on, sizeof on);
			else
				on[0] = 0;
			htest_note("server %s: session of %s has %d outstanding (server queue %d)", sn, on, ss->iMsgCount, delivered);
			}
		}
	servers.Signal();
	}

static void ProbeFn(TAny*)
	{
	DObjectCon& procs = *K::Containers[EProcess];
	DObjectCon& threads = *K::Containers[EThread];
	procs.Wait();
	TBool efile = EFalse;
	char n[128];
	for (TInt i = 0; i < procs.Count(); ++i)
		{
		DProcess* p = (DProcess*)procs[i];
		Name(p, n, sizeof n);
		htest_note("process %s exit type %d reason %d", n, p->iExitType, p->iExitReason);
		if (HasPart(n, "efile"))
			efile = p->iExitType == EExitPending;
		}
	TInt alive = 0;
	// EStart ends once start-up is handed over (as on the ARM board: exit type Kill, reason 0)
	TBool fbserv = EFalse;
	TBool estart = WasSeen("EStart"), sysagent = WasSeen("sysagt2svr"), wserv = WasSeen("EwSrv");
	for (TInt i = 0; i < procs.Count(); ++i)
		{
		DProcess* p = (DProcess*)procs[i];
		Name(p, n, sizeof n);
		if (p->iExitType == EExitPending)
			++alive;
		if (HasPart(n, "FbServ") && p->iExitType == EExitPending)
			fbserv = ETrue;


		}
	procs.Signal();
	// the ISC stand-ins: both built from iscpeer.inl, each with its own port
	DObjectCon& ldds = *K::Containers[ELogicalDevice];
	ldds.Wait();
	TBool isc = EFalse, iscext = EFalse, iad = EFalse;
	for (TInt i = 0; i < ldds.Count(); ++i)
		{
		Name(ldds[i], n, sizeof n);
		if (!strcmp(n, "NokiaISCDriver")) isc = ETrue;
		if (!strcmp(n, "NokiaISCDriverExtension")) iscext = ETrue;
		if (!strcmp(n, "IADDriver")) iad = ETrue;		// the ISA Access stand-in (Belle: the 808)
		}
	ldds.Signal();
	threads.Wait();
	TInt stuck = 0;
	for (TInt i = 0; i < threads.Count(); ++i)
		{
		DThread* t = (DThread*)threads[i];
		// a user thread waiting inside an exec is at kernel level 1; deeper, its completion callbacks never run
		// (a callback may legitimately wait unsignalled: completions without a status)
		if (t->iThreadType == EThreadUser && t->iNThread.iRequestSemaphore.iCount < 0 && ((NThread&)t->iNThread).iInKernel > 1)
			++stuck;
		Name(t, n, sizeof n);
		htest_note("thread %s state %d exit %d/%d sem %d callbacks %p timer %d inkernel %d", n, t->iNThread.iNState, t->iExitType,
		           t->iExitReason, t->iNThread.iRequestSemaphore.iCount, t->iNThread.iUserModeCallbacks, t->iTimer.iState,
		           ((NThread&)t->iNThread).iInKernel);
		}
	threads.Signal();
	htest_note("%u exec calls trapped from EUSER", HostedExecTraps);
	extern volatile TUint32 HostedSwitches;
	htest_note("thread switches %u", HostedSwitches);
	extern volatile TUint64 HostedIdleNs;
	htest_note("idle %u ms of %u ms", (TUint32)(HostedIdleNs / 1000000), (TUint32)((hnow_ns() - T0) / 1000000));
	extern volatile TUint32 HostedTicks, HostedTicksSkipped;
	htest_note("kernel ticks %u at %u ms (%u tick interrupts, %u suppressed)", NKern::TickCount(),
	           (TUint32)((hnow_ns() - T0) / 1000000), HostedTicks - HostedTicksSkipped, HostedTicksSkipped);
	htest_note("tick queue ms timer: state %d trigger %u (now %u) dfc queued %d", K::TickQ->iMsTimer.i_NTimer_iState,
	           K::TickQ->iMsTimer.iTriggerTime, NKern::TickCount(), K::TickQ->iTickDfc.Queued());
	NThreadBase* tt = K::TimerDfcQ->iThread;
	htest_note("scheduler dfc pending %d reschedule %d kernlock %d; timer thread state %d dfcq ready %d",
	           TheScheduler.iDfcPendingFlag, TheScheduler.iRescheduleNeededFlag, TheScheduler.iKernCSLocked,
	           tt->iNState, K::TimerDfcQ->iPresent[0] | K::TimerDfcQ->iPresent[1]);
	extern TUint32 DiagMs, DiagMsAdded, DiagTickDfc, DiagSetTime;
	htest_note("DIAG ms callbacks %u (dfc added %u), tick dfc runs %u, set-time cancels %u", DiagMs, DiagMsAdded, DiagTickDfc, DiagSetTime);
	// the tick queue's links: delta (ticks after the previous one), the owner of thread timers
	{
	TTickQ::Wait();
	TInt n = 0;
	TInt acc = 0;
	for (SDblQueLink* l = K::TickQ->iA.iNext; l != &K::TickQ->iA && n < 16; l = l->iNext, ++n)
		{
		TTickLink* tl = (TTickLink*)l;
		acc += tl->iDelta;
		char on[64] = "";
		for (TInt i = 0; i < threads.Count(); ++i)
			if ((TAny*)&((DThread*)threads[i])->iTimer == tl->iPtr)
				Name(threads[i], on, sizeof on);
		htest_note("tick link %d: delta %d (at +%d ticks) cb %p ptr %p %s", n, tl->iDelta, acc, tl->iCallBack, tl->iPtr, on);
		}
	TTickQ::Signal();
	}
	htest_note("tick queue: lastms %u lastdelta %d tickperiod %d msperiod %d rounding %d/%d now %u", K::TickQ->iLastMs,
	           K::TickQ->iLastDelta, K::TickQ->iTickPeriod, K::TickQ->iMsTickPeriod, K::TickQ->iRounding,
	           K::TickQ->iPrevRounding, NKern::TickCount());
	for (TInt i = 0; i < SeenCount; ++i)
		htest_note("started: %s at %u ms", Seen[i], SeenMs[i]);
	ServersReport();
	// efile.exe's .data at its run address was initialised from the ROM (regression: it was left zero)
	const TRomEntry* se = (const TRomEntry*)Epoc::RomHeader().iSecondaryFile;
	const TRomImageHeader& sh = *(const TRomImageHeader*)se->iAddressLin;
	HCHECK(sh.iDataSize == 0 || !memcmp((const TAny*)sh.iDataBssLinearBase, (const TAny*)sh.iDataAddress, 16) ||
	       efile, "efile.exe .data at %08x came from the ROM", sh.iDataBssLinearBase);
	HCHECK(efile, "efile.exe was created and is alive");
	HCHECK(HostedExecTraps > 0, "it made exec calls through EUSER (%u)", HostedExecTraps);
	// regressions 2026-11-12: a second EXE's .data at the same run address (KErrInUse, Fserv fault 11);
	// a loader-created EXE copied its .data from address 0; modules from one source shared inline code
	HCHECK(estart, "EStart.exe (the second EXE, data at the same address) ran");
	// past the modem (start-up waits for its replies): the System Agent and the window server
	HCHECK(sysagent, "the System Agent server was started (modem replies arrive)");
	HCHECK(wserv, "the window server was started");
	// regressions 2026-11-12: the variant HAL (EVariantHalVariantInfo) and kernel-side faults on a bad
	// client descriptor (IPC exception trap) each killed FbServ
	extern volatile TUint32 HostedExceptions;
	HCHECK(fbserv, "the font and bitmap server is alive (%u hardware exceptions handled)", HostedExceptions);
	// regression 2026-11-12: an empty command line (address 0, length 0) made the loader fail (-38)
	// regression 2026-11-12: a trapped kernel-side fault left iInKernel one too high (FbServ deadlock)
	HCHECK(stuck == 0, "no user thread waits at a kernel level above 1 (callbacks would never run) (%d)", stuck);
	// regression 2026-11-12: a module's own include path gave it the real syborg.h (plain memory reads)
	extern TUint32 HostedIoAccesses(TUint32 aBase);
	extern const TUint32* HostedFbPixels(TInt& aWidth, TInt& aHeight, TInt& aPitch);
	HCHECK(HostedIoAccesses(0xC6005000u) > 4, "the display drivers reached the framebuffer model through the bus (%u accesses)",
	       HostedIoAccesses(0xC6005000u));
	HCHECK(WasSeen("C_info"), "mobilecrashstarter started mc_info.exe (a RAM-loaded EXE from ROFS)");
	HCHECK(HostedExeDataSwitches > 0, "EXE data areas were switched on process switch (%u)", HostedExeDataSwitches);
	HCHECK((isc && iscext) || iad, "the modem stand-ins installed: NokiaISCDriver and NokiaISCDriverExtension (%d %d), or IADDriver (%d)",
	       isc, iscext, iad);
	HCHECK(alive >= 6, "%d processes alive (EStart starts the system servers)", alive);
	// regression 2026-11-12: a second process of a loaded EXE was refused (KErrNotSupported); the 808's start-up
	// list starts a second cntsrv.exe and failed (state 117, reset)
	// regression 2026-11-12: Lookup(0) of an image without named exports returned the word before its export
	// directory; libdl took it for a named symbol table (the 808's weatherpublisher crashed)
	{
	TInt plain = 0, named = 0, wrong = 0;
	DCodeSeg::Wait();
	for (SDblQueLink* l = DCodeSeg::GlobalList.First(); l != &DCodeSeg::GlobalList.iA; l = l->iNext)
		{
		DCodeSeg* cs = _LOFF(l, DCodeSeg, iLink);
		extern TBool HostedCodeSegBuiltIn(DCodeSeg* aSeg);
		if (HostedCodeSegBuiltIn(cs))
			continue;					// built-in hosted modules: their own export table
		TBool nmd = cs->iAttr & ECodeSegAttNmdExpData;
		TLibraryFunction f = cs->Lookup(0);
		nmd ? ++named : ++plain;
		if (!nmd && f)
			++wrong;
		}
	DCodeSeg::Signal();
	HCHECK(wrong == 0 && plain > 0, "ordinal 0 is NULL for images without named exports (%d of %d wrong; %d have them)",
	       wrong, plain, named);
	}
	extern TUint32 HostedBTraceFast, HostedBTraceFastAsm;
	HCHECK(HostedBTraceFast > 0 && HostedBTraceFastAsm > 0, "filtered BTraceOut calls answered without entering the kernel "
	       "(%u by Dispatch, then %u by the trampoline)", HostedBTraceFast, HostedBTraceFastAsm);
	extern TUint32 HostedCodeSegMemoryInfos;
	htest_note("code segment memory infos answered: %u", HostedCodeSegMemoryInfos);
	extern TUint32 HostedExeAttaches, HostedExeAttachFailures;
	HCHECK(HostedExeAttachFailures == 0, "second processes of loaded EXEs created (%u, %u refused)", HostedExeAttaches,
	       HostedExeAttachFailures);
	if (hgetenv("NVDELTA"))
		{
		// htest --env NVDELTA=1: C:'s writes of this run as /out/nvmem0.delta (tools/hosted/nvdelta.py)
		extern TInt HostedNvmemSaveDelta(TInt aIndex, const char* aPath);
		htest_note("C: delta: %d blocks to /out/nvmem0.delta", HostedNvmemSaveDelta(0, "/out/nvmem0.delta"));
		}
	extern TUint32 HostedUserSpMisaligned;
	// bug 2026-11-12: ARMv6K phones' user stacks started 4 mod 8 (varargs read the wrong slot: the 808's SQLite
	// formatted 3 as "12884901891" and took every database for corrupt)
	HCHECK(HostedUserSpMisaligned == 0, "user threads entered with sp mod 8 as the phone's kernel (ckernel.cia) leaves it (%u differ)", HostedUserSpMisaligned);
	LocalWindowTest();
	PhysicalCommitTest();
	extern void HostedProfileReport();
	HostedProfileReport();
	// the screen as the phone shows it (HostedFbScreen: the panel's view of the framebuffer, or the host-composed
	// frame with --gl), written for the host (/out, htest's vmout/)
	extern TBool HostedFbScreen(TUint32* aOut, TInt aMaxPixels, TInt& aWidth, TInt& aHeight);
	TInt fw = 0, fh = 0, pitch = 0;
	TAny* scr = hmmap(0, 1024 * 1024 * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	const TUint32* px = !HMAP_FAILED(scr) && HostedFbScreen((TUint32*)scr, 1024 * 1024, fw, fh) ? (const TUint32*)scr : NULL;
	pitch = fw * 4;
	if (px)
		{
		TInt lit = 0;
		for (TInt y = 0; y < fh; ++y)
			for (TInt x = 0; x < fw; ++x)
				lit += (((const TUint32*)((const TUint8*)px + y * pitch))[x] & 0xffffff) != 0;
		htest_note("screen %dx%d: %d non-black pixels", fw, fh, lit);
		// the guest's video RAM as well (the picture under a host-composed frame), /out/kn_efile-vram.ppm
		{
		TInt vw = 0, vh = 0, vp = 0;
		const TUint32* v = HostedFbPixels(vw, vh, vp);
		TInt vfd = v ? hsys(SYS_openat, -100, "/out/kn_efile-vram.ppm", 0x241, 0644) : -1;
		if (vfd >= 0)
			{
			char hdr[32];
			hsys(SYS_write, vfd, hdr, hsnprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", vw, vh));
			TUint8 line[3 * 1024];
			TInt lit = 0;
			for (TInt y = 0; y < vh; ++y)
				{
				const TUint32* row = (const TUint32*)((const TUint8*)v + y * vp);
				for (TInt x = 0; x < vw && x < 1024; ++x)
					{
					line[3 * x] = (TUint8)(row[x] >> 16);
					line[3 * x + 1] = (TUint8)(row[x] >> 8);
					line[3 * x + 2] = (TUint8)row[x];
					lit += (row[x] & 0xffffff) != 0;
					}
				hsys(SYS_write, vfd, line, 3 * (vw < 1024 ? vw : 1024));
				}
			hsys(SYS_close, vfd);
			htest_note("video RAM %dx%d: %d non-black pixels", vw, vh, lit);
			}
		}
		extern TUint32 HostedGlFrameSeq();
		htest_note("host-composed frames published: %u", HostedGlFrameSeq() / 2);
		TInt fd = hsys(SYS_openat, -100, "/out/kn_efile-fb.ppm", 0x241, 0644);
		if (fd >= 0)
			{
			char hdr[32];
			TInt hl = hsnprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", fw, fh);
			hsys(SYS_write, fd, hdr, hl);
			TUint8 line[3 * 1024];
			for (TInt y = 0; y < fh; ++y)
				{
				const TUint32* row = (const TUint32*)((const TUint8*)px + y * pitch);
				for (TInt x = 0; x < fw && x < 1024; ++x)
					{
					line[3 * x] = (TUint8)(row[x] >> 16);
					line[3 * x + 1] = (TUint8)(row[x] >> 8);
					line[3 * x + 2] = (TUint8)row[x];
					}
				hsys(SYS_write, fd, line, 3 * fw);
				}
			hsys(SYS_close, fd);
			}
		}
	else
		htest_note("framebuffer not enabled");
	hexit(htest_done());
	}

static void Name(DObject* aObj, char* aOut, TInt aMax);
extern void (*HostedFaultHook)(const TDesC8& aCategory, TInt aReason);
static void FaultHook(const TDesC8& aCategory, TInt aReason)
	{
	TBuf8<32> cat(aCategory.Left(32));
	// start-up now runs well past the media drivers: a kernel fault before the probe is a regression
	HCHECK(EFalse, "no kernel fault before the probe (fault %d after %u exec calls)", aReason, HostedExecTraps);
	HCHECK(HostedExecTraps > 3000, "efile.exe made more than 3000 exec calls through EUSER (%u)", HostedExecTraps);
	DObjectCon& procs = *K::Containers[EProcess];
	TBool efile = EFalse;
	char n[128];
	for (TInt i = 0; i < procs.Count(); ++i)
		{
		Name(procs[i], n, sizeof n);
		if (strstr(n, "efile"))
			efile = ETrue;
		}
	HCHECK(efile, "efile.exe was created");
	const TRomEntry* se = (const TRomEntry*)Epoc::RomHeader().iSecondaryFile;
	const TRomImageHeader& sh = *(const TRomImageHeader*)se->iAddressLin;
	HCHECK(!memcmp((const TAny*)(sh.iDataBssLinearBase + 0x100), (const TAny*)(sh.iDataAddress + 0x100), 8) ||
	       HostedExecTraps > 3000, "efile.exe .data came from the ROM");
	hexit(htest_done());
	}

// every process that started a thread (ended ones leave the container before the probe)
static TUint StartHook(TKernelEvent aEvent, TAny* a1, TAny*, TAny*)
	{
	if (aEvent == EEventStartThread && SeenCount < 96)
		{
		char n[128];
		Name(((DThread*)a1)->iOwningProcess, n, sizeof n);
		TInt l = Min((TInt)strlen(n), 31);
		n[l] = 0;
		for (TInt i = 0; i < SeenCount; ++i)
			if (!strcmp(Seen[i], n))
				return DKernelEventHandler::ERunNext;
		memcpy(Seen[SeenCount], n, l);
		Seen[SeenCount][l] = 0;
		SeenMs[SeenCount++] = (TUint32)((hnow_ns() - T0) / 1000000);
		}
	return DKernelEventHandler::ERunNext;
	}
// aPart in aName, ignoring ASCII case (the N95 names "EStart.exe", "FbServ.exe"; the 808 "estart.exe", "fbserv.exe")
static TBool HasPart(const char* aName, const char* aPart)
	{
	for (; *aName; ++aName)
		{
		TInt k = 0;
		while (aPart[k] && aName[k] && (aName[k] | 0x20) == (aPart[k] | 0x20))
			++k;
		if (!aPart[k])
			return ETrue;
		}
	return EFalse;
	}

static TBool WasSeen(const char* aPart)
	{
	for (TInt i = 0; i < SeenCount; ++i)
		if (HasPart(Seen[i], aPart))
			return ETrue;
	return EFalse;
	}


// ---- the local window (mlocal.cpp, docs/hosted-eka2/chunk-address-space.md). Bug 2026-11-12: every process's
// local chunks shared one window and were clamped to 16 MB; the 808 ran out at ~130 processes (STDLIBS-INIT -4).
extern TInt HostedLocalForceUnit;
extern TInt HostedLocalFreeUnit(TInt aUnits);
extern void LocalSwitch(DWin32Process* aProcess);
extern TUint32 HostedLocalDisplaced, HostedLocalCopies, HostedLocalSharedAddr;

static DChunk* MakeLocal(DProcess* aProcess, TInt aMax, TInt aCommit)
	{
	SChunkCreateInfo c;
	c.iGlobal = EFalse;
	c.iAtt = 0;
	c.iForceFixed = EFalse;
	c.iOperations = SChunkCreateInfo::EAdjust;
	c.iRunAddress = 0;
	c.iType = EUserData;
	c.iMaxSize = aMax;
	c.iInitialBottom = 0;
	c.iInitialTop = aCommit;
	c.iPreallocated = 0;
	c.iOwner = aProcess;
	c.iMapAttr = 0;
	c.iClearByte = 0;
	DChunk* ch = NULL;
	TLinAddr run;
	return aProcess->NewChunk(ch, c, run) == KErrNone ? ch : NULL;
	}

static void LocalWindowTest()
	{
	DObjectCon& procs = *K::Containers[EProcess];
	DProcess* user[48];
	TInt n = 0;
	procs.Wait();
	for (TInt i = 0; i < procs.Count() && n < 48; ++i)
		{
		DProcess* p = (DProcess*)procs[i];
		if (p != K::TheKernelProcess && p->iExitType == EExitPending && !p->iThreadQ.IsEmpty() && p->Open() == KErrNone)
			user[n++] = p;
		}
	procs.Signal();
	HCHECK(n >= 2, "local window: %d user processes to test with", n);
	if (n < 2)
		return;
	NKern::ThreadEnterCS();
	DProcess* a = user[0];
	DProcess* b = user[1];
	DThread* ta = _LOFF(a->iThreadQ.First(), DThread, iProcessLink);
	DThread* tb = _LOFF(b->iThreadQ.First(), DThread, iProcessLink);
	// two processes' chunks at the same address, each with its own contents
	HostedLocalForceUnit = HostedLocalFreeUnit(2);
	DChunk* x = MakeLocal(a, 0x20000, 0x2000);
	DChunk* y = MakeLocal(b, 0x20000, 0x2000);
	HostedLocalForceUnit = -1;
	HCHECK(x && y && x->Base() == y->Base() && (TLinAddr)x->Base() >= 0x01000000 && (TLinAddr)x->Base() < 0x3f000000,
	       "local chunks of two processes at one address (%08x, %08x)", x ? x->Base() : 0, y ? y->Base() : 0);
	if (x && y)
		{
		TUint32 va = 0xa1a1a1a1, vb = 0xb2b2b2b2, ra = 0, rb = 0;
		TUint8* at = x->Base() + 0x1000;
		TInt w1 = Kern::ThreadRawWrite(ta, at, &va, 4, NULL), w2 = Kern::ThreadRawWrite(tb, at, &vb, 4, NULL);
		TInt r1 = Kern::ThreadRawRead(ta, at, &ra, 4), r2 = Kern::ThreadRawRead(tb, at, &rb, 4);
		HCHECK(!w1 && !w2 && !r1 && !r2 && ra == va && rb == vb, "each process reads back its own word (%x %x; %d %d %d %d)",
		       ra, rb, w1, w2, r1, r2);
		TUint32 d0 = HostedLocalDisplaced;
		NKern::Lock();
		LocalSwitch((DWin32Process*)b);
		NKern::Unlock();
		HCHECK(*(volatile TUint32*)at == vb && HostedLocalDisplaced > d0, "switching to the second maps its page there");
		ra = 0;
		HCHECK(Kern::ThreadRawRead(ta, at, &ra, 4) == KErrNone && ra == va, "the first one's word through its file meanwhile");
		NKern::Lock();
		LocalSwitch((DWin32Process*)a);
		NKern::Unlock();
		HCHECK(*(volatile TUint32*)at == va, "switching back maps the first one's page again");
		x->Adjust(0x1000);
		HCHECK(Kern::ThreadRawRead(ta, at, &ra, 4) == KErrBadDescriptor, "a decommitted page refuses a copy");
		HCHECK(Kern::ThreadRawRead(tb, at, &rb, 4) == KErrNone && rb == vb, "the other process's page is untouched");
		}
	if (x)
		x->Close(NULL);
	if (y)
		y->Close(NULL);
	// full maxima: two 34 MB heaps per process, more than the window holds without sharing addresses
	DChunk* big[96];
	TInt made = 0, failed = 0;
	for (TInt i = 0; i < n; ++i)
		for (TInt k = 0; k < 2; ++k)
			{
			DChunk* c = MakeLocal(user[i], 0x2200000, 0x1000);
			c ? (void)(big[made++] = c) : (void)++failed;
			}
	HCHECK(failed == 0 && (TUint64)made * 0x2200000 > (0x3f000000 - 0x01000000),
	       "%d local chunks of 34 MB (%u MB in all) created, %d failed", made, (TUint32)(((TUint64)made * 0x2200000) >> 20), failed);
	for (TInt i = 0; i < made; ++i)
		big[i]->Close(NULL);
	NKern::ThreadLeaveCS();
	for (TInt i = 0; i < n; ++i)
		user[i]->Close(NULL);
	htest_note("local window: %u displacements, %u file copies, %u chunks on shared addresses", HostedLocalDisplaced,
	           HostedLocalCopies, HostedLocalSharedAddr);
	}


// ---- physical commits (bug 2026-11-12: Kern::ChunkCommitPhysical failed, KErrNotSupported; the 808's audio
// stand-in got no DSP window, the audio engine panicked, start-up waited on its tone with the animation shown)
static void PhysicalCommitTest()
	{
	NKern::ThreadEnterCS();
	TPhysAddr phys = 0;
	TInt r = Epoc::AllocPhysicalRam(0x4000, phys, 16);
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = 0x10000;
	info.iMapAttr = EMapAttrCachedMax;
	info.iOwnsMemory = EFalse;
	info.iDestroyedDfc = NULL;
	DChunk* ch = NULL;
	TLinAddr kaddr = 0;
	TUint32 attr;
	TInt rc = r == KErrNone ? Kern::ChunkCreate(info, ch, kaddr, attr) : r;
	TInt rp = rc == KErrNone ? Kern::ChunkCommitPhysical(ch, 0x4000, 0x4000, phys) : rc;
	HCHECK(rc == KErrNone && rp == KErrNone, "a chunk that does not own its memory: created %d, physical commit %d", rc, rp);
	if (rp == KErrNone)
		{
		volatile TUint32* viaPhys = (volatile TUint32*)HostedPhysicalToLinear(phys, 0x4000);
		volatile TUint32* viaChunk = (volatile TUint32*)(kaddr + 0x4000);
		viaPhys[5] = 0x5a5a1234;
		viaChunk[0x3ff] = 0x77665544;
		TLinAddr ka;
		TUint32 pa = 0;
		TInt ra = Kern::ChunkPhysicalAddress(ch, 0x4000, 0x1000, ka, attr, pa, NULL);
		HCHECK(viaChunk[5] == 0x5a5a1234 && viaPhys[0x3ff] == 0x77665544, "the chunk and the physical RAM are the same pages");
		HCHECK(ra == KErrNone && pa == phys, "the chunk reports the real physical address (%08x, %08x)", pa, phys);
		HCHECK(ch->Decommit(0x4000, 0x4000) == KErrNone && viaPhys[5] == 0x5a5a1234,
		       "decommit leaves the driver's memory alone");
		}
	if (ch)
		Kern::ChunkClose(ch);
	NKern::ThreadLeaveCS();
	}

static TDfc* ProbeDfc;
static NTimer* ProbeTimer;
static void ProbeTimerFn(TAny*) { ProbeDfc->Enque(); }

static TDfc* PhoneDiagDfc;
static NTimer* PhoneTimer;
extern volatile TUint32* HostedDisplayShm();
static void PhoneDiag(TAny*)
	{
	hprintf("PHONEDIAG begin\n");
	ServersReport();
	DObjectCon& threads = *K::Containers[EThread];
	threads.Wait();
	for (TInt i = 0; i < threads.Count(); ++i)
		{
		DThread* t = (DThread*)threads[i];
		if (t->iOwningProcess == K::TheKernelProcess || t->iExitType != EExitPending)
			continue;
		char n[96], w[64];
		Name(t, n, sizeof n);
		w[0] = 0;
		if (t->iWaitObj)
			Name((DObject*)t->iWaitObj, w, sizeof w);
		if (t->iNThread.iNState != NThreadBase::EReady)
			htest_note("thread %s: nstate %d wait %s", n, t->iNThread.iNState, w);
		}
	threads.Signal();
	hprintf("PHONEDIAG end\n");
	}

static void PhoneTimerFn(TAny*)
	{
	static TUint32 Seen;
	volatile TUint32* shm = HostedDisplayShm();
	if (shm && shm[0x80 / 4] != Seen)
		{
		Seen = shm[0x80 / 4];
		PhoneDiagDfc->Enque();
		}
	PhoneTimer->Again(500);
	}

static TInt ProbeEntry(TInt aReason)
	{
	if (aReason == KModuleEntryReasonExtensionInit0)
		return KErrNone;
	(new DKernelEventHandler(&StartHook, NULL))->Add();
	if (hgetenv("PHONE"))
		{
		// htest --phone: the phone runs until the window closes; a diagnosis on request (Ctrl+P in the window:
		// shared word 0x80 bumped): the servers' unanswered messages and every waiting user thread
		PhoneDiagDfc = new TDfc(&PhoneDiag, 0, K::DfcQ0, 3);
		PhoneTimer = new NTimer(&PhoneTimerFn, 0);
		PhoneTimer->OneShot(500, ETrue);
		return KErrNone;
		}
	ProbeDfc = new TDfc(&ProbeFn, 0, K::DfcQ0, 3);
	ProbeTimer = new NTimer(&ProbeTimerFn, 0);
	ProbeTimer->OneShot(hgetenv_hex("PROBE_MS", 0) ? hgetenv_hex("PROBE_MS", 0) : 15000, ETrue);	// htest --env PROBE_MS=hex
	return KErrNone;
	}

static const char* strstr_(const char* h, const char* n)
	{
	TInt l = strlen(n);
	for (; *h; ++h)
		if (!memcmp(h, n, l)) return h;
	return 0;
	}

int main(int, char**)
	{
	T0 = hnow_ns();
	extern void HostedProfileStart();
	if (hgetenv_hex("PROFILE", 0))
		HostedProfileStart();
	SRomArgs a;
	if (!RomReadArgs(a))
		{
		htest_note("SKIP: no phone ROM given (htest --rom DEVICE)");
		hexit(77);
		}
	HCHECK(RomLoad(a), "core ROM loaded at %08x", a.iBase);
	LxInit();
	HCHECK(HostedExecTrapInit(a.iStubLo, a.iStubHi) == 0, "exec trap on EUSER's stubs %08x..%08x", a.iStubLo, a.iStubHi);
	extern TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage);
	HCHECK(HostedBoardInit(a.iBoardConfig, a.iRofs[0] ? a.iRofs : NULL) == KErrNone, "hosted board: I/O area, board config, ROFS window %s", a.iRofs);
	extern TInt HostedBoardDrives(const char* aNvmem, const char* aNvmem2);
	HCHECK(HostedBoardDrives(a.iNvmem, a.iNvmem2) == KErrNone, "NVMemory drives C: %s, E: %s (private mappings)", a.iNvmem, a.iNvmem2);
	extern TInt HostedFbAttach(TInt aWidth, TInt aHeight);
	HCHECK(a.iFbWidth && HostedFbAttach(a.iFbWidth, a.iFbHeight) == KErrNone, "framebuffer %ux%u", a.iFbWidth, a.iFbHeight);
	hsys(SYS_prctl, 15 /* PR_SET_NAME */, "symbian");		// the process as Linux's top shows it
	if (hgetenv("PHONE") && !hgetenv("PHONE_NODISPLAY"))
		{
		// htest --phone: PHONE = the ivshmem BAR's sysfs file; the host window (tools/hosted/phone.py) maps it too
		extern TInt HostedDisplayStart(const char* aPath);
		HCHECK(HostedDisplayStart(hgetenv("PHONE")) == KErrNone, "the phone's screen in shared memory %s", hgetenv("PHONE"));
		}
	if (hgetenv("GLCHAN"))
		{
		// htest --gl: syborg-gl on the host GL server (the phones that compose on the GPU)
		extern TInt HostedGlAttach(const char* aPath);
		HCHECK(HostedGlAttach(hgetenv("GLCHAN")) == KErrNone, "syborg-gl on the host GL server (%s)", hgetenv("GLCHAN"));
		}
	if (a.iTouch)
		{
		extern TInt HostedPtrAttach();
		HCHECK(HostedPtrAttach() == KErrNone, "touch screen (syborg-pointer, the mouse in the phone window)");
		}
	extern TInt HostedKeybAttach();
	HCHECK(HostedKeybAttach() == KErrNone, "keyboard (syborg-keyboard, keys from the phone window)");
	extern TInt HostedSndAttach();
	HCHECK(HostedSndAttach() == KErrNone, "sound device (syborg-snd, no output)");
	extern TInt HostedModemAttach(const char* aConfig, const char* aReplies);
	HCHECK(HostedModemAttach(a.iBoardConfig, a.iReplies) == KErrNone, "fake modem (shared core), replies %s", a.iReplies);
	HostedUseTestVariant();
	htest_note("exec personality %u", a.iPersonality);
	HostedSetKernelConfig(a.iPersonality << 25);		// the device's user-side exec numbering
	HostedSetKernelConfig(EKernelConfigPlatSecDiagnostics);	// print every failed capability check
	HostedAddExtension(&ProbeEntry);
	HostedSetDebugMask(0, hgetenv_hex("KTRACE", (1u << KBOOT) | (1u << KPANIC)));
	extern TInt HostedTickSkip;
	HostedTickSkip = hgetenv_hex("TICK_SKIP", 1);
	HostedTraceExecs = hgetenv_hex("XTRACE", 0);	// htest --xtrace N: the first N exec calls (execnames.py)
	extern const char* HostedTraceExecsMatch;
	HostedTraceExecsMatch = hgetenv("XTRACE_THREAD");	// htest --env XTRACE_THREAD=FbServ: only those
	HostedFaultHook = &FaultHook;
	// htest --env PROBES=addr[:rN=V],...: code probes at ROM addresses (Thumb with bit 0 set); one-shot, or with a
	// register condition (hex value) armed until it matches (e.g. User::Panic with r1 = 0xb: a USER 11 panic)
	extern TInt HostedAddProbe(TUint32 aAddr);
	extern TInt HostedAddProbeIf(TUint32 aAddr, TInt aReg, TUint32 aValue);
	if (const char* pr = hgetenv("PROBES"))
		for (const char* q = pr; *q; )
			{
			TUint32 x = 0, v = 0;
			TInt reg = -1;
			while (*q && *q != ',' && *q != ':') { x = x * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); ++q; }
			if (q[0] == ':' && q[1] == 'r')
				{
				q += 2;
				reg = 0;
				while (*q >= '0' && *q <= '9') reg = reg * 10 + (*q++ - '0');
				if (*q == '=') ++q;
				while (*q && *q != ',') { v = v * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); ++q; }
				}
			if (*q == ',') ++q;
			if (x) reg >= 0 ? HostedAddProbeIf(x, reg, v) : HostedAddProbe(x);
			}
	HostedBootEpoc(a.iBase);
	return htest_done();
	}
