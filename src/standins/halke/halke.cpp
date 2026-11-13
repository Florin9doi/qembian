// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// HALKE.LDD stand-in for the Sony Ericsson P1i (UIQ 3): SE's HAL kernel extension, opened as "HALKE"
// (file halke.ldd, version 1.0.1) by the phone's own Hal.dll and EStart (docs/devices/se-p1i/bring-up.md).
//   control 0 (a1 = HAL attribute, a2 = TInt* value): get. Hal.dll serves attributes 0..7 (EManufacturer ..
//             EDeviceFamilyRev) this way [V Hal.dll f807f5f4]; EStart panics ESTART_3 when EMachineUid (5)
//             fails [V]. Values: board config halke.attr<N> (the profile's [[standins]] config, from the
//             phone's Z:\resource\apps\ProductInfo.ini); an attribute with no value: KErrNotSupported.
//   control 1 (a1 = attribute, a2 = value): set (EStart restores HAL.DAT through it [V EStart f80b141e]):
//             kept for later gets.
//   control 2: end of the HAL.DAT restore [V EStart f80b143e]: KErrNone.
#include "nokiastub.h"
#include "boardconfig.h"

static const TInt KAttrs = 8;
static TInt Value[KAttrs];
static TBool Known[KAttrs];
static TBool Loaded = EFalse;

static void Load()
	{
	if (Loaded)
		return;
	Loaded = ETrue;
	static const char* const KKeys[KAttrs] = { "halke.attr0", "halke.attr1", "halke.attr2", "halke.attr3",
											   "halke.attr4", "halke.attr5", "halke.attr6", "halke.attr7" };
	for (TInt i = 0; i < KAttrs; ++i)
		{
		const TInt KUnset = (TInt)0x80000001;
		TInt v = BoardConfigInt(KKeys[i], KUnset);
		Known[i] = v != KUnset;
		Value[i] = v;
		}
	}

static TInt Control(DThread* aClient, TInt aFn, TAny* a1, TAny* a2, const TUint32*)
	{
	Load();
	TInt attr = (TInt)a1;
	switch (aFn)
		{
	case 0:
		if (attr < 0 || attr >= KAttrs || !Known[attr])
			return KErrNotSupported;
		return Kern::ThreadRawWrite(aClient, a2, &Value[attr], sizeof(TInt));
	case 1:
		if (attr < 0 || attr >= KAttrs)
			return KErrNotSupported;
		Value[attr] = (TInt)a2;
		Known[attr] = ETrue;
		return KErrNone;
	case 2:
		return KErrNone;
	default:
		return KStubNotHandled;		// logged by nokiastub.h, KErrNone
		}
	}

NO_CONTROLS
NOKIA_STUB_LDD_CTLHOOK("HALKE", KCtl, ETrue, Control)
