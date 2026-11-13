// Hosted EKA2: a sampling profiler for speed work (htest --env PROFILE=1). ITIMER_PROF fires every 1 ms of
// process CPU time (user and system); the handler records the interrupted pc. HostedProfileReport prints
// the split by region and the hottest addresses (symbolise with tools/hosted/profsym.py). Original work
// for qembian.
#include "nk_priv.h"

static const TInt KMaxSamples = 1 << 20;
static TUint32* Samples;
static TUint32* Callers;				// the lr of each sample: who called a hot leaf (memmove, hmmap)
static TUint32* Threads;				// the nanothread running (0: a host helper thread): who uses the CPU
static volatile TInt SampleCount;

static void ProfHandler(int, hsiginfo*, hucontext* aUc)
	{
	TInt i = __atomic_fetch_add(&SampleCount, 1, __ATOMIC_RELAXED);
	if (i < KMaxSamples)
		{
		Samples[i] = aUc->mc.pc | ((aUc->mc.cpsr >> 5) & 1);		// bit 0: Thumb
		Callers[i] = aUc->mc.lr;
		SLxThread* t = LxSelf();
		Threads[i] = t ? (TUint32)t->iNThread : 0;
		}
	}

void HostedProfileStart()
	{
	Samples = (TUint32*)hmmap(0, KMaxSamples * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	Callers = (TUint32*)hmmap(0, KMaxSamples * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	Threads = (TUint32*)hmmap(0, KMaxSamples * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(Samples) || HMAP_FAILED(Callers) || HMAP_FAILED(Threads))
		return;
	hsigaction(27 /* SIGPROF */, ProfHandler, SA_RESTART);
	struct { TInt32 is, iu, vs, vu; } it = { 0, 1000, 0, 1000 };		// interval and value: 1 ms
	hsys(104 /* setitimer */, 2 /* ITIMER_PROF */, &it, 0);
	}

// phone mode (Ctrl+P): report what was sampled since the last request, then start a new window
void HostedProfileReset()
	{
	__atomic_store_n(&SampleCount, 0, __ATOMIC_RELAXED);
	}

TBool HostedProfileOn()
	{
	return Samples != 0 && !HMAP_FAILED(Samples);
	}

static const char* Region(TUint32 pc)
	{
	if (pc >= 0xd0000000u && pc < 0xd1000000u) return "kernel";
	if (pc >= 0x80000000u && pc < 0x84000000u) return "rom";
	if (pc >= 0x70000000u && pc < 0x78000000u) return "ramcode";
	if (pc >= 0xffff0000u) return "vectors";
	return "other";
	}

// the aTop most frequent (aA[i], aB[i]) pairs as "PROF <label> a b count" (aSingle: aA alone, "PROF <label> a count")
static void TopPairs(const TUint32* aA, const TUint32* aB, TInt n, const char* aLabel, TInt aTop, TBool aSingle)
	{
	TUint64* k = (TUint64*)hmmap(0, n * 8 + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	TUint32* c = (TUint32*)hmmap(0, n * 4 + 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(k) || HMAP_FAILED(c))
		return;
	for (TInt i = 0; i < n; ++i)
		k[i] = (TUint64)aA[i] << 32 | (aSingle ? 0 : aB[i]);
	for (TInt gap = n / 2; gap > 0; gap /= 2)
		for (TInt i = gap; i < n; ++i)
			for (TInt j = i; j >= gap && k[j - gap] > k[j]; j -= gap)
				{
				TUint64 t = k[j]; k[j] = k[j - gap]; k[j - gap] = t;
				}
	TInt runs = 0;
	for (TInt i = 0; i < n; )
		{
		TInt j = i;
		while (j < n && k[j] == k[i]) ++j;
		k[runs] = k[i];
		c[runs++] = j - i;
		i = j;
		}
	for (TInt m = 0; m < aTop; ++m)
		{
		TInt best = -1;
		for (TInt r = 0; r < runs; ++r)
			if (c[r] && (best < 0 || c[r] > c[best]))
				best = r;
		if (best < 0)
			break;
		if (aSingle)
			hprintf("PROF %s %08x %u\n", aLabel, (TUint32)(k[best] >> 32), c[best]);
		else
			hprintf("PROF %s %08x %08x %u\n", aLabel, (TUint32)(k[best] >> 32), (TUint32)k[best], c[best]);
		c[best] = 0;
		}
	hmunmap(k, n * 8 + 8);
	hmunmap(c, n * 4 + 4);
	}

void HostedProfileReport()
	{
	TInt n = SampleCount < KMaxSamples ? SampleCount : KMaxSamples;
	if (!n)
		return;
	static const char* const KNames[] = { "kernel", "rom", "ramcode", "vectors", "other" };
	for (TInt r = 0; r < 5; ++r)
		{
		TInt c = 0;
		for (TInt i = 0; i < n; ++i)
			c += Region(Samples[i] & ~1u) == KNames[r];
		hprintf("PROF region %s %d samples (%d%%)\n", KNames[r], c, c * 100 / n);
		}
	// while the arrays still line up: samples per thread, the hottest (thread, pc) pairs, the hottest (pc, lr) pairs
	TopPairs(Threads, Threads, n, "thread", 40, ETrue);
	TopPairs(Threads, Samples, n, "tpc", 200, EFalse);
	TopPairs(Samples, Callers, n, "lr", 150, EFalse);
	// the hottest addresses: sort a copy (in place: the run is over), count runs
	TUint32* s = Samples;
	for (TInt gap = n / 2; gap > 0; gap /= 2)		// shell sort
		for (TInt i = gap; i < n; ++i)
			for (TInt j = i; j >= gap && s[j - gap] > s[j]; j -= gap)
				{
				TUint32 t = s[j]; s[j] = s[j - gap]; s[j - gap] = t;
				}
	// runs of equal pcs as (pc, count) pairs in their own buffer; then the 60 largest
	TUint32* r2 = (TUint32*)hmmap(0, n * 8 + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(r2))
		return;
	TInt runs = 0;
	for (TInt i = 0; i < n; )
		{
		TInt j = i;
		while (j < n && s[j] == s[i]) ++j;
		r2[2 * runs] = s[i];
		r2[2 * runs + 1] = j - i;
		++runs;
		i = j;
		}
	for (TInt k = 0; k < 150; ++k)
		{
		TInt best = -1;
		for (TInt r = 0; r < runs; ++r)
			if (r2[2 * r + 1] && (best < 0 || r2[2 * r + 1] > r2[2 * best + 1]))
				best = r;
		if (best < 0)
			break;
		hprintf("PROF pc %08x %u\n", r2[2 * best], r2[2 * best + 1]);
		r2[2 * best + 1] = 0;
		}
	hprintf("PROF total %d samples\n", n);
	hmunmap(r2, n * 8 + 8);
	}
