// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i crypto.ldd stand-in: SE's crypto engine driver under crys.dll / dcl.dll (DRM crypto,
// drmclockhandler.aps in the second APS process). The real driver does no crypto: controls 0..4 hand out the
// user address of a hardware register block (one out word at a1), 5..9 release them; crys.dll then programs the
// engine with plain loads and stores (docs/research/p1i-crypto-ldd.md). Without a driver dcl.dll called
// DoControl on a null handle (KERN-EXEC 0), refusing the controls left it a garbage base (data abort), and the
// APS process died, SysStart with it (2026-11-17).
// H-p1i-crypto-absent (docs/hacks.md): RAM pages stand in for the registers, zero-filled: every engine wait
// times out (crys returns an error after ~4 M polls) and the DRM crypto calls fail cleanly.
#include "tablestub.h"
#include <platform.h>
#include <kernel/kern_priv.h>
#include <memmodel/epoc/platform.h>

// crypto.ldd hands crys.dll the user address of a hardware register block (ctl 0..4, one out word at a1)
// and releases it (ctl 5..9). H-p1i-crypto-absent: RAM pages stand in for the registers.
const TInt KCryptoWindows = 5;
const TUint KWindowSize = 0x1000;
const TUint8 KFill = 0xff;			// waits pass, results are junk: 0x00 (waits time out, CRYS errors) made drmclockhandler panic USER 23 (2026-11-17)

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
