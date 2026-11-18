// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// flip.dll stand-in (Sony Ericsson P990i keypad flip). The phone's flip.dll is a kernel extension that reads the
// flip's GPIO pin (5) through SE's GPIODRIVER and publishes it in two Publish & Subscribe properties [V R6E28
// flip.dll 0xf839aa68..0xf839ac18]:
//   0x100039EA/2  SE's hardware-state category: QUiConfigServer (UIQ's UI configuration: pen style 240x320 when
//                 open, softkey style 240x256 = wsini screen mode 4 when closed), lcdhandler/startuphandler.aps
//   0x102086EF/1  keyboardscan.dll's (the keypad mapping: the flip's keys press the QWERTY keys under it [I])
// value 1 = open, 0 = closed [V 2026-11-17: 0 at boot gives the softkey UI with the bottom 64 rows dark];
// flip.dll starts with 1. Policies copied from flip.dll (read: always pass; write: its second policy).
// Here the pin is QEMU's syborg-sensor switch bit (SWITCHES register, bit `flip.bit`, default 0; set = closed),
// polled every 100 ms (flip.dll debounces with an NTimer too); the emulator window's Flip button sets it.
// H-p990i-flip (docs/hacks.md).
#include <kernel/kern_priv.h>
#include <kernel/sproperty.h>
#include "syborg.h"
#include "boardconfig.h"

enum { KRegId = 0, KRegSwitches = 6, KRegSwitchSeq = 7 };
const TUint32 KSensorId = 0xc51d0030;
const TInt KPollMs = 100;
static const TUint32 KReadPolicy[2] = { 0xffffff01, 0xffffffff };	// flip.dll .rodata 0xf839ada8
static const TUint32 KWritePolicy[2] = { 0xffff0202, 0xffffffff };	// flip.dll .rodata 0xf839adb0
static const TUint KCat[2] = { 0x100039EA, 0x102086EF };
static const TUint KKey[2] = { 2, 1 };

static inline TUint32 Reg(TInt aReg) { return ReadReg(KHwBaseSensor, aReg); }

class DFlip : public DBase
	{
public:
	DFlip() : iTimer(TimerFn, this), iDfc(DfcFn, this, Kern::DfcQue0(), 1) {}
	TInt Start();
private:
	static void TimerFn(TAny* aPtr) { ((DFlip*)aPtr)->iDfc.Add(); }
	static void DfcFn(TAny* aPtr) { ((DFlip*)aPtr)->Poll(); }
	void Poll();
	void Publish();
	RPropertyRef iProp[2];
	NTimer iTimer;
	TDfc iDfc;
	TUint32 iSeq;
	TInt iBit;
	TInt iOpen;
	};

TInt DFlip::Start()
	{
	iBit = BoardConfigInt("flip.bit", 0);
	TSecurityPolicy rd, wr;
	memcpy(&rd, KReadPolicy, sizeof(rd));
	memcpy(&wr, KWritePolicy, sizeof(wr));
	for (TInt i = 0; i < 2; ++i)
		{
		TInt r = iProp[i].Attach(TUid::Uid(KCat[i]), KKey[i]);
		if (r == KErrNone)
			{
			r = iProp[i].Define(RProperty::EInt, rd, wr);
			if (r == KErrAlreadyExists)
				r = KErrNone;
			}
		if (r != KErrNone)
			{
			Kern::Printf("SYBSTUB: flip %08x/%d define %d", KCat[i], KKey[i], r);
			return r;
			}
		}
	TBool present = Reg(KRegId) == KSensorId;
	iSeq = present ? Reg(KRegSwitchSeq) : 0;
	iOpen = present ? !(Reg(KRegSwitches) >> iBit & 1) : 1;
	Publish();
	Kern::Printf("SYBSTUB: flip %s at boot (switch bit %d%s)", iOpen ? "open" : "closed", iBit, present ? "" : ", no sensor device");
	if (present)
		iTimer.OneShot(NKern::TimerTicks(KPollMs));
	return KErrNone;
	}

void DFlip::Publish()
	{
	for (TInt i = 0; i < 2; ++i)
		iProp[i].Set(iOpen);
	}

void DFlip::Poll()
	{
	TUint32 seq = Reg(KRegSwitchSeq);
	if (seq != iSeq)
		{
		iSeq = seq;
		TInt open = !(Reg(KRegSwitches) >> iBit & 1);
		if (open != iOpen)
			{
			iOpen = open;
			Publish();
			Kern::Printf("SYBSTUB: flip %s", open ? "opened" : "closed");
			}
		}
	iTimer.OneShot(NKern::TimerTicks(KPollMs));
	}

DECLARE_STANDARD_EXTENSION()
	{
	DFlip* f = new DFlip;
	return f ? f->Start() : KErrNoMemory;
	}
