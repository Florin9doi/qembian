// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i MPEG4Dec.ldd stand-in: "MPEG4DECDRV", the hardware MPEG-4 video decoder the start-up
// animation player (system_animation_player.exe) loads for z:\private\102030FD\startup.mpg4 (and
// shutdown.mpg4). Without it LoadLogicalDevice fails -1, the player leaves before it reports to APS, and
// start-up waits forever [V R6E30].
//
// H-p1i-video-absent (docs/hacks.md): no decoder, the stream ends at once. The player [V, RunL ee5d66fa]:
//   control 0 = set-up, then five notification requests 0..4, re-armed by their RunL:
//     0 picture out (RunL: controls 9, 4), 1 and 3 re-arm only, 2 reports its status to the video
//     observer, 4 reports 0 to it (end of stream: the player stops once video and sound are done);
//   control 8 = input buffer {ptr, len} for the stream data (a shared chunk on the phone [I]), 5 = start.
// Here: control 8 fails with KErrUnderflow (no data is fed, decoding never starts), requests 0..3 stay
// pending, request 4 completes with KErrNone once per set-up (control 0), so the player sees the video end.
// Rejected variants [V]: accepting control 8 without a buffer let the player decode garbage over its own
// stack (KERN-EXEC 3); refusing the channel made it report the error synchronously, and its observer
// stopped an active scheduler that had not started (E32USER-CBase 45); completing every request at
// once starved the scheduler (requests 0 and 1 re-armed and completed in a loop).
#include "tablestub.h"

static TBool EndArmed;		// set by control 0: the next request 4 completes (one client: the player)

static TInt CtlHook(DThread*, TInt aFn, TAny*, TAny*, const TUint32*)
	{
	if (aFn == 0)
		EndArmed = ETrue;
	return KStubNotHandled;
	}

static TInt ReqHook(DThread*, TInt aReq, const TUint32*, TInt)
	{
	if (aReq == 4 && EndArmed)
		{
		EndArmed = EFalse;
		return KErrNone;			// end of stream
		}
	return KStubNotHandled;			// table / default: pending
	}

static const SStubCtl KCtl[] =
	{
	{ 8, EReturn, KErrUnderflow, 0, 0 },	// no input buffer: nothing is decoded
	};
TABLE_STUB_LDD_HOOKS_CANCELNUM("MPEG4DECDRV", KCtl, EFalse, ReqHook, CtlHook)	// DoCancel(n) = request n [V player ee5d66c8..]
