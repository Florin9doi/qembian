// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i DspControlLdd.ldd stand-in: "DSP_CONTROL_DRV", the audio DSP control channel
// (codec module load from \private\10200B19\DspCodecModule.bin, filter coefficients) of the start-up
// animation player and M3SAudioDevMgr (mmmgmtserver). The player panics "Could not load DspControlLDD"
// without it [V].
// H-p1i-dsp-absent (docs/hacks.md): there is no DSP; controls return KErrNone, requests complete at
// once with KErrNone, every call is logged ("SYBSTUB:"), except:
//   controls 0 and 1, a1 -> { out: buffer address, w1, in: size, w3 }: DSP memory for a code/data module.
//   The client reads the module file into the buffer at word 0 (TPtr8(word0, 0, size); player ee5d73b8,
//   [V]). Unanswered (word 0 = 0) the read failed KErrBadDescriptor; mmmgmtserver died -38 with it, the
//   window server that starts it failed to initialise (WSERV-INTERNAL 13) and SysStart with it
//   (2026-11-17). Here: a shared chunk of that size, mapped into the client's process, its user address in
//   word 0 (w1, presumably the DSP-side address, stays as the client set it [I]).
#include "tablestub.h"
#include <platform.h>
#include <kernel/kern_priv.h>
#include <memmodel/epoc/platform.h>

static TInt DspMemory(DThread* aClient, TAny* a1, const TUint32* aWords)
	{
	TUint size = aWords[2];
	if (!size || size > 0x400000)				// 4 MB: the modules are ~300 KB [V R6E30]
		return KErrArgument;
	NKern::ThreadEnterCS();
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = Kern::RoundToPageSize(size);
	info.iMapAttr = EMapAttrCachedMax;
	info.iOwnsMemory = ETrue;
	info.iDestroyedDfc = NULL;
	DChunk* chunk = NULL;
	TLinAddr kaddr = 0;
	TUint32 attr;
	TInt r = Kern::ChunkCreate(info, chunk, kaddr, attr);
	if (r == KErrNone)
		r = Kern::ChunkCommit(chunk, 0, info.iMaxSize);
	TLinAddr user = 0;
	if (r == KErrNone)
		{
		TInt h = Kern::MakeHandleAndOpen(aClient, (DObject*)chunk);	// maps it into the client's process; the
		r = h < 0 ? h : KErrNone;							// client's handle keeps it alive
		if (r == KErrNone)
			user = (TLinAddr)Kern::ChunkUserBase(chunk, aClient);
		}
	if (chunk)
		Kern::ChunkClose(chunk);					// our reference; the client's handle stays
	NKern::ThreadLeaveCS();
	if (r == KErrNone)
		r = Kern::ThreadRawWrite(aClient, a1, &user, sizeof(user));
	Kern::Printf("SYBSTUB: DSP_CONTROL_DRV module memory %d bytes at %08x for %O: %d", size, user, aClient, r);
	return r;
	}

static TInt CtlHook(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* aWords)
	{
	if ((aFn == 0 || aFn == 1) && a1)
		return DspMemory(aClient, a1, aWords);
	return KStubNotHandled;
	}

NO_CONTROLS
TABLE_STUB_LDD_CTLHOOK("DSP_CONTROL_DRV", KCtl, ETrue, CtlHook)
