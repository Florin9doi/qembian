// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P990i YUV2RGB.LDD stand-in: "YUV2RGBDRV", the hardware YUV->RGB converter of intergraphapi.dll
// (CamServer's render engine). Without the file LoadLogicalDevice("YUV2RGB") fails -1, the render session
// leaves, and CamServer panics "CamSrv:start err" -1; the Phone app (CallControl -> ECamSemcPlugin) then dies
// E32USER-CBase 63 (docs/research/p990i-camera.md, H-p990i-yuv2rgb-absent). No converter here: control 0x43
// (a buffer, [I]) is refused, so a conversion fails cleanly; the others succeed.
#include "tablestub.h"
static const SStubCtl KCtl[] = {
	{ 0x43, EReturn, KErrNotSupported, 0, 0 },
	{ 0,    EReturn, KErrNone, 0, 0 },
	{ 1,    EReturn, KErrNone, 0, 0 },
	{ 2,    EReturn, KErrNone, 0, 0 },
	{ 3,    EReturn, KErrNone, 0, 0 },
	{ 4,    EReturn, KErrNone, 0, 0 },
};
TABLE_STUB_LDD("YUV2RGBDRV", KCtl, ETrue)
