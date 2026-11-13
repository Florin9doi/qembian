// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P1i ExtSoundPdd.pdd stand-in: an empty physical device "EXT_SOUND_DRV.SUZI_PDD" (the
// firmware PDD's name [V]) so that User::LoadPhysicalDevice("ExtSoundPdd.pdd") succeeds; the
// extsoundldd stand-in asks for no physical device, so Create() is never called (H-p1i-sound-silent).
#include <kernel/kernel.h>

class DExtSoundPdd : public DPhysicalDevice
	{
public:
	DExtSoundPdd() { iVersion = TVersion(255, 255, 0xffff); }
	virtual TInt Install()
		{
		_LIT(KName, "EXT_SOUND_DRV.SUZI_PDD");
		Kern::Printf("SYBSTUB: install EXT_SOUND_DRV.SUZI_PDD (PDD, load only)");
		return SetName(&KName);
		}
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&) { aChannel = NULL; return KErrNotSupported; }
	virtual TInt Validate(TInt, const TDesC8*, const TVersion&) { return KErrNone; }
	};

DECLARE_STANDARD_PDD()
	{
	return new DExtSoundPdd;
	}
