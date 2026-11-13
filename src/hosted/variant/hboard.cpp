// Hosted EKA2: the hosted board. Drivers written for our syborg board (the ARM QEMU machine) find their
// device pages at the syborg I/O addresses (KPrimaryIOBase 0xC6000000 + device * 0x1000, the peripheral
// window at physical 0xC0000000) and their "physical" windows (ROFS flash at 0x60000000, driver RAM at
// 0x20000000). Register access goes through the I/O bus (include/board/syborg.h routes ReadReg/WriteReg
// here): device models answer with the QEMU syborg devices' semantics (NVMemory: syborg_nvmem.c), other
// pages are plain memory. Interrupts: class Interrupt, delivered by an 'interrupt' host thread. The
// platform device's board-config string (key=value;..., QEMU's syborg-platform.config) is filled in.
// Original work for qembian.

#include <kernel/kern_priv.h>
#include <assp.h>
#include <platform.h>

extern TInt HostedMapPhysicalFile(TPhysAddr aPhys, const char* aPath);

static const TLinAddr KHostedIoBase = 0xC6000000u;		// KPrimaryIOBase (multiple memory model)
static const TUint32 KHostedIoSize = 0x40000;			// 64 device pages
static const TLinAddr KPlatformDevice = KHostedIoBase + 0x11 * 0x1000;	// KHwSVPPlatformDevice
static const TPhysAddr KFlashPhys = 0x60000000u;
static const TPhysAddr KHostedIoPhys = 0xC0000000u;
extern TInt HostedAddPhysicalWindow(TPhysAddr aPhys, TUint32 aSize, TLinAddr aLin);

TInt HostedBoardRam();
TInt HostedNvmemAttach(TInt aIndex, TUint32 aIoBase, TInt aIrq, const char* aImage);

// the drives: C: (NVMemory, device 0x10, interrupt 12) and E: (device 0x0d, interrupt 14), as the board
TInt HostedBoardDrives(const char* aNvmem, const char* aNvmem2)
	{
	TInt r = HostedNvmemAttach(0, KHostedIoBase + 0x10 * 0x1000, 12, aNvmem);
	if (r == KErrNone)
		r = HostedNvmemAttach(1, KHostedIoBase + 0x0d * 0x1000, 14, aNvmem2);
	return r;
	}

// set up before the kernel boots; aBoardConfig: the device profile's syborg-platform.config string
TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage)
	{
	if (TInt r = HostedBoardRam())
		return r;
	TAny* io = hmmap((TAny*)KHostedIoBase, KHostedIoSize, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if ((TLinAddr)io != KHostedIoBase)
		return KErrInUse;
	// the I/O area is the board's peripheral window (KHwBasePeripherals 0xC0000000 on the ARM board)
	if (TInt r = HostedAddPhysicalWindow(KHostedIoPhys, KHostedIoSize, KHostedIoBase))
		return r;
	if (aBoardConfig)
		{
		char* cfg = (char*)(KPlatformDevice + 0x400);
		TInt n = 0;
		while (aBoardConfig[n] && n < 0x3ff)
			{
			cfg[n] = aBoardConfig[n];
			++n;
			}
		cfg[n] = 0;
		}
	if (aRofsImage)
		{
		TInt r = HostedMapPhysicalFile(KFlashPhys, aRofsImage);
		if (r != KErrNone)
			return r;
		}
	return KErrNone;
	}

// ======================================================================== the I/O bus
typedef TUint32 (*TIoRead)(TAny* aCtx, TUint32 aOffset);
typedef void (*TIoWrite)(TAny* aCtx, TUint32 aOffset, TUint32 aValue);
struct SIoDevice { TUint32 iBase, iSize; TIoRead iRead; TIoWrite iWrite; TAny* iCtx; volatile TUint32 iAccesses; };
static SIoDevice IoDevices[16];
static TInt IoDeviceCount;

TInt HostedIoAttach(TUint32 aBase, TUint32 aSize, TIoRead aRead, TIoWrite aWrite, TAny* aCtx)
	{
	if (IoDeviceCount == 16)
		return KErrOverflow;
	SIoDevice& d = IoDevices[IoDeviceCount++];
	d.iBase = aBase; d.iSize = aSize; d.iRead = aRead; d.iWrite = aWrite; d.iCtx = aCtx;
	return KErrNone;
	}

// does [aLin, aLin + aSize) contain a device page with a model (register accesses must go through it)?
TBool HostedIoModelled(TLinAddr aLin, TUint32 aSize)
	{
	for (TInt i = 0; i < IoDeviceCount; ++i)
		if (aLin < IoDevices[i].iBase + IoDevices[i].iSize && IoDevices[i].iBase < aLin + aSize)
			return ETrue;
	return EFalse;
	}

// register accesses a device model has served (tests: drivers reach their device through the bus)
TUint32 HostedIoAccesses(TUint32 aBase)
	{
	for (TInt i = 0; i < IoDeviceCount; ++i)
		if (IoDevices[i].iBase == aBase)
			return IoDevices[i].iAccesses;
	return 0;
	}

TUint32 HostedIoRead(TUint32 aAddr)
	{
	for (TInt i = 0; i < IoDeviceCount; ++i)
		if (aAddr - IoDevices[i].iBase < IoDevices[i].iSize)
			{
			++IoDevices[i].iAccesses;
			return IoDevices[i].iRead(IoDevices[i].iCtx, aAddr - IoDevices[i].iBase);
			}
	return *(volatile TUint32*)aAddr;		// a device without a model: plain memory
	}

void HostedIoWrite(TUint32 aAddr, TUint32 aValue)
	{
	for (TInt i = 0; i < IoDeviceCount; ++i)
		if (aAddr - IoDevices[i].iBase < IoDevices[i].iSize)
			{
			++IoDevices[i].iAccesses;
			IoDevices[i].iWrite(IoDevices[i].iCtx, aAddr - IoDevices[i].iBase, aValue);
			return;
			}
	*(volatile TUint32*)aAddr = aValue;
	}

// ======================================================================== interrupts
// class Interrupt (kernel/arm/assp.h) as the board's interrupt controller: device models raise an
// interrupt line; a delivery thread (an 'interrupt' host thread) runs the bound ISR between
// StartOfInterrupt/EndOfInterrupt while the line is raised and enabled.
static const TInt KMaxIrq = 64;
struct SIrq { TIsr iIsr; TAny* iPtr; volatile TInt iEnabled, iRaised; };
static SIrq Irqs[KMaxIrq];
static volatile TInt IrqSeq;
static TBool IrqThreadStarted;

static TInt IrqThread(TAny*)
	{
	hsys(SYS_prctl, 15 /* PR_SET_NAME */, "h-irq");	// its name in the Linux view (lxtop)
	for (;;)
		{
		TInt seq = __atomic_load_n(&IrqSeq, __ATOMIC_SEQ_CST);
		TBool any = EFalse;
		for (TInt n = 0; n < KMaxIrq; ++n)
			{
			SIrq& q = Irqs[n];
			if (q.iRaised && q.iEnabled && q.iIsr)
				{
				any = ETrue;
				__atomic_store_n(&q.iRaised, 0, __ATOMIC_SEQ_CST);	// edge: the device raises again if needed
				StartOfInterrupt();
				q.iIsr(q.iPtr);
				EndOfInterrupt();
				}
			}
		if (!any)
			hpfutex_wait(&IrqSeq, seq);
		}
	}

void HostedRaiseIrq(TInt aIrq)
	{
	if (aIrq < 0 || aIrq >= KMaxIrq)
		return;
	__atomic_store_n(&Irqs[aIrq].iRaised, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&IrqSeq, 1, __ATOMIC_SEQ_CST);
	hpfutex_wake(&IrqSeq, 1);
	}

TBool HostedIrqRaised(TInt aIrq)		// tests
	{
	return aIrq >= 0 && aIrq < KMaxIrq && Irqs[aIrq].iRaised;
	}

void HostedLowerIrq(TInt aIrq)
	{
	if (aIrq >= 0 && aIrq < KMaxIrq)
		__atomic_store_n(&Irqs[aIrq].iRaised, 0, __ATOMIC_SEQ_CST);
	}

static void StartIrqThread()
	{
	if (!IrqThreadStarted)
		{
		IrqThreadStarted = ETrue;
		CreateLinuxThread(EThreadEvent, &IrqThread, 0, ETrue);
		}
	}

EXPORT_C TInt Interrupt::Bind(TInt anId, TIsr anIsr, TAny* aPtr)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	if (Irqs[anId].iIsr)
		return KErrInUse;
	Irqs[anId].iPtr = aPtr;
	Irqs[anId].iIsr = anIsr;
	StartIrqThread();
	return KErrNone;
	}

EXPORT_C TInt Interrupt::Unbind(TInt anId)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	if (!Irqs[anId].iIsr)
		return KErrGeneral;
	Irqs[anId].iEnabled = 0;
	Irqs[anId].iIsr = NULL;
	return KErrNone;
	}

EXPORT_C TInt Interrupt::Enable(TInt anId)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	if (!Irqs[anId].iIsr)
		return KErrGeneral;
	Irqs[anId].iEnabled = 1;
	if (Irqs[anId].iRaised)
		HostedRaiseIrq(anId);		// a pending line fires once enabled
	return KErrNone;
	}

EXPORT_C TInt Interrupt::Disable(TInt anId)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	Irqs[anId].iEnabled = 0;
	return KErrNone;
	}

EXPORT_C TInt Interrupt::Clear(TInt anId)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	return KErrNone;		// edge delivery: nothing latched
	}

EXPORT_C TInt Interrupt::SetPriority(TInt, TInt)
	{
	return KErrNotSupported;
	}

// ======================================================================== physical RAM (DMA buffers)
extern TInt HostedAddPhysicalWindow(TPhysAddr aPhys, TUint32 aSize, TLinAddr aLin);
extern TLinAddr HostedPhysicalToLinear(TPhysAddr aPhys, TUint32 aSize);
static const TPhysAddr KRamPhys = 0x20000000u;		// a "physical" range for driver buffers
static const TUint32 KRamSize = 0x02000000u;		// 32 MB
static TUint32 RamTop;
static volatile TInt RamLock;

// a memory file, so that a physical commit (Kern::ChunkCommitPhysical: a driver's buffer in a chunk that user
// code maps) shows the same pages at the chunk's address (HostedPhysicalRamFd, mchunk.cpp)
static TInt RamFd = -1;
static TInt BoardRamInit()
	{
	RamFd = hmemfd("board-ram");
	if (RamFd < 0 || hftruncate(RamFd, KRamSize) < 0)
		return KErrNoMemory;
	TAny* m = hmmap(0, KRamSize, PROT_READ | PROT_WRITE, MAP_SHARED, RamFd, 0);
	if (HMAP_FAILED(m))
		return KErrNoMemory;
	return HostedAddPhysicalWindow(KRamPhys, KRamSize, (TLinAddr)m);
	}

// the memory file behind physical RAM [aPhys, aPhys + aSize) and its offset there, or -1 (not board RAM)
TInt HostedPhysicalRamFd(TPhysAddr aPhys, TUint32 aSize, TUint32& aOffset)
	{
	if (RamFd < 0 || aPhys < KRamPhys || aPhys - KRamPhys >= KRamSize || aSize > KRamSize - (aPhys - KRamPhys))
		return -1;
	aOffset = aPhys - KRamPhys;
	return RamFd;
	}

// bump allocation (driver buffers live for the system's life; frees are not reused yet)
EXPORT_C TInt Epoc::AllocPhysicalRam(TInt aSize, TPhysAddr& aPhysAddr, TInt aAlign)
	{
	TUint32 align = aAlign > 12 ? (1u << aAlign) : 0x1000;
	while (__atomic_exchange_n(&RamLock, 1, __ATOMIC_ACQUIRE)) {}
	TUint32 at = (RamTop + align - 1) & ~(align - 1);
	TInt r = KErrNoMemory;
	if (aSize > 0 && at + (TUint32)aSize <= KRamSize)
		{
		RamTop = at + ((aSize + 0xfff) & ~0xfff);
		aPhysAddr = KRamPhys + at;
		r = KErrNone;
		}
	__atomic_store_n(&RamLock, 0, __ATOMIC_RELEASE);
	return r;
	}

EXPORT_C TInt Epoc::FreePhysicalRam(TPhysAddr, TInt)
	{
	return KErrNone;
	}

// ======================================================================== NVMemory (syborg-nvmem)
// as src/qemu-syborg/hw/arm/syborg_nvmem.c: registers (word index) ID 0, OFFSET 4, SIZE 8, DIRECTION 12,
// EXECUTE 16, SHM_BASE 20, NV_SIZE 24, SHM_SIZE 28, STATUS 32 (read lowers the interrupt), ENABLE 36;
// 512-byte sectors. The drive image is mapped private (copy-on-write): writes never reach the file.
// The drive image is read with pread64 and never written: a write goes into a copy-on-write overlay of
// 64 KB blocks in memory (the run's changes are lost at its end, as with the private mapping before).
// Bug 2026-11-12: the image was mapped whole and its size kept in 32 bits, so the 808's 8 GB E: could not
// be attached in a 32-bit process (kn_efile: "NVMemory drives" failed). Regression: kn_board (a 5 GB image).
struct SNvmem
	{
	TInt iFd;
	TUint64 iBytes;
	TUint8** iBlocks;			// overlay: one pointer per 64 KB block, NULL = the image's own bytes
	TInt iIrq;
	TUint32 iOffset, iSize, iDirection, iShmBase, iShmSize, iStatus, iEnabled;
	};
static SNvmem Nvmems[2];
static const TUint32 KSector = 512;
static const TUint32 KBlockShift = 16, KBlock = 1u << KBlockShift;

static TInt ImageRead(SNvmem& s, TUint64 aPos, TUint8* aBuf, TUint32 aLen)
	{
	while (aLen)
		{
		TInt r = hsys(180 /* pread64 */, s.iFd, aBuf, aLen, 0, (TUint32)aPos, (TUint32)(aPos >> 32));
		if (r <= 0)
			return KErrCorrupt;
		aBuf += r; aPos += r; aLen -= r;
		}
	return KErrNone;
	}

// copy between the drive and a buffer, block by block (overlay first, else the image)
static TInt NvmemCopy(SNvmem& s, TUint64 aPos, TUint8* aBuf, TUint32 aLen, TBool aWrite)
	{
	while (aLen)
		{
		TUint32 b = (TUint32)(aPos >> KBlockShift), in = (TUint32)aPos & (KBlock - 1);
		TUint32 n = KBlock - in < aLen ? KBlock - in : aLen;
		TUint8* blk = s.iBlocks[b];
		if (aWrite && !blk)
			{
			TAny* m = hmmap(0, KBlock, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (HMAP_FAILED(m))
				return KErrNoMemory;
			blk = (TUint8*)m;
			TUint64 start = (TUint64)b << KBlockShift;
			TUint32 have = s.iBytes - start < KBlock ? (TUint32)(s.iBytes - start) : KBlock;
			if (ImageRead(s, start, blk, have) != KErrNone)
				return KErrCorrupt;
			s.iBlocks[b] = blk;
			}
		if (aWrite)
			memcpy(blk + in, aBuf, n);
		else if (blk)
			memcpy(aBuf, blk + in, n);
		else if (ImageRead(s, aPos, aBuf, n) != KErrNone)
			return KErrCorrupt;
		aBuf += n; aPos += n; aLen -= n;
		}
	return KErrNone;
	}

static TUint32 NvmemRead(TAny* aCtx, TUint32 aOff)
	{
	SNvmem& s = *(SNvmem*)aCtx;
	switch (aOff >> 2)
		{
	case 0x00: return 0xdeadbeefu;			// SYBORG_ID_NVMEM (syborg.h; not checked by the driver)
	case 0x04: return s.iOffset;
	case 0x08: return s.iSize;
	case 0x0c: return s.iDirection;
	case 0x14: return s.iShmBase;
	case 0x1c: return s.iShmSize;
	case 0x18: return (TUint32)(s.iBytes / KSector);
	case 0x20: HostedLowerIrq(s.iIrq); return s.iStatus;
	default: return 0;
		}
	}

// GLSTAT: drive transfers (reads, writes, bytes, ns spent in them): is storage what the phone waits on?
TUint32 HostedNvReads, HostedNvWrites, HostedNvBytes;
TUint64 HostedNvNs;

static void NvmemExecute(SNvmem& s)
	{
	TUint64 t0 = hnow_ns();
	TUint64 bytes = (TUint64)s.iSize * KSector;
	TUint64 pos = (TUint64)s.iOffset * KSector;
	TUint8* shm = (TUint8*)HostedPhysicalToLinear(s.iShmBase, s.iShmSize);
	TInt ok = s.iBlocks && s.iSize && bytes <= s.iShmSize && pos + bytes <= s.iBytes && shm &&
	          (s.iDirection == 1 || s.iDirection == 2);
	if (ok)
		ok = NvmemCopy(s, pos, shm, (TUint32)bytes, s.iDirection == 2) == KErrNone;
	s.iStatus = ok ? s.iSize : 0xffffffffu;
	s.iDirection == 2 ? ++HostedNvWrites : ++HostedNvReads;
	HostedNvBytes += (TUint32)bytes;
	HostedNvNs += hnow_ns() - t0;
	HostedRaiseIrq(s.iIrq);
	}

static void NvmemWrite(TAny* aCtx, TUint32 aOff, TUint32 aValue)
	{
	SNvmem& s = *(SNvmem*)aCtx;
	switch (aOff >> 2)
		{
	case 0x04: s.iOffset = aValue; break;
	case 0x08: s.iSize = aValue; break;
	case 0x0c: s.iDirection = aValue; break;
	case 0x10: NvmemExecute(s); break;
	case 0x14: s.iShmBase = aValue; break;
	case 0x1c: s.iShmSize = aValue; break;
	case 0x24: s.iEnabled = aValue; break;
	default: break;
		}
	}

// debugging: the drive's overlay (the run's writes) as a delta file: per changed 64 KB block, a u32 block number
// and its 64 KB (tools/hosted/nvdelta.py applies it to a copy of the image). Returns the blocks written or < 0.
TInt HostedNvmemSaveDelta(TInt aIndex, const char* aPath)
	{
	SNvmem& s = Nvmems[aIndex];
	if (!s.iBlocks)
		return KErrNotReady;
	TInt fd = hsys(SYS_openat, -100, aPath, 0x41 | 0x200 | 0x20000, 0644);
	if (fd < 0)
		return fd;
	TInt n = 0;
	TUint32 blocks = (TUint32)((s.iBytes + KBlock - 1) >> KBlockShift);
	for (TUint32 b = 0; b < blocks; ++b)
		if (s.iBlocks[b])
			{
			hsys(SYS_write, fd, &b, 4);
			hsys(SYS_write, fd, s.iBlocks[b], KBlock);
			++n;
			}
	hsys(SYS_close, fd);
	return n;
	}

// an NVMemory device at its syborg I/O page with its interrupt, backed by a drive image of any size
TInt HostedNvmemAttach(TInt aIndex, TUint32 aIoBase, TInt aIrq, const char* aImage)
	{
	SNvmem& s = Nvmems[aIndex];
	s.iIrq = aIrq;
	if (aImage && aImage[0])
		{
		// O_LARGEFILE: a 32-bit process gets EOVERFLOW past 2 GB without it (arm64 does not force it for compat tasks)
		TInt fd = hsys(SYS_openat, -100, aImage, 0x20000 /* O_LARGEFILE */, 0);
		if (fd < 0)
			return fd;
		TUint64 size = 0;
		if (hsys(140 /* _llseek */, fd, 0, 0, &size, 2 /* SEEK_END */) < 0 || size < KSector)
			{
			hsys(SYS_close, fd);
			return KErrCorrupt;
			}
		TUint32 blocks = (TUint32)((size + KBlock - 1) >> KBlockShift);
		TAny* t = hmmap(0, blocks * sizeof(TUint8*), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (HMAP_FAILED(t))
			{
			hsys(SYS_close, fd);
			return KErrNoMemory;
			}
		s.iFd = fd;
		s.iBytes = size;
		s.iBlocks = (TUint8**)t;
		}
	return HostedIoAttach(aIoBase, 0x1000, &NvmemRead, &NvmemWrite, &s);
	}

TInt HostedBoardRam()
	{
	return BoardRamInit();
	}

// ======================================================================== framebuffer (syborg-fb)
// as src/qemu-syborg/hw/arm/syborg_fb.c: registers by word index (ID 0, BASE 1 = physical address of the
// pixels, HEIGHT 2, WIDTH 3, ..., BPP 8, ROW_PITCH 12, ENABLED 13, frame count 17, refresh rate 18;
// palette 0x100..0x1ff), size from the device profile (syborg-fb.width/height). The pixels are the
// driver's own video RAM (a physical RAM window): HostedFbPixels() finds them. Interrupt (EIntFb 4) as QEMU's:
// INT_MASK 6 (1 VSYNC, 2 BASE_UPDATE), INT_CAUSE 7 (write 1 to clear); the line is high while CAUSE & MASK.
// VSYNC comes from a host thread at REFRESH_RATE (18) Hz, which also counts FRAME_COUNT (17); BASE_UPDATE at
// the next vsync after a BASE write. Bug 2026-11-12: no interrupt at all, so the N95 display stand-in, which
// copies posted frames and completes EndUpdate and vsync waiters on VSYNC, never did on the hosted board
// (the screensaver's picture never reached the screen). Regression: kn_board (vsync).
static volatile TUint32 FbRegs[0x200];
static const TInt KFbIrq = 4;				// EIntFb (syborg.h)
static volatile TInt FbNeedBaseInt, FbTid;

// the line as a level: the edge-style interrupt model is raised whenever an enabled cause is pending
static void FbUpdateIrq()
	{
	if (FbRegs[7] & FbRegs[6])
		HostedRaiseIrq(KFbIrq);
	}

static int FbVsyncThread(void*)
	{
	hsys(SYS_prctl, 15 /* PR_SET_NAME */, "h-vsync");	// its name in the Linux view (lxtop)
	for (;;)
		{
		TUint32 hz = FbRegs[18] ? FbRegs[18] : 60;
		hsleep_us(1000000 / hz);
		__atomic_add_fetch(&FbRegs[17], 1, __ATOMIC_SEQ_CST);
		TUint32 cause = 1 | (__atomic_exchange_n(&FbNeedBaseInt, 0, __ATOMIC_SEQ_CST) ? 2 : 0);
		__atomic_or_fetch(&FbRegs[7], cause, __ATOMIC_SEQ_CST);
		FbUpdateIrq();
		}
	}

static TUint32 FbRead(TAny*, TUint32 aOff)
	{
	TUint32 r = aOff >> 2;
	if (r == 0)
		return 0xc51d0006u;				// SYBORG_ID_FB
	if (r == 17)
		return FbRegs[17];				// frame count: vsyncs since attach
	return r < 0x200 ? FbRegs[r] : 0;
	}

static void FbWrite(TAny*, TUint32 aOff, TUint32 aValue)
	{
	TUint32 r = aOff >> 2;
	if (r == 7)
		{
		__atomic_and_fetch(&FbRegs[7], ~aValue, __ATOMIC_SEQ_CST);	// interrupt cause: write 1 to clear
		FbUpdateIrq();
		}
	else if (r == 6)
		{
		FbRegs[6] = aValue & 3;
		FbUpdateIrq();
		}
	else if (r == 1)
		{
		FbRegs[1] = aValue;
		FbNeedBaseInt = 1;
		}
	else if (r == 4)
		FbRegs[4] = aValue & 3;
	else if (r && r != 17 && r != 18 && r < 0x200)
		FbRegs[r] = aValue;
	}

TInt HostedFbAttach(TInt aWidth, TInt aHeight)
	{
	FbRegs[2] = aHeight;
	FbRegs[3] = aWidth;
	FbRegs[8] = 32;
	FbRegs[18] = 60;
	if (!FbTid)
		{
		TAny* stack = hmmap(0, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (HMAP_FAILED(stack) || hthread(&FbVsyncThread, NULL, stack, 0x4000, &FbTid) < 0)
			return KErrNoMemory;
		}
	return HostedIoAttach(KHostedIoBase + 0x05 * 0x1000, 0x1000, &FbRead, &FbWrite, NULL);	// KHwBaseClcd
	}

// the visible pixels (enabled, base set), or NULL; geometry out
const TUint32* HostedFbPixels(TInt& aWidth, TInt& aHeight, TInt& aPitch)
	{
	aWidth = FbRegs[3];
	aHeight = FbRegs[2];
	aPitch = FbRegs[12] ? FbRegs[12] : aWidth * 4;
	if (!FbRegs[13] || !FbRegs[1])
		return NULL;
	return (const TUint32*)HostedPhysicalToLinear(FbRegs[1], aPitch * aHeight);
	}

// The screen as the panel shows it (the QEMU device's model, src/qemu-syborg/hw/arm/syborg_fb.c): the picture
// in memory is WIDTH x HEIGHT at ORIENTATION 0/2 and HEIGHT x WIDTH at 1/3 and is turned ORIENTATION quarter
// turns clockwise onto the screen (the N95's native-landscape panel: the portrait UI at 3, the screensaver's
// landscape picture at 0, H-lcd-native-landscape); in partial mode (PARTIAL_START/END/MODE, the panel's
// power-saving idle mode the N95 screensaver uses) only the band is shown, in 8 colours without the full-colour
// bit; a blanked or disabled panel keeps its picture, dimmed to aDimPercent. Bug 2026-11-12: the hosted board
// stored these registers but showed memory as it is, so the N95 screensaver's landscape picture came out cut
// and sheared. Regression: kn_board (fbcompose).
void HostedFbCompose(const TUint32* aPic, TInt aPitch, TInt aW, TInt aH, TUint32 aRot, TUint32 aPartialMode,
                     TUint32 aStart, TUint32 aEnd, TBool aDim, TInt aDimPercent, TUint32* aOut)
	{
	aRot &= 3;
	TInt pw = (aRot & 1) ? aH : aW, ph = (aRot & 1) ? aW : aH;
	TBool partial = aPartialMode & 1, eight = partial && !(aPartialMode & 2), rows = aPartialMode & 4;
	for (TInt y = 0; y < aH; ++y)
		for (TInt x = 0; x < aW; ++x)
			{
			TInt px, py;
			switch (aRot)
				{
			case 1: px = y; py = ph - 1 - x; break;		// picture top -> screen right
			case 2: px = pw - 1 - x; py = ph - 1 - y; break;
			case 3: px = pw - 1 - y; py = x; break;		// picture top -> screen left
			default: px = x; py = y; break;
				}
			TUint32 p = ((const TUint32*)((const TUint8*)aPic + py * aPitch))[px] & 0xffffff;
			if (partial)
				{
				TUint32 c = rows ? (TUint32)py : (TUint32)px;
				if (c < aStart || c > aEnd)
					p = 0;
				else if (eight)
					p = (p & 0x800000 ? 0xff0000 : 0) | (p & 0x8000 ? 0xff00 : 0) | (p & 0x80 ? 0xff : 0);
				}
			if (aDim)
				p = ((p >> 16 & 0xff) * aDimPercent / 100) << 16 | ((p >> 8 & 0xff) * aDimPercent / 100) << 8
				  | (p & 0xff) * aDimPercent / 100;
			aOut[y * aW + x] = p;
			}
	}

// the screen when the panel shows the picture as it is (orientation 0, no partial mode, not dimmed): the picture
// (the host-composed frame, else video RAM), its geometry, and the frame's sequence (0: video RAM). The display
// thread then copies changed rows straight from it, and skips an unchanged host frame altogether (2026-11-13:
// composing every pixel 33 times a second took ~19% of a CPU on the 808). NULL when the slow path is needed.
const TUint32* HostedFbPlain(TInt& aWidth, TInt& aHeight, TInt& aPitch, TUint32& aGlSeq)
	{
	aWidth = FbRegs[3];
	aHeight = FbRegs[2];
	aGlSeq = 0;
	if ((FbRegs[4] & 3) || (FbRegs[16] & 1) || !FbRegs[13] || FbRegs[5] || !FbRegs[1] || (FbRegs[8] && FbRegs[8] != 32))
		return NULL;
	extern TBool HostedGlFrame(TUint32& aSeq, TInt& aW, TInt& aH, const TUint32*& aPixels);
	TUint32 seq;
	TInt hw, hh;
	const TUint32* hpx;
	if (HostedGlFrame(seq, hw, hh, hpx) && hw == aWidth && hh == aHeight)
		{
		aPitch = hw * 4;
		aGlSeq = seq;
		return hpx;
		}
	aPitch = FbRegs[12] ? FbRegs[12] : aWidth * 4;
	return (const TUint32*)HostedPhysicalToLinear(FbRegs[1], aPitch * aHeight);
	}

// the board's screen into aOut (WIDTH x HEIGHT words): EFalse when there is no picture (no base, not 32 bpp)
TBool HostedFbScreen(TUint32* aOut, TInt aMaxPixels, TInt& aWidth, TInt& aHeight)
	{
	aWidth = FbRegs[3];
	aHeight = FbRegs[2];
	TUint32 rot = FbRegs[4] & 3;
	TInt pw = (rot & 1) ? aHeight : aWidth, ph = (rot & 1) ? aWidth : aHeight;
	TInt pitch = FbRegs[12] ? FbRegs[12] : pw * 4;
	if (!FbRegs[1] || (FbRegs[8] && FbRegs[8] != 32) || aWidth <= 0 || aHeight <= 0 || aWidth * aHeight > aMaxPixels)
		return EFalse;
	// the host-composed screen when the guest composes on the host GL server (as QEMU's syborg-fb: used when its
	// size is the picture's), else the guest's video RAM. Bug 2026-11-12: the 808 window stayed black.
	extern TBool HostedGlFrame(TUint32& aSeq, TInt& aW, TInt& aH, const TUint32*& aPixels);
	extern TUint32 HostedGlFrameSeq();
	TUint32 seq;
	TInt hw, hh;
	const TUint32* hpx;
	for (TInt tries = 0; tries < 3 && HostedGlFrame(seq, hw, hh, hpx) && hw == pw && hh == ph; ++tries)
		{
		HostedFbCompose(hpx, hw * 4, aWidth, aHeight, rot, FbRegs[16] & 7, FbRegs[14], FbRegs[15],
		                !FbRegs[13] || FbRegs[5], 25, aOut);
		if (HostedGlFrameSeq() == seq)
			return ETrue;
		}
	const TUint32* pic = (const TUint32*)HostedPhysicalToLinear(FbRegs[1], pitch * ph);
	if (!pic)
		return EFalse;
	HostedFbCompose(pic, pitch, aWidth, aHeight, rot, FbRegs[16] & 7, FbRegs[14], FbRegs[15],
	                !FbRegs[13] || FbRegs[5], 25, aOut);
	return ETrue;
	}
