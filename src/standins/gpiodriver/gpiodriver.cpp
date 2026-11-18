// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i GPIODRIVER.LDD stand-in: "GPIODRIVER", SE's GPIO / I2C expander channel (STCF03
// flash-LED controller, MPE801 expander in the firmware driver [V strings]). Opened by EStart, APS,
// Hal.dll, AtCommands and the service menus [V names]; EStart and APS get KErrNotFound without it [V].
// H-p1i-gpio (docs/hacks.md): controls return KErrNone (GPIO reads give 0), requests stay pending
// (GPIO change notifications), every call is logged ("SYBSTUB:").
#include "tablestub.h"
NO_CONTROLS
TABLE_STUB_LDD("GPIODRIVER", KCtl, 1)
