// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i ExtSoundLdd.ldd stand-in: "EXT_SOUND_DRV", the sound output channel the start-up
// animation player (system_animation_player.exe, ROFS1) and A2dp.dll open. The player panics "Could not
// load SoundLDD" when the LDD is missing, and APS (startuphandler.aps) dies after it [V R6E30 boot].
// Stand-in H-p1i-sound-silent (docs/hacks.md): every control returns KErrNone, every request completes
// at once with KErrNone, nothing is played; each call is logged ("SYBSTUB:") as the specification
// for a real sound path later. Player calls seen statically [V]: controls 0, 2, 3, 4, 5, 7, 8, 9, 10,
// 11, 46 (cancel), requests 0..5 (5 is waited for synchronously).
// Except:
//   control 2, a1 -> { ?, ?, ?, buffer size }: the play buffer size (0xfa0 for SecurityApp's tone) [V
//   ZebraMMFSwCodecWrapper.dll 0x8d64; meaning of words 0..2 I].
//   control 3, a1 -> { out: buffer user address, out: its size }: the play buffer (ZebraMMFSwCodecWrapper.dll
//   0x8fee wraps it in a TPtr8 and a CMMFPtrBuffer [V]). Unanswered, the address stayed 0 and SecurityApp died
//   KERN-EXEC 3 in ZebraMMFDevSoundPlugin.dll (memmove into it) after Flight mode (2026-11-17). Here: a shared
//   chunk mapped into the client's process (one per process, kept), size = the last control 2 size.
#include "tablestub.h"
#include <platform.h>
#include <kernel/kern_priv.h>
#include <memmodel/epoc/platform.h>

const TUint KMaxBuffer = 0x10000;
static TUint BufferSize = 0x1000;

struct TMapped { DProcess* iProcess; TUint iId; TLinAddr iUser; };
static TMapped Mapped[8];

static TInt UserBuffer(DThread* aClient, TLinAddr& aUser)
	{
	DProcess* p = aClient->iOwningProcess;
	for (TInt i = 0; i < 8; ++i)
		if (Mapped[i].iProcess == p && Mapped[i].iId == p->iId)
			{ aUser = Mapped[i].iUser; return KErrNone; }
	NKern::ThreadEnterCS();
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = KMaxBuffer;
	info.iMapAttr = EMapAttrCachedMax;
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
		memset((TAny*)kaddr, 0, info.iMaxSize);
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

static TInt CtlHook(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* aWords)
	{
	if (aFn == 2 && a1 && aWords[3] && aWords[3] <= KMaxBuffer)
		BufferSize = aWords[3];
	if (aFn == 3 && a1)
		{
		TLinAddr user = 0;
		TInt r = UserBuffer(aClient, user);
		TUint32 out[2] = { (TUint32)user, BufferSize };
		if (r == KErrNone)
			r = Kern::ThreadRawWrite(aClient, a1, out, sizeof(out));
		Kern::Printf("SYBSTUB: EXT_SOUND_DRV play buffer %08x, %d bytes for %O: %d", user, BufferSize, aClient, r);
		return r;
		}
	return KStubNotHandled;
	}

NO_CONTROLS
TABLE_STUB_LDD_CTLHOOK("EXT_SOUND_DRV", KCtl, ETrue, CtlHook)
