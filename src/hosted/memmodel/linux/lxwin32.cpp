// Hosted EKA2: Win32 virtual-memory calls on Linux (see lxwin32.h). Original work for qembian.
#include <hrt.h>
#include "lxwin32.h"

static const TUint32 KPageShift = 12;
static TUint8 PageState[1u << (32 - KPageShift)];		// 0 free, 1 reserved, 2 committed (1 MB)
static TUint32 ReservedSize[1u << (32 - KPageShift)] __attribute__((section(".bss.lxwin32")));
static volatile TInt Lock;

static void Take() { while (__atomic_exchange_n(&Lock, 1, __ATOMIC_ACQUIRE)) hsys(SYS_sched_yield); }
static void Give() { __atomic_store_n(&Lock, 0, __ATOMIC_RELEASE); }

static void SetState(TUint32 aBase, TUint32 aSize, TUint8 aState)
	{
	for (TUint32 p = aBase >> KPageShift, e = (aBase + aSize + 0xfff) >> KPageShift; p < e; ++p)
		PageState[p] = aState;
	}

static int Prot(DWORD aProtect)
	{
	switch (aProtect)
		{
	case PAGE_NOACCESS: return 0;
	case PAGE_READONLY: return PROT_READ;
	case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
	default: return PROT_READ | PROT_WRITE;
		}
	}

LPVOID VirtualAlloc(LPVOID aAddr, SIZE_T aSize, DWORD aType, DWORD aProtect)
	{
	if (!aSize)
		return 0;
	TUint32 size = (aSize + 0xfff) & ~0xfffu;
	Take();
	LPVOID r = 0;
	if (aType & MEM_RESERVE)
		{
		TInt flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (aAddr ? MAP_FIXED_NOREPLACE : 0);
		void* m = hmmap(aAddr, size, (aType & MEM_COMMIT) ? Prot(aProtect) : 0, flags, -1, 0);
		if (!HMAP_FAILED(m) && (!aAddr || m == aAddr))
			{
			SetState((TUint32)m, size, (aType & MEM_COMMIT) ? 2 : 1);
			ReservedSize[(TUint32)m >> KPageShift] = size;
			r = m;
			}
		}
	else if (aType & MEM_COMMIT)
		{
		TUint32 base = (TUint32)aAddr & ~0xfffu;
		size = ((TUint32)aAddr + aSize + 0xfff - base) & ~0xfffu;
		TBool ok = 1;
		for (TUint32 p = base >> KPageShift, e = (base + size) >> KPageShift; p < e; ++p)
			if (!PageState[p]) ok = 0;			// must be reserved first
		if (ok && hsys(SYS_mprotect, base, size, Prot(aProtect)) == 0)
			{
			SetState(base, size, 2);
			r = aAddr;
			}
		}
	Give();
	return r;
	}

// User-visible chunks get addresses below 0x80000000, as in every EPOC memory model: user code treats such
// addresses as positive TInts. Bug 2026-11-12: chunks were reserved wherever Linux put them (0xC...), FbServ's
// session init returned a heap address that read as a negative error, and the 808's window server failed to
// initialise (EWsPanicFailedToInitialise). First fit from a moving cursor, 64 KB aligned; MAP_FIXED_NOREPLACE
// finds what is taken (freed chunks are found again on the next lap). Regression: kn_board.
static TUint32 UserCursor = KUserWindowLo;
LPVOID VirtualReserveUser(SIZE_T aSize, DWORD aProtect)
	{
	TUint32 size = (aSize + 0xffff) & ~0xffffu;
	if (!size || size > KUserWindowHi - KUserWindowLo)
		return 0;
	Take();
	LPVOID r = 0;
	for (TUint32 a = UserCursor, tried = 0; tried < KUserWindowHi - KUserWindowLo; )
		{
		if (a + size > KUserWindowHi || a + size < a)
			{
			tried += KUserWindowHi - a;
			a = KUserWindowLo;
			continue;
			}
		void* m = hmmap((void*)a, size, 0, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
		if ((TUint32)m == a)
			{
			SetState(a, size, 1);
			ReservedSize[a >> KPageShift] = size;
			UserCursor = a + size;
			r = m;
			break;
			}
		if (!HMAP_FAILED(m))
			hsys(91 /* munmap */, m, size);		// an old kernel placed it elsewhere: not ours
		a += 0x10000;
		tried += 0x10000;
		}
	Give();
	return r;
	}

// the user window's use, for the "no room" report: reservations by size class, total reserved and committed
void HostedUserWindowReport()
	{
	TUint32 n[6] = { 0 }, total = 0, committed = 0, largest = 0, run = 0;
	for (TUint32 a = KUserWindowLo; a < KUserWindowHi; a += 0x1000)
		{
		TUint32 sz = ReservedSize[a >> KPageShift];
		if (sz)
			{
			total += sz;
			n[sz <= 0x100000 ? 0 : sz <= 0x400000 ? 1 : sz <= 0x1000000 ? 2 : sz <= 0x4000000 ? 3 : 4]++;
			}
		committed += PageState[a >> KPageShift] == 2;
		run = PageState[a >> KPageShift] ? 0 : run + 0x1000;
		if (run > largest)
			largest = run;
		}
	hprintf("HOSTED: user window: %u MB reserved (%u up to 1 MB, %u up to 4 MB, %u up to 16 MB, %u up to 64 MB, "
	             "%u larger), %u MB committed, largest free run %u KB\n", total >> 20, n[0], n[1], n[2], n[3], n[4],
	             committed >> 8, largest >> 10);
	}

TBool VirtualFree(LPVOID aAddr, SIZE_T aSize, DWORD aType)
	{
	TUint32 base = (TUint32)aAddr & ~0xfffu;
	Take();
	TBool ok = 0;
	if (aType & MEM_DECOMMIT)
		{
		TUint32 size = ((TUint32)aAddr + aSize + 0xfff - base) & ~0xfffu;
		hsys(220 /* madvise */, base, size, 4 /* MADV_DONTNEED: zero on next commit */);
		ok = hsys(SYS_mprotect, base, size, 0) == 0;
		SetState(base, size, 1);
		}
	else if (aType & MEM_RELEASE)
		{
		TUint32 size = ReservedSize[base >> KPageShift];
		if (size && hmunmap((void*)base, size) == 0)
			{
			SetState(base, size, 0);
			ReservedSize[base >> KPageShift] = 0;
			ok = 1;
			}
		}
	Give();
	return ok;
	}

SIZE_T VirtualQuery(LPVOID aAddr, MEMORY_BASIC_INFORMATION* aInfo, SIZE_T)
	{
	TUint32 p = (TUint32)aAddr >> KPageShift, e = p;
	TUint8 s = PageState[p];
	while (e < (1u << (32 - KPageShift)) && PageState[e] == s)
		++e;
	aInfo->BaseAddress = (LPVOID)(p << KPageShift);
	aInfo->RegionSize = (e - p) << KPageShift;
	aInfo->State = s == 2 ? MEM_COMMIT : s == 1 ? MEM_RESERVE : MEM_FREE;
	return sizeof(*aInfo);
	}
