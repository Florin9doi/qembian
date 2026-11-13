// kn_board: the hosted board's device models without a phone ROM: the I/O bus, the NVMemory device
// (syborg-nvmem register semantics: transfers, status, interrupt, errors; the drive image mapped
// private, never written back), driver physical RAM and the physical windows. Each case is a regression
// or edge case of src/hosted/variant/hboard.cpp and src/hosted/memmodel/linux/mphys.cpp.
#include <kernel/kern_priv.h>

extern TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage);
extern TInt HostedNvmemAttach(TInt aIndex, TUint32 aIoBase, TInt aIrq, const char* aImage);
extern TUint32 HostedIoRead(TUint32 aAddr);
extern void HostedIoWrite(TUint32 aAddr, TUint32 aValue);
extern TBool HostedIrqRaised(TInt aIrq);
extern TLinAddr HostedPhysicalToLinear(TPhysAddr aPhys, TUint32 aSize);

static const TUint32 KIo = 0xC6000000u;
static const TUint32 KNv = KIo + 0x10 * 0x1000;		// device 0x10, as the board's C:
static const TInt KIrq = 12;
enum { ROff = 0x04, RSize = 0x08, RDir = 0x0c, RExec = 0x10, RShm = 0x14, RNvSize = 0x18, RShmSize = 0x1c, RStatus = 0x20 };

// as the driver reaches them: syborg.h ReadReg/WriteReg, base + (register << 2)
static TUint32 Rd(TInt aReg) { return HostedIoRead(KNv + (aReg << 2)); }
static void Wr(TInt aReg, TUint32 aV) { HostedIoWrite(KNv + (aReg << 2), aV); }

static TUint32 Transfer(TUint32 aSector, TUint32 aCount, TUint32 aDir, TPhysAddr aShm, TUint32 aShmSize)
	{
	Wr(ROff, aSector); Wr(RSize, aCount); Wr(RDir, aDir); Wr(RShm, aShm); Wr(RShmSize, aShmSize);
	Wr(RExec, aDir);
	return Rd(RStatus);
	}

int main(int, char**)
	{
	// a 64-sector drive image, sector n filled with byte n
	const char* img = "/tmp/kn_board.img";
	TInt fd = hsys(SYS_openat, -100, img, 0x41 | 0x200 /* O_WRONLY|O_CREAT|O_TRUNC */, 0644);
	HCHECK(fd >= 0, "drive image created (%d)", fd);
	TUint8 sec[512];
	for (TInt n = 0; n < 64; ++n)
		{
		memset(sec, n, sizeof sec);
		hsys(SYS_write, fd, sec, sizeof sec);
		}
	hsys(SYS_close, fd);

	HCHECK(HostedBoardInit(NULL, NULL) == KErrNone, "board: I/O area at %08x", KIo);
	// I/O bus: an address without a device model is plain memory
	HostedIoWrite(KIo + 0x3000, 0x12345678);
	HCHECK(HostedIoRead(KIo + 0x3000) == 0x12345678, "unmodelled device page reads back what was written");
	HCHECK(Epoc::LinearToPhysical(KIo + 0x3004) == 0xC0003004u, "I/O area is the peripheral window (phys %08x)",
	       Epoc::LinearToPhysical(KIo + 0x3004));
	HCHECK(Epoc::LinearToPhysical(0x00001000) == KPhysAddrInvalid, "no window: KPhysAddrInvalid");

	// driver physical RAM
	TPhysAddr a = 0, b = 0;
	HCHECK(Epoc::AllocPhysicalRam(0x40000, a) == KErrNone, "AllocPhysicalRam 256 KB -> %08x", a);
	HCHECK(Epoc::AllocPhysicalRam(0x1000, b, 16) == KErrNone && (b & 0xffff) == 0, "aligned allocation (64 KB) -> %08x", b);
	TPhysAddr c = 0;
	HCHECK(Epoc::AllocPhysicalRam(0x7fffffff, c) == KErrNoMemory, "an allocation larger than the RAM fails");
	HCHECK(Epoc::AllocPhysicalRam(0, c) == KErrNoMemory, "a zero-size allocation fails");
	TUint8* shm = (TUint8*)HostedPhysicalToLinear(a, 0x40000);
	HCHECK(shm != NULL, "the buffer is reachable (lin %08x)", shm);
	HCHECK(Epoc::LinearToPhysical((TLinAddr)shm + 0x200) == a + 0x200, "linear -> physical round trip");

	// NVMemory
	HCHECK(HostedNvmemAttach(0, KNv, KIrq, img) == KErrNone, "NVMemory attached with %s", img);
	HCHECK(Rd(RNvSize) == 64, "NV_MEMORY_SIZE in sectors (%u)", Rd(RNvSize));
	HCHECK(!HostedIrqRaised(KIrq), "no interrupt before a transfer");
	TUint32 st = Transfer(3, 2, 1, a, 0x40000);
	HCHECK(st == 2, "read of 2 sectors: status = sector count (%d)", st);
	HCHECK(shm[0] == 3 && shm[511] == 3 && shm[512] == 4 && shm[1023] == 4, "read data: sectors 3 and 4");
	Wr(ROff, 0);
	Wr(RExec, 1);
	HCHECK(HostedIrqRaised(KIrq), "a transfer raises the interrupt");
	(void)Rd(RStatus);
	HCHECK(!HostedIrqRaised(KIrq), "reading STATUS lowers it");
	memset(shm, 0xab, 512);
	st = Transfer(10, 1, 2, a, 0x40000);
	HCHECK(st == 1, "write of 1 sector (%d)", st);
	memset(shm, 0, 512);
	st = Transfer(10, 1, 1, a, 0x40000);
	HCHECK(st == 1 && shm[0] == 0xab && shm[511] == 0xab, "the write reads back");
	fd = hsys(SYS_openat, -100, img, 0, 0);
	hsys(19 /* lseek */, fd, 10 * 512, 0);
	TUint8 x = 0;
	hsys(SYS_read, fd, &x, 1);
	hsys(SYS_close, fd);
	HCHECK(x == 10, "the image file is unchanged (private mapping): %02x", x);
	HCHECK(Transfer(63, 1, 1, a, 0x40000) == 1, "the last sector reads");
	HCHECK(Transfer(63, 2, 1, a, 0x40000) == 0xffffffffu, "a transfer past the end fails (-1)");
	HCHECK(Transfer(0, 2, 1, a, 512) == 0xffffffffu, "a transfer larger than the shared buffer fails");
	HCHECK(Transfer(0, 1, 3, a, 0x40000) == 0xffffffffu, "an unknown direction fails");
	HCHECK(Transfer(0, 0, 1, a, 0x40000) == 0xffffffffu, "a zero-sector transfer fails");
	HCHECK(Transfer(0, 1, 1, 0x00001000, 0x40000) == 0xffffffffu, "a buffer outside every window fails");
	HCHECK(Rd(ROff) == 0 && Rd(RSize) == 1 && Rd(RDir) == 1, "registers read back");

	// a drive image larger than 4 GB (the 808's 8 GB E:). Bug 2026-11-12: the image was mapped whole with a
	// 32-bit size and could not be attached in a 32-bit process. Sparse 5 GB file, marker past 4 GB.
	{
	const char* big = "/tmp/kn_board_big.img";
	TInt bfd = hsys(SYS_openat, -100, big, 0x42 | 0x200 | 0x20000 /* O_LARGEFILE */, 0644);
	const TUint64 bigSize = 5ull << 30, mark = (9ull << 29);		// 5 GB, marker at 4.5 GB
	hsys(194 /* ftruncate64 */, bfd, 0, (TUint32)bigSize, (TUint32)(bigSize >> 32));
	TUint8 m[512];
	memset(m, 0x5a, sizeof m);
	hsys(181 /* pwrite64 */, bfd, m, sizeof m, 0, (TUint32)mark, (TUint32)(mark >> 32));
	hsys(SYS_close, bfd);
	const TUint32 nv2 = KIo + 0x0d * 0x1000;
	HCHECK(HostedNvmemAttach(1, nv2, 14, big) == KErrNone, "a 5 GB drive image attaches");
	HCHECK(HostedIoRead(nv2 + (RNvSize << 2)) == (TUint32)(bigSize / 512), "its size in sectors (%u)", HostedIoRead(nv2 + (RNvSize << 2)));
	TUint32 sec = (TUint32)(mark / 512);
	auto T2 = [&](TUint32 aSec, TUint32 aDir) {
		HostedIoWrite(nv2 + (ROff << 2), aSec); HostedIoWrite(nv2 + (RSize << 2), 1); HostedIoWrite(nv2 + (RDir << 2), aDir);
		HostedIoWrite(nv2 + (RShm << 2), a); HostedIoWrite(nv2 + (RShmSize << 2), 0x40000); HostedIoWrite(nv2 + (RExec << 2), aDir);
		return HostedIoRead(nv2 + (RStatus << 2)); };
	memset(shm, 0, 512);
	HCHECK(T2(sec, 1) == 1 && shm[0] == 0x5a && shm[511] == 0x5a, "a sector past 4 GB reads the image's bytes");
	memset(shm, 0xc3, 512);
	HCHECK(T2(sec + 1, 2) == 1, "a write past 4 GB goes to the overlay");
	memset(shm, 0, 512);
	HCHECK(T2(sec + 1, 1) == 1 && shm[0] == 0xc3 && shm[511] == 0xc3, "and reads back");
	HCHECK(T2(sec, 1) == 1 && shm[0] == 0x5a, "the neighbouring sector in the same block is still the image's");
	bfd = hsys(SYS_openat, -100, big, 0x20000, 0);
	TUint8 x2 = 0;
	TUint64 p2 = mark + 512;
	hsys(180 /* pread64 */, bfd, &x2, 1, 0, (TUint32)p2, (TUint32)(p2 >> 32));
	hsys(SYS_close, bfd);
	HCHECK(x2 == 0, "the image file itself is unchanged (%02x)", x2);
	HCHECK(T2((TUint32)(bigSize / 512), 1) == 0xffffffffu, "the sector after the end fails");
	}

	// syborg-snd sink: a ring drains at its playback rate, watermark and end-of-stream interrupts
	extern TInt HostedSndAttach();
	HCHECK(HostedSndAttach() == KErrNone, "sound device attached");
	const TUint32 snd = KIo + 0x0e * 0x1000, sl = snd + 0x100;		// slot 0
	HCHECK(HostedIoRead(snd) == 0xc51d0012u, "sound device ID");
	HostedIoWrite(snd + 7 * 4, 1);				// interrupt enable, slot 0
	HostedIoWrite(sl + 1 * 4, 0);				// S16
	HostedIoWrite(sl + 2 * 4, 48000);
	HostedIoWrite(sl + 3 * 4, 2);				// 192000 bytes a second
	HostedIoWrite(sl + 4 * 4, a);
	HostedIoWrite(sl + 5 * 4, 19200);			// 100 ms of ring
	HostedIoWrite(sl + 8 * 4, 9600);			// watermark: half free
	HostedIoWrite(sl + 0 * 4, 1);				// start
	HCHECK(HostedIoRead(sl + 11 * 4) & 1, "slot running after START");
	HostedIoWrite(sl + 6 * 4, 19200);			// a full ring written
	TUint64 t0 = hnow_ns();
	hsleep_us(60000);
	TUint32 rp = HostedIoRead(sl + 7 * 4);
	TUint32 ms = (TUint32)((hnow_ns() - t0) / 1000000);
	HCHECK(rp >= 9600 && rp <= 19200, "READ_POS advanced at the playback rate: %u bytes in %u ms", rp, ms);
	HCHECK(HostedIrqRaised(13) && (HostedIoRead(snd + 6 * 4) & 1), "watermark interrupt raised");
	HostedIoWrite(snd + 6 * 4, 1);				// acknowledge
	HostedIoWrite(sl + 0 * 4, 16);				// EOS
	hsleep_us(150000);
	TUint32 sst = HostedIoRead(sl + 11 * 4);
	HCHECK((sst & 2) && !(sst & 1) && HostedIoRead(sl + 7 * 4) == 19200, "drained after EOS: stopped, EOS flagged (%x)", sst);
	HostedIoWrite(sl + 5 * 4, 4);				// ring too small
	HostedIoWrite(sl + 0 * 4, 1);
	HCHECK(HostedIoRead(sl + 11 * 4) & 8, "an invalid ring is refused (error flag)");

	// syborg-keyboard (hkeyb.cpp, QEMU syborg_input.c semantics) and the phone window's key ring (hdisplay.cpp)
	extern TInt HostedKeybAttach();
	extern void HostedKeybPush(TUint32 aCode);
	extern TInt HostedKeybDropped;
	extern TInt HostedDisplayStart(const char* aPath);
	const TUint32 kb = KIo + 0x03 * 0x1000;
	HCHECK(HostedKeybAttach() == KErrNone, "keyboard attached");
	HCHECK(HostedIoRead(kb) == 0xc51d0002u && HostedIoRead(kb + 4 * 4) == 64, "keyboard ID, FIFO size 64");
	HCHECK(HostedIoRead(kb + 2 * 4) == 0 && HostedIoRead(kb + 1 * 4) == 0xffffffffu, "empty FIFO: count 0, DATA reads -1");
	HostedKeybPush(0x40000010);
	HCHECK(!HostedIrqRaised(2), "no interrupt while INT_ENABLE is 0");
	HostedIoWrite(kb + 3 * 4, 1);
	HCHECK(HostedIrqRaised(2) && HostedIoRead(kb + 3 * 4) == 1, "enabling with a queued key raises the interrupt");
	HostedKeybPush(0xc0000010);
	HCHECK(HostedIoRead(kb + 2 * 4) == 2, "two codes queued");
	HCHECK(HostedIoRead(kb + 4) == 0x40000010 && HostedIoRead(kb + 4) == 0xc0000010, "DATA pops in order (press, release)");
	HCHECK(!HostedIrqRaised(2), "the interrupt drops with the FIFO empty");
	for (TInt i = 0; i < 70; ++i)
		HostedKeybPush(0x40000000 | i);
	HCHECK(HostedIoRead(kb + 2 * 4) == 64 && HostedKeybDropped == 6, "a full FIFO drops (64 kept, %d dropped)", HostedKeybDropped);
	for (TInt i = 0; i < 64; ++i)
		(void)HostedIoRead(kb + 4);
	// the key ring: a pressed entry (bit 31) becomes a press of that scan code, a released one a release.
	// Bug 2026-11-12 (caught before commit): the first ring API returned the entry as TInt with -1 for
	// "empty", and a pressed entry has bit 31 set
	const char* shmPath = "/tmp/kn_board.shm";
	fd = hsys(SYS_openat, -100, shmPath, 0x42 | 0x200, 0644);
	hftruncate(fd, 0x100000);
	hsys(SYS_close, fd);
	HCHECK(HostedDisplayStart(shmPath) == KErrNone, "display shared memory on a plain file");
	fd = hsys(SYS_openat, -100, shmPath, 2, 0);
	volatile TUint32* ring = (volatile TUint32*)hmmap(0, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	hsys(SYS_close, fd);
	HCHECK(ring[0] == 0x42465048u, "header magic written");
	TUint32 head = ring[0x100 / 4];
	ring[0x108 / 4 + head % 64] = 0x80000000u | 0xa4;		// EStdKeyDevice0 pressed
	ring[0x108 / 4 + (head + 1) % 64] = 0xa4;				// released
	ring[0x100 / 4] = head + 2;
	for (TInt i = 0; i < 50 && HostedIoRead(kb + 2 * 4) < 2; ++i)
		hsleep_us(5000);
	HCHECK(ring[0x104 / 4] == head + 2, "the VM consumed the ring (tail %u)", ring[0x104 / 4]);
	TUint32 k1 = HostedIoRead(kb + 4), k2 = HostedIoRead(kb + 4);
	HCHECK(k1 == 0x400000a4u && k2 == 0xc00000a4u, "ring -> keyboard: press %08x, release %08x", k1, k2);
	// the touch screen (bug 2026-11-12: the hosted board had none, the 808 could not be used): registers as QEMU's
	// syborg-pointer v2, and the window's pointer ring through the display thread (moves between two polls merged)
	{
	extern TInt HostedPtrAttach();
	extern void HostedPtrPush(TUint32 aX, TUint32 aY, TUint32 aButtons);
	const TUint32 pt = KIo + 0x04 * 0x1000;
	HCHECK(HostedPtrAttach() == KErrNone && HostedIoRead(pt) == 0xc51d0006u && HostedIoRead(pt + 9 * 4) == 2,
	       "touch screen: ID SYBORG_ID_TOUCHSCREEN, version 2");
	HostedIoWrite(pt + 7 * 4, 1);
	HostedPtrPush(100, 200, 1);
	HostedPtrPush(300, 400, 1);
	HostedPtrPush(300, 400, 0);
	HCHECK(HostedIoRead(pt + 2 * 4) == 3 && HostedIrqRaised(3), "three samples queued, EIntPointer raised");
	TUint32 f[3], x[3], y[3];
	for (TInt i = 0; i < 3; ++i)
		{
		HostedIoWrite(pt + 1 * 4, 1);
		f[i] = HostedIoRead(pt + 14 * 4);
		x[i] = HostedIoRead(pt + 3 * 4);
		y[i] = HostedIoRead(pt + 4 * 4);
		}
	HCHECK(f[0] == 1 && f[1] == 4 && f[2] == 2 && x[0] == 100 && y[0] == 200 && x[2] == 300 && y[2] == 400,
	       "DOWN at 100,200, MOVE, UP at 300,400 (flags %x %x %x)", f[0], f[1], f[2]);
	HCHECK(HostedIoRead(pt + 2 * 4) == 0, "the FIFO is empty after three latches");
	TUint32 ph = ring[0x400 / 4];
	TUint32 ents[5][2] = { { 10 | 20 << 16, 1 }, { 11 | 21 << 16, 1 }, { 12 | 22 << 16, 1 }, { 13 | 23 << 16, 1 },
	                       { 14 | 24 << 16, 0 } };
	for (TInt i = 0; i < 5; ++i)
		{
		ring[0x408 / 4 + 2 * ((ph + i) % 64)] = ents[i][0];
		ring[0x408 / 4 + 2 * ((ph + i) % 64) + 1] = ents[i][1];
		}
	ring[0x400 / 4] = ph + 5;
	for (TInt i = 0; i < 50 && ring[0x404 / 4] != ph + 5; ++i)
		hsleep_us(5000);
	TUint32 n = HostedIoRead(pt + 2 * 4);
	HostedIoWrite(pt + 1 * 4, 1);
	TUint32 f0 = HostedIoRead(pt + 14 * 4);
	HostedIoWrite(pt + 1 * 4, 1);
	TUint32 f1 = HostedIoRead(pt + 14 * 4), x1 = HostedIoRead(pt + 3 * 4);
	HostedIoWrite(pt + 1 * 4, 1);
	TUint32 f2 = HostedIoRead(pt + 14 * 4), x2 = HostedIoRead(pt + 3 * 4);
	HCHECK(n == 3 && f0 == 1 && f1 == 4 && x1 == 13 && f2 == 2 && x2 == 14,
	       "ring -> touch: down, the last of three moves, up (%u entries; %x %x/%u %x/%u)", n, f0, f1, x1, f2, x2);
	HostedIoWrite(pt + 7 * 4, 0);
	}
	// a modelled device page is not mapped for user mode (stores would bypass the model)
	{
	DPlatChunkHw* hw = NULL;
	HCHECK(DPlatChunkHw::New(hw, 0xC0000000u + 0x03 * 0x1000, 0x1000, EMapAttrUserRw) == KErrNotSupported,
	       "user mapping of the keyboard's (modelled) page refused");
	// (the mappings that are allowed allocate a DPlatChunkHw from the kernel heap: not here, no kernel booted)
	}
	// user-visible chunks are reserved below 0x80000000 (bug 2026-11-12: a heap address returned as a TInt read
	// as a negative error and the 808's window server failed to initialise)
	{
	extern TAny* VirtualReserveUser(TUint32 aSize, TUint32 aProtect);
	TUint32 u1 = (TUint32)VirtualReserveUser(0x30000, 4), u2 = (TUint32)VirtualReserveUser(0x10000, 4);
	HCHECK(u1 >= 0x01000000u && u1 + 0x30000 <= 0x70000000u && u2 >= 0x01000000u && u2 < 0x70000000u,
	       "user reservations in the user window (%08x, %08x)", u1, u2);
	HCHECK(u2 >= u1 + 0x30000 || u2 + 0x10000 <= u1, "they do not overlap");
	HCHECK((TInt)u1 > 0 && (TInt)u2 > 0, "their addresses are positive TInts");
	}
	// the panel's view of the frame buffer (bug 2026-11-12: orientation and partial mode were stored, not shown,
	// so the N95 screensaver's landscape picture, ORIENTATION 3, came out cut and sheared)
	{
	extern void HostedFbCompose(const TUint32* aPic, TInt aPitch, TInt aW, TInt aH, TUint32 aRot, TUint32 aPartialMode,
	                            TUint32 aStart, TUint32 aEnd, TBool aDim, TInt aDimPercent, TUint32* aOut);
	TUint32 pic[4 * 2], out[8];
	for (TInt py = 0; py < 2; ++py)
		for (TInt px = 0; px < 4; ++px)
			pic[py * 4 + px] = 0x10 * py + px + 1;		// a 3x2 picture, pitch 4 words (padding column 4)
	HostedFbCompose(pic, 16, 3, 2, 0, 0, 0, 0, EFalse, 25, out);
	HCHECK(out[0] == 1 && out[2] == 3 && out[3] == 0x11 && out[5] == 0x13, "orientation 0: memory as it is, pitch honoured");
	HostedFbCompose(pic, 16, 2, 3, 3, 0, 0, 0, EFalse, 25, out);	// screen 2x3: the picture turned 3 quarters
	HCHECK(out[0] == 3 && out[1] == 0x13 && out[2] == 2 && out[4] == 1 && out[5] == 0x11,
	       "orientation 3: picture top on the screen's left (%x %x / %x %x / %x %x)", out[0], out[1], out[2], out[3], out[4], out[5]);
	HostedFbCompose(pic, 16, 2, 3, 1, 0, 0, 0, EFalse, 25, out);
	HCHECK(out[0] == 0x11 && out[1] == 1 && out[4] == 0x13 && out[5] == 3, "orientation 1: picture top on the screen's right");
	HostedFbCompose(pic, 16, 3, 2, 2, 0, 0, 0, EFalse, 25, out);
	HCHECK(out[0] == 0x13 && out[5] == 1, "orientation 2: upside down");
	TUint32 band[4] = { 0x808080, 0x7f80ff, 0x123456, 0xffffff };
	HostedFbCompose(band, 16, 4, 1, 0, 1, 1, 2, EFalse, 25, out);	// columns 1..2, 8 colours
	HCHECK(out[0] == 0 && out[1] == 0x00ffff && out[2] == 0 && out[3] == 0,
	       "partial mode: outside the band black, inside 8 colours (%06x %06x)", out[1], out[2]);
	HostedFbCompose(band, 16, 4, 1, 0, 3, 1, 2, EFalse, 25, out);
	HCHECK(out[1] == 0x7f80ff && out[2] == 0x123456, "partial mode with full colour");
	HostedFbCompose(pic, 16, 3, 2, 0, 5, 1, 1, EFalse, 25, out);	// a band of rows: row 1 only
	HCHECK(out[0] == 0 && out[3] == 0 && out[5] == 0, "partial mode by rows: row 0 black, row 1 in 8 colours");
	TUint32 grey = 0xc8c8c8;
	HostedFbCompose(&grey, 4, 1, 1, 0, 0, 0, 0, ETrue, 25, out);
	HCHECK(out[0] == 0x323232, "a blanked panel is dimmed to 25%% (%06x)", out[0]);
	}
	// the host-composed frame as the screen (bug 2026-11-12: with GL on the host the 808's window stayed black:
	// the screen read only video RAM). A fake channel file stands in for the GL server.
	{
	extern TInt HostedFbAttach(TInt aWidth, TInt aHeight);
	extern TInt HostedGlAttach(const char* aPath);
	extern TBool HostedFbScreen(TUint32* aOut, TInt aMaxPixels, TInt& aWidth, TInt& aHeight);
	const char* chanPath = "/tmp/kn_board-gl.chan";
	TInt cfd = hsys(SYS_openat, -100, chanPath, 0x42 | 0x200, 0644);
	hftruncate(cfd, 64u << 20);
	volatile TUint32* ch = (volatile TUint32*)hmmap(0, 64u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
	hsys(SYS_close, cfd);
	ch[0] = 0x434c4753u; ch[1] = 3; ch[2] = 1;			// magic, version 3, server up
	HCHECK(HostedFbAttach(4, 2) == KErrNone && HostedGlAttach(chanPath) == KErrNone, "framebuffer and a fake GL channel attached");
	TPhysAddr vram = 0;
	HCHECK(Epoc::AllocPhysicalRam(0x1000, vram) == KErrNone, "video RAM");
	TUint32* v = (TUint32*)HostedPhysicalToLinear(vram, 0x1000);
	for (TInt i = 0; i < 8; ++i)
		v[i] = 0x111111;
	const TUint32 fb = 0xC6005000u;
	HostedIoWrite(fb + 1 * 4, vram); HostedIoWrite(fb + 13 * 4, 1); HostedIoWrite(fb + 4 * 4, 0);
	TUint32 scr[8];
	TInt sw = 0, sh = 0;
	HCHECK(HostedFbScreen(scr, 8, sw, sh) && sw == 4 && sh == 2 && scr[0] == 0x111111, "no frame published: video RAM (%06x)", scr[0]);
	volatile TUint32* frame = ch + (56u << 20) / 4;
	for (TInt i = 0; i < 8; ++i)
		frame[i] = 0x220000 + i;
	ch[33] = 4; ch[34] = 2; ch[32] = 3;				// size; sequence odd: being written
	HostedFbScreen(scr, 8, sw, sh);
	HCHECK(scr[0] == 0x111111, "a frame being written (odd sequence) is not shown");
	ch[32] = 4;
	HostedFbScreen(scr, 8, sw, sh);
	HCHECK(scr[0] == 0x220000 && scr[7] == 0x220007, "a whole frame is the screen (%06x %06x)", scr[0], scr[7]);
	HostedIoWrite(fb + 4 * 4, 1);					// a quarter turn: the picture must be 2x4, the frame is 4x2
	HostedFbScreen(scr, 8, sw, sh);
	HCHECK(scr[0] == 0x111111, "a frame of another size than the picture is not used");
	HostedIoWrite(fb + 4 * 4, 0);
	HostedIoWrite(fb + 5 * 4, 1);					// blanked: the host frame is dimmed as video RAM would be
	HostedFbScreen(scr, 8, sw, sh);
	HCHECK(scr[0] == 0x080000, "a blanked panel dims the host frame too (%06x)", scr[0]);
	ch[0] = 0;
	}
	// the framebuffer interrupt (bug 2026-11-12: none on the hosted board; the N95 display stand-in posts frames
	// and completes EndUpdate on VSYNC, so the screensaver never reached the screen)
	{
	extern void HostedLowerIrq(TInt aIrq);
	const TUint32 fb = 0xC6005000u;
	HostedIoWrite(fb + 6 * 4, 0);
	HostedIoWrite(fb + 7 * 4, 3);
	HostedLowerIrq(4);
	TUint32 f0 = HostedIoRead(fb + 17 * 4);
	hsleep_us(60000);
	TUint32 f1 = HostedIoRead(fb + 17 * 4);
	HCHECK(f1 >= f0 + 2 && f1 <= f0 + 6, "FRAME_COUNT counts vsyncs at 60 Hz (%u in 60 ms)", f1 - f0);
	HCHECK((HostedIoRead(fb + 7 * 4) & 1) && !HostedIrqRaised(4), "VSYNC sets INT_CAUSE; masked: no interrupt");
	HostedIoWrite(fb + 6 * 4, 1);
	HCHECK(HostedIrqRaised(4), "unmasking a pending VSYNC raises EIntFb");
	HostedLowerIrq(4);
	HostedIoWrite(fb + 7 * 4, 1);
	HCHECK(!(HostedIoRead(fb + 7 * 4) & 1) && !HostedIrqRaised(4), "writing 1 clears the cause");
	hsleep_us(40000);
	HCHECK(HostedIrqRaised(4), "the next vsync raises it again");
	HostedIoWrite(fb + 6 * 4, 2);
	HostedIoWrite(fb + 7 * 4, 3);
	HostedLowerIrq(4);
	HostedIoWrite(fb + 1 * 4, HostedIoRead(fb + 1 * 4));
	hsleep_us(40000);
	HCHECK((HostedIoRead(fb + 7 * 4) & 2) && HostedIrqRaised(4), "a BASE write raises BASE_UPDATE at the next vsync");
	HostedIoWrite(fb + 6 * 4, 0);
	}
	return htest_done();
	}
