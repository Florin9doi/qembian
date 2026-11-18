// N95 dvfs_interface.ldd stand-in (see tablestub.h): Nokia's dynamic voltage/frequency scaling
// interface. MmfAudioServer's resource manager (MmRmServer.DLL 0x80931934) loads it with
// LeaveIfError, opens channel "dvfs_interface" (version x.x.1540) and asks for a performance level
// with a synchronous DoRequest(0, status, level <= 100) + LeaveIfError (0x80931a84). Our board has
// one fixed clock: every request completes with KErrNone at once. Hack H-dvfs (docs/hacks.md).
#include "tablestub.h"
NO_CONTROLS
TABLE_STUB_LDD2("dvfs_interface", KCtl, 1, ETrue)
