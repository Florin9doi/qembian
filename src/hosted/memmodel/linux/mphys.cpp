// Hosted EKA2: "physical" memory for drivers written against a board. A phone's drivers map physical
// ranges (DPlatChunkHw: device windows, flash, DMA buffers); the hosted board declares windows that back
// physical ranges with host memory: anonymous memory, or a file mapped read-only (the composed ROFS image
// as the flash window at 0x60000000, as the ARM board loads it). Original work for qembian.

#include "memmodel.h"
#include "lxwin32.h"

struct SPhysWindow { TPhysAddr iPhys; TUint32 iSize; TLinAddr iLin; };
static SPhysWindow Windows[16];
static TInt WindowCount;

TInt HostedAddPhysicalWindow(TPhysAddr aPhys, TUint32 aSize, TLinAddr aLin)
	{
	if (WindowCount == 16)
		return KErrOverflow;
	Windows[WindowCount].iPhys = aPhys;
	Windows[WindowCount].iSize = aSize;
	Windows[WindowCount].iLin = aLin;
	++WindowCount;
	return KErrNone;
	}

// a file as a read-only physical window (its length, rounded up to pages)
TInt HostedMapPhysicalFile(TPhysAddr aPhys, const char* aPath)
	{
	TInt fd = hsys(SYS_openat, -100, aPath, 0, 0);
	if (fd < 0)
		return fd;
	TUint32 size = hsys(19 /* lseek */, fd, 0, 2 /* SEEK_END */);
	if ((TInt)size <= 0)
		{
		hsys(SYS_close, fd);
		return KErrCorrupt;
		}
	TAny* m = hmmap(0, (size + 0xfff) & ~0xfffu, PROT_READ, MAP_PRIVATE, fd, 0);
	hsys(SYS_close, fd);
	if (HMAP_FAILED(m))
		return KErrNoMemory;
	return HostedAddPhysicalWindow(aPhys, (size + 0xfff) & ~0xfffu, (TLinAddr)m);
	}

TLinAddr HostedPhysicalToLinear(TPhysAddr aPhys, TUint32 aSize)
	{
	for (TInt i = 0; i < WindowCount; ++i)
		{
		const SPhysWindow& w = Windows[i];
		if (aPhys >= w.iPhys && aPhys - w.iPhys < w.iSize && aSize <= w.iSize - (aPhys - w.iPhys))
			return w.iLin + (aPhys - w.iPhys);
		}
	return 0;
	}

EXPORT_C TInt DPlatChunkHw::New(DPlatChunkHw*& aChunk, TPhysAddr aAddr, TInt aSize, TUint aAttribs)
	{
	return DoNew(aChunk, aAddr, aSize, aAttribs);
	}

TInt DPlatChunkHw::DoNew(DPlatChunkHw*& aChunk, TPhysAddr aAddr, TInt aSize, TUint aAttribs)
	{
	aChunk = NULL;
	TLinAddr lin = HostedPhysicalToLinear(aAddr, aSize);
	if (!lin)
		{
		Kern::Printf("HOSTED: DPlatChunkHw %08x+%x: no board window there", aAddr, aSize);
		return KErrArgument;
		}
	// A user mapping of a device page that has a model would let stores bypass the model (the hosted I/O area
	// is plain memory under it): refused, so drivers fall back to kernel register access (d_display's GL
	// kick: ctl 0x7f12 instead of the user-written KICK). Regression: kn_board.
	extern TBool HostedIoModelled(TLinAddr aLin, TUint32 aSize);
	if ((aAttribs & 0x444) && HostedIoModelled(lin, aSize))
		{
		Kern::Printf("HOSTED: DPlatChunkHw %08x+%x: a modelled device page is not mapped for user mode", aAddr, aSize);
		return KErrNotSupported;
		}
	DPlatChunkHw* c = new DPlatChunkHw;
	if (!c)
		return KErrNoMemory;
	c->iPhysAddr = aAddr;
	c->iLinAddr = lin;
	c->iSize = aSize;
	c->iAttribs = aAttribs;
	aChunk = c;
	return KErrNone;
	}

// the reverse of the board windows (KPhysAddrInvalid outside them)
EXPORT_C TPhysAddr Epoc::LinearToPhysical(TLinAddr aLinAddr)
	{
	for (TInt i = 0; i < WindowCount; ++i)
		{
		const SPhysWindow& w = Windows[i];
		if (aLinAddr - w.iLin < w.iSize)
			return w.iPhys + (aLinAddr - w.iLin);
		}
	return KPhysAddrInvalid;
	}
