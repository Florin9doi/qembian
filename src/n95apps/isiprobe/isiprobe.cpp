// isiprobe.exe: sends ISI messages to the modem over an ISC channel (IscApi.dll, the N95's RIscApi) and
// writes every message that comes back to a file. Runs on the real phone (self-signed, OnePatch4All) and on
// the emulated one (qembian, linked to the modem firmware: docs/research/n95-cmt-emulation.md).
//
// Input:  E:\isiprobe\in.txt (else C:\isiprobe\in.txt), one command per line:
//           # comment
//           ch N          ISC channel to open (default 60; must be unused by the phone's own clients)
//           wait MS       how long to wait for the answer to each message (default 2000)
//           HEX...        an ISI message as IscApi sends it: 00 00 00 RES LEN_HI LEN_LO 00 00 TID ID DATA
//                         (spaces optional; the length bytes may be 0000: filled in from the message size)
// Output: the same folder, out.txt: "S hex" per message sent, "R hex" per message received, "T" when no
//         answer came in time, "E text" on errors, "DONE n" at the end. Also "ISIPROBE:" lines via RDebug.
// The answer to a message is the first received one with the same resource and transaction id.
#include <e32base.h>
#include <e32debug.h>
#include <f32file.h>

// RIscApi (IscApi.dll, docs/devices/nokia-n95-8gb-rm320/n95-isc-isi-protocol.md section 2): object size 0x44,
// called by ordinal through isiprobe's own import library (iscapi.def)
extern "C" {
void IscCtor(TAny* aSelf);                                                          // #29
void IscOpen(TAny* aSelf, TUint16 aChannel, TRequestStatus& aStatus, TOwnerType aType,
             const TDesC8* aInfo);                                                  // #21
TInt IscSend(TAny* aSelf, const TDesC8& aMsg);                                      // #23
void IscReceive(TAny* aSelf, TRequestStatus& aStatus, TDes8& aBuf, TUint16& aNeeded);   // #26
void IscReceiveCancel(TAny* aSelf);                                                 // #5
void IscClose(TAny* aSelf);                                                         // #25
}

static const TInt KMaxMsg = 8192;

// no static objects with constructors: startup.s does not run them (a static TBuf8 had max length 0 and
// panicked USER 23 on the first append), so the line buffer is allocated in E32Main
static RFs gFs;
static RFile gOut;
static TPtr8 gLine(NULL, 0);

static void Log(const TDesC8& aText)
	{
	gOut.Write(aText);
	gOut.Write(_L8("\n"));
	}

static void LogHex(const char* aTag, const TDesC8& aMsg)
	{
	gLine.Zero();
	gLine.Append((const TUint8*)aTag, User::StringLength((const TUint8*)aTag));
	gLine.Append(' ');
	for (TInt i = 0; i < aMsg.Length(); i++)
		gLine.AppendNumFixedWidth(aMsg[i], EHex, 2);
	Log(gLine);
	RDebug::Printf("ISIPROBE: %s len %d", aTag, aMsg.Length());
	}

static TInt HexVal(TUint8 c)
	{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
	}

// parses hex digits (spaces ignored); returns KErrCorrupt on anything else
static TInt ParseHex(const TDesC8& aIn, TDes8& aOut)
	{
	aOut.Zero();
	TInt hi = -1;
	for (TInt i = 0; i < aIn.Length(); i++)
		{
		TUint8 c = aIn[i];
		if (c == ' ' || c == '\t' || c == '\r')
			continue;
		TInt v = HexVal(c);
		if (v < 0)
			return KErrCorrupt;
		if (hi < 0)
			hi = v;
		else
			{
			if (aOut.Length() == aOut.MaxLength())
				return KErrOverflow;
			aOut.Append((TUint8)(hi << 4 | v));
			hi = -1;
			}
		}
	return hi < 0 ? KErrNone : KErrCorrupt;
	}

static TInt OpenFiles(TFileName& aDir)
	{
	static const TText* const KDirs[] = { (const TText*)L"E:\\isiprobe\\", (const TText*)L"C:\\isiprobe\\" };
	for (TInt i = 0; i < 2; i++)
		{
		TFileName in;
		in.Copy(TPtrC(KDirs[i]));
		in.Append(_L("in.txt"));
		TEntry e;
		if (gFs.Entry(in, e) == KErrNone)
			{
			aDir.Copy(TPtrC(KDirs[i]));
			TFileName out(aDir);
			out.Append(_L("out.txt"));
			return gOut.Replace(gFs, out, EFileWrite | EFileShareExclusive);
			}
		}
	return KErrNotFound;
	}

GLDEF_C TInt E32Main()
	{
	RDebug::Printf("ISIPROBE: start");
	HBufC8* lineBuf = HBufC8::New(KMaxMsg * 2 + 16);
	if (!lineBuf)
		return KErrNoMemory;
	gLine.Set(lineBuf->Des());
	TInt r = gFs.Connect();
	if (r != KErrNone)
		return r;
	TFileName dir;
	r = OpenFiles(dir);
	if (r != KErrNone)
		{
		RDebug::Printf("ISIPROBE: no in.txt (%d)", r);
		gFs.Close();
		return r;
		}
	TFileName inName(dir);
	inName.Append(_L("in.txt"));
	RFile in;
	TInt size = 0;
	HBufC8* script = NULL;
	if ((r = in.Open(gFs, inName, EFileRead | EFileShareReadersOnly)) == KErrNone && (r = in.Size(size)) == KErrNone)
		{
		script = HBufC8::New(size);
		if (script)
			{
			TPtr8 p = script->Des();
			r = in.Read(p);
			}
		else
			r = KErrNoMemory;
		}
	in.Close();
	if (r != KErrNone)
		{
		Log(_L8("E cannot read in.txt"));
		gOut.Close();
		gFs.Close();
		return r;
		}

	TUint8 iscObj[0x80];                    // RIscApi is 0x44 bytes; room to spare
	Mem::FillZ(iscObj, sizeof iscObj);
	IscCtor(iscObj);
	TBool open = EFalse;
	TInt channel = 60, waitMs = 2000, sent = 0;
	HBufC8* txBuf = HBufC8::New(KMaxMsg);
	HBufC8* rxBuf = HBufC8::New(KMaxMsg);
	RTimer timer;
	timer.CreateLocal();
	TRequestStatus rxStatus = KErrNone;
	TBool rxPending = EFalse;
	TUint16 needed = 0;
	// the receive descriptor must outlive the pending Receive: the kernel writes through it when a message
	// comes (bug: a TPtr8 local to the block was gone by then, the real phone's driver saw a garbage max length
	// and completed KErrNoMemory "needed 12")
	TPtr8 rx(NULL, 0);
	if (rxBuf)
		rx.Set(rxBuf->Des());

	TPtrC8 rest(*script);
	while (rest.Length() && txBuf && rxBuf)
		{
		TInt nl = rest.Locate('\n');
		TPtrC8 line = nl < 0 ? rest : rest.Left(nl);
		rest.Set(nl < 0 ? TPtrC8() : rest.Mid(nl + 1));
		while (line.Length() && (line[0] == ' ' || line[0] == '\t'))
			line.Set(line.Mid(1));
		while (line.Length() && (line[line.Length() - 1] == '\r' || line[line.Length() - 1] == ' '))
			line.Set(line.Left(line.Length() - 1));
		if (!line.Length() || line[0] == '#')
			continue;
		if (line.Left(3) == _L8("ch ") || line.Left(5) == _L8("wait "))
			{
			TLex8 lex(line.Mid(line[0] == 'c' ? 3 : 5));
			TInt v = 0;
			if (lex.Val(v) == KErrNone)
				{
				if (line[0] == 'c')
					channel = v;
				else
					waitMs = v;
				}
			continue;
			}
		TPtr8 tx = txBuf->Des();
		if (ParseHex(line, tx) != KErrNone || tx.Length() < 10)
			{
			gLine.Copy(_L8("E bad line: "));
			gLine.Append(line.Left(200));
			Log(gLine);
			continue;
			}
		if (tx[4] == 0 && tx[5] == 0)
			{
			tx[4] = (TUint8)((tx.Length() - 6) >> 8);
			tx[5] = (TUint8)(tx.Length() - 6);
			}
		if (!open)
			{
			TRequestStatus st;
			IscOpen(iscObj, (TUint16)channel, st, EOwnerThread, NULL);
			User::WaitForRequest(st);
			if (st.Int() != KErrNone)
				{
				gLine.Format(_L8("E open channel %d: %d"), channel, st.Int());
				Log(gLine);
				break;
				}
			open = ETrue;
			gLine.Format(_L8("I channel %d open"), channel);
			Log(gLine);
			}
		if (!rxPending)
			{
			rx.Zero();
			IscReceive(iscObj, rxStatus, rx, needed);
			rxPending = ETrue;
			}
		r = IscSend(iscObj, tx);
		LogHex("S", tx);
		sent++;
		if (r != KErrNone)
			{
			gLine.Format(_L8("E send %d"), r);
			Log(gLine);
			continue;
			}
		// wait for the answer (same resource and transaction), logging whatever else arrives
		TRequestStatus tStatus;
		timer.After(tStatus, waitMs * 1000);
		TBool answered = EFalse;
		while (!answered)
			{
			User::WaitForRequest(rxStatus, tStatus);
			if (rxStatus != KRequestPending)
				{
				rxPending = EFalse;
				if (rxStatus.Int() == KErrNone || rxStatus.Int() == KErrOverflow)
					{
					LogHex("R", rx);
					if (rx.Length() > 8 && rx[3] == tx[3] && rx[8] == tx[8])
						answered = ETrue;
					}
				else
					{
					gLine.Format(_L8("E receive %d (needed %d)"), rxStatus.Int(), needed);
					Log(gLine);
					if (rxStatus.Int() != KErrNoMemory)
						break;
					}
				rx.Zero();
				IscReceive(iscObj, rxStatus, rx, needed);
				rxPending = ETrue;
				}
			if (tStatus != KRequestPending)
				break;
			}
		if (answered)
			{
			timer.Cancel();
			User::WaitForRequest(tStatus);
			}
		else
			Log(_L8("T"));
		}
	if (rxPending)
		{
		IscReceiveCancel(iscObj);
		User::WaitForRequest(rxStatus);
		}
	if (open)
		IscClose(iscObj);
	gLine.Format(_L8("DONE %d"), sent);
	Log(gLine);
	RDebug::Printf("ISIPROBE: done, %d sent", sent);
	timer.Close();
	delete txBuf;
	delete rxBuf;
	delete script;
	gOut.Close();
	gFs.Close();
	delete lineBuf;
	return KErrNone;
	}

// Startup glue called from startup.s (as dsbtest: the EABI path of the 9.x uc_exe.cpp, no static objects
// with constructors)
struct SProbeThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt ProbeSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");

extern "C" void DsbRunThread(TBool aNotFirst, SProbeThreadCreateInfo& aInfo)
	{
	TInt r = ProbeSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
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

extern "C" void DsbInvariant() { User::Invariant(); }
extern "C" void DsbHandleException(TAny* aInfo) { User::HandleException(aInfo); }
