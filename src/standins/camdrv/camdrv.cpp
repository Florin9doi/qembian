// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P990i camdrv.ldd stand-in: the camera channel CamServer opens per sensor (unit 3 = MI-SOC2010-J main
// camera, unit 4 = MI-SOC366 front camera; DoCreate("camdrv.ldd", 1.0.1073, unit, no PDD name) after loading the
// sensor's PDD file; docs/research/p990i-camera.md). Without it CCamera::NewL left -1 in the Phone app (CallControl
// -> ECam -> ECamSemcPlugin), whose error path then panicked E32USER-CBase 63 (H-p990i-camera-absent).
// A camera that is there but never delivers a frame: controls succeed (control 2, the 96-byte capability block, is
// left as the client passed it [I: its result is ignored]), requests stay pending until cancelled; all logged.
#include "tablestub.h"
NO_CONTROLS
TABLE_STUB_LDD("camdrv.ldd", KCtl, ETrue)
