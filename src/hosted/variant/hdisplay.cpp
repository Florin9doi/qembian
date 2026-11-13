// Hosted EKA2: the phone's screen for a host window. The test VM has an ivshmem device whose memory is a file
// on the host (tools/hosted/htest.py --phone, tools/hosted/phone.py); a host thread here copies the board's
// framebuffer as the panel shows it (HostedFbScreen: rotation, partial mode, dimming) into it about 30 times a second, only the rows that
// changed, and bumps a frame counter; the host viewer maps the same file and draws it. The same memory carries
// key events the other way (a ring, read by the keypad model). Original work for qembian.
//
// Layout (all little-endian u32):
//   0x000 magic 'HPFB'  0x004 version 1  0x008 width  0x00c height  0x010 pitch (bytes)  0x014 frame counter
//   0x100 key ring: 0x100 head (host writes)  0x104 tail (guest writes)  0x108 + 4 * i: 64 entries
//         (entry: bit 31 pressed, bits 0..15 the key code)
//   0x080 diagnosis requests (host bumps: kn_efile PhoneDiag lists who waits on whom)
//   0x400 pointer ring (touch phones): 0x400 head (host writes)  0x404 tail (guest writes)  0x408 + 8 * i: 64
//         entries {x | y << 16 (absolute 0..32767 over the screen), buttons (bit 0: the finger is down)}
//   0x1000 pixels, XRGB8888, height rows of pitch bytes

#include <kernel/kern_priv.h>

extern TBool HostedFbScreen(TUint32* aOut, TInt aMaxPixels, TInt& aWidth, TInt& aHeight);
extern const TUint32* HostedFbPlain(TInt& aWidth, TInt& aHeight, TInt& aPitch, TUint32& aGlSeq);

static const TUint32 KMagic = 0x42465048;		// "HPFB"
static const TUint32 KPixels = 0x1000;
static volatile TUint32* Shm;
static TUint32 ShmBytes;
static TUint32* Shadow;
static TInt ShadowBytes;
static volatile TInt DisplayTid;
volatile TUint32 HostedDisplayFrames;

static TUint32* Frame;
static const TInt KMaxPixels = 1024 * 1024;

static void Present()
	{
	if (!Frame)
		{
		TAny* m = hmmap(0, KMaxPixels * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (HMAP_FAILED(m))
			return;
		Frame = (TUint32*)m;
		}
	TInt w = 0, h = 0;
	// fast path: the picture as it is (HostedFbPlain): nothing to do for an unchanged host frame, else the changed
	// rows straight from it
	TInt pitch = 0;
	TUint32 glSeq = 0;
	static TUint32 LastGlSeq;
	const TUint32* plain = HostedFbPlain(w, h, pitch, glSeq);
	static TUint32 TracedSeq;
	if (glSeq && glSeq != TracedSeq)
		{
		extern void HostedGlTraceFrame(TUint32 aSeq);
		HostedGlTraceFrame(glSeq);			// htest --env GLTRACE=1
		TracedSeq = glSeq;
		}
	if (plain && glSeq && glSeq == LastGlSeq)
		return;
	LastGlSeq = 0;
	if (plain)
		{
		if (KPixels + (TUint32)(w * h * 4) > ShmBytes)
			return;
		}
	else if (!HostedFbScreen(Frame, KMaxPixels, w, h) || KPixels + (TUint32)(w * h * 4) > ShmBytes)
		return;
	if (ShadowBytes < w * h * 4)
		{
		TAny* m = hmmap(0, w * h * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (HMAP_FAILED(m))
			return;
		Shadow = (TUint32*)m;
		ShadowBytes = w * h * 4;
		memset(Shadow, 0xff, ShadowBytes);
		}
	Shm[2] = w;
	Shm[3] = h;
	Shm[4] = w * 4;
	TBool any = EFalse;
	TUint32* dst = (TUint32*)((TUint8*)Shm + KPixels);
	for (TInt y = 0; y < h; ++y)
		{
		const TUint32* row = plain ? (const TUint32*)((const TUint8*)plain + y * pitch) : Frame + y * w;
		TUint32* sh = Shadow + y * w;
		if (!memcmp(sh, row, w * 4))
			continue;
		memcpy(sh, row, w * 4);
		memcpy(dst + y * w, row, w * 4);
		any = ETrue;
		}
	extern TUint32 HostedGlFrameSeq();
	if (glSeq && HostedGlFrameSeq() == glSeq)
		LastGlSeq = glSeq;				// copied whole: skip it until the server publishes another
	if (any)
		{
		__atomic_thread_fence(__ATOMIC_RELEASE);
		Shm[5] = Shm[5] + 1;			// a new frame: the host redraws
		++HostedDisplayFrames;
		}
	}

TBool HostedDisplayNextKey(TUint32& aEntry);
extern void HostedKeybPush(TUint32 aCode);
extern void HostedPtrPush(TUint32 aX, TUint32 aY, TUint32 aButtons);
extern void HostedGlStat();
static TBool GlStat;

// the pointer ring into the touch model: every press and release, of the moves between two only the last
// (one sample per 10 ms poll: the panel's report rate)
static void DrainPointer()
	{
	if (!Shm)
		return;
	TUint32 head = Shm[0x400 / 4], tail = Shm[0x404 / 4];
	if (head == tail)
		return;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	TUint32 lastXy = 0, lastButtons = 0;
	TBool held = EFalse;
	static TUint32 Buttons;
	for (; tail != head; ++tail)
		{
		TUint32 xy = Shm[0x408 / 4 + 2 * (tail % 64)], b = Shm[0x408 / 4 + 2 * (tail % 64) + 1];
		if ((b & 1) == (Buttons & 1))
			{
			lastXy = xy;
			lastButtons = b;
			held = ETrue;					// a move: the latest wins
			continue;
			}
		if (held)
			HostedPtrPush(lastXy & 0xffff, lastXy >> 16, lastButtons);
		held = EFalse;
		HostedPtrPush(xy & 0xffff, xy >> 16, b);
		Buttons = b;
		}
	if (held)
		HostedPtrPush(lastXy & 0xffff, lastXy >> 16, lastButtons);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	Shm[0x404 / 4] = tail;
	}

// keys every 10 ms (a host thread's poll of the shared ring, no system call), the screen every 30 ms
static int DisplayThread(void*)
	{
	hsys(SYS_prctl, 15 /* PR_SET_NAME */, "h-display");	// its name in the Linux view (lxtop)
	GlStat = hgetenv("GLSTAT") != NULL;
	for (TInt n = 0;; ++n)
		{
		for (TUint32 v; HostedDisplayNextKey(v); )
			{
			// ring entry: bit 31 pressed, bits 0..15 the Symbian scan code -> the keyboard's code
			// (bit 30: already a scan code; bit 31: released)
			HostedKeybPush(0x40000000u | (v & 0xff) | ((v & 0x80000000u) ? 0 : 0x80000000u));
			}
		DrainPointer();
		if (n % 3 == 0)
			Present();
		if (n % 100 == 0 && GlStat)
			HostedGlStat();
		hsleep_us(10000);
		}
	}

// the shared memory (kn_efile's phone diagnosis reads its request word, 0x80); NULL without a window
volatile TUint32* HostedDisplayShm() { return Shm; }

// the key ring in the same memory: the next entry, EFalse when empty
TBool HostedDisplayNextKey(TUint32& aEntry)
	{
	if (!Shm)
		return EFalse;
	TUint32 head = Shm[0x100 / 4], tail = Shm[0x104 / 4];
	if (head == tail)
		return EFalse;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	aEntry = Shm[0x108 / 4 + tail % 64];
	__atomic_thread_fence(__ATOMIC_RELEASE);
	Shm[0x104 / 4] = tail + 1;
	return ETrue;
	}

// show the phone's screen in the shared memory at aPath (the ivshmem BAR's sysfs resource file)
TInt HostedDisplayStart(const char* aPath)
	{
	TInt fd = hsys(SYS_openat, -100, aPath, 2 /* O_RDWR */, 0);
	if (fd < 0)
		return KErrNotFound;
	TInt size = hsys(19 /* lseek */, fd, 0, 2);
	if (size <= 0)
		size = 0x100000;
	TAny* m = hmmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	hsys(SYS_close, fd);
	if (HMAP_FAILED(m))
		return KErrNoMemory;
	Shm = (volatile TUint32*)m;
	ShmBytes = size;
	Shm[0] = KMagic;
	Shm[1] = 1;
	Shm[5] = 0;
	Shm[0x104 / 4] = Shm[0x100 / 4];		// keys pressed before boot are dropped
	hprintf("HOSTED: display in shared memory %s (%u bytes)\n", aPath, ShmBytes);
	TAny* stack = hmmap(0, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(stack) || hthread(&DisplayThread, NULL, stack, 0x10000, &DisplayTid) < 0)
		return KErrNoMemory;
	return KErrNone;
	}
