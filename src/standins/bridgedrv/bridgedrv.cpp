// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// BRIDGEDRV.LDD stand-in for the Sony Ericsson P1i (UIQ 3): the packet link to the CCPU (the Ericsson
// modem CPU) under caifmux.csy (docs/devices/se-p1i/ccpu-link.md). A forwarder: the CCPU itself (CAIF link
// layer, BridgeServer/OPA protocol, its state) is the host's fake CCPU in the modem core
// (src/qemu-syborg/hw/arm/syborg_ccpu.c, port 0 of the syborg-modem device); this driver hands it every
// packet caifmux writes (TX kind 6, channel = unit) and gives caifmux what comes back (RX kind 4).
//
// Client side [V caifmux.csy, R6E30]: User::LoadLogicalDevice("BRIDGEDRV.LDD"); DoCreate("BridgeDRV",
// TVersion(1,0,0x431), unit 1 or 2 (Z:\private\101F7989\caif.ini DECMCCPU_PORT/VEICCPU_PORT), no info).
//   DoRequest 0  read:  a1 = TDes8* buffer (~16 KB), a2 = TInt* max length: one CAIF packet per completion
//   DoRequest 1  write: a1 = const TDesC8* packet: one CAIF packet
//   DoControl 0  a1 = TDes8* config (16 bytes {4|7, 1, 0, 1} words): KErrNone
//   DoControl 1/2 cancel the pending read / write [I]: KErrCancel
//   DoControl 3  idle hint (caifmux CTimer: no traffic since the last tick), result ignored [V]: KErrNone
// No serial framing: the descriptor length is the packet length.
//
// A packet from the CCPU becomes readable KReplyMs after it arrived (H-p1i-ccpu-latency): answers inside
// caifmux's write raced BServer's DSR wait (CDecmLinkMgr 13433 on 2 boots in 7). Every packet is logged
// ("SYBCAIF: uN ccpu <-/->"), the CCPU's notes too ("SYBCAIF: ccpu: ...").
#include <kernel/kernel.h>
#include "syborg.h"

_LIT(KLddName, "BridgeDRV");
const TInt KMaxPkt = 256;			// the CCPU's packets (CCPU_MAX_PKT); a longer write is forwarded cut
const TInt KCcpuUnits = 8;			// CCPU_UNITS
const TInt KQueueLen = 16;
const TInt KLogBytes = 64;			// hex dump length of a logged packet
const TInt KReplyMs = 2;			// CCPU reply latency

// syborg-modem port 0 (syborg_modem.c): registers, TX buffer +0x1000, RX head frame +0x1800
enum { KMTxChannel = 0x04, KMTxLen = 0x08, KMTxGo = 0x0C, KMRxCount = 0x10, KMRxChannel = 0x14, KMRxLen = 0x18,
	KMRxKind = 0x1C, KMRxPop = 0x20, KMIntEnable = 0x24, KMTxBuf = 0x1000, KMRxBuf = 0x1800 };
enum { KMOpen = 3, KMClose = 4, KMCaif = 6 };		// TX kinds
enum { KMLog = 2, KMCaifDeliver = 4 };				// RX kinds
#ifdef __HOSTED__
TUint32 HostedIoRead(TUint32 aAddr);
void HostedIoWrite(TUint32 aAddr, TUint32 aValue);
static inline TUint32 ModemRead(TInt aOff) { return HostedIoRead(KHwBaseModem + aOff); }
static inline void ModemWrite(TInt aOff, TUint32 aValue) { HostedIoWrite(KHwBaseModem + aOff, aValue); }
#else
static inline TUint32 ModemRead(TInt aOff) { return *(volatile TUint32*)(KHwBaseModem + aOff); }
static inline void ModemWrite(TInt aOff, TUint32 aValue) { *(volatile TUint32*)(KHwBaseModem + aOff) = aValue; }
#endif

class DBridgeChannel;

class DBridgeFactory : public DLogicalDevice
	{
public:
	DBridgeFactory() : iRxDfc(RxDfcFn, this, 1)
		{
		iVersion = TVersion(255, 255, 0xffff);		// the client asks 1.0.1073
		iParseMask = KDeviceAllowUnit;
		iUnitsMask = 0xffffffffu;
		}
	virtual TInt Install()
		{
		TInt r = Kern::MutexCreate(iLock, KLddName, KMutexOrdGeneral0);
		if (r != KErrNone)
			return r;
		iRxDfc.SetDfcQ(Kern::DfcQue1());
		r = Interrupt::Bind(EIntModem0, Isr, this);
		if (r != KErrNone)
			return r;
		ModemWrite(KMIntEnable, 1);
		Interrupt::Enable(EIntModem0);
		Kern::Printf("SYBSTUB: install BridgeDRV (forwards to the host's fake CCPU, modem port 0)");
		return SetName(&KLddName);
		}
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
	void Forward(TInt aKind, TInt aUnit, const TUint8* aData, TInt aLen);	// caller holds iLock
	void Drain();															// caller holds iLock
	static void Isr(TAny* aPtr);
	static void RxDfcFn(TAny* aPtr);
public:
	DMutex* iLock;
	DBridgeChannel* iUnits[KCcpuUnits];
	TDfc iRxDfc;
	TUint8 iBuf[KMaxPkt + 4];
	};

struct SPkt { TInt iLen; TUint8 iData[KMaxPkt]; };

class DBridgeChannel : public DLogicalChannelBase
	{
public:
	DBridgeChannel(DBridgeFactory* aF) : iF(aF), iLatency(LatencyDfc, this, Kern::DfcQue0(), 1) {}
	virtual ~DBridgeChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8*, const TVersion& aVer);
	virtual TInt Request(TInt aFn, TAny* a1, TAny* a2);
	void Queue(const TUint8* aData, TInt aLen);		// a packet from the CCPU (caller holds iLock)
private:
	void Write(DThread* aT, TRequestStatus* aS, TAny* aDes);
	void TryDeliver();
	void Log(const char* aWhat, const TUint8* aData, TInt aLen) const;
	void CompleteRead(TInt aReason);
	static void LatencyDfc(TAny* aSelf);
private:
	DBridgeFactory* iF;
	TInt iUnit;
	SPkt iQ[KQueueLen];
	TInt iQHead, iQCount;
	TInt iQReady;					// queued packets whose latency has passed (the oldest ones)
	NTimer iTimer;					// reply latency (KReplyMs), runs iLatency
	TDfc iLatency;
	DThread* iRxThread;				// pending read: its thread, status and TDes8*
	TRequestStatus* iRxStatus;
	TAny* iRxDes;
	TUint8 iIn[KMaxPkt];			// the packet being written
	};

TInt DBridgeFactory::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DBridgeChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

// one frame to the modem port (TX buffer, channel = unit << 8 | unit, kind), then what it answered at once
void DBridgeFactory::Forward(TInt aKind, TInt aUnit, const TUint8* aData, TInt aLen)
	{
	for (TInt i = 0; i < aLen; i += 4)
		{
		TUint32 w = 0;
		for (TInt k = 0; k < 4 && i + k < aLen; ++k)
			w |= (TUint32)aData[i + k] << (8 * k);
		ModemWrite(KMTxBuf + i, w);
		}
	ModemWrite(KMTxChannel, (TUint32)(aUnit << 8 | aUnit));
	ModemWrite(KMTxLen, (TUint32)aLen);
	ModemWrite(KMTxGo, (TUint32)aKind);
	Drain();
	}

void DBridgeFactory::Drain()
	{
	while (ModemRead(KMRxCount))
		{
		TInt kind = ModemRead(KMRxKind), ch = ModemRead(KMRxChannel), len = ModemRead(KMRxLen);
		if (len > KMaxPkt)
			len = KMaxPkt;
		for (TInt i = 0; i < len; i += 4)
			{
			TUint32 w = ModemRead(KMRxBuf + i);
			for (TInt k = 0; k < 4 && i + k < len; ++k)
				iBuf[i + k] = (TUint8)(w >> (8 * k));
			}
		ModemWrite(KMRxPop, 1);
		if (kind == KMLog)
			{
			iBuf[len] = 0;
			Kern::Printf("SYBCAIF: %s", iBuf);
			continue;
			}
		TInt unit = ch & 0xff;
		DBridgeChannel* c = kind == KMCaifDeliver && unit < KCcpuUnits ? iUnits[unit] : NULL;
		if (c)
			c->Queue(iBuf, len);
		else
			Kern::Printf("SYBCAIF: u%d closed, CCPU frame (kind %d) dropped", unit, kind);
		}
	}

void DBridgeFactory::Isr(TAny* aPtr)		// the CCPU sent something on its own
	{
	DBridgeFactory* f = (DBridgeFactory*)aPtr;
	Interrupt::Disable(EIntModem0);
	f->iRxDfc.Add();
	}

void DBridgeFactory::RxDfcFn(TAny* aPtr)
	{
	DBridgeFactory* f = (DBridgeFactory*)aPtr;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*f->iLock);
	f->Drain();
	Kern::MutexSignal(*f->iLock);
	NKern::ThreadLeaveCS();
	Interrupt::Enable(EIntModem0);
	}

TInt DBridgeChannel::DoCreate(TInt aUnit, const TDesC8*, const TVersion& aVer)
	{
	if (aUnit <= 0 || aUnit >= KCcpuUnits)
		return KErrNotSupported;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iF->iLock);
	TInt r = KErrInUse;
	if (!iF->iUnits[aUnit])
		{
		iUnit = aUnit;
		iF->iUnits[aUnit] = this;
		iF->Forward(KMOpen, aUnit, NULL, 0);
		r = KErrNone;
		}
	Kern::MutexSignal(*iF->iLock);
	NKern::ThreadLeaveCS();
	Kern::Printf("SYBSTUB: BridgeDRV open unit %d ver %d.%d.%d by %O: %d", aUnit, aVer.iMajor, aVer.iMinor, aVer.iBuild,
				 &Kern::CurrentThread(), r);
	return r;
	}

DBridgeChannel::~DBridgeChannel()
	{
	iTimer.Cancel();
	iLatency.Cancel();
	if (iUnit > 0 && iF->iUnits[iUnit] == this)
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*iF->iLock);
		iF->iUnits[iUnit] = NULL;
		iF->Forward(KMClose, iUnit, NULL, 0);
		Kern::MutexSignal(*iF->iLock);
		NKern::ThreadLeaveCS();
		}
	}

void DBridgeChannel::Log(const char* aWhat, const TUint8* aData, TInt aLen) const
	{
	TBuf8<KLogBytes * 2 + 4> hex;
	for (TInt i = 0; i < aLen && i < KLogBytes; ++i)
		hex.AppendNumFixedWidth(aData[i], EHex, 2);
	if (aLen > KLogBytes)
		hex.Append(_L8(".."));
	Kern::Printf("SYBCAIF: u%d %s %d: %S", iUnit, aWhat, aLen, &hex);
	}

void DBridgeChannel::CompleteRead(TInt aReason)
	{
	if (iRxStatus)
		Kern::RequestComplete(iRxThread, iRxStatus, aReason);		// sets iRxStatus = NULL
	}

// deliver the oldest ready packet to a pending read (caller holds iLock)
void DBridgeChannel::TryDeliver()
	{
	if (!iRxStatus || !iQReady)
		return;
	SPkt& p = iQ[iQHead];
	TPtrC8 d(p.iData, p.iLen);
	TInt r = Kern::ThreadDesWrite(iRxThread, iRxDes, d, 0, KChunkShiftBy0, &Kern::CurrentThread());
	Log("ccpu ->", p.iData, p.iLen);
	iQHead = (iQHead + 1) % KQueueLen;
	--iQCount;
	--iQReady;
	CompleteRead(r);
	}

void DBridgeChannel::Queue(const TUint8* aData, TInt aLen)
	{
	if (iQCount == KQueueLen || aLen > KMaxPkt)
		{
		Kern::Printf("SYBCAIF: u%d CCPU packet dropped (queue full or too long)", iUnit);
		return;
		}
	SPkt& p = iQ[(iQHead + iQCount) % KQueueLen];
	p.iLen = aLen;
	memcpy(p.iData, aData, aLen);
	++iQCount;
	if (!iTimer.IsPending())
		iTimer.OneShot(NKern::TimerTicks(KReplyMs), iLatency);
	}

void DBridgeChannel::LatencyDfc(TAny* aSelf)
	{
	DBridgeChannel* c = (DBridgeChannel*)aSelf;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*c->iF->iLock);
	c->iQReady = c->iQCount;
	c->TryDeliver();
	Kern::MutexSignal(*c->iF->iLock);
	NKern::ThreadLeaveCS();
	}

// one packet from caifmux to the CCPU (caller holds iLock)
void DBridgeChannel::Write(DThread* aT, TRequestStatus* aS, TAny* aDes)
	{
	TInt len = Kern::ThreadGetDesLength(aT, aDes);
	TPtr8 in(iIn, KMaxPkt);
	TInt r = len < 0 ? len : Kern::ThreadDesRead(aT, aDes, in, 0, KChunkShiftBy0);
	Kern::RequestComplete(aT, aS, r);
	if (r != KErrNone)
		return;
	Log("ccpu <-", iIn, len);
	iF->Forward(KMCaif, iUnit, iIn, in.Length());
	}

TInt DBridgeChannel::Request(TInt aFn, TAny* a1, TAny* a2)
	{
	DThread* t = &Kern::CurrentThread();
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iF->iLock);
	TInt r = KErrNone;
	if (aFn == (TInt)KMaxTInt)				// DoCancel(mask)
		{
		if ((TInt)a1 & 1)
			CompleteRead(KErrCancel);
		}
	else if (aFn < 0)							// DoRequest(n, status, a1, a2): a2 -> { a1, a2 }
		{
		TRequestStatus* s = (TRequestStatus*)a1;
		TUint32 args[2] = {0, 0};
		if (a2)
			Kern::ThreadRawRead(t, a2, args, sizeof(args));
		switch (~aFn)
			{
		case 0:
			if (iRxStatus)
				Kern::RequestComplete(t, s, KErrInUse);
			else
				{
				iRxThread = t;
				iRxStatus = s;
				iRxDes = (TAny*)args[0];
				TryDeliver();
				}
			break;
		case 1:
			Write(t, s, (TAny*)args[0]);
			break;
		default:
			Kern::Printf("SYBCAIF: u%d request %d not supported", iUnit, ~aFn);
			Kern::RequestComplete(t, s, KErrNotSupported);
			}
		}
	else
		{
		switch (aFn)
			{
		case 0:
			break;							// configuration: accepted
		case 1:
			CompleteRead(KErrCancel);
			break;
		case 2:
			break;							// writes complete at once: nothing pending
		case 3:
			break;							// idle hint from caifmux's timer (no reads or writes since the last tick) [V]
		default:
			Kern::Printf("SYBCAIF: u%d control %d not supported", iUnit, aFn);
			r = KErrNotSupported;
			}
		}
	Kern::MutexSignal(*iF->iLock);
	NKern::ThreadLeaveCS();
	return r;
	}

DECLARE_STANDARD_LDD()
	{
	return new DBridgeFactory;
	}
