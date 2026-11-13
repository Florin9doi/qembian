// Hosted EKA2: the syborg-gl device (QEMU board: src/qemu-syborg/hw/arm/syborg_gl.c) on the hosted board.
// The device itself runs in the host GL server (src/hosted/glserver: the same syborg_gl.c, on the host's GPU);
// here its register page (device 0x0f, as KHwBaseGl in the d_display stand-in) forwards every register read
// and write over the channel (include/board/syborg_glchan.h, an ivshmem BAR in the test VM) and waits for the
// answer. While it waits it serves the server's memory callbacks: the kick's buffers (virtual addresses of
// this one hosted process, copied with process_vm_readv/writev so a bad pointer is an error, not a fault) and
// physical ones (the board's windows: command buffers, the frame buffer). A helper host thread serves the
// callbacks that come with no request running (the composed frame, written after a timer in the server).
// User mode never maps this page (DPlatChunkHw refuses modelled device pages for user mappings): the guest's
// GL clients kick through the kernel (d_display ctl 0x7f12), which writes PID/FLAGS/KICK here.
// Original work for qembian.
#include <kernel/kern_priv.h>
#include <board/syborg_glchan.h>

extern TInt HostedIoAttach(TUint32 aBase, TUint32 aSize, TUint32 (*aRead)(TAny*, TUint32),
                           void (*aWrite)(TAny*, TUint32, TUint32), TAny* aCtx);
extern TLinAddr HostedPhysicalToLinear(TPhysAddr aPhys, TUint32 aSize);

static const TUint32 KGlIo = 0xC6000000u + 0x0f * 0x1000;	// KHwBaseGl (d_display)
static volatile TUint32* Chan;
static volatile TInt ServeLock;
static volatile TInt HelperTid;
TUint32 HostedGlRequests, HostedGlCallbacks, HostedGlCallbackBytes, HostedGlWlogFaults, HostedGlPrefetched;
TUint64 HostedGlRequestNs;				// time spent in requests (waiting for the server included)

static TUint64 MonoNs()
	{
	struct { TInt32 s, ns; } t;
	hsys(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, &t);
	return (TUint64)t.s * 1000000000u + t.ns;
	}

// htest --env GLSTAT=1: requests, callbacks and the time in requests every 2 s (the display thread calls this)
void HostedGlStat()
	{
	static TUint64 Last;
	static TUint32 R0, C0;
	static TUint64 N0;
	TUint64 now = MonoNs();
	if (!Chan || now - Last < 2000000000u)
		return;
	extern volatile TUint32 HostedSwitches, HostedSuspends, HostedExeDataSwitches, HostedExecTraps, HostedArm11Emulated,
	                        HostedExceptions, HostedTicks, HostedTicksSkipped;
	extern TUint32 HostedLocalMaps, HostedLocalDisplaced, HostedLocalCopies;
	static TUint32 S0[11];
	TUint32 S[11] = { HostedSwitches, HostedSuspends, HostedExeDataSwitches, HostedExecTraps, HostedArm11Emulated,
	                  HostedExceptions, HostedTicks, HostedTicksSkipped, HostedLocalMaps, HostedLocalDisplaced,
	                  HostedLocalCopies };
	if (Last)
		{
		hprintf("GLSTAT %u req/s %u cb/s %u us/s in requests, %u us per request\n", (HostedGlRequests - R0) / 2,
		        (HostedGlCallbacks - C0) / 2, (TUint32)((HostedGlRequestNs - N0) / 2000),
		        HostedGlRequests - R0 ? (TUint32)((HostedGlRequestNs - N0) / 1000 / (HostedGlRequests - R0)) : 0);
		hprintf("HSTAT per s: %u switches %u suspends %u exe-data %u exec-traps %u arm11-emul %u exceptions "
		        "%u ticks (%u skipped) %u local-maps %u displaced %u local-copies\n", (S[0] - S0[0]) / 2,
		        (S[1] - S0[1]) / 2, (S[2] - S0[2]) / 2, (S[3] - S0[3]) / 2, (S[4] - S0[4]) / 2, (S[5] - S0[5]) / 2,
		        (S[6] - S0[6]) / 2, (S[7] - S0[7]) / 2, (S[8] - S0[8]) / 2, (S[9] - S0[9]) / 2, (S[10] - S0[10]) / 2);
		}
	extern TUint32 HostedEventSpinHits, HostedEventSleeps;
	static TUint32 H0, Z0;
	if (Last)
		hprintf("HSPIN per s: %u hand-overs caught spinning, %u after sleeping\n", (HostedEventSpinHits - H0) / 2,
		        (HostedEventSleeps - Z0) / 2);
	H0 = HostedEventSpinHits;
	Z0 = HostedEventSleeps;
	memcpy(S0, S, sizeof S);
	// the five busiest exec calls of the last 2 s, with the return address of the last caller
	extern TUint32 HostedExecCount[2][256], HostedExecLastLr[2][256];
	static TUint32 E0[2][256];
	if (Last)
		{
		for (TInt k = 0; k < 5; ++k)
			{
			TInt bf = 0, bn = 0;
			TUint32 best = 0;
			for (TInt f = 0; f < 2; ++f)
				for (TInt i = 0; i < 256; ++i)
					if (HostedExecCount[f][i] - E0[f][i] > best)
						{
						best = HostedExecCount[f][i] - E0[f][i];
						bf = f;
						bn = i;
						}
			if (!best)
				break;
			hprintf("EXECTOP %s %02x: %u/s, last from %08x\n", bf ? "fast" : "slow", bn, best / 2, HostedExecLastLr[bf][bn]);
			E0[bf][bn] = HostedExecCount[bf][bn];		// excluded from the next pick
			}
		}
	memcpy(E0, HostedExecCount, sizeof E0);
	Last = now;
	R0 = HostedGlRequests;
	C0 = HostedGlCallbacks;
	N0 = HostedGlRequestNs;
	}

static inline TUint32 Ld(TInt i) { return __atomic_load_n(&Chan[i], __ATOMIC_ACQUIRE); }
static inline void St(TInt i, TUint32 v) { __atomic_store_n(&Chan[i], v, __ATOMIC_RELEASE); }

struct SIov { TAny* iBase; TUint32 iLen; };
static TUint8 Bounce[0x10000];			// write callbacks: the BAR -> here -> guest memory (ServeLock held)

// copy within this process, safely: process_vm_readv/writev on ourselves returns EFAULT for a bad address
static TInt SelfCopy(TAny* aDst, const TAny* aSrc, TUint32 aLen)
	{
	SIov local = { aDst, aLen }, remote = { (TAny*)aSrc, aLen };
	TInt r = hsys(376 /* process_vm_readv */, hgetpid(), &local, 1, &remote, 1, 0);
	return r == (TInt)aLen ? KErrNone : KErrBadDescriptor;
	}

// v3 write log (syborg_glchan.h): the server's virtual writes of the running request, applied in order; ServeLock
// held. Applied before any callback and after the response.
static TUint32 WlogApplied, WlogOffset;
static void ApplyWlog()
	{
	TUint32 n = Ld(GLCHAN_W_WLOG_N);
	const TUint8* base = (const TUint8*)(Chan + GLCHAN_WLOG / 4);
	for (; WlogApplied < n; ++WlogApplied)
		{
		const TUint32* e = (const TUint32*)(base + WlogOffset);
		TUint32 va = e[0], len = e[1];
		for (TUint32 done = 0; done < len; )
			{
			TUint32 c = len - done < sizeof(Bounce) ? len - done : sizeof(Bounce);
			memcpy(Bounce, (const TUint8*)(e + 2) + done, c);
			if (SelfCopy((TAny*)(va + done), Bounce, c) != KErrNone)
				++HostedGlWlogFaults;
			done += c;
			}
		WlogOffset += 8 + ((len + 3) & ~3u);
		}
	}

// one pending callback, if any (the requester and the helper thread both call this; one serves it)
static void ServeCallback()
	{
	TUint32 seq = Ld(GLCHAN_W_CB_SEQ);
	if (seq == Ld(GLCHAN_W_CB_DONE))
		return;
	if (__atomic_exchange_n(&ServeLock, 1, __ATOMIC_ACQUIRE))
		return;
	seq = Ld(GLCHAN_W_CB_SEQ);
	if (seq != Ld(GLCHAN_W_CB_DONE))
		{
		ApplyWlog();						// the server's earlier writes first (order)
		TUint32 op = Ld(GLCHAN_W_CB_OP), addr = Ld(GLCHAN_W_CB_ADDR), len = Ld(GLCHAN_W_CB_LEN);
		TUint8* data = (TUint8*)(Chan + GLCHAN_DATA / 4);
		TInt r = len <= GLCHAN_DATA_MAX ? KErrNone : KErrArgument;
		if (r == KErrNone)
			{
			switch (op)
				{
			case GLCHAN_CB_READ_VIRT: r = SelfCopy(data, (const TAny*)addr, len); break;
			case GLCHAN_CB_WRITE_VIRT:
				// staged through ordinary memory: process_vm_* cannot pin pages of the channel's PCI BAR
				// mapping (VM_PFNMAP). Bug 2026-11-12: every write into guest memory was refused (EFAULT),
				// so each EGL call's result was lost and the 808's EGL clients failed. Regression: kn_gl (htest --gl).
				for (TUint32 done = 0; r == KErrNone && done < len; )
					{
					TUint32 n = len - done < sizeof(Bounce) ? len - done : sizeof(Bounce);
					memcpy(Bounce, data + done, n);
					r = SelfCopy((TAny*)(addr + done), Bounce, n);
					done += n;
					}
				break;
			case GLCHAN_CB_READ_PHYS:
			case GLCHAN_CB_WRITE_PHYS:
				{
				TLinAddr lin = HostedPhysicalToLinear(addr, len);
				if (!lin)
					r = KErrArgument;
				else if (op == GLCHAN_CB_READ_PHYS)
					memcpy(data, (const TAny*)lin, len);
				else
					memcpy((TAny*)lin, data, len);
				break;
				}
			default: r = KErrArgument;
				}
			}
		++HostedGlCallbacks;
		HostedGlCallbackBytes += len;
		St(GLCHAN_W_CB_STATUS, r == KErrNone ? 0 : 1);
		St(GLCHAN_W_CB_DONE, seq);
		}
	__atomic_store_n(&ServeLock, 0, __ATOMIC_RELEASE);
	}

static int HelperThread(void*)
	{
	hsys(SYS_prctl, 15 /* PR_SET_NAME */, "h-glhelper");	// its name in the Linux view (lxtop)
	for (TUint32 idle = 0;; )
		{
		if (Ld(GLCHAN_W_CB_SEQ) != Ld(GLCHAN_W_CB_DONE))
			{
			ServeCallback();
			idle = 0;
			continue;
			}
		hsleep_us(++idle < 200 ? 100 : 1000);
		}
	}

// v3 (syborg_glchan.h): writes to plain registers wait for the next request; a write to KICK, EXEC or BIND carries
// them, and its answer (the register after the write: the run's result) answers the read that follows. A kick's
// descriptor and command buffer are copied to the prefetch area first. 4 round trips per kick become 1, and its
// descriptor/command reads and result write no longer call back.
enum { KRegBuf = 2, KRegLen = 3, KRegExec = 4, KRegOut = 5, KRegOutLen = 6, KRegKick = 7, KRegPid = 8,
       KRegFlags = 9, KRegBind = 10 };
static TUint32 PreReg[GLCHAN_PRE_MAX], PreVal[GLCHAN_PRE_MAX];
static TInt PreN;
static TInt CachedReg = -1;
static TUint32 CachedVal;
static TUint32 PfVa[GLCHAN_PF_ENTRIES], PfLen[GLCHAN_PF_ENTRIES];
static TInt PfN;
TUint32 HostedGlSaved;					// register accesses answered without a round trip (tests)

static TBool Deferred(TUint32 aReg)
	{
	return aReg == KRegBuf || aReg == KRegLen || aReg == KRegOut || aReg == KRegOutLen || aReg == KRegPid ||
	       aReg == KRegFlags;
	}

// a kick's descriptor and its command words into the prefetch area (the requester's own memory: current)
static void PrefetchKick(TUint32 aKick)
	{
	PfN = 0;
	struct { TUint32 iBuf, iLen, iOut, iOutLen, iResult; } k;
	if ((aKick & 3) || SelfCopy(&k, (const TAny*)aKick, sizeof(k)) != KErrNone)
		return;
	TUint32 off = 0;
	const TUint32 va[2] = { aKick, k.iBuf }, len[2] = { sizeof(k), k.iLen };
	for (TInt i = 0; i < 2; ++i)
		{
		if (!len[i] || off + len[i] > GLCHAN_PF_MAX)
			continue;
		if (SelfCopy((TUint8*)(Chan + GLCHAN_PF / 4) + off, (const TAny*)va[i], len[i]) != KErrNone)
			continue;
		St(GLCHAN_W_PF + 3 * PfN, va[i]);
		St(GLCHAN_W_PF + 3 * PfN + 1, len[i]);
		St(GLCHAN_W_PF + 3 * PfN + 2, off);
		PfVa[PfN] = va[i];
		PfLen[PfN] = len[i];
		++PfN;
		HostedGlPrefetched += len[i];
		off = (off + len[i] + 63) & ~63u;
		}
	}

// one register access: the request, then wait for its answer and serve callbacks meanwhile. Interrupts are
// masked on a nanothread (as the other device models): no other nanothread may start a request in between.
static TUint32 Request(TUint32 aOp, TUint32 aReg, TUint32 aValue)
	{
	TInt irq = -1;
	if (LxSelf() && LxSelf()->iNThread && NKern::CurrentContext() != NKern::EInterrupt)
		irq = NKern::DisableAllInterrupts();
	TUint32 r = aReg >> 2;
	TUint32 v;
	if (aOp == GLCHAN_REQ_WRITE && Deferred(r) && PreN < GLCHAN_PRE_MAX)
		{
		PreReg[PreN] = aReg;
		PreVal[PreN++] = aValue;
		CachedReg = -1;
		++HostedGlSaved;
		v = 0;
		}
	else if (aOp == GLCHAN_REQ_READ && (TInt)r == CachedReg)
		{
		v = CachedVal;					// the answer of the write before (KICK, EXEC, BIND: the result)
		CachedReg = -1;
		++HostedGlSaved;
		}
	else
		{
		// the requesting Symbian thread's process (no DThread on a plain host thread: tests without a kernel)
		NThread* nt = LxSelf() && LxSelf()->iNThread ? NKern::CurrentThread() : NULL;
		DThread* t = nt ? _LOFF(nt, DThread, iNThread) : NULL;
		PfN = 0;
		if (aOp == GLCHAN_REQ_WRITE && r == KRegKick)
			PrefetchKick(aValue);
		for (TInt i = 0; i < PreN; ++i)
			{
			St(GLCHAN_W_PRE + 2 * i, PreReg[i]);
			St(GLCHAN_W_PRE + 2 * i + 1, PreVal[i]);
			}
		St(GLCHAN_W_REQ_NPRE, PreN);
		St(GLCHAN_W_REQ_NPF, PfN);
		PreN = 0;
		St(GLCHAN_W_REQ_OP, aOp);
		St(GLCHAN_W_REQ_REG, aReg);
		St(GLCHAN_W_REQ_VALUE, aValue);
		St(GLCHAN_W_REQ_USER, 0);			// user mode never reaches this page (see above)
		St(GLCHAN_W_REQ_PID, t && t->iOwningProcess ? t->iOwningProcess->iId : 0);
		TUint64 t0 = MonoNs();
		while (__atomic_exchange_n(&ServeLock, 1, __ATOMIC_ACQUIRE))
			;
		St(GLCHAN_W_WLOG_N, 0);				// the last request's log is applied: never again
		WlogApplied = WlogOffset = 0;
		__atomic_store_n(&ServeLock, 0, __ATOMIC_RELEASE);
		TUint32 seq = Ld(GLCHAN_W_REQ_SEQ) + 1;
		St(GLCHAN_W_REQ_SEQ, seq);
		for (TUint32 n = 0; Ld(GLCHAN_W_RESP_SEQ) != seq; ++n)
			{
			ServeCallback();
			if (n > 2000)
				hsys(SYS_sched_yield);
			}
		v = Ld(GLCHAN_W_RESP_VALUE);
		while (__atomic_exchange_n(&ServeLock, 1, __ATOMIC_ACQUIRE))
			;						// the helper thread may be serving a callback
		ApplyWlog();					// the server's writes of this request
		__atomic_store_n(&ServeLock, 0, __ATOMIC_RELEASE);
		CachedReg = aOp == GLCHAN_REQ_WRITE && (r == KRegKick || r == KRegExec || r == KRegBind) ? (TInt)r : -1;
		CachedVal = v;
		if (aOp == GLCHAN_REQ_WRITE)
			v = 0;
		++HostedGlRequests;
		HostedGlRequestNs += MonoNs() - t0;
		}
	if (irq >= 0)
		NKern::RestoreInterrupts(irq);
	return v;
	}

static TUint32 GlRead(TAny*, TUint32 aOff) { return Request(GLCHAN_REQ_READ, aOff, 0); }
static void GlWrite(TAny*, TUint32 aOff, TUint32 aValue) { Request(GLCHAN_REQ_WRITE, aOff, aValue); }

// the host-composed screen (GLCHAN_FRAME), if the server has published one: its sequence (even), size and
// pixels in the channel. The caller copies and calls again to see that the sequence did not move meanwhile.
TBool HostedGlFrame(TUint32& aSeq, TInt& aW, TInt& aH, const TUint32*& aPixels)
	{
	if (!Chan)
		return EFalse;
	aSeq = Ld(GLCHAN_W_FRAME_SEQ);
	if (!aSeq || (aSeq & 1))
		return EFalse;
	aW = Ld(GLCHAN_W_FRAME_W);
	aH = Ld(GLCHAN_W_FRAME_H);
	aPixels = (const TUint32*)((const TUint8*)Chan + GLCHAN_FRAME);
	return aW > 0 && aH > 0 && (TUint32)(aW * aH * 4) <= GLCHAN_FRAME_MAX;
	}

TUint32 HostedGlFrameSeq() { return Chan ? Ld(GLCHAN_W_FRAME_SEQ) : 0; }

// attach the device to the channel at aPath (the ivshmem BAR's sysfs file); the server must be up
TInt HostedGlAttach(const char* aPath)
	{
	TInt fd = hsys(SYS_openat, -100, aPath, 2 /* O_RDWR */, 0);
	if (fd < 0)
		return KErrNotFound;
	TAny* m = hmmap(0, GLCHAN_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	hsys(SYS_close, fd);
	if (HMAP_FAILED(m))
		return KErrNoMemory;
	Chan = (volatile TUint32*)m;
	for (TInt i = 0; i < 1000 && (Ld(GLCHAN_W_MAGIC) != GLCHAN_MAGIC || Ld(GLCHAN_W_SERVER_UP) != 1); ++i)
		hsleep_us(10000);
	if (Ld(GLCHAN_W_MAGIC) != GLCHAN_MAGIC || Ld(GLCHAN_W_VERSION) != GLCHAN_VERSION)
		return KErrNotReady;
	St(GLCHAN_W_CB_DONE, Ld(GLCHAN_W_CB_SEQ));
	TAny* stack = hmmap(0, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(stack) || hthread(&HelperThread, NULL, stack, 0x10000, &HelperTid) < 0)
		return KErrNoMemory;
	hprintf("HOSTED: syborg-gl on the host GL server (%s)\n", aPath);
	return HostedIoAttach(KGlIo, 0x1000, &GlRead, &GlWrite, NULL);
	}
