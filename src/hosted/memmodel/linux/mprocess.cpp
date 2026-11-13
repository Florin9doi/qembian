// Hosted EKA2 port of memmodel/emul/win32/mprocess.cpp (EPL v1.0, Nokia; header above): class names kept,
// Win32 virtual memory calls through lxwin32.h (mmap/mprotect). Other changes are marked "hosted:".
// Copyright (c) 1994-2009 Nokia Corporation and/or its subsidiary(-ies).
// All rights reserved.
// This component and the accompanying materials are made available
// under the terms of the License "Eclipse Public License v1.0"
// which accompanies this distribution, and is available
// at the URL "http://www.eclipse.org/legal/epl-v10.html".
//
// Initial Contributors:
// Nokia Corporation - initial contribution.
//
// Contributors:
//
// Description:
// e32\memmodel\emul\win32\mprocess.cpp
// 
//

#include "memmodel.h"
#include "lxwin32.h"

#define iMState		iWaitLink.iSpare1

extern const char* JustInTime;
extern TBool DisableWSDWarning;
const TInt KMaxWsdDllsPerProcess = 256;

/********************************************
 * Process
 ********************************************/
DWin32Process::DWin32Process()
: iDllData(KMaxWsdDllsPerProcess, _FOFF(SProcessDllDataBlock,iCodeSeg)), iExeDataFd(-1)
	{
	// hosted: no just-in-time debugger property
	}

DWin32Process::~DWin32Process()
	{
	__KTRACE_OPT(KMMU,Kern::Printf("DWin32Process destruct"));
	Destruct();
	RemoveDllData();
	iDllData.Close();
	MM::ExeDataRelease(this);
	LocalSpaceRelease(this);
	}

TInt DWin32Process::NewChunk(DChunk*& aChunk, SChunkCreateInfo& aInfo, TLinAddr& aRunAddr)
	{
	aChunk=NULL;
	DWin32Chunk* pC=new DWin32Chunk;
	if (!pC)
		return KErrNoMemory;
	pC->iChunkType=aInfo.iType;
	if (!aInfo.iGlobal && (iAttributes & DWin32Process::EPrivate)!=0)
		pC->iAttributes |= DWin32Chunk::EPrivate;
	pC->iOwningProcess=(aInfo.iGlobal)?NULL:this;
	TInt r=pC->Create(aInfo);
	if (r==KErrNone && (aInfo.iOperations & SChunkCreateInfo::EAdjust))
		{
		__ASSERT_ALWAYS(aInfo.iRunAddress==0,MM::Panic(MM::EInvalidChunkCreate));
		if (aInfo.iPreallocated==0)
			{
			if (pC->iAttributes & DChunk::EDisconnected)
				{
				r=pC->Commit(aInfo.iInitialBottom,aInfo.iInitialTop-aInfo.iInitialBottom);
				}
			else if (pC->iAttributes & DChunk::EDoubleEnded)
				{
				r=pC->AdjustDoubleEnded(aInfo.iInitialBottom,aInfo.iInitialTop);
				}
			else
				{
				r=pC->Adjust(aInfo.iInitialTop);
				}
			}
		if (r==KErrNone)
			{
			aRunAddr=(TLinAddr)pC->Base();
			}
		}
	if (r==KErrNone)
		{
		pC->iDestroyedDfc = aInfo.iDestroyedDfc;
		aChunk=(DChunk*)pC;
		}
	else
		pC->Close(NULL);	// NULL since chunk can't have been added to process
	return r;
	}

TInt DWin32Process::DoCreate(TBool aKernelProcess, TProcessCreateInfo& /*aInfo*/)
	{
	__KTRACE_OPT(KPROC,Kern::Printf("DWin32Process::DoCreate %O",this))
	iAttributes=aKernelProcess ? ESupervisor|EPrivate : 0;
	// force iDllData to reserve KMaxWsdDllsPerProcess. The Append will
	// create the space, the Remove will not free it.
	SProcessDllDataBlock data = {0,0,0,-1};
	TInt err = iDllData.Append(data);
	if(err==KErrNone) 
		{
			__ASSERT_ALWAYS(iDllData.Count()==1,MM::Panic(MM::EWsdBadReserve));
			iDllData.Remove(0);
		}
	return err;
	}

TInt DWin32Process::CreateDataBssStackArea(TProcessCreateInfo& aInfo)
//
// hosted: a ROM EXE's .data/.bss live at its fixed run address (0x00400000 in the multiple memory
// model), which every EXE shares: each process keeps its area in its own memory file, mapped at the run
// address while one of its threads runs (MM::DoProcessSwitch) and at a private alias for the kernel.
// (Win32: managed by the PE loader, every EXE at its own address.)
//
	{
	__KTRACE_OPT(KPROC,Kern::Printf("DWin32Process::CreateDataBssStackArea %O data %08x+%x bss %x run %08x",this,
		aInfo.iDataLoadAddress, aInfo.iDataSize, aInfo.iBssSize, aInfo.iDataRunAddress));
	TInt size = aInfo.iDataSize + aInfo.iBssSize;
	if (!size || !aInfo.iDataRunAddress)
		return KErrNone;
	TLinAddr base = aInfo.iDataRunAddress & ~0xfffu;
	TInt map = ((aInfo.iDataRunAddress + size + 0xfff) & ~0xfffu) - base;
	TInt fd = hmemfd("exedata");
	if (fd < 0)
		return KErrNoMemory;
	if (hftruncate(fd, map) < 0)
		{
		hsys(SYS_close, fd);
		return KErrNoMemory;
		}
	TAny* alias = hmmap(0, map, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (HMAP_FAILED(alias))
		{
		hsys(SYS_close, fd);
		return KErrNoMemory;
		}
	iExeDataBase = base;
	iExeDataSize = map;
	iExeDataFd = fd;
	iExeDataAlias = (TLinAddr)alias;
	if (aInfo.iDataSize)		// .bss: the new file reads as zeros
		memcpy((TAny*)(iExeDataAlias + aInfo.iDataRunAddress - base), (const TAny*)aInfo.iDataLoadAddress, aInfo.iDataSize);
	return KErrNone;
	}

TInt DWin32Process::AddChunk(DChunk* aChunk,TBool /*isReadOnly*/)
	{
	// as the multiple and flexible memory models: a process-local chunk is never opened by another process (its
	// addresses are its own process's here too, mlocal.cpp)
	if (((DWin32Chunk*)aChunk)->iLocalSpace && aChunk->iOwningProcess != this)
		return KErrAccessDenied;
	return KErrNone;
	}


TInt DWin32Process::NewShPool(DShPool*& aPool, TShPoolCreateInfo& aInfo)
	{
	aPool = NULL;
	DWin32ShPool* pC = NULL;

	if (aInfo.iInfo.iFlags & TShPoolCreateInfo::EPageAlignedBuffer)
		{
		pC = new DWin32AlignedShPool();
		}
	else
		{
		pC = new DWin32NonAlignedShPool();
		}

	if (pC == NULL)
		{
		return KErrNoMemory;
		}

	TInt r = pC->Create(this, aInfo);

	if (r == KErrNone)
		{
		aPool = pC;
		}
	else
		{
		pC->Close(NULL);
		}

	return r;
	} // DWin32Process::NewShPool


void DWin32Process::Release()
	{
	DProcess::Release();
	}

void DWin32Process::FinalRelease()
	{
	}

// a process's copy of a DLL's .data/.bss: heap blocks, or (iWsdMap) one memory file of the data region's pages,
// .data then .bss as at the DLL's address, viewed by the kernel at iDataCopy (the switch maps the same file at the
// DLL's address, so both see the same pages and nothing is copied out)
static TInt WsdBytes(DWin32CodeSeg* aSeg)
	{
	return (aSeg->iRealDataSize + aSeg->iRealBssSize + 0xfff) & ~0xfff;
	}

static TBool DllDataNew(DWin32CodeSeg* aSeg, SProcessDllDataBlock& aData)
	{
	aData.iFd = -1;
	aData.iDataCopy = aData.iBssCopy = NULL;
	if (!aSeg->iWsdMap)
		{
		aData.iDataCopy = Kern::Alloc(aSeg->iRealDataSize ? aSeg->iRealDataSize : 4);
		aData.iBssCopy = Kern::Alloc(aSeg->iRealBssSize ? aSeg->iRealBssSize : 4);
		return aData.iDataCopy && aData.iBssCopy;
		}
	TInt fd = hmemfd("wsd");
	if (fd < 0)
		return EFalse;
	TAny* m = hftruncate(fd, WsdBytes(aSeg)) < 0 ? NULL :
	          hmmap(0, WsdBytes(aSeg), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (!m || HMAP_FAILED(m))
		{
		hsys(SYS_close, fd);
		return EFalse;
		}
	aData.iFd = fd;
	aData.iDataCopy = (TUint8*)m + (aSeg->iDataDest & 0xfff);
	aData.iBssCopy = (TUint8*)aData.iDataCopy + aSeg->iRealDataSize;
	return ETrue;
	}

static void DllDataFree(DWin32CodeSeg* aSeg, SProcessDllDataBlock& aData)
	{
	if (aData.iFd < 0)
		{
		Kern::Free(aData.iDataCopy);
		Kern::Free(aData.iBssCopy);
		return;
		}
	// the DLL's address may still map this file (the process was live): it stays valid until the next switch-in
	// of another process maps that one's file; nobody uses it meanwhile (iLiveProcess is NULL)
	hmunmap((TAny*)((TLinAddr)aData.iDataCopy & ~0xfffu), WsdBytes(aSeg));
	hsys(SYS_close, aData.iFd);
	aData.iFd = -1;
	}

TInt DWin32Process::MapCodeSeg(DCodeSeg* aSeg)
	{
	
	__KTRACE_OPT(KDLL,Kern::Printf("Process %O MapCodeSeg(%C)", this, aSeg));
	if (!aSeg || !aSeg->IsDll())
		return KErrNone;
	
	DWin32CodeSeg* seg = (DWin32CodeSeg*)aSeg;
	if (seg->iRealDataSize == 0 && seg->iRealBssSize == 0)
		return KErrNone;
	
	if (!DisableWSDWarning && this!=K::TheKernelProcess) 
        Kern::Printf("WARNING!!: WSD Lib Loaded- Process %O, has loaded Lib %C"
                     " which has %d bytes of WSD",this,aSeg,
                     seg->iRealDataSize + seg->iRealBssSize);
	
	// remove any existing copy of this code seg (should never happen)
	UnmapCodeSeg(aSeg);
	
	SProcessDllDataBlock data;
	data.iCodeSeg = seg;
	
	TInt count=0;
	TInt err = KErrNone;

	// hosted: the copies come from the kernel heap (Win32 used the host heap with scheduling disabled), or, for
	// large data, from the process's own memory file (iWsdMap: the kernel's view of what is mapped on switch-in)
	if (!DllDataNew(seg, data))
		{
		err = KErrNoMemory;
		goto failed;
		}

	memcpy(data.iDataCopy, seg->iDataCopy, seg->iRealDataSize);	// start with init data
	memclr(data.iBssCopy, seg->iRealBssSize);					// initialized to zeros

	NKern::Lock();
	count = iDllData.Count();
	if (count == KMaxWsdDllsPerProcess)
		err = KErrOverflow;
	if (!err)
		err = iDllData.InsertInUnsignedKeyOrder(data);
	NKern::Unlock();
	if (err)
		goto failed;
			
	return KErrNone;	

failed:
	DllDataFree(seg, data);

	return err;
	}

void DWin32Process::UnmapCodeSeg(DCodeSeg* aSeg)
	{
	if (!aSeg || !aSeg->IsDll())
		return;
	
	DWin32CodeSeg* seg = (DWin32CodeSeg*)aSeg;
	if (seg->iRealDataSize == 0 && seg->iRealBssSize == 0)
		return;
		
	SProcessDllDataBlock data;
	data.iCodeSeg = seg;
	NKern::Lock();
	if (seg->iLiveProcess == this)
		seg->iLiveProcess = NULL;
	TInt ix = iDllData.FindInUnsignedKeyOrder(data);
	if (ix >= 0)
		{
		data = iDllData[ix];
		iDllData.Remove(ix);
		}
	NKern::Unlock();
	
	if (ix < 0)
		return;

	DllDataFree(seg, data);

	__KTRACE_OPT(KDLL,Kern::Printf("Process %O UnmapCodeSeg(%C)", this, aSeg));
	}

void DWin32Process::RemoveDllData()
	{
	// unmap all DLL data with kernel locked
	TInt count = iDllData.Count();
	for (TInt ii=count-1; ii>=0; ii--)
		{
		SProcessDllDataBlock data = iDllData[ii];
		NKern::Lock();
		if (data.iCodeSeg->iLiveProcess == this)
			data.iCodeSeg->iLiveProcess = NULL;
		iDllData.Remove(ii);
		NKern::Unlock();
		DllDataFree(data.iCodeSeg, data);
		}
	}

// A second process of an EXE that is already loaded (as DEpocProcess::AttachExistingCodeSeg, pprocess.cpp): the
// code is shared, the process gets its own .data/.bss from the segment's initial image (ROM header or the RAM
// load copy) at the same run address. Bug 2026-11-12: refused here (the Win32 emulator's rule: KErrNotSupported),
// so the 808's start-up list could not start its second cntsrv.exe ("-nontransient") and failed (system state
// 117, the modem reset the phone). Regression: kn_efile (a second instance of a running EXE).
TUint32 HostedExeAttaches, HostedExeAttachFailures;		// kn_efile

TInt DWin32Process::AttachExistingCodeSeg(TProcessCreateInfo& aInfo)
	{
	DWin32CodeSeg& s = *(DWin32CodeSeg*)iTempCodeSeg;
	s.Info(aInfo);
	TInt r = CreateDataBssStackArea(aInfo);
	__KTRACE_OPT(KDLL, Kern::Printf("DWin32Process %O AttachExistingCodeSeg %C: data %08x+%x+%x: %d", this, &s,
		aInfo.iDataRunAddress, aInfo.iDataSize, aInfo.iBssSize, r));
	++(r == KErrNone ? HostedExeAttaches : HostedExeAttachFailures);
	return r;
	}



void DThread::IpcExcHandler(TExcTrap* aTrap, DThread* aThread, TAny* aContext)
	{
	aThread->iIpcClient = 0;
	TIpcExcTrap& xt=*(TIpcExcTrap*)aTrap;
	TWin32ExcInfo& info=*(TWin32ExcInfo*)aContext;
	TLinAddr va=(TLinAddr)info.iExcDataAddress;
	if (va>=xt.iRemoteBase && (va-xt.iRemoteBase)<xt.iSize)
		xt.Exception(KErrBadDescriptor);	// problem accessing remote address - 'leave' so an error code will be returned
	if (xt.iLocalBase && va>=xt.iLocalBase && (va-xt.iLocalBase)<xt.iSize)
		NKern::UnlockSystem();		// problem accessing local address - return and panic current thread as usual
	// otherwise return and fault kernel
	}

TInt DThread::RawRead(const TAny* aSrc, TAny* aDest, TInt aLength, TInt /*aFlags*/, TIpcExcTrap* /*aExcTrap*/)
//
// Read from the thread's process.
// aSrc is run address of memory to read
// aDest is current address of destination
// Enter and leave with system locked
//
	{
	if (iMState==EDead)
		return KErrDied;
	const TUint8* pS=(const TUint8*)aSrc;
	TUint8* pD=(TUint8*)aDest;
	// hosted: another process's local chunk: through its memory file (mlocal.cpp)
	TInt lr = LocalCopy(this, (TLinAddr)aSrc, aDest, aLength, EFalse);
	if (lr <= 0)
		return lr;
	TBool kernelLocked = EFalse;
	const TUint8* pC=(const TUint8*)MM::CurrentAddress(this,pS,aLength,EFalse,kernelLocked);
	// NULL from a non-null address: an EXE data range running past the area. Bug 2026-11-12: a
	// zero-length copy from address 0 (an empty command line) failed with KErrBadDescriptor and the
	// loader refused to create mc_info.exe (kn_efile).
	if (!kernelLocked && !pC && pS)
		return KErrBadDescriptor;
	if (kernelLocked)
		{
		// kernel locked because of DLL WSD IPC, do it all in one big block
		TInt r = KErrNone;
		__KTRACE_OPT(KTHREAD2,Kern::Printf("DThread::RawRead %08x",pC));
		if (!pC)
			r = KErrBadDescriptor;
		else
			memcpy(pD,pC,aLength);
		NKern::Unlock();
		return r;
		}
	TBool check=ETrue;
	while (aLength)
		{
		if (check)
			{
			if (iMState==EDead)
				return KErrDied;
			__KTRACE_OPT(KTHREAD2,Kern::Printf("DThread::Read %08x",pS));
			}
		TInt l=Min(aLength,K::MaxMemCopyInOneGo);
		memcpy(pD,pC,l);
		pD+=l;
		pC+=l;
		aLength-=l;
		if (aLength)
			check=NKern::FlashSystem();
		}
	return KErrNone;
	}

// debugging (htest --env IPCSUM=NAME): every IPC write into a process whose name starts with NAME is logged
// with its length and CRC-32, to compare what a process received with the source file on the host
static TUint32 Crc32(const TUint8* p, TInt n)
	{
	TUint32 c = 0xffffffffu;
	while (n--)
		{
		c ^= *p++;
		for (TInt k = 0; k < 8; ++k)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	return ~c;
	}

static void IpcSum(DThread* aT, const TAny* aSrc, TInt aLength, const TAny* aDest)
	{
	static const char* who = (const char*)1;
	if (who == (const char*)1)
		who = hgetenv("IPCSUM");
	if (!who || !aT->iOwningProcess || !aT->iOwningProcess->iName)
		return;
	const TDesC8& n = *aT->iOwningProcess->iName;
	TInt l = 0;
	while (who[l] && l < n.Length() && who[l] == n[l])
		++l;
	if (who[l])
		return;
	hprintf("IPCSUM %08x %d %08x\n", (TUint32)aDest, aLength, Crc32((const TUint8*)aSrc, aLength));
	}

TInt DThread::RawWrite(const TAny* aDest, const TAny* aSrc, TInt aLength, TInt /*aFlags*/, DThread* /*anOriginatingThread*/, TIpcExcTrap* /*aExcTrap*/)
//
// Write to the thread's process.
// aDest is run address of memory to write
// aSrc is current address of destination
// anOriginatingThread is the thread on behalf of which this operation is performed (eg client of device driver).
// Enter and leave with system locked
//
	{
	if (iMState==EDead)
		return KErrDied;
	TUint8* pD=(TUint8*)aDest;
	const TUint8* pS=(const TUint8*)aSrc;
	IpcSum(this, aSrc, aLength, aDest);
	TInt lr = LocalCopy(this, (TLinAddr)aDest, (TAny*)aSrc, aLength, ETrue);	// as RawRead
	if (lr <= 0)
		return lr;
	TBool kernelLocked = EFalse;
	TUint8* pC=(TUint8*)MM::CurrentAddress(this,pD,aLength,ETrue,kernelLocked);
	if (!kernelLocked && !pC && pD)		// as RawRead
		return KErrBadDescriptor;
	if (kernelLocked)
		{
		// kernel locked because of DLL WSD IPC, do it all in one big block
		TInt r = KErrNone;
		__KTRACE_OPT(KTHREAD2,Kern::Printf("DThread::RawWrite %08x",pC));
		if (!pC)
			r = KErrBadDescriptor;
		else
			memcpy(pC,pS,aLength);
		NKern::Unlock();
		return r;
		}
	TBool check=ETrue;
	while (aLength)
		{
		if (check)
			{
			if (iMState==EDead)
				return KErrDied;
			__KTRACE_OPT(KTHREAD2,Kern::Printf("DThread::Write %08x",pD));
			}
		TInt l=Min(aLength,K::MaxMemCopyInOneGo);
		memcpy(pC,pS,l);
		pC+=l;
		pS+=l;
		aLength-=l;
		if (aLength)
			check=NKern::FlashSystem();
		}
	return KErrNone;
	}

TInt DThread::ReadAndParseDesHeader(const TAny* aSrc, TDesHeader& aDest)
//
// Read and parse the header of a remote descriptor.
// Enter and leave with system locked.
//
	{
	__ASSERT_SYSTEM_LOCK;
	static const TUint8 LengthLookup[16]={4,8,12,8,12,0,0,0,0,0,0,0,0,0,0,0};
	if (iMState == EDead)
		return KErrDied;
	// hosted: a header in another process's local chunk: its first word (the type) gives the length (mlocal.cpp)
	TUint32 hdr[3];
	TInt lr = LocalCopy(this, (TLinAddr)aSrc, hdr, 4, EFalse);
	if (lr < 0)
		return lr;
	if (lr == KErrNone)
		{
		TInt l = (TInt)(hdr[0] >> KShiftDesType8) < 16 ? LengthLookup[hdr[0] >> KShiftDesType8] : 0;
		if (!l || ((TLinAddr)aSrc & 3) || (l > 4 && LocalCopy(this, (TLinAddr)aSrc + 4, hdr + 1, l - 4, EFalse) != KErrNone))
			return KErrBadDescriptor;
		wordmove(&aDest, hdr, l);
		return K::ParseDesHeader(aSrc, (TRawDesHeader&)aDest, aDest);
		}
	TBool kernelLocked = EFalse;
	const TUint32* pS=(const TUint32*)MM::CurrentAddress(this,aSrc,sizeof(TDesC8),EFalse,kernelLocked);
	if (!pS || (TInt(pS)&3)!=0)
		{
		if (kernelLocked)
			NKern::Unlock();
		return KErrBadDescriptor;
		}
	TInt type=0;
	XTRAPD(r, XT_DEFAULT,					\
		type=*pS>>KShiftDesType8;			\
		TInt l=LengthLookup[type];			\
		if (l==0)							\
			r=KErrBadDescriptor;			\
		else								\
			wordmove(&aDest,pS,l);			\
		);
	if (kernelLocked)
		NKern::Unlock();
	if (r!=KErrNone)
		return r;
	return K::ParseDesHeader(aSrc, (TRawDesHeader&)aDest, aDest);
	}

DChunk* DThread::OpenSharedChunk(const TAny* aAddress, TBool /*aWrite*/, TInt& aOffset)
	{
	DWin32Chunk* chunk=0;
	DObjectCon& chunks=*K::Containers[EChunk];
	chunks.Wait();
	TInt count=chunks.Count();
	TInt i;
	TUint offset=0;
	for(i=0;i<count;i++)
		{
		DWin32Chunk* pC=(DWin32Chunk*)chunks[i];
		offset = (TUint)aAddress-(TUint)pC->Base();
		if(offset<TUint(pC->iMaxSize))
			{
			chunk = pC;
			break;
			}
		}
	chunks.Signal();

	if(!chunk)
		return 0;

	if((chunk->iChunkType!=ESharedKernelSingle && chunk->iChunkType!=ESharedKernelMultiple))
		return 0;
	if(chunk->Open()!=KErrNone)
		return 0;
	aOffset = offset;
	return chunk;
	}

TInt DThread::PrepareMemoryForDMA(const TAny* /*aLinAddr*/, TInt /*aSize*/, TPhysAddr* /*aPhysicalPageList*/)
	{
	return KErrNotSupported;
	}

TInt DThread::ReleaseMemoryFromDMA(const TAny* /*aLinAddr*/, TInt /*aSize*/, TPhysAddr* /*aPhysicalPageList*/)
	{
	return KErrNotSupported;
	}

