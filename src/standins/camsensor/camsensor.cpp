// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// Sony Ericsson P990i camera sensor PDD stand-ins (DMsoc2010.pdd "camdrv.ldd.Msoc2010", DMsoc366.pdd
// "camdrv.ldd.Msoc366", the names CamServer's table holds [V CamServer .data 0x400050]): load only, so that
// CamServer's LoadPhysicalDevice succeeds; the camdrv stand-in asks for no physical device (H-p990i-camera-absent).
#include <kernel/kernel.h>

#define STR2(x) #x
#define STR(x) STR2(x)

class DCamSensorPdd : public DPhysicalDevice
	{
public:
	DCamSensorPdd() { iVersion = TVersion(255, 255, 0xffff); }
	virtual TInt Install()
		{
		_LIT(KName, "camdrv.ldd." STR(CAM_SENSOR));
		Kern::Printf("SYBSTUB: install %S (camera sensor PDD, load only)", &KName);
		return SetName(&KName);
		}
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&) { aChannel = NULL; return KErrNotSupported; }
	virtual TInt Validate(TInt, const TDesC8*, const TVersion&) { return KErrNone; }
	};

DECLARE_STANDARD_PDD()
	{
	return new DCamSensorPdd;
	}
