// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i DspControlLdd.ldd stand-in: "DSP_CONTROL_DRV", the audio DSP control channel
// (codec module load from \private\10200B19\DspCodecModule.bin, filter coefficients) of the start-up
// animation player and M3SAudioDevMgr. The player panics "Could not load DspControlLDD" without it [V].
// H-p1i-dsp-absent (docs/hacks.md): there is no DSP; controls return KErrNone, requests complete at
// once with KErrNone, every call is logged ("SYBSTUB:").
#include "nokiastub.h"
NO_CONTROLS
NOKIA_STUB_LDD2("DSP_CONTROL_DRV", KCtl, 1, ETrue)
