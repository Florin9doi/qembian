// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i lcd_ctrl.ldd stand-in: "LCDCTRL", SE's LCD controller channel (panel power, backlight,
// display state) of Hal.dll (controls 27, 36 [V names, earlier boots]), startuphandler.aps (APS loads
// "lcd_ctrl" after the start-up OPA calls and gets KErrNotFound without it [V R6E30 trace]),
// lcdhandler.aps and bco.exe. The frame buffer itself is ours (the Syborg display).
// H-p1i-lcdctrl (docs/hacks.md): controls return KErrNone (values read as 0), requests stay pending
// (display event notifications [I]), every call is logged ("SYBSTUB:").
#include "tablestub.h"
NO_CONTROLS
TABLE_STUB_LDD("LCDCTRL", KCtl, 1)
