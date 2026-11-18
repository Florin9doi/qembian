// alfsw.exe: flips ALF's forced-software-rendering switch on a live phone and back.
//
// Why: the 808 and 701 compose the screen into a CPU bitmap and upload it every dirty rect
// (docs/devices/nokia-808-rm807/frame-uploads.md). In ALF that state is reachable only through
// CAlfBridge::ForceSwRendering (iForcedSwRendering) or CAlfAppUi::NotifyLowMemory, both over the
// app-server session: EAlfForceSwRendering = 4301, EAlfEnableLowMemoryState = 4300
// (external/oss.FCL.sf.mw.uiaccelerator/.../alfconstants.h). This app sends 4301(EFalse), waits, and
// sends 4301(ETrue) so the phone ends where it started. Host-side GL stats show the effect.
//
// The session is the window server's ALF app server: name "%08x_%08x_AppServer" with the interface uid in
// both fields (CAlfAppUi::ConstructL, alfappui.cpp:833: Format(name, appUid, appUid.iUid)); a few small
// differentiators are tried after it in case a build differs. Plain EXE, no app framework; all output via
// RDebug::Printf, prefixed "ALFSW:".

#include <e32base.h>
#include <e32debug.h>

static const TUint32 KAlfAppServerUid3 = 0x10282845;	// KAlfAppServerInterfaceUid3 (alfuids.h)
static const TInt KEAlfForceSwRendering = 4301;		// EAlfForceSwRendering (alfconstants.h)
static const TInt KHoldOffSeconds = 30;			// how long software rendering stays off

class RAlfSess : public RSessionBase
	{
public:
	TInt Open(const TDesC& aName, const TVersion& aVersion)
		{
		return CreateSession(aName, aVersion, 1);
		}
	TInt Send(TInt aFunction, const TIpcArgs& aArgs)	// SendReceive is protected in RSessionBase
		{
		return SendReceive(aFunction, aArgs);
		}
	};

static TInt Flip()
	{
	for (TInt i = 0; i <= 8; ++i)
		{
		// i == 0: the server's own name (uid in both fields); then the small differentiators
		const TUint32 diff = (i == 0) ? KAlfAppServerUid3 : (TUint32)(i - 1);
		TBuf<64> name;
		name.Format(_L("%08x_%08x_AppServer"), KAlfAppServerUid3, diff);
		// CAknAppServer::NewSessionL checks the client version with User::QueryVersionSupported, which needs
		// the major to match exactly: sweep majors (and a few minors) until one is accepted.
		RAlfSess sess;
		TInt err = KErrNotFound;
		for (TInt major = 0; major <= 20 && err != KErrNone; ++major)
			{
			for (TInt minor = 0; minor <= 2 && err != KErrNone; ++minor)
				{
				err = sess.Open(name, TVersion(major, minor, 0));
				if (err != KErrNotFound && err != KErrNotSupported)
					RDebug::Printf("ALFSW: open %S v%d.%d.0 -> %d", &name, major, minor, err);
				}
			}
		if (err != KErrNone)
			{
			RDebug::Printf("ALFSW: %S: no version accepted (last %d)", &name, err);
			continue;
			}
		RDebug::Printf("ALFSW: session %S opened", &name);
		RDebug::Printf("ALFSW: session %S opened", &name);

		TInt r = sess.Send(KEAlfForceSwRendering, TIpcArgs((TInt)0));
		RDebug::Printf("ALFSW: ForceSwRendering(EFalse) -> %d (holding %d s)", r, KHoldOffSeconds);
		User::After(KHoldOffSeconds * 1000000);

		TInt r2 = sess.Send(KEAlfForceSwRendering, TIpcArgs((TInt)1));
		RDebug::Printf("ALFSW: ForceSwRendering(ETrue) -> %d (restored)", r2);
		sess.Close();
		return KErrNone;
		}
	RDebug::Printf("ALFSW: no ALF app server session found");
	return KErrNotFound;
	}

GLDEF_C TInt E32Main()
	{
	Flip();
	return KErrNone;
	}

// Startup glue called from startup.s (the EABI path of the 9.x uc_exe.cpp, minus static ctors and the
// global-destructor TLS hook: this program has no static objects with ctors/dtors).
struct SAlfswThreadCreateInfo		// leading part of SStdEpocThreadCreateInfo (u32std.h)
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
class UserHeap;
IMPORT_C TInt DsbSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");

extern "C" void AlfswRunThread(TBool aNotFirst, SAlfswThreadCreateInfo& aInfo)
	{
	TInt r = DsbSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
		if (aNotFirst)
			r = (*aInfo.iFunction)(aInfo.iPtr);
		else
			{
			User::InitProcess();
			r = E32Main();
			}
		}
	User::Exit(r);
	}

extern "C" void AlfswInvariant() { User::Invariant(); }
extern "C" void AlfswHandleException(TAny* aInfo) { User::HandleException(aInfo); }
