// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// PowerVR services.ldd stand-in, "no device" (SE P1i): the window server loads "services"
// (User::LoadLogicalDevice) and its BitGdi/FbsCli 2D path goes through PVR2DIF -> PVR2D -> services_um.dll, whose
// bridge calls are DoControl(n, in, out) on this channel. Imagination's kernel half (services.ldd, pvrsrv.dll,
// pvrclcdc.dll) is not in our kernel ROM; without any driver the window server panicked (2026-11-17).
// H-p1i-pvr-absent (docs/hacks.md): the channel opens, every bridge call is refused with KErrNotSupported and
// logged, so the PowerVR user side sees a platform with no device [I: whether PVR2D/BitGdi then fall back to
// software is what this stand-in tests].
#include "tablestub.h"

static TInt CtlHook(DThread*, TInt, TAny*, TAny*, const TUint32*)
	{
	return KErrNotSupported;
	}

NO_CONTROLS
TABLE_STUB_LDD_CTLHOOK("services", KCtl, EFalse, CtlHook)
