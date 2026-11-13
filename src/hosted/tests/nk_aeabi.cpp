// nk_aeabi: the phone ROM's own 64-bit arithmetic helpers (RVCT runtime drtaeabi.dll: __aeabi_uldivmod,
// __aeabi_ldivmod, __aeabi_llsl, __aeabi_lmul; addresses from htest --env AEABI=uldivmod,ldivmod,llsl,lmul) run
// natively on the host CPU and checked against plain C. Written while chasing the 808's SQL server, whose
// databases read as corrupt only on the hosted kernel (a table's root page came out 0).
#include <nk_priv.h>
#include "romload.h"

static TUint32 Fn[4];
static TUint32 R[4];
// call an ARM/Thumb function with r0-r3 and keep r0-r3 of its return (the AEABI {quot, rem} convention)
static void Call(TUint32 aFn, TUint32 a0, TUint32 a1, TUint32 a2, TUint32 a3)
	{
	register TUint32 r0 __asm__("r0") = a0, r1 __asm__("r1") = a1, r2 __asm__("r2") = a2, r3 __asm__("r3") = a3;
	register TUint32 ip __asm__("ip") = aFn;
	__asm__ volatile("blx ip" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(ip) : : "lr", "memory", "cc");
	R[0] = r0; R[1] = r1; R[2] = r2; R[3] = r3;
	}

static TUint64 Q(TUint64 n, TUint64 d) { TUint64 q = 0, r = 0; for (TInt i = 63; i >= 0; --i) { r = r << 1 | (n >> i & 1); if (r >= d) { r -= d; q |= 1ull << i; } } return q; }

static TUint32 Seed = 12345;
static TUint32 Rnd() { Seed = Seed * 1103515245u + 12345u; return Seed; }

int main(int, char**)
	{
	SRomArgs a;
	if (!RomReadArgs(a) || !hgetenv("AEABI"))
		{
		htest_note("SKIP: needs htest --rom DEVICE --env AEABI=uldivmod,ldivmod,llsl,lmul (hex addresses)");
		hexit(77);
		}
	HCHECK(RomLoad(a), "ROM loaded");
	const char* p = hgetenv("AEABI");
	for (TInt i = 0; i < 4; ++i)
		{
		for (Fn[i] = 0; (*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'f'); ++p)
			Fn[i] = Fn[i] * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
		if (*p == ',')
			++p;
		}
	htest_note("helpers at %08x %08x %08x %08x", Fn[0], Fn[1], Fn[2], Fn[3]);
	TInt bad[4] = { 0, 0, 0, 0 };
	for (TInt n = 0; n < 20000; ++n)
		{
		TUint64 x = (TUint64)Rnd() << 32 | Rnd(), y = (TUint64)Rnd() << (Rnd() % 33) | (Rnd() % 1000 + 1);
		if (n % 4 == 0) x >>= Rnd() % 60;
		if (n % 3 == 0) y = Rnd() % 4096 + 1;
		// unsigned divide: {q, r}
		Call(Fn[0], (TUint32)x, (TUint32)(x >> 32), (TUint32)y, (TUint32)(y >> 32));
		TUint64 q = Q(x, y), rm = x - q * y;
		if (((TUint64)R[1] << 32 | R[0]) != q || ((TUint64)R[3] << 32 | R[2]) != rm)
			if (bad[0]++ < 5)
				htest_note("uldivmod %08x%08x / %08x%08x: got %08x%08x r %08x%08x, want %08x%08x r %08x%08x",
				           (TUint32)(x >> 32), (TUint32)x, (TUint32)(y >> 32), (TUint32)y, R[1], R[0], R[3], R[2],
				           (TUint32)(q >> 32), (TUint32)q, (TUint32)(rm >> 32), (TUint32)rm);
		// signed divide (truncating)
		TInt64 sx = (TInt64)x, sy = (TInt64)y * ((n & 1) ? -1 : 1);
		Call(Fn[1], (TUint32)sx, (TUint32)((TUint64)sx >> 32), (TUint32)sy, (TUint32)((TUint64)sy >> 32));
		TUint64 ax = sx < 0 ? 0 - (TUint64)sx : (TUint64)sx, ay = sy < 0 ? 0 - (TUint64)sy : (TUint64)sy;
		TUint64 sq = Q(ax, ay);
		TInt64 wq = ((sx < 0) != (sy < 0)) ? -(TInt64)sq : (TInt64)sq, wr = sx - wq * sy;
		if (((TUint64)R[1] << 32 | R[0]) != (TUint64)wq || ((TUint64)R[3] << 32 | R[2]) != (TUint64)wr)
			if (bad[1]++ < 5)
				htest_note("ldivmod mismatch for %08x%08x / %08x%08x", (TUint32)((TUint64)sx >> 32), (TUint32)sx,
				           (TUint32)((TUint64)sy >> 32), (TUint32)sy);
		// shift left
		TUint32 sh = Rnd() % 64;
		Call(Fn[2], (TUint32)x, (TUint32)(x >> 32), sh, 0);
		if (((TUint64)R[1] << 32 | R[0]) != x << sh && bad[2]++ < 5)
			htest_note("llsl %08x%08x << %u: got %08x%08x", (TUint32)(x >> 32), (TUint32)x, sh, R[1], R[0]);
		// multiply
		Call(Fn[3], (TUint32)x, (TUint32)(x >> 32), (TUint32)y, (TUint32)(y >> 32));
		if (((TUint64)R[1] << 32 | R[0]) != x * y && bad[3]++ < 5)
			htest_note("lmul %08x%08x * %08x%08x: got %08x%08x", (TUint32)(x >> 32), (TUint32)x, (TUint32)(y >> 32),
			           (TUint32)y, R[1], R[0]);
		}
	HCHECK(!bad[0], "__aeabi_uldivmod: %d of 20000 wrong", bad[0]);
	HCHECK(!bad[1], "__aeabi_ldivmod: %d of 20000 wrong", bad[1]);
	HCHECK(!bad[2], "__aeabi_llsl: %d of 20000 wrong", bad[2]);
	HCHECK(!bad[3], "__aeabi_lmul: %d of 20000 wrong", bad[3]);
	return htest_done();
	}
