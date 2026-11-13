// Hosted EKA2: process-local chunks (heaps and other non-global user chunks) in a local window, as the multiple
// memory model's local section: every process may use the whole window [KLocalWindowLo, KLocalWindowHi), so
// local chunks of different processes may have the same addresses, and each chunk keeps its full maximum.
// Design: docs/hosted-eka2/chunk-address-space.md. Original work for qembian.
//
// All Symbian processes are one Linux process here, so only one set of pages can be mapped at an address.
//  - Reservation is logical: a process's allocator (64 KB units). A new chunk gets addresses no other process
//    uses while the window has room for that, else addresses another process uses as well.
//  - A process's committed pages live in its sparse memory file, at offset (address - KLocalWindowLo).
//    A committed run is mapped at its address and stays mapped until another process's chunk needs that address
//    (it is then "displaced": DWin32Chunk::iLocalMapped false). A process switch maps the incoming process's
//    displaced chunks again, so most switches cost nothing (addresses rarely collide).
//  - The kernel reaches the local memory of a process other than the current thread's with pread/pwrite on that
//    process's file (LocalCopy), never through the shared addresses.
//  - Uncommitted pages fault while nobody else maps the address. Deviation [G]: a page of a chunk that is not
//    committed but is committed in another process's chunk at the same address is that process's page.
// The page table below (one owner per page of the window) and the chunk lists change with the kernel locked, as
// the process switch (MM::DoProcessSwitch) reads them with the kernel locked.

#include "memmodel.h"

static const TInt KUnitShift = 16;
static const TInt KUnits = (KLocalWindowHi - KLocalWindowLo) >> KUnitShift;
static const TInt KPages = (KLocalWindowHi - KLocalWindowLo) >> 12;
static TUint16 UnitUse[KUnits];				// how many processes reserved each unit
static DWin32Chunk* PageOwner[KPages];		// whose committed page is mapped there (NULL: PROT_NONE)
static TUint16 UnitMapped[KUnits];			// mapped pages per unit (placement of chunks on shared addresses)
static SLocalSpace* LiveSpace;				// the process last switched in
static TInt SharedUnits;					// units more than one process reserved: displacement possible
static TBool WindowReady;
TUint32 HostedLocalMaps, HostedLocalDisplaced, HostedLocalCopies, HostedLocalSharedAddr;	// tests

// debugging: htest --env LOCALTRACE=1 prints chunk creation, commits and displacements
static TInt Tracing()
	{
	static TInt t = -1;
	if (t < 0)
		t = hgetenv("LOCALTRACE") ? 1 : 0;
	return t;
	}

static TInt Fallocate(TInt aFd, TInt aMode, TUint32 aOff, TUint32 aLen)
	{
	return hsys(352 /* fallocate */, aFd, aMode, aOff, 0, aLen, 0);
	}

static TBool WindowInit()
	{
	if (WindowReady)
		return ETrue;
	TAny* m = hmmap((TAny*)KLocalWindowLo, KLocalWindowHi - KLocalWindowLo, PROT_NONE,
	                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if ((TLinAddr)m != KLocalWindowLo)
		{
		Kern::Printf("HOSTED: the local chunk window %08x-%08x is taken (%08x)", KLocalWindowLo, KLocalWindowHi, m);
		return EFalse;
		}
	WindowReady = ETrue;
	return ETrue;
	}

// tests (kn_efile): the next local chunk at this unit (-1: normal placement); the first run of units no process uses
TInt HostedLocalForceUnit = -1;
TInt HostedLocalFreeUnit(TInt aUnits)
	{
	for (TInt u = 0, run = 0; u < KUnits; ++u)
		{
		run = UnitUse[u] ? 0 : run + 1;
		if (run == aUnits)
			return u - aUnits + 1;
		}
	return -1;
	}

TBool LocalChunkWanted(DWin32Chunk* aChunk)
	{
	return aChunk->iOwningProcess && aChunk->iChunkType == EUserData;
	}

// a new space (kernel heap: not under the RAM allocator mutex, which ranks below it)
static SLocalSpace* NewSpace(DWin32Process* aProcess)
	{
	SLocalSpace* s = new SLocalSpace;
	if (!s)
		return NULL;
	s->iUnits = TBitMapAllocator::New(KUnits, ETrue);
	s->iFd = hmemfd("local");
	if (!s->iUnits || s->iFd < 0 || hftruncate(s->iFd, KLocalWindowHi - KLocalWindowLo) < 0)
		{
		if (s->iFd >= 0)
			hsys(SYS_close, s->iFd);
		delete s->iUnits;
		delete s;
		return NULL;
		}
	s->iRefs = 1;
	s->iProcess = aProcess;
	return s;
	}

static void DeleteSpace(SLocalSpace* aSpace)
	{
	hsys(SYS_close, aSpace->iFd);
	delete aSpace->iUnits;
	delete aSpace;
	}

static void SpaceClose(SLocalSpace* aSpace)
	{
	NKern::Lock();
	TBool last = --aSpace->iRefs == 0;
	if (last && LiveSpace == aSpace)
		LiveSpace = NULL;
	NKern::Unlock();
	if (last)
		DeleteSpace(aSpace);
	}

// aUnits free in the process's allocator, unused by every other process: the first such run
static TInt FindUnits(SLocalSpace* aSpace, TInt aUnits)
	{
	for (TInt u = 0, run = 0; u < KUnits; ++u)
		{
		TBool ok = !aSpace->iUnits->NotFree(u, 1) && !UnitUse[u];
		run = ok ? run + 1 : 0;
		if (run == aUnits)
			return u - aUnits + 1;
		}
	return -1;
	}

// the window is full: aUnits free in the process's allocator, where other processes have the fewest pages mapped
// (the new chunk's pages rarely displace theirs: 808 at 15 s, 5357 displacements with first fit)
static TInt FindSharedUnits(SLocalSpace* aSpace, TInt aUnits)
	{
	TInt best = -1;
	TUint32 bestCost = 0xffffffffu, cost = 0;
	for (TInt u = 0, own = 0; u < KUnits; ++u)
		{
		cost += UnitMapped[u];
		own += aSpace->iUnits->NotFree(u, 1) ? 1 : 0;
		if (u >= aUnits)
			{
			cost -= UnitMapped[u - aUnits];
			own -= aSpace->iUnits->NotFree(u - aUnits, 1) ? 1 : 0;
			}
		if (u >= aUnits - 1 && !own && cost < bestCost)
			{
			best = u - aUnits + 1;
			bestCost = cost;
			if (!cost)
				break;
			}
		}
	return best;
	}

TInt LocalChunkCreate(DWin32Chunk* aChunk)
	{
	if (!WindowInit())
		return KErrNoMemory;
	TInt units = (aChunk->iMaxSize + (1 << KUnitShift) - 1) >> KUnitShift;
	TBitMapAllocator* pages = TBitMapAllocator::New(aChunk->iMaxSize >> 12, ETrue);
	if (!pages)
		return KErrNoMemory;
	DWin32Process* proc = (DWin32Process*)aChunk->iOwningProcess;
	SLocalSpace* spare = proc->iLocalSpace ? NULL : NewSpace(proc);
	MM::Wait();
	if (!proc->iLocalSpace && spare)
		{
		proc->iLocalSpace = spare;
		spare = NULL;
		}
	SLocalSpace* s = proc->iLocalSpace;
	TInt u = s ? (HostedLocalForceUnit >= 0 ? HostedLocalForceUnit : FindUnits(s, units)) : -1;
	if (s && u < 0)
		{
		u = FindSharedUnits(s, units);		// addresses another process uses as well
		if (u >= 0)
			++HostedLocalSharedAddr;
		}
	if (u < 0)
		{
		MM::Signal();
		delete pages;
		if (spare)
			DeleteSpace(spare);
		Kern::Printf("HOSTED: local chunk %O of max %x bytes: its process has no room for it", aChunk, aChunk->iMaxSize);
		return KErrNoMemory;
		}
	s->iUnits->Alloc(u, units);
	for (TInt i = u; i < u + units; ++i)
		if (++UnitUse[i] == 2)
			++SharedUnits;
	++s->iRefs;
	aChunk->iLocalSpace = s;
	aChunk->iLocalPages = pages;
	aChunk->iLocalMapped = ETrue;
	aChunk->iBase = (TUint8*)(KLocalWindowLo + (u << KUnitShift));
	if (Tracing())
		Kern::Printf("LOCAL: create %O at %08x max %x (%s)", aChunk, aChunk->iBase, aChunk->iMaxSize,
		             UnitUse[u] > 1 ? "shared addresses" : "own addresses");
	NKern::Lock();
	aChunk->iLocalNext = s->iChunks;
	s->iChunks = aChunk;
	NKern::Unlock();
	MM::Signal();
	if (spare)
		DeleteSpace(spare);			// another thread of the process made one meanwhile
	return KErrNone;
	}

// map pages [aFirst, aFirst + aCount) of the window (committed pages of aChunk) at their addresses; kernel locked
static void MapRun(DWin32Chunk* aChunk, TInt aFirst, TInt aCount)
	{
	TLinAddr a = KLocalWindowLo + (aFirst << 12);
	TAny* m = hmmap((TAny*)a, aCount << 12, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, aChunk->iLocalSpace->iFd,
	                a - KLocalWindowLo);
	__NK_ASSERT_ALWAYS((TLinAddr)m == a);
	++HostedLocalMaps;
	for (TInt p = aFirst; p < aFirst + aCount; ++p)
		{
		DWin32Chunk* old = PageOwner[p];
		if (old && old != aChunk)
			{
			old->iLocalMapped = EFalse;
			++HostedLocalDisplaced;
			}
		if (!old)
			++UnitMapped[p >> (KUnitShift - 12)];
		PageOwner[p] = aChunk;
		}
	}

// unmap the pages of [aFirst, aFirst + aCount) that aChunk has mapped; kernel locked
static void UnmapOwned(DWin32Chunk* aChunk, TInt aFirst, TInt aCount)
	{
	for (TInt p = aFirst; p < aFirst + aCount; )
		{
		if (PageOwner[p] != aChunk)
			{
			++p;
			continue;
			}
		TInt q = p;
		while (q < aFirst + aCount && PageOwner[q] == aChunk)
			{
			PageOwner[q] = NULL;
			--UnitMapped[q++ >> (KUnitShift - 12)];
			}
		hmmap((TAny*)(KLocalWindowLo + (p << 12)), (q - p) << 12, PROT_NONE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
		p = q;
		}
	}

// map every committed run of aChunk that is not mapped; kernel locked
static void MapChunk(DWin32Chunk* aChunk)
	{
	TInt first = ((TLinAddr)aChunk->iBase - KLocalWindowLo) >> 12;
	TInt n = aChunk->iMaxSize >> 12;
	for (TInt i = 0; i < n; )
		{
		if (aChunk->iLocalPages->NotAllocated(i, 1) || PageOwner[first + i] == aChunk)
			{
			++i;
			continue;
			}
		TInt j = i;
		while (j < n && !aChunk->iLocalPages->NotAllocated(j, 1) && PageOwner[first + j] != aChunk)
			++j;
		MapRun(aChunk, first + i, j - i);
		i = j;
		}
	aChunk->iLocalMapped = ETrue;
	}

TInt LocalCommit(DWin32Chunk* aChunk, TInt aOffset, TInt aSize)
	{
	__ASSERT_MUTEX(MM::RamAllocatorMutex);
	if (aSize <= 0)
		return KErrNone;
	if (MM::FreeMemory + MM::CacheMemory < aSize)
		{
		MM::AllocFailed = ETrue;
		return KErrNoMemory;
		}
	TInt reclaimed = aSize - MM::FreeMemory;
	if (reclaimed <= 0)
		MM::FreeMemory -= aSize;
	else
		{
		MM::FreeMemory = 0;
		MM::CacheMemory -= reclaimed;
		MM::ReclaimedCacheMemory += reclaimed;
		}
	MM::CheckMemoryCounters();
	SLocalSpace* s = aChunk->iLocalSpace;
	TInt first = ((TLinAddr)aChunk->iBase + aOffset - KLocalWindowLo) >> 12, n = aSize >> 12;
	TLinAddr a = (TLinAddr)aChunk->iBase + aOffset;
	NKern::Lock();
	aChunk->iLocalPages->Alloc(aOffset >> 12, n);
	// mapped now if its process is the live one (it may displace others), or nobody else maps these addresses
	if (TheCurrentThread->iOwningProcess == s->iProcess)
		LiveSpace = s;						// its thread runs (the space may be newer than its last switch-in)
	TBool map = LiveSpace == s;
	if (!map)
		{
		map = ETrue;
		for (TInt p = first; p < first + n; ++p)
			if (PageOwner[p] && PageOwner[p] != aChunk)
				map = EFalse;
		}
	if (map)
		{
		MapRun(aChunk, first, n);
		if (aChunk->iClearByte)
			memset((TAny*)a, aChunk->iClearByte, aSize);
		}
	else
		aChunk->iLocalMapped = EFalse;
	NKern::Unlock();
	if (Tracing())
		Kern::Printf("LOCAL: commit %O %08x+%x %s (live %d)", aChunk, a, aSize, map ? "mapped" : "not mapped", LiveSpace == s);
	if (!map && aChunk->iClearByte)			// the file's new pages read as zeros
		{
		TUint8 fill[256];
		memset(fill, aChunk->iClearByte, sizeof(fill));
		for (TInt done = 0; done < aSize; done += sizeof(fill))
			hsys(181 /* pwrite64 */, s->iFd, fill, sizeof(fill), 0, a - KLocalWindowLo + done, 0);
		}
	return KErrNone;
	}

TInt LocalDecommit(DWin32Chunk* aChunk, TInt aOffset, TInt aSize)
	{
	__ASSERT_MUTEX(MM::RamAllocatorMutex);
	TInt first = ((TLinAddr)aChunk->iBase + aOffset - KLocalWindowLo) >> 12, n = aSize >> 12, freed = 0;
	NKern::Lock();
	for (TInt i = aOffset >> 12; i < (aOffset >> 12) + n; ++i)
		if (!aChunk->iLocalPages->NotAllocated(i, 1))
			{
			aChunk->iLocalPages->Free(i);
			freed += 0x1000;
			}
	UnmapOwned(aChunk, first, n);
	NKern::Unlock();
	// the memory goes back, and a later commit reads as zeros
	Fallocate(aChunk->iLocalSpace->iFd, 3 /* FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE */,
	          (TLinAddr)aChunk->iBase + aOffset - KLocalWindowLo, aSize);
	MM::FreeMemory += freed;
	return freed;
	}

void LocalChunkDestroy(DWin32Chunk* aChunk)
	{
	SLocalSpace* s = aChunk->iLocalSpace;
	if (!s)
		return;
	MM::Wait();
	LocalDecommit(aChunk, 0, aChunk->iMaxSize);
	NKern::Lock();
	for (DWin32Chunk** pp = &s->iChunks; *pp; pp = &(*pp)->iLocalNext)
		if (*pp == aChunk)
			{
			*pp = aChunk->iLocalNext;
			break;
			}
	NKern::Unlock();
	TInt units = (aChunk->iMaxSize + (1 << KUnitShift) - 1) >> KUnitShift;
	TInt u = ((TLinAddr)aChunk->iBase - KLocalWindowLo) >> KUnitShift;
	s->iUnits->Free(u, units);
	for (TInt i = u; i < u + units; ++i)
		if (--UnitUse[i] == 1)
			--SharedUnits;
	MM::Signal();
	delete aChunk->iLocalPages;
	aChunk->iLocalPages = NULL;
	aChunk->iLocalSpace = NULL;
	aChunk->iBase = NULL;
	SpaceClose(s);
	}

// a process switch (kernel locked): its displaced chunks are mapped again
void LocalSwitch(DWin32Process* aProcess)
	{
	SLocalSpace* s = aProcess->iLocalSpace;
	LiveSpace = s;
	if (!s)
		return;
	for (DWin32Chunk* c = s->iChunks; c; c = c->iLocalNext)
		if (!c->iLocalMapped)
			MapChunk(c);
	}

void LocalSpaceRelease(DWin32Process* aProcess)
	{
	SLocalSpace* s = aProcess->iLocalSpace;
	if (!s)
		return;
	aProcess->iLocalSpace = NULL;
	s->iProcess = NULL;
	SpaceClose(s);
	}

// every page of [aAddr, aAddr + aLength) mapped for aChunk; kernel locked
static TBool Mapped(DWin32Chunk* aChunk, TLinAddr aAddr, TInt aLength)
	{
	for (TInt p = (aAddr - KLocalWindowLo) >> 12, e = (aAddr + aLength - 1 - KLocalWindowLo) >> 12; p <= e; ++p)
		if (PageOwner[p] != aChunk)
			return EFalse;
	return ETrue;
	}

// every page of the range mapped for one of aProcess's chunks; kernel locked
static TBool MappedFor(DWin32Process* aProcess, TLinAddr aAddr, TInt aLength)
	{
	if (aAddr + aLength > KLocalWindowHi || !aProcess->iLocalSpace)
		return EFalse;
	for (TInt p = (aAddr - KLocalWindowLo) >> 12, e = (aAddr + aLength - 1 - KLocalWindowLo) >> 12; p <= e; ++p)
		if (!PageOwner[p] || PageOwner[p]->iLocalSpace != aProcess->iLocalSpace)
			return EFalse;
	return ETrue;
	}

// The kernel's access to aThread's process at aAddr (RawRead, RawWrite, descriptor headers): 1 when the caller can
// go on as before (not the local window; the current thread's own process; or no address is shared by two
// processes, so nothing is displaced: a direct copy, ~2% faster N95 start-up than a file copy each), else the
// copy's result: KErrNone, or KErrBadDescriptor when the range is not committed in one of its local chunks.
TInt LocalCopy(DThread* aThread, TLinAddr aAddr, TAny* aBuf, TInt aLength, TBool aWrite)
	{
	if (aAddr - KLocalWindowLo >= KLocalWindowHi - KLocalWindowLo)
		return 1;
	DWin32Process* proc = (DWin32Process*)aThread->iOwningProcess;
	if (proc == TheCurrentThread->iOwningProcess)
		return 1;							// its chunks are mapped while its thread runs
	if (!SharedUnits)
		return 1;							// no address is shared: every committed page is mapped where it belongs
	if (aLength < 0 || aAddr + aLength > KLocalWindowHi)
		return KErrBadDescriptor;
	if (!aLength)
		return KErrNone;
	TInt r = KErrBadDescriptor;
	NKern::Lock();
	SLocalSpace* s = proc->iLocalSpace;
	for (DWin32Chunk* c = s ? s->iChunks : NULL; c; c = c->iLocalNext)
		{
		TLinAddr b = (TLinAddr)c->iBase;
		if (aAddr < b || aAddr + aLength > b + c->iMaxSize)
			continue;
		TInt first = (aAddr - b) >> 12, last = (aAddr + aLength - 1 - b) >> 12;
		if (c->iLocalPages->NotAllocated(first, last - first + 1))
			break;
		// mapped where it belongs: a plain copy (nothing displaces it while the kernel is locked), when the other side
		// is not in the local window or is mapped there (else a fault here would be under the kernel lock)
		if (Mapped(c, aAddr, aLength) && ((TLinAddr)aBuf - KLocalWindowLo >= KLocalWindowHi - KLocalWindowLo ||
		                                  MappedFor((DWin32Process*)TheCurrentThread->iOwningProcess, (TLinAddr)aBuf, aLength)))
			{
			aWrite ? memcpy((TAny*)aAddr, aBuf, aLength) : memcpy(aBuf, (const TAny*)aAddr, aLength);
			r = KErrNone;
			break;
			}
		TInt n = hsys(aWrite ? 181 /* pwrite64 */ : 180 /* pread64 */, s->iFd, aBuf, aLength, 0, aAddr - KLocalWindowLo, 0);
		r = n == aLength ? KErrNone : KErrBadDescriptor;
		++HostedLocalCopies;
		break;
		}
	NKern::Unlock();
	return r;
	}
