// iscrx.h: what a receive does when the client's buffer is smaller than the queued message (iscpeer.inl).
//
// The N95's own NokiaISCDriver.ldd [V, disassembly 0x80119fb8..0x8011a230, RM-320 35.0.001]: Receive (request 4)
// and DataReceive (request 6) peek at the queue head; if its length exceeds the client descriptor's MaxLength
// they write the length as a TUint16 through the client's "needed" pointer and complete with KErrNoMemory (-4),
// keeping the message queued. KErrOverflow (-9) is only used on a *successful* delivery, to say the receive
// queue had overflowed earlier. Every client read here grows its buffer on -4 and re-posts: SAServer's
// SAIscService (N95 FUN_809120c2, E52 0x80a002b8), NokiaDSY FUN_808ba8fe. On any other error they re-post the
// same buffer, which is why answering -9 spun SAServer 1.5 million times (2026-11-18, product profile 1B 2F).
//
// Fail-safe on top of the real behaviour (docs/hacks.md H-isc-rx-overflow): a client that re-posts a buffer that
// is still too small more than KIscRxMaxRefusals times in a row loses that message instead of spinning forever.
// Plain C++, no kernel headers: tests/iscpeer_overflow_test.py compiles it on the host.
#ifndef ISCRX_H
#define ISCRX_H

enum TIscRxAction { EIscRxDeliver, EIscRxTooSmall, EIscRxDrop };

const int KIscRxMaxRefusals = 3;	// a client that grows its buffer needs one refusal

struct TIscRxGuard
	{
	int iRefused;		// consecutive refusals of the queue head
	int iWarned;		// the first refusal on this queue was logged
	};

// aLen: the queued message, aMax: the client descriptor's MaxLength (negative: unreadable descriptor)
inline TIscRxAction IscRxDecide(TIscRxGuard& aG, int aLen, int aMax)
	{
	if (aMax >= 0 && aLen <= aMax)
		{
		aG.iRefused = 0;
		return EIscRxDeliver;
		}
	if (++aG.iRefused > KIscRxMaxRefusals)
		{
		aG.iRefused = 0;
		return EIscRxDrop;
		}
	return EIscRxTooSmall;
	}

#endif
