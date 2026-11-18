// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Table-driven stand-ins for phone logical devices, any vendor (kernel side; first written for the N95's Nokia
// drivers, now also the SE P1i's; vendor-neutral name since 2026-11-17). Vendor-specific behaviour is declared by
// each driver's own file (its table, hooks, iCancelByNumber), never here. Each LDD declares
//   - the factory name its user-side clients open (RBusLogicalChannel::DoCreate)
//   - a table of DoControl() numbers with an action:
//       EWriteInt   *(TInt*)a1 = value (a1 points at a TInt or a TPckgBuf<TInt> payload)
//       EComplete   complete the TRequestStatus* found in the first word at a1 with value
//       EReturn     return value
//     anything not in the table returns KErrNone and touches nothing
// Every channel open and control is logged on the debug port ("SYBSTUB:"), which is the
// specification for proper fakes later.

#include <kernel/kernel.h>

enum TStubAction { EWriteInt, EComplete, EReturn, EWriteBlock, EPendRequest };
// EPendRequest: table entry { KStubRequest|n, EPendRequest } keeps DoRequest(n) pending even when
// the device completes requests by default (e.g. 'wait for next job' notifications);
// { KStubRequest|n, EComplete, value } completes DoRequest(n) with that value
const TInt KStubRequest = 0x10000;
const TInt KStubMaxPending = 32;		// requests 0..31 are kept while pending (for DoCancel)
// EWriteBlock: copy iSize bytes from iData to a1 (e.g. a capabilities structure)
struct SStubCtl { TInt iFn; TStubAction iAction; TInt iValue; const TAny* iData; TInt iSize; };
// optional per-device request hook: return KErrNone..KErrXxx to complete the request with it, or
// KStubNotHandled to fall back to the table/default. aArgs = the two DoRequest argument words;
// aMajor = the major version of the interface the client opened the channel with (one driver name
// can carry two interfaces: Custom is 1.0 on the N73, 2.0 on the N95, as the real driver negotiates)
const TInt KStubNotHandled = 0x7fff0000;
typedef TInt (*TStubRequestHook)(DThread* aClient, TInt aReq, const TUint32* aArgs, TInt aMajor);
// optional per-device control hook: called before the table with the first 4 words at a1 (if
// readable); return a value to return it from DoControl, or KStubNotHandled to fall through.
typedef TInt (*TStubControlHook)(DThread* aClient, TInt aFn, TAny* a1, TAny* a2, const TUint32* aWords);

class DTableStubFactory : public DLogicalDevice
	{
public:
	DTableStubFactory(const TDesC& aName, const SStubCtl* aCtl, TInt aCount, TBool aChannels, TBool aCompleteRequests,
					  TStubRequestHook aHook = NULL, TStubControlHook aCtlHook = NULL)
		: iName(aName), iCtl(aCtl), iCount(aCount), iChannels(aChannels), iCompleteRequests(aCompleteRequests), iHook(aHook),
		  iCtlHook(aCtlHook), iCancelByNumber(EFalse)
		{
		iVersion = TVersion(255, 255, 0xffff);	// accept any client version
		iParseMask = KDeviceAllowUnit | KDeviceAllowInfo;
		iUnitsMask = 0xffffffffu;
		}
	virtual TInt Install()
		{
		Kern::Printf("SYBSTUB: install %S%s", &iName, iChannels ? "" : " (load only)");
		return SetName(&iName);
		}
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
public:
	const TDesC& iName;
	const SStubCtl* iCtl;
	TInt iCount;
	TBool iChannels;
	TBool iCompleteRequests;	// DoRequest(): complete at once with KErrNone, else leave pending
	TStubRequestHook iHook;
	TStubControlHook iCtlHook;
	// DoCancel(a): pending requests are kept and completed with KErrCancel. How a names them is the client's
	// driver protocol, declared per device: a bit mask (1 << n, the default) or the request number itself
	// (SE P1i MPEG4DECDRV: the start-up animation player calls DoCancel(0..4); bug 2026-11-17: nothing was
	// completed, its CActive::Cancel waited forever and APS's start-up waited for the player's exit)
	TBool iCancelByNumber;
	};

class DTableStubChannel : public DLogicalChannelBase
	{
public:
	DTableStubChannel(DTableStubFactory* aF) : iF(aF) {}
	virtual TInt DoCreate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		iUnit = aUnit;
		iMajor = aVer.iMajor;
		Kern::Printf("SYBSTUB: %S open unit %d ver %d.%d.%d by %O", &iF->iName, aUnit,
					 aVer.iMajor, aVer.iMinor, aVer.iBuild, &Kern::CurrentThread());
		return KErrNone;
		}
	virtual TInt Request(TInt aFn, TAny* a1, TAny* a2)
		{
		if (aFn < 0 && aFn != (TInt)KMaxTInt)
			{
			// DoRequest(n, status, args): aFn = ~n, a1 = TRequestStatus*, a2 -> { arg1, arg2 }
			TRequestStatus* s = (TRequestStatus*)a1;
			TUint32 args[2] = {0, 0};
			if (a2)
				Kern::ThreadRawRead(&Kern::CurrentThread(), a2, args, sizeof(args));
			TBool pend = !iF->iCompleteRequests;
			TInt result = KErrNone;
			TInt hr = iF->iHook ? iF->iHook(&Kern::CurrentThread(), ~aFn, args, iMajor) : KStubNotHandled;
			for (TInt i=0; i<iF->iCount; ++i)
				if (iF->iCtl[i].iFn == (KStubRequest | ~aFn))
					{
					if (iF->iCtl[i].iAction == EPendRequest)
						pend = ETrue;
					else if (iF->iCtl[i].iAction == EComplete)
						{ pend = EFalse; result = iF->iCtl[i].iValue; }
					}
			if (hr != KStubNotHandled)
				{ pend = EFalse; result = hr; }
			Kern::Printf("SYBSTUB: %S u%d request 0x%x args %08x %08x %s %d by %O", &iF->iName, iUnit, ~aFn,
						 args[0], args[1], pend ? "pending" : "completed", result, &Kern::CurrentThread());
			if (!pend && s)
				Kern::RequestComplete(s, result);
			else if (pend && s && ~aFn >= 0 && ~aFn < KStubMaxPending)
				{
				if (iPending[~aFn])			// re-issued while pending: the old one is lost, as before
					Kern::Printf("SYBSTUB: %S u%d request 0x%x issued again while pending", &iF->iName, iUnit, ~aFn);
				iPending[~aFn] = s;
				}
			return KErrNone;
			}
		if (aFn == (TInt)KMaxTInt)
			{
			TUint v = (TUint)a1;
			TInt n = 0;
			for (TInt i = 0; i < KStubMaxPending; ++i)
				if (iPending[i] && (iF->iCancelByNumber ? (TUint)i == v : (v >> i) & 1))
					{
					Kern::RequestComplete(iPending[i], KErrCancel);	// nulls iPending[i]
					++n;
					}
			Kern::Printf("SYBSTUB: %S u%d cancel %s %08x: %d completed", &iF->iName, iUnit,
						 iF->iCancelByNumber ? "request" : "mask", v, n);
			return KErrNone;
			}
		TUint32 w[4] = {0, 0, 0, 0};
		TInt rr = KErrArgument;
		if (((TLinAddr)a1 & 3) == 0 && (TLinAddr)a1 >= 0x1000)
			rr = Kern::ThreadRawRead(&Kern::CurrentThread(), a1, w, sizeof(w));
		if (iF->iCtlHook)
			{
			TInt hr = iF->iCtlHook(&Kern::CurrentThread(), aFn, a1, a2, w);
			if (hr != KStubNotHandled)
				{
				// our private controls (0x7f00..0x7fff: host GL transport, vsync) run every frame:
				// logging them costs emulated serial output per call; errors are still logged
				if (aFn < 0x7f00 || aFn > 0x7fff || hr < 0)
					Kern::Printf("SYBSTUB: %S u%d ctl %d a1=%08x a2=%08x [%08x %08x %08x %08x] hook %d",
								 &iF->iName, iUnit, aFn, a1, a2, w[0], w[1], w[2], w[3], hr);
				return hr;
				}
			}
		const SStubCtl* c = NULL;
		for (TInt i=0; i<iF->iCount; ++i)
			if (iF->iCtl[i].iFn == aFn)
				c = &iF->iCtl[i];
		TInt r = KErrNone;
		const char* what = "ignored";
		if (c && c->iAction == EWriteInt && rr == KErrNone)
			{
			Kern::ThreadRawWrite(&Kern::CurrentThread(), a1, &c->iValue, sizeof(TInt));
			what = "wrote int";
			}
		else if (c && c->iAction == EComplete && rr == KErrNone && w[0])
			{
			TRequestStatus* s = (TRequestStatus*)w[0];
			Kern::RequestComplete(s, c->iValue);
			what = "completed";
			}
		else if (c && c->iAction == EWriteBlock && rr == KErrNone)
			{
			Kern::ThreadRawWrite(&Kern::CurrentThread(), a1, c->iData, c->iSize);
			what = "wrote block";
			}
		else if (c && c->iAction == EReturn)
			{
			r = c->iValue;
			what = "returned";
			}
		Kern::Printf("SYBSTUB: %S u%d ctl %d a1=%08x a2=%08x [%08x %08x %08x %08x] %s %d",
					 &iF->iName, iUnit, aFn, a1, a2, w[0], w[1], w[2], w[3], what, c ? c->iValue : 0);
		return r;
		}
private:
	DTableStubFactory* iF;
	TInt iUnit;
	TInt iMajor;
	TRequestStatus* iPending[KStubMaxPending];	// pending requests by number (client thread = the caller)
	};

inline TInt DTableStubFactory::Create(DLogicalChannelBase*& aChannel)
	{
	if (!iChannels)
		{
		aChannel = NULL;
		return KErrNotSupported;
		}
	aChannel = new DTableStubChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

#define TABLE_STUB_LDD2(name, table, channels, completeRequests) \
	_LIT(KStubName, name); \
	DECLARE_STANDARD_LDD() \
		{ return new DTableStubFactory(KStubName, table, sizeof(table)/sizeof(table[0]), channels, completeRequests); }
#define TABLE_STUB_LDD(name, table, channels) TABLE_STUB_LDD2(name, table, channels, EFalse)
#define TABLE_STUB_LDD_HOOK(name, table, hook) \
	_LIT(KStubName, name); \
	DECLARE_STANDARD_LDD() \
		{ return new DTableStubFactory(KStubName, table, sizeof(table)/sizeof(table[0]), ETrue, ETrue, hook); }

#define TABLE_STUB_LDD_CTLHOOK(name, table, completeRequests, ctlhook) \
	_LIT(KStubName, name); \
	DECLARE_STANDARD_LDD() \
		{ return new DTableStubFactory(KStubName, table, sizeof(table)/sizeof(table[0]), ETrue, completeRequests, NULL, ctlhook); }

#define TABLE_STUB_LDD_HOOKS(name, table, completeRequests, reqhook, ctlhook) \
	_LIT(KStubName, name); \
	DECLARE_STANDARD_LDD() \
		{ return new DTableStubFactory(KStubName, table, sizeof(table)/sizeof(table[0]), ETrue, completeRequests, reqhook, ctlhook); }

#define TABLE_STUB_LDD_HOOKS_CANCELNUM(name, table, completeRequests, reqhook, ctlhook) \
	_LIT(KStubName, name); \
	DECLARE_STANDARD_LDD() \
		{ DTableStubFactory* f = new DTableStubFactory(KStubName, table, sizeof(table)/sizeof(table[0]), ETrue, \
													   completeRequests, reqhook, ctlhook); \
		  if (f) f->iCancelByNumber = ETrue; return f; }

#define NO_CONTROLS static const SStubCtl KCtl[] = { { -1, EReturn, 0, 0, 0 } };
