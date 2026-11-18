// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// threadpriority.ldd stand-in ("THREADPRIORITY_LDD", Sony Ericsson UIQ 3). The phone's own LDD (P990i R6E28 core
// 0xf83501a8, 1 KB, imports ekern only) is a kernel-side firmware file our composition drops; the P990i's Phone app
// (CallControl.dll ConstructL 0x11f8a: LoadLogicalDevice + DoCreate, version 1.0.0, unit -1) leaves without it, and
// its error path then panics E32USER-CBase 63 (docs/devices/se-p990i/bring-up.md, H-semc-threadpriority).
// The same protocol as the original [V its DLogicalChannelBase::Request 0xf83502cc]:
//   every call needs WriteDeviceData (capability 5), else KErrPermissionDenied;
//   fn 0 (a1 = thread id, a2 = priority): Kern::SetThreadPriority(a2, thread)   (absolute: realtime priorities too)
//   fn 1 (a1 = thread id, a2 = TInt[3] out): the thread's priority state - the original copies three DThread words
//        (+0x34, +0x2c0, +0x144 of its own kernel's layout [I which members]); here iThreadPriority, the nanokernel
//        priority and iDefaultPriority
//   no thread with that id: KErrNotFound; other fn: KErrNotSupported. Both under the thread container's mutex.
#include <kernel/kern_priv.h>

class DThreadPriorityChannel : public DLogicalChannelBase
	{
public:
	virtual TInt Request(TInt aFn, TAny* a1, TAny* a2);
	};

TInt DThreadPriorityChannel::Request(TInt aFn, TAny* a1, TAny* a2)
	{
	if (!Kern::CurrentThreadHasCapability(ECapabilityWriteDeviceData, __PLATSEC_DIAGNOSTIC_STRING("THREADPRIORITY.LDD")))
		return KErrPermissionDenied;
	if (aFn != 0 && aFn != 1)
		return KErrNotSupported;
	TInt r = KErrNone;
	NKern::ThreadEnterCS();
	DObjectCon& threads = *Kern::Containers()[EThread];
	threads.Wait();
	DThread* t = Kern::ThreadFromId((TUint)a1);
	if (!t)
		r = KErrNotFound;
	else if (aFn == 0)
		r = Kern::SetThreadPriority((TInt)a2, t);
	else
		{
		TInt w[3] = { t->iThreadPriority, t->iNThread.iPriority, t->iDefaultPriority };
		kumemput32(a2, w, sizeof(w));
		}
	threads.Signal();
	NKern::ThreadLeaveCS();
	Kern::Printf("SYBSTUB: THREADPRIORITY fn %d thread %x %d -> %d", aFn, (TUint)a1, aFn ? 0 : (TInt)a2, r);
	return r;
	}

class DThreadPriorityFactory : public DLogicalDevice
	{
public:
	DThreadPriorityFactory()
		{
		iVersion = TVersion(1, 0, 0);
		iParseMask = 0;					// no units, no PDD
		}
	virtual TInt Install() { _LIT(KName, "THREADPRIORITY_LDD"); return SetName(&KName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DLogicalChannelBase*& aChannel)
		{
		aChannel = new DThreadPriorityChannel;
		return aChannel ? KErrNone : KErrNoMemory;
		}
	};

DECLARE_STANDARD_LDD()
	{
	return new DThreadPriorityFactory;
	}
