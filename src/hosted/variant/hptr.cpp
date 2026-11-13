// Hosted EKA2: the syborg pointer as a touch screen (QEMU: src/qemu-syborg/hw/arm/syborg_input.c, v2 typed
// events) for the touch stand-in exyin (src/standins/exyin): ID, LATCH (pops the FIFO into the registers),
// FIFO_COUNT, X, Y, Z, BUTTONS, INT_ENABLE, FIFO_SIZE, VERSION 2, CAPS, SLOT, TS lo/hi (us), FLAGS
// (DOWN/UP/MOVE), STATS. Interrupt EIntPointer while the FIFO holds entries and the interrupt is enabled.
// Samples come from the phone window's pointer ring (hdisplay.cpp, every 10 ms: the 808's 100 Hz panel rate):
// HostedPtrPush, one entry per sample, DOWN/UP from the left-button edge. Coordinates are absolute 0..32767
// (QEMU's input range), as exyin expects. Original work for qembian.
#include <kernel/kern_priv.h>

extern TInt HostedIoAttach(TUint32 aBase, TUint32 aSize, TUint32 (*aRead)(TAny*, TUint32),
                           void (*aWrite)(TAny*, TUint32, TUint32), TAny* aCtx);
extern void HostedRaiseIrq(TInt aIrq);
extern void HostedLowerIrq(TInt aIrq);

static const TUint32 KPtrIo = 0xC6000000u + 0x04 * 0x1000;		// KHwBaseKmiPointer
static const TInt KPtrIrq = 3;										// EIntPointer
static const TUint32 KPtrId = 0xc51d0006u;							// SYBORG_ID_TOUCHSCREEN
enum { KDown = 1, KUp = 2, KMove = 4, KCoalesced = 8 };
static const TInt KFifo = 256;

struct SPtrEvent { TUint32 iX, iY, iButtons, iFlags; TUint64 iTs; };
static SPtrEvent Fifo[KFifo];
static SPtrEvent Cur;
static TInt Head, Count;
static TUint32 IntEnable, Dropped, Coalesced, SyncedButtons;
static volatile TInt Lock;
static TBool Attached;

static void Acquire() { while (__atomic_exchange_n(&Lock, 1, __ATOMIC_ACQUIRE)) hsys(SYS_sched_yield); }
static void Release() { __atomic_store_n(&Lock, 0, __ATOMIC_RELEASE); }
// as hkeyb.cpp: a nanothread holds the lock with interrupts disabled (exyin's ISR reads these registers)
static TInt Enter()
	{
	TInt irq = -1;
	if (LxSelf() && LxSelf()->iNThread && NKern::CurrentContext() != NKern::EInterrupt)
		irq = NKern::DisableAllInterrupts();
	Acquire();
	return irq;
	}
static void Leave(TInt aIrq)
	{
	Release();
	if (aIrq >= 0)
		NKern::RestoreInterrupts(aIrq);
	}

static void UpdateIrq()
	{
	if (Count && IntEnable)
		HostedRaiseIrq(KPtrIrq);
	else
		HostedLowerIrq(KPtrIrq);
	}

static TUint64 NowUs()
	{
	struct { TInt32 s, ns; } t;
	hsys(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, &t);
	return (TUint64)t.s * 1000000 + t.ns / 1000;
	}

static TUint32 PtrRead(TAny*, TUint32 aOff)
	{
	TInt irq = Enter();
	TUint32 v = 0;
	switch (aOff >> 2)
		{
	case 0: v = KPtrId; break;
	case 2: v = Count; break;
	case 3: v = Cur.iX; break;
	case 4: v = Cur.iY; break;
	case 6: v = Cur.iButtons; break;
	case 7: v = IntEnable; break;
	case 8: v = KFifo; break;
	case 9: v = 2; break;
	case 10: v = 3 | 1 << 8; break;				// timestamps, typed events; one slot
	case 12: v = (TUint32)Cur.iTs; break;
	case 13: v = (TUint32)(Cur.iTs >> 32); break;
	case 14: v = Cur.iFlags; break;
	case 15: v = (Dropped & 0xffff) | Coalesced << 16; break;
		}
	Leave(irq);
	return v;
	}

static void PtrWrite(TAny*, TUint32 aOff, TUint32 aValue)
	{
	TInt irq = Enter();
	switch (aOff >> 2)
		{
	case 1:										// latch the oldest entry
		if (Count)
			{
			Cur = Fifo[Head];
			Head = (Head + 1) % KFifo;
			--Count;
			}
		UpdateIrq();
		break;
	case 7:
		IntEnable = aValue & 1;
		UpdateIrq();
		break;
		}
	Leave(irq);
	}

// one sample from the host (any thread): absolute 0..32767, left button in bit 0. A press or a release is
// never dropped; with the FIFO nearly full a move replaces the newest queued move (as QEMU).
void HostedPtrPush(TUint32 aX, TUint32 aY, TUint32 aButtons)
	{
	if (!Attached)
		return;
	TInt irq = Enter();
	TUint32 was = SyncedButtons & 1, now = aButtons & 1;
	SPtrEvent e = { aX & 0x7fff, aY & 0x7fff, aButtons, now && !was ? (TUint32)KDown : !now && was ? (TUint32)KUp : (TUint32)KMove, NowUs() };
	SPtrEvent* last = Count ? &Fifo[(Head + Count - 1) % KFifo] : NULL;
	if ((e.iFlags & KMove) && last && (last->iFlags & KMove) && Count >= KFifo - 8)
		{
		*last = e;
		last->iFlags |= KCoalesced;
		++Coalesced;
		}
	else if (Count < KFifo)
		{
		Fifo[(Head + Count) % KFifo] = e;
		++Count;
		}
	else if (last)
		{
		*last = e;
		++Dropped;
		}
	SyncedButtons = aButtons;
	UpdateIrq();
	Leave(irq);
	}

TInt HostedPtrAttach()
	{
	Attached = ETrue;
	return HostedIoAttach(KPtrIo, 0x1000, &PtrRead, &PtrWrite, NULL);
	}
