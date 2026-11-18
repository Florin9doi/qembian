// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i DspBootLdd.ldd stand-in: "DSP_BOOT_DRV", loaded by APS's DspBootThread
// (startuphandler.aps) before it starts the start-up animation [V R6E30 boot: KErrNotFound without it].
// H-p1i-dsp-absent (docs/hacks.md): there is no DSP to boot; controls return KErrNone, requests
// complete at once with KErrNone, every call is logged ("SYBSTUB:").
#include "tablestub.h"
NO_CONTROLS
TABLE_STUB_LDD2("DSP_BOOT_DRV", KCtl, 1, ETrue)
