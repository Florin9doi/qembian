// kn_gl: the syborg-gl device on the host GL server through the board (src/hosted/variant/hgl.cpp,
// src/hosted/glserver). Runs with htest --gl (the channel's ivshmem BAR; skips without it). The register path,
// a kernel kick of buffers at virtual addresses of this process (the device reads the commands and writes the
// read-back through the channel's memory callbacks), and refusals. Regression for bug 2026-11-12: callbacks
// that write guest memory were refused (process_vm_* cannot pin the BAR's pages) and every EGL result was lost.
#include <kernel/kern_priv.h>

extern TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage);
extern TInt HostedGlAttach(const char* aPath);
extern TUint32 HostedIoRead(TUint32 aAddr);
extern void HostedIoWrite(TUint32 aAddr, TUint32 aValue);
extern TUint32 HostedGlCallbacks;

static const TUint32 KGl = 0xC6000000u + 0x0f * 0x1000;
enum { RId = 0, RCaps = 1, RKick = 7, RPid = 8, RFlags = 9 };
static TUint32 Rd(TInt r) { return HostedIoRead(KGl + 4 * r); }
static void Wr(TInt r, TUint32 v) { HostedIoWrite(KGl + 4 * r, v); }

struct SKick { TUint32 iBuf, iLen, iOut, iOutLen, iResult; };
static TUint32 Cmds[64];
static TUint32 Out[16];
static SKick Kick;

int main(int, char**)
	{
	const char* chan = hgetenv("GLCHAN");
	if (!chan)
		{
		htest_note("SKIP: no GL channel (htest --gl)");
		hexit(77);
		}
	HCHECK(HostedBoardInit(NULL, NULL) == KErrNone, "board");
	HCHECK(HostedGlAttach(chan) == KErrNone, "syborg-gl attached to the host GL server");
	HCHECK(Rd(RId) == 0xc51d0011u, "ID through the channel (%08x)", Rd(RId));
	if (!(Rd(RCaps) & 1))
		{
		htest_note("SKIP: the host has no GL (CAPS 0)");
		hexit(77);
		}
	HCHECK(ETrue, "CAPS: host GL ready");
	// OWNER 7; SURF 1 (8x4); CLEAR red; READ 2 rows into Out (virtual: a memory-callback write)
	TUint32 c[] = { 8, 3, 7,  1, 6, 1, 8, 4, 0,  5, 10, 1, 1, 0xff0000, 0, 0, 0, 8, 4,  7, 6, 1, 0, 0, 2 };
	memcpy(Cmds, c, sizeof c);
	memset(Out, 0xaa, sizeof Out);
	Kick.iBuf = (TUint32)Cmds; Kick.iLen = sizeof c; Kick.iOut = (TUint32)Out; Kick.iOutLen = sizeof Out;
	Kick.iResult = 0x5a5a5a5a;
	TUint32 cb0 = HostedGlCallbacks;
	Wr(RPid, 7); Wr(RFlags, 0);
	Wr(RKick, (TUint32)&Kick);
	TUint32 res = Rd(RKick);
	HCHECK(res == 0, "a kernel kick of virtual buffers: result %d", res);
	HCHECK(Kick.iResult == 0, "the device wrote the kick's result into our memory (%08x)", Kick.iResult);
	TBool red = ETrue;
	for (TInt i = 0; i < 16; ++i)
		red = red && (Out[i] & 0xffffff) == 0xff0000;
	HCHECK(red, "the read-back reached our memory through a write callback (%08x)", Out[0]);
	// channel v3 (2026-11-13): the descriptor and commands are prefetched, the result and pixels come back in the
	// write log, PID/FLAGS travel with the kick: one round trip, no callback
	extern TUint32 HostedGlPrefetched, HostedGlSaved;
	HCHECK(HostedGlCallbacks == cb0 && HostedGlPrefetched >= sizeof c + sizeof Kick,
	       "no memory callback: %u bytes prefetched, %u callbacks", HostedGlPrefetched, HostedGlCallbacks - cb0);
	HCHECK(HostedGlSaved >= 3, "PID, FLAGS and the result read without their own round trips (%u)", HostedGlSaved);
	Wr(RKick, (TUint32)&Kick);
	HCHECK(Rd(RKick) == 0xffffffffu, "a kick without PID is refused");
	Wr(RPid, 7);
	Wr(RKick, 0x00000004);
	HCHECK(Rd(RKick) == 0xffffffffu, "a kick descriptor at an unmapped address is refused, not a fault");
	return htest_done();
	}
