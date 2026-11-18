// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// A USB device controller with no hardware ("no cable, ever") for EPL's platform-independent USB client layer
// (kernel/eka/drivers/usbcc: chapter 9, descriptors, endpoint queues) and its eusbc.ldd: phones whose USB manager
// must find a working client driver (SE P1i usbsvr / usbmsman, 2026-11-17) get the real descriptor handling and
// RDevUsbcClient protocol, and a cable that never attaches. Built as usbcc.dll (kernel extension) with the EPL
// sources; src/standins/usbcnull/eusbc.mmp builds EPL's LDD on it. H-usbc-null (docs/hacks.md).
#include <kernel/kern_priv.h>
#include <drivers/usbc.h>

// endpoint 0 both ways, then 15 more each way taking bulk or interrupt (a generous full-speed controller: with two
// bulk pairs and one interrupt endpoint the SE P1i's USB personality start completed KErrNotSupported, 2026-11-17)
static const TUsbcEndpointCaps KEndpoints[] =
	{
	{KUsbEpSize64, (KUsbEpTypeControl | KUsbEpDirOut)},		// endpoint 0 out
	{KUsbEpSize64, (KUsbEpTypeControl | KUsbEpDirIn)},		// endpoint 0 in
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirIn)},
	{KUsbEpSize64 | KUsbEpSize32 | KUsbEpSize16 | KUsbEpSize8, (KUsbEpTypeBulk | KUsbEpTypeInterrupt | KUsbEpDirOut)},
	};
static const TInt KTotalEndpoints = sizeof(KEndpoints) / sizeof(KEndpoints[0]);

class DNullUsbcc : public DUsbClientController
	{
public:
	TInt Construct();
	virtual TInt SignalRemoteWakeup() { return KErrGeneral; }
	virtual void DumpRegisters() {  }
	virtual TDfcQue* DfcQ(TInt) { return Kern::DfcQue0(); }
	virtual TInt SetDeviceAddress(TInt) { return KErrNone; }
	virtual TInt ConfigureEndpoint(TInt, const TUsbcEndpointInfo&) { return KErrNone; }
	virtual TInt DeConfigureEndpoint(TInt) { return KErrNone; }
	virtual TInt AllocateEndpointResource(TInt, TUsbcEndpointResource) { return KErrNone; }
	virtual TInt DeAllocateEndpointResource(TInt, TUsbcEndpointResource) { return KErrNone; }
	virtual TBool QueryEndpointResource(TInt, TUsbcEndpointResource) const { return EFalse; }
	virtual TInt SetupEndpointRead(TInt, TUsbcRequestCallback&) { return KErrNone; }
	virtual TInt SetupEndpointWrite(TInt, TUsbcRequestCallback&) { return KErrNone; }
	virtual TInt CancelEndpointRead(TInt) { return KErrNone; }
	virtual TInt CancelEndpointWrite(TInt) { return KErrNone; }
	virtual TInt SetupEndpointZeroRead() { return KErrNone; }
	virtual TInt SetupEndpointZeroWrite(const TUint8*, TInt, TBool) { return KErrNone; }
	virtual TInt SendEp0ZeroByteStatusPacket() { return KErrNone; }
	virtual TInt StallEndpoint(TInt) { return KErrNone; }
	virtual TInt ClearStallEndpoint(TInt) { return KErrNone; }
	virtual TInt EndpointStallStatus(TInt) const { return EFalse; }
	virtual TInt EndpointErrorStatus(TInt) const { return KErrNone; }
	virtual TInt ResetDataToggle(TInt) { return KErrNone; }
	virtual TInt SynchFrameNumber() const { return 0; }
	virtual void SetSynchFrameNumber(TInt) {  }
	virtual TInt StartUdc() { return KErrNone; }
	virtual TInt StopUdc() { return KErrNone; }
	virtual TInt UdcConnect() { return KErrNone; }
	virtual TInt UdcDisconnect() { return KErrNone; }
	virtual TBool UsbConnectionStatus() const { return EFalse; }	// no cable, ever
	virtual TBool UsbPowerStatus() const { return EFalse; }
	virtual TBool DeviceSelfPowered() const { return ETrue; }
	virtual const TUsbcEndpointCaps* DeviceEndpointCaps() const { return KEndpoints; }
	virtual TInt DeviceTotalEndpoints() const { return KTotalEndpoints; }
	virtual TBool SoftConnectCaps() const { return ETrue; }
	virtual TBool DeviceStateChangeCaps() const { return EFalse; }
	virtual void Suspend() {  }
	virtual void Resume() {  }
	virtual void Reset() {  }
	virtual void Ep0ReadSetupPktProceed() {  }
	virtual void Ep0ReceiveProceed() {  }
	};

// UTF-16 string descriptor texts (wide literals: kernel-side _LIT is 8-bit), as the template PSL does
static const wchar_t KManufacturer[] = L"qembian";
static const wchar_t KProduct[] = L"virtual phone";
static const wchar_t KSerial[] = L"0000000000";
static const wchar_t KConfig[] = L"default";

static TUsbcStringDescriptor* StringDesc(const wchar_t* aText, TInt aBytes)
	{
	return TUsbcStringDescriptor::New(TPtr8((TUint8*)aText, aBytes - 2, aBytes - 2));
	}

TInt DNullUsbcc::Construct()
	{
	TUsbcDeviceDescriptor* dev = TUsbcDeviceDescriptor::New(0, 0, 0, 64, 0x0fce, 0x0001, 0x0100, 1);
	TUsbcConfigDescriptor* conf = TUsbcConfigDescriptor::New(1, ETrue, EFalse, 0);
	TUsbcLangIdDescriptor* lang = TUsbcLangIdDescriptor::New(0x0409);
	if (!dev || !conf || !lang)
		return KErrNoMemory;
	if (!InitialiseBaseClass(dev, conf, lang, StringDesc(KManufacturer, sizeof(KManufacturer)),
							 StringDesc(KProduct, sizeof(KProduct)), StringDesc(KSerial, sizeof(KSerial)),
							 StringDesc(KConfig, sizeof(KConfig))))
		return KErrGeneral;
	return KErrNone;
	}

DECLARE_STANDARD_EXTENSION()
	{
	DNullUsbcc* c = new DNullUsbcc;
	if (!c)
		return KErrNoMemory;
	TInt r = c->Construct();
	if (r == KErrNone && c->RegisterUdc(0) == NULL)
		r = KErrGeneral;
	Kern::Printf("SYBSTUB: usbcc null USB device controller (no cable): %d", r);
	if (r != KErrNone)
		delete c;
	return r;
	}
