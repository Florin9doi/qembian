// uiprobe: the in-app half of `sydb ui` (docs/ui-probe.md). An ECom plugin of the Uikon library
// interface (CEikLibrary2, 0x10004cc1): CEikonEnv loads every implementation of it while it is
// constructed, so every Eikon/Avkon app (and Eikon-based server) gets this DLL, the way it gets
// Avkon's AknInit. Once the app's active scheduler exists, the plugin adds one active object to the
// UI thread that waits on the agent's P&S request property (category = devagent's SID):
//
//   request  key 0x7569, byte array "<tid hex> <seq>"      (set by devagent's `ui` command)
//   reply    key 0x756a, large byte array "<seq>\n" + lines (set by the probe whose thread is <tid>)
//
// The probe runs in RunL, between two events of the app, on the thread that owns the controls, and
// uses the controls' own API: ComponentControl() for children, the list box model's MdcaPoint
// for item text (also for arrays an app computes), the menu pane's items, positions and flags.
// Class names come from the C++ RTTI the ROM's vtables carry (vtable[-1]); this file is built with
// -fno-rtti, so it reads the typeinfo objects itself (Itanium C++ ABI layout).
//
// Lines (fields tab-separated; text escaped: \\ \t \n, \uXXXX outside 0x20..0x7e):
//   A  exe  sid  appui-class
//   C  id  parent  class  kind  x  y  w  h  flags        x, y: Position() (relative to the control's
//                                                       window; a window's to its parent window); flags: 4 window, 8 focused, 0x10 activated,
//                                                       0x20 invisible, 0x40 dimmed (uidump.py F_*)
//   S  id...                                            control stack, highest priority first
//   T  id  text                                         a text the control shows (label, title)
//   L  id  count  current  top  bottom  cols  model     list box / grid
//   I  id  index  text                                  list item text (MdcaPoint/ItemText)
//   M  id  selected  count                              menu pane
//   N  id  index  command  cascade  flags  text         menu item
//   E  id  text                                         editor contents
//   G  recovered                                        the fault guard works (a test fault came back)
//   B  id  slot  w  h  mode  hash  file  bmp  mask         a picture the control holds (slot: image, icon[i]
//                                                       of a list/grid icon array, m+OFF a member); hash:
//                                                       FNV-1a of the pixels; file/bmp/mask: Avkon's icon
//                                                       record (empty/-1 when not an Avkon icon)
//   W  id  x  y  w  h  text                                text drawn into control id's window (window coords;
//                                                       w h 0 for a point) while each window redraws once
//   X  why                                              the dump was cut (buffer full) or a control faulted
// No C++ exception runtime in this toolchain: nothing here may leave (the leaving calls used are
// guarded so that they cannot).
#include <e32base.h>
#include <e32property.h>
#include <e32debug.h>
#include <e32svr.h>
#include <e32rom.h>
#include <coemain.h>
#include <coeaui.h>
#include <coecntrl.h>
#include <w32std.h>
#include <eiklibry.h>
#include <eikenv.h>
#include <eiklbx.h>
#include <eiklbm.h>
#include <eiklbv.h>
#include <eiktxlbm.h>
#include <akngrid.h>
#include <akngridview.h>
#include <eikmenup.h>
#include <eiklabel.h>
#include <eikedwin.h>
#include <akntitle.h>
#include <eikspane.h>
#include <ecom/implementationproxy.h>
#include <fbs.h>
#include <gulicon.h>
#include <eikimage.h>
#include <eikfrlb.h>
#include <eikfrlbd.h>
#include <eikclb.h>
#include <eikclbd.h>

static const TUid KAgentCat = {0xE0001DB7};		// devagent's SID: it defines both properties
static const TUint KKeyReq = 0x7569;
static const TUint KKeyResp = 0x756a;
static const TInt KImplUid = 0xE0001DD1;
static const TInt KMaxOut = 65000;
static const TInt KMaxNodes = 600;
static const TInt KMaxItems = 300;

// ------------------------------------------------------------------------------------------------
// RTTI (Itanium C++ ABI): an object's vtable[-1] is its std::type_info; type_info = {vptr, name};
// __si_class_type_info adds one base; __vmi_class_type_info adds flags, count, {base, offset_flags}[]

struct TTi { const void* iVptr; const char* iName; };

static const TTi* TypeOf(const void* aObj)
	{
	const void* const* vt = *(const void* const* const*)aObj;
	return vt ? (const TTi*)vt[-1] : NULL;
	}

static TBool StrEq(const char* a, const char* b)
	{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
	}

static TBool HasPrefix(const char* a, const char* p)
	{
	while (*p && *a == *p) { a++; p++; }
	return *p == 0;
	}

// "11CEikListBox" names CEikListBox; nested names (N..E) never match a plain class name
static TBool NameIs(const char* aMangled, const char* aCls)
	{
	if (!aMangled || *aMangled < '0' || *aMangled > '9')
		return EFalse;
	while (*aMangled >= '0' && *aMangled <= '9')
		aMangled++;
	return StrEq(aMangled, aCls);
	}

static TBool IsA(const TTi* aTi, const char* aCls, TInt aDepth = 0)
	{
	if (!aTi || aDepth > 16)
		return EFalse;
	if (NameIs(aTi->iName, aCls))
		return ETrue;
	const TTi* kind = TypeOf(aTi);			// the type_info object's own class
	if (!kind)
		return EFalse;
	const TInt* w = (const TInt*)aTi;
	if (HasPrefix(kind->iName, "N10__cxxabiv120__si_class_type_info"))
		return IsA((const TTi*)w[2], aCls, aDepth + 1);
	if (HasPrefix(kind->iName, "N10__cxxabiv121__vmi_class_type_info"))
		{
		TInt n = w[3];
		for (TInt i = 0; i < n && i < 8; i++)
			if (IsA((const TTi*)w[4 + 2 * i], aCls, aDepth + 1))
				return ETrue;
		}
	return EFalse;
	}

// where the aCls base sits inside an object of type aTi (-1: not a base). A class whose CCoeControl
// base is not its first base (Bluetooth UI) has its CCoeControl* at an offset from the object start:
// using the start crashed in CONE (KERN-EXEC 3). Virtual bases are not followed.
static TInt BaseOffset(const TTi* aTi, const char* aCls, TInt aDepth = 0)
	{
	if (!aTi || aDepth > 16)
		return -1;
	if (NameIs(aTi->iName, aCls))
		return 0;
	const TTi* kind = TypeOf(aTi);
	if (!kind)
		return -1;
	const TInt* w = (const TInt*)aTi;
	if (HasPrefix(kind->iName, "N10__cxxabiv120__si_class_type_info"))
		return BaseOffset((const TTi*)w[2], aCls, aDepth + 1);
	if (HasPrefix(kind->iName, "N10__cxxabiv121__vmi_class_type_info"))
		{
		TInt n = w[3];
		for (TInt i = 0; i < n && i < 8; i++)
			{
			TInt of = w[5 + 2 * i];
			if (of & 1)
				continue;			// virtual base
			TInt r = BaseOffset((const TTi*)w[4 + 2 * i], aCls, aDepth + 1);
			if (r >= 0)
				return r + (of >> 8);
			}
		}
	return -1;
	}

// the complete object an interface pointer points into: vtable[-2] is the offset to the top
static const void* TopOf(const void* aP)
	{
	const TInt* vt = *(const TInt* const*)aP;
	return (const TUint8*)aP + vt[-2];
	}

// ------------------------------------------------------------------------------------------------
// output

class TOut
	{
public:
	TOut(HBufC8* aB) : iB(aB), iFull(EFalse) {}
	TBool Room(TInt aN) { if (iB->Length() + aN + 16 > iB->Des().MaxLength()) iFull = ETrue; return !iFull; }
	void Tag(char aTag) { if (Room(4)) { iB->Des().Append(aTag); } }
	void Int(TInt aV) { if (Room(12)) { iB->Des().Append('\t'); iB->Des().AppendNum(aV); } }
	void Hex(TUint aV) { if (Room(12)) { iB->Des().Append(_L8("\t0x")); iB->Des().AppendNum(aV, EHex); } }
	void Str8(const char* aS)
		{
		TPtrC8 s((const TUint8*)(aS ? aS : "?"));
		if (Room(s.Length() + 1)) { iB->Des().Append('\t'); iB->Des().Append(s.Left(200)); }
		}
	void Text(const TDesC16& aT)
		{
		if (!Room(aT.Length() * 6 + 1))
			return;
		TPtr8 d = iB->Des();
		d.Append('\t');
		for (TInt i = 0; i < aT.Length() && i < 600; i++)
			{
			TUint c = aT[i];
			if (c == '\\') d.Append(_L8("\\\\"));
			else if (c == '\t') d.Append(_L8("\\t"));
			else if (c == '\n') d.Append(_L8("\\n"));
			else if (c >= 0x20 && c < 0x7f) d.Append((TUint8)c);
			else { d.Append(_L8("\\u")); d.AppendNumFixedWidth(c, EHex, 4); }
			}
		}
	void End() { if (!iFull) iB->Des().Append('\n'); }
	HBufC8* iB;
	TBool iFull;
	};

static TBool InRange(TUint a, TUint lo, TUint hi) { return a >= lo && a < hi && !(a & 3); }
static TBool CodeOrRom(TUint a) { return InRange(a, 0x70000000u, 0x7f000000u) || InRange(a, 0x80000000u, 0xc0000000u); }

// ------------------------------------------------------------------------------------------------
// fault guard: a bad pointer must not kill the app. While the probe works, a user exception handler
// (KExceptionFault) is installed; on a fault it finds the kernel's exception frame on this stack
// ([type, code, FAR, FSR, CPSR, r0..r15], 21 words, saved sp = frame + 84) and sends the thread to
// uiprobe_fault_land with the current guard's buffer in r0: uiprobe_setjmp then returns 1 there. The
// guard buffer is in DLL TLS (no writable static data in this DLL). A fault outside a guard removes
// the handler and returns: the instruction faults again and the app dies as it would have.

// Dll::Tls / SetTls normally come from the static edll.lib: here from our entry stub (startup.s), as
// src/khr/eglos.cpp: the DLL is its entry point plus the code segment ID the loader writes at entry+12
extern "C" void _E32Dll(void);
TAny* Dll::Tls() { return UserSvr::DllTls((TInt)&_E32Dll, *(((TInt*)&_E32Dll) + 3)); }
TInt Dll::SetTls(TAny* aPtr) { return UserSvr::DllSetTls((TInt)&_E32Dll, *(((TInt*)&_E32Dll) + 3), aPtr); }

extern "C" TInt uiprobe_setjmp(TUint* aBuf);
extern "C" void uiprobe_fault_land();

static void FaultHandler(TExcType aType)
	{
	TUint* jb = (TUint*)Dll::Tls();
	volatile TUint here = 0;
	// the stack address is laundered through asm: walking a pointer derived from &here past that
	// local is undefined behaviour, and GCC dropped the frame writes below as stores to a dead local
	// (the thread then faulted again and again until ViewSrv killed the app)
	TUint sp = (TUint)&here;
	asm volatile("" : "+r"(sp));
	TUint* p = (TUint*)sp;
	for (TInt i = 0; jb && i < 2048; i++, p++)
		{
		if (p[0] == (TUint)aType && (p[4] & 0x1f) == 0x10 && p[18] == (TUint)(p + 21))
			{
			p[5] = (TUint)jb;					// r0
			p[5 + 15] = (TUint)&uiprobe_fault_land;	// pc
			p[4] &= ~0x20u;						// ARM state
			return;
			}
		}
	RDebug::Printf("UIPROBE: fault type %d outside a guard (or no frame found): left to the app", aType);
	User::SetExceptionHandler(NULL, 0);
	}

// GUARDED(stmt, onfault): stmt runs with a fresh recovery point; a fault in it runs onfault instead.
// Locals changed inside stmt must not be relied on after a fault (they may live in restored registers).
#define GUARDED(stmt, onfault) \
	do { TUint jb_[10]; TAny* outer_ = Dll::Tls(); Dll::SetTls(jb_); \
		if (uiprobe_setjmp(jb_) == 0) { stmt; } else { onfault; } \
		Dll::SetTls(outer_); } while (0)

// protected CCoeControl members, through a derived class (no data, never instantiated)
class TPeek : public CCoeControl
	{
public:
	static TBool Activated(const CCoeControl* c) { return static_cast<const TPeek*>(c)->IsActivated(); }
	};

// ------------------------------------------------------------------------------------------------
// the dump

class TDumper
	{
public:
	TDumper(TOut& aO) : iO(aO), iCount(0) {}
	TInt Walk(CCoeControl* aC, TInt aParent, TInt aDepth);
	void Node(CCoeControl* aC, TInt aParent, TInt aDepth, TInt aId);
	void Windows(RWindowTreeNode& aNode, TInt aParent, TInt aDepth);
	void HeapControls();
	void Icons(CCoeControl* aC, const TTi* aTi, TInt aId);
	void Bitmap(TInt aId, const char* aSlot, TInt aIndex, const CFbsBitmap* aBm);
	void Drawn(CWindowGc& aGc);
	TInt Count() const { return iCount; }
	CCoeControl* At(TInt i) const { return (CCoeControl*)iSeen[i]; }
	const CFbsBitmap* iBm[256];
	TInt iNBm;
	TInt IdOf(const CCoeControl* aC) const
		{
		for (TInt i = 0; i < iCount; i++)
			if (iSeen[i] == aC) return i;
		return -1;
		}
private:
	void Adapters(CCoeControl* aC, const TTi* aTi, TInt aId);
	TOut& iO;
	const CCoeControl* iSeen[KMaxNodes];
	TInt iCount;

	};

static const char* const KKinds[] = {
	"CAknGrid", "CEikListBox", "CEikMenuPane", "CEikMenuBar", "CEikLabel", "CEikCba", "CEikCbaButton",
	"CEikEdwin", "CEikDialog", "CAknTitlePane", "CAknQueryControl", "CAknTabGroup", "CAknTab",
	"CEikButtonGroupContainer", "CAknNavigationControlContainer", NULL };

TInt TDumper::Walk(CCoeControl* aC, TInt aParent, TInt aDepth)
	{
	if (!aC || aDepth > 24 || iCount >= KMaxNodes || IdOf(aC) >= 0 || iO.iFull)
		return -1;
	TInt id = iCount;
	iSeen[iCount++] = aC;
	// a fault anywhere in this control (its RTTI, its own virtuals, an adapter) skips the rest of it
	GUARDED(Node(aC, aParent, aDepth, id), (iO.End(), iO.Tag('X'), iO.Str8("fault in control"), iO.Int(id), iO.End()));
	return id;
	}

void TDumper::Node(CCoeControl* aC, TInt aParent, TInt aDepth, TInt id)
	{
	const TTi* ti = TypeOf(aC);
	const char* kind = NULL;
	for (TInt k = 0; KKinds[k] && !kind; k++)
		if (IsA(ti, KKinds[k])) kind = KKinds[k];
	// Position(), not PositionRelativeToScreen(): the latter asks wserv (InquireOffset) and faulted
	// (KERN-EXEC 3 in Ws32) on one of the Menu's controls; the host adds the window positions up
	TPoint p = aC->Position();
	TSize s = aC->Size();
	TUint flags = (aC->OwnsWindow() ? 4 : 0) | (aC->IsFocused() ? 8 : 0) | (TPeek::Activated(aC) ? 0x10 : 0)
		| (aC->IsVisible() ? 0 : 0x20) | (aC->IsDimmed() ? 0x40 : 0);
	iO.Tag('C'); iO.Int(id); iO.Int(aParent); iO.Str8(ti ? ti->iName : NULL); iO.Str8(kind ? kind : "-");
	iO.Int(p.iX); iO.Int(p.iY); iO.Int(s.iWidth); iO.Int(s.iHeight); iO.Hex(flags); iO.End();
	// a control the framework has not activated may be half made (a menu pane never opened crashed in
	// its own CountComponentControls, KERN-EXEC 3): only its node is reported. Everything on screen is
	// activated: the framework calls these same functions to draw it.
	if (!(flags & 0x10))
		return;
	Adapters(aC, ti, id);
	Icons(aC, ti, id);
	TInt n = aC->CountComponentControls();
	for (TInt i = 0; i < n && i < 200; i++)
		Walk(aC->ComponentControl(i), id, aDepth + 1);
	}

void TDumper::Adapters(CCoeControl* aC, const TTi* aTi, TInt aId)
	{
	if (IsA(aTi, "CEikLabel"))
		{
		const TDesC* t = static_cast<CEikLabel*>(aC)->Text();
		if (t && t->Length()) { iO.Tag('T'); iO.Int(aId); iO.Text(*t); iO.End(); }
		}
	else if (IsA(aTi, "CAknTitlePane"))
		{
		const TDesC* t = static_cast<CAknTitlePane*>(aC)->Text();
		if (t && t->Length()) { iO.Tag('T'); iO.Int(aId); iO.Text(*t); iO.End(); }
		}
	else if (IsA(aTi, "CEikEdwin"))
		{
		CEikEdwin* e = static_cast<CEikEdwin*>(aC);
		TInt len = e->TextLength();
		HBufC* b = HBufC::New(len > 0 ? len : 1);
		if (b)
			{
			TPtr bp = b->Des();
			e->GetText(bp);
			iO.Tag('E'); iO.Int(aId); iO.Text(bp); iO.End();
			delete b;
			}
		}
	else if (IsA(aTi, "CEikMenuPane"))
		{
		CEikMenuPane* m = static_cast<CEikMenuPane*>(aC);
		TInt n = m->NumberOfItemsInPane();		// ItemDataByIndexL leaves only outside [0, n)
		iO.Tag('M'); iO.Int(aId); iO.Int(m->SelectedItem()); iO.Int(n); iO.End();
		for (TInt i = 0; i < n && i < KMaxItems; i++)
			{
			CEikMenuPaneItem::SData& d = m->ItemDataByIndexL(i);
			iO.Tag('N'); iO.Int(aId); iO.Int(i); iO.Int(d.iCommandId); iO.Int(d.iCascadeId); iO.Hex(d.iFlags);
			iO.Text(d.iText); iO.End();
			}
		}
	else if (IsA(aTi, "CEikListBox"))
		{
		CEikListBox* lb = static_cast<CEikListBox*>(aC);
		MListBoxModel* model = lb->Model();
		CListBoxView* view = lb->View();
		TInt n = model ? model->NumberOfItems() : -1;
		TInt cols = 0;
		if (IsA(aTi, "CAknGrid") && static_cast<CAknGrid*>(aC)->GridView())
			cols = static_cast<CAknGrid*>(aC)->GridView()->NumberOfColsInView();
		const TTi* mti = model ? TypeOf(TopOf(model)) : NULL;
		iO.Tag('L'); iO.Int(aId); iO.Int(n); iO.Int(view ? lb->CurrentItemIndex() : -1);
		iO.Int(view ? view->TopItemIndex() : -1); iO.Int(view ? view->BottomItemIndex() : -1); iO.Int(cols);
		iO.Str8(mti ? mti->iName : NULL); iO.End();
		// text models: ItemText(i) (a filtered model maps through its filter); MListBoxModel is the
		// primary base of MTextListBoxModel, so the pointer is the same
		if (model && IsA(mti, "CTextListBoxModel"))
			{
			MTextListBoxModel* tm = static_cast<MTextListBoxModel*>(model);
			for (TInt i = 0; i < n && i < KMaxItems; i++)
				{
				iO.Tag('I'); iO.Int(aId); iO.Int(i); iO.Text(tm->ItemText(i)); iO.End();
				}
			}
		}
	}

// a window's client handle is, for every window CONE creates, its CCoeControl (CreateWindowL passes
// `this`); an app may make windows of its own with other handles, so a handle is used only when it
// points into this thread's heap at an object whose RTTI says CCoeControl

static CCoeControl* AsControlIn(TUint a, TUint aLo, TUint aHi)
	{
	if (!InRange(a, aLo, aHi))
		return NULL;
	TUint vt = *(TUint*)a;
	if (!CodeOrRom(vt))
		return NULL;
	TUint ti = ((TUint*)vt)[-1];
	TUint name = ((TUint*)ti)[1];			// a string: any alignment (Cone's are odd)
	if (!CodeOrRom(ti) || !CodeOrRom(name & ~3u))
		return NULL;
	TInt of = BaseOffset((const TTi*)ti, "CCoeControl");
	return of >= 0 ? (CCoeControl*)(a + of) : NULL;
	}

static CCoeControl* AsControl(TInt32 aHandle)
	{
	RHeap& h = User::Heap();
	return AsControlIn((TUint)aHandle, (TUint)h.Base(), (TUint)h.Base() + h.Size());
	}

// every live control of the app: the heap's own walk (RHeap::EWalk), allocated cells whose object is a
// CCoeControl. Catches what neither the window tree nor the control stack reaches (the Menu's grid view
// sits behind a window whose client handle is a bare CCoeControl without the window). The heap is
// locked during the walk: the callback must not allocate nor call the heap.
struct SCollect { CCoeControl** iArr; TInt iN, iMax; TUint iLo, iHi; };
static void CollectCell(TAny* aPtr, RHeap::TCellType aType, TAny* aCell, TInt aLen)
	{
	SCollect* c = (SCollect*)aPtr;
	if (aType != RHeap::EGoodAllocatedCell || c->iN >= c->iMax || aLen < 4 + 0x34)
		return;
	volatile CCoeControl* ctl = NULL;
	GUARDED(ctl = AsControlIn((TUint)aCell + 4, c->iLo, c->iHi), ctl = NULL);	// UREL cell header: the length word
	if (ctl)
		c->iArr[c->iN++] = (CCoeControl*)ctl;
	}

// every window of the app, front to back: the window-owning controls (views of apps whose containers
// are not on the control stack, e.g. the Menu's grid)
void TDumper::Windows(RWindowTreeNode& aNode, TInt aParent, TInt aDepth)
	{
	if (aDepth > 16 || iO.iFull)
		return;
	TInt32 h = aNode.Child();
	for (TInt n = 0; h && n < 200; n++)
		{
		volatile CCoeControl* vc = NULL;
		GUARDED(vc = AsControl(h), vc = NULL);
		CCoeControl* c = (CCoeControl*)vc;
		if (!c)
			break;					// not a CONE window: its siblings cannot be reached
		TInt id = IdOf(c);
		if (id < 0)
			id = Walk(c, aParent, 0);
		RDrawableWindow* w = c->DrawableWindow();
		if (!w)
			break;
		Windows(*w, id, aDepth + 1);
		h = w->NextSibling();
		}
	}

void TDumper::HeapControls()
	{
	RHeap& h = User::Heap();
	SCollect c;
	c.iMax = 2000;
	c.iN = 0;
	c.iLo = (TUint)h.Base();
	c.iHi = c.iLo + h.Size();
	c.iArr = (CCoeControl**)User::Alloc(c.iMax * sizeof(CCoeControl*));
	if (!c.iArr)
		return;
	h.DebugFunction(RHeap::EWalk, (TAny*)&CollectCell, &c);
	// parents first: a control is placed under Parent() once its parent has an id
	for (TInt pass = 0; pass < 8; pass++)
		{
		TBool more = EFalse;
		for (TInt i = 0; i < c.iN; i++)
			{
			CCoeControl* ctl = c.iArr[i];
			if (!ctl || IdOf(ctl) >= 0)
				continue;
			CCoeControl* par = ctl->Parent();
			TInt pid = par ? IdOf(par) : -1;
			if (par && pid < 0 && pass < 7)
				{
				more = ETrue;				// its parent comes later in this pass or the next
				continue;
				}
			Walk(ctl, pid, 0);
			}
		if (!more)
			break;
		}
	User::Free(c.iArr);
	}

// ------------------------------------------------------------------------------------------------
// pictures: what a control shows as an image. Identity that survives restarts: Avkon's icon record
// (AknIcon.dll: CAknBitmap +0x1c -> record; record +0x8 file name (shared HBufC, "z:\resource\apps\"
// stripped when flag 1, extension when flag 8), +0xc bitmap id, +0xe mask id, +0x18 the bitmap; ids
// from 0x4000 are MIF icons), and a hash of the pixels (same picture, same size, same theme).

static TUint32 Fnv(const TUint8* aP, TInt aN, TUint32 aH)
	{
	for (TInt i = 0; i < aN; i++) { aH ^= aP[i]; aH *= 16777619u; }
	return aH;
	}

void TDumper::Bitmap(TInt aId, const char* aSlot, TInt aIndex, const CFbsBitmap* aBm)
	{
	if (!aBm || iO.iFull)
		return;
	for (TInt i = 0; i < iNBm; i++)
		if (iBm[i] == aBm) return;
	if (iNBm < 256) iBm[iNBm++] = aBm;
	if (!aBm->Handle())
		return;
	TSize s = aBm->SizeInPixels();
	TDisplayMode mode = aBm->DisplayMode();
	TUint32 h = 2166136261u;
	if (!aBm->IsCompressedInRAM() && s.iWidth > 0 && s.iHeight > 0 && s.iWidth * s.iHeight <= 320 * 320)
		{
		TInt row = CFbsBitmap::ScanLineLength(s.iWidth, mode), stride = aBm->DataStride();
		aBm->LockHeap();
		const TUint8* d = (const TUint8*)aBm->DataAddress();
		for (TInt y = 0; d && y < s.iHeight; y++)
			h = Fnv(d + y * stride, row, h);
		aBm->UnlockHeap();
		}
	else
		h = 0;
	TBuf<80> file;
	TInt bmp = -1, mask = -1;
	if (IsA(TypeOf(aBm), "CAknBitmap"))
		{
		const TUint8* rec = *(const TUint8* const*)((const TUint8*)aBm + 0x1c);
		if (rec && (*(const CFbsBitmap* const*)(rec + 0x18) == aBm || *(const CFbsBitmap* const*)(rec + 0x1c) == aBm))
			{
			bmp = *(const TInt16*)(rec + 0xc);
			mask = *(const TInt16*)(rec + 0xe);
			TUint flags = *(const TUint*)(rec + 0x14);
			const TDesC* name = *(const TDesC* const*)(rec + 8);
			if (flags & 1) file.Append(_L("z:\\resource\\apps\\"));
			if (name) file.Append(name->Left(file.MaxLength() - file.Length() - 4));
			if (flags & 8) file.Append(bmp >= 0x4000 ? _L(".mif") : _L(".mbm"));
			}
		}
	TBuf8<24> slot;
	slot.Copy(TPtrC8((const TUint8*)aSlot));
	if (aIndex >= 0) { slot.Append('['); slot.AppendNum(aIndex); slot.Append(']'); }
	iO.Tag('B'); iO.Int(aId); iO.Str8((const char*)slot.PtrZ()); iO.Int(s.iWidth); iO.Int(s.iHeight); iO.Int(mode);
	iO.Hex(h); iO.Text(file); iO.Int(bmp); iO.Int(mask); iO.End();
	}

static CArrayPtr<CGulIcon>* IconArrayOf(CCoeControl* aC, const TTi* aTi)
	{
	if (IsA(aTi, "CAknGrid"))
		{
		CFormattedCellListBoxItemDrawer* dr = static_cast<CAknGrid*>(aC)->ItemDrawer();
		return dr && dr->FormattedCellData() ? dr->FormattedCellData()->IconArray() : NULL;
		}
	if (IsA(aTi, "CEikFormattedCellListBox"))
		{
		CFormattedCellListBoxItemDrawer* dr = static_cast<CEikFormattedCellListBox*>(aC)->ItemDrawer();
		return dr && dr->FormattedCellData() ? dr->FormattedCellData()->IconArray() : NULL;
		}
	if (IsA(aTi, "CEikColumnListBox"))
		{
		CColumnListBoxItemDrawer* dr = static_cast<CEikColumnListBox*>(aC)->ItemDrawer();
		return dr && dr->ColumnData() ? dr->ColumnData()->IconArray() : NULL;
		}
	return NULL;
	}

void TDumper::Icons(CCoeControl* aC, const TTi* aTi, TInt aId)
	{
	if (IsA(aTi, "CEikImage"))
		Bitmap(aId, "image", -1, static_cast<CEikImage*>(aC)->Bitmap());
	CArrayPtr<CGulIcon>* arr = IconArrayOf(aC, aTi);
	for (TInt i = 0; arr && i < arr->Count() && i < 64; i++)
		if (arr->At(i))
			Bitmap(aId, "icon", i, arr->At(i)->Bitmap());
	// members: pointers in the control's own heap cell to a CFbsBitmap or a CGulIcon (tabs, custom
	// buttons such as the Calculator's); each candidate is checked under the fault guard
	const TUint8* top = (const TUint8*)TopOf(aC);
	TInt len = *(const TInt*)(top - 4) - 4;			// UREL heap cell header
	RHeap& hp = User::Heap();
	TUint lo = (TUint)hp.Base(), hi = lo + hp.Size();
	if (!InRange((TUint)top, lo, hi) || len < 4 || len > 0x2000)
		return;
	for (TInt off = 0; off < len && off < 0x400; off += 4)
		{
		TUint w = *(const TUint*)(top + off);
		if (!InRange(w, lo, hi) || w == (TUint)top)
			continue;
		volatile const CFbsBitmap* bm = NULL;
		GUARDED(
			{
			TUint vt = *(TUint*)w;
			if (CodeOrRom(vt))
				{
				const TTi* t = (const TTi*)((TUint*)vt)[-1];
				if (IsA(t, "CFbsBitmap")) bm = (const CFbsBitmap*)w;
				else if (IsA(t, "CGulIcon")) bm = ((const CGulIcon*)w)->Bitmap();
				}
			}, bm = NULL);
		if (bm)
			Bitmap(aId, "m", off, (const CFbsBitmap*)bm);
		}
	}

// ------------------------------------------------------------------------------------------------
// drawn text: the app's own CWindowGc (CCoeEnv::SystemGc) gets, for the length of one redraw of each
// top-level window, a copy of its vtable whose DrawText / DrawTextVertical / Activate entries record
// and then call the real ones; slot numbers come from C++ member pointers (the SDK headers' layout)

const TInt KVtWords = 200;
struct SDrawRec
	{
	TDumper* iD;
	TOut* iO;
	TInt iWin;
	const TUint* iOrig;
	TInt iDt1, iDt2, iDv1, iDv2, iAct;
	TUint iVt[KVtWords + 2];
	};

static SDrawRec* RecOf(CWindowGc* aG)
	{
	return (SDrawRec*)((TUint8*)*(TUint**)aG - _FOFF(SDrawRec, iVt) - 8);
	}

template <class T> static TInt Slot(T aPm)
	{
	TUint w[2];
	Mem::Copy(w, &aPm, 8);
	return (w[1] & 1) && w[0] / 4 < (TUint)KVtWords ? (TInt)(w[0] / 4) : -1;	// ARM ABI: virtual -> vtable offset
	}

static void Rec(SDrawRec* r, const TDesC& aT, TInt x, TInt y, TInt w, TInt h)
	{
	TOut& o = *r->iO;
	o.Tag('W'); o.Int(r->iWin); o.Int(x); o.Int(y); o.Int(w); o.Int(h); o.Text(aT); o.End();
	}

typedef void (*TFDt1)(CWindowGc*, const TDesC&, const TPoint&);
typedef void (*TFDt2)(CWindowGc*, const TDesC&, const TRect&, TInt, CGraphicsContext::TTextAlign, TInt);
typedef void (*TFDv1)(CWindowGc*, const TDesC&, const TPoint&, TBool);
typedef void (*TFDv2)(CWindowGc*, const TDesC&, const TRect&, TInt, TBool, CGraphicsContext::TTextAlign, TInt);
typedef void (*TFAct)(CWindowGc*, RDrawableWindow&);

static void HDt1(CWindowGc* g, const TDesC& t, const TPoint& p)
	{ SDrawRec* r = RecOf(g); Rec(r, t, p.iX, p.iY, 0, 0); ((TFDt1)r->iOrig[r->iDt1])(g, t, p); }
static void HDt2(CWindowGc* g, const TDesC& t, const TRect& b, TInt bl, CGraphicsContext::TTextAlign al, TInt m)
	{ SDrawRec* r = RecOf(g); Rec(r, t, b.iTl.iX, b.iTl.iY, b.Width(), b.Height()); ((TFDt2)r->iOrig[r->iDt2])(g, t, b, bl, al, m); }
static void HDv1(CWindowGc* g, const TDesC& t, const TPoint& p, TBool up)
	{ SDrawRec* r = RecOf(g); Rec(r, t, p.iX, p.iY, 0, 0); ((TFDv1)r->iOrig[r->iDv1])(g, t, p, up); }
static void HDv2(CWindowGc* g, const TDesC& t, const TRect& b, TInt bl, TBool up, CGraphicsContext::TTextAlign al, TInt m)
	{ SDrawRec* r = RecOf(g); Rec(r, t, b.iTl.iX, b.iTl.iY, b.Width(), b.Height()); ((TFDv2)r->iOrig[r->iDv2])(g, t, b, bl, up, al, m); }
static void HAct(CWindowGc* g, RDrawableWindow& w)
	{
	SDrawRec* r = RecOf(g);
	r->iWin = -1;					// the control whose window this is
	for (TInt i = 0; i < r->iD->Count(); i++)
		if (r->iD->At(i)->OwnsWindow() && r->iD->At(i)->DrawableWindow() == &w) { r->iWin = i; break; }
	((TFAct)r->iOrig[r->iAct])(g, w);
	}

void TDumper::Drawn(CWindowGc& aGc)
	{
	SDrawRec* r = (SDrawRec*)User::AllocZ(sizeof(SDrawRec));
	if (!r)
		return;
	r->iD = this;
	r->iO = &iO;
	r->iWin = -1;
	r->iOrig = *(const TUint**)&aGc;
	r->iDt1 = Slot<void (CWindowGc::*)(const TDesC&, const TPoint&)>(&CWindowGc::DrawText);
	r->iDt2 = Slot<void (CWindowGc::*)(const TDesC&, const TRect&, TInt, CGraphicsContext::TTextAlign, TInt)>(&CWindowGc::DrawText);
	r->iDv1 = Slot<void (CWindowGc::*)(const TDesC&, const TPoint&, TBool)>(&CWindowGc::DrawTextVertical);
	r->iDv2 = Slot<void (CWindowGc::*)(const TDesC&, const TRect&, TInt, TBool, CGraphicsContext::TTextAlign, TInt)>(&CWindowGc::DrawTextVertical);
	r->iAct = Slot<void (CWindowGc::*)(RDrawableWindow&)>(&CWindowGc::Activate);
	if (r->iDt1 < 0 || r->iDt2 < 0 || r->iDv1 < 0 || r->iDv2 < 0 || r->iAct < 0)
		{
		User::Free(r);
		iO.Tag('X'); iO.Str8("drawn text: no vtable slots"); iO.End();
		return;
		}
	volatile TInt copied = 0;
	GUARDED({ for (TInt i = 0; i < KVtWords + 2; i++) r->iVt[i] = r->iOrig[i - 2]; copied = 1; }, copied = 0);
	if (!copied)
		{
		User::Free(r);
		return;
		}
	r->iVt[2 + r->iDt1] = (TUint)&HDt1;
	r->iVt[2 + r->iDt2] = (TUint)&HDt2;
	r->iVt[2 + r->iDv1] = (TUint)&HDv1;
	r->iVt[2 + r->iDv2] = (TUint)&HDv2;
	r->iVt[2 + r->iAct] = (TUint)&HAct;
	*(const TUint**)&aGc = &r->iVt[2];
	for (TInt i = 0; i < iCount && !iO.iFull; i++)
		{
		CCoeControl* c = (CCoeControl*)iSeen[i];
		if (!c->OwnsWindow() || !c->IsVisible() || !TPeek::Activated(c) || c->Parent())
			continue;
		r->iWin = i;
		GUARDED(c->DrawNow(), (iO.End(), iO.Tag('X'), iO.Str8("fault while redrawing"), iO.Int(i), iO.End()));
		}
	*(const TUint**)&aGc = r->iOrig;				// always back, also after a fault
	User::Free(r);
	}

static void Dump(TOut& o)
	{
	CCoeEnv* env = CCoeEnv::Static();
	if (!env)
		{
		o.Tag('X'); o.Str8("no CCoeEnv"); o.End();
		return;
		}
	// the fault guard proves itself on each dump: a read of address 0x10 must come back here
	volatile TInt recovered = 0;
	GUARDED((void)*(volatile TUint*)0x10, recovered = 1);
	o.Tag('G'); o.Str8(recovered ? "recovered" : "no-fault"); o.End();
	CCoeAppUi* aui = env->AppUi();
	RProcess me;
	TFileName exe = me.FileName();
	o.Tag('A'); o.Text(exe); o.Hex(me.SecureId().iId); o.Str8(aui && TypeOf(aui) ? TypeOf(aui)->iName : NULL); o.End();
	TDumper* d = new TDumper(o);
	if (!d)
		return;
	// the app's windows, then the status pane's (its own window group when it has one). Not
	// CEikStatusPaneBase::ControlL: it left in akncapserver although PaneCapabilities said present, and
	// a leave through this -fno-exceptions code aborts the app (USER-EXEC 3)
	d->Windows(env->RootWin(), -1, 0);
	CEikStatusPaneBase* sp = CEikStatusPaneBase::Current();
	if (sp && sp->WindowGroup() && sp->WindowGroup() != &env->RootWin())
		d->Windows(*sp->WindowGroup(), -1, 0);
	// the control stack: CCoeAppUi::iStack (+0xc), a CArrayFixFlat of {control, priority, flags, ...}
	// (no public accessor in 9.2; the class is checked by its RTTI name first)
	if (aui)
		{
		CBase* st = *(CBase**)((TUint8*)aui + 12);
		if (st && TypeOf(st) && NameIs(TypeOf(st)->iName, "CCoeControlStack"))
			{
			CArrayFixBase* a = (CArrayFixBase*)st;
			TInt ids[64];
			TInt nids = 0;
			for (TInt i = 0; i < a->Count() && i < 64; i++)
				{
				CCoeControl* c = *(CCoeControl**)a->At(i);
				TInt id = d->IdOf(c);
				if (id < 0)
					id = d->Walk(c, -1, 0);
				if (id >= 0) ids[nids++] = id;
				}
			o.Tag('S');
			for (TInt i = 0; i < nids; i++) o.Int(ids[i]);
			o.End();
			}
		}
	d->HeapControls();
	d->Drawn(env->SystemGc());
	if (o.iFull)
		{
		o.iFull = EFalse;
		o.Tag('X'); o.Str8("buffer full"); o.End();
		}
	delete d;
	}

// ------------------------------------------------------------------------------------------------
// the active object on the UI thread

class CUiProbe : public CActive
	{
public:
	CUiProbe() : CActive(EPriorityHigh) { CActiveScheduler::Add(this); }
	~CUiProbe() { Cancel(); iReq.Close(); }
	void Start()
		{
		if (iReq.Attach(KAgentCat, KKeyReq) == KErrNone)
			Wait();
		}
private:
	void Wait() { iReq.Subscribe(iStatus); SetActive(); }
	void DoCancel() { iReq.Cancel(); }
	void RunL()
		{
		TInt st = iStatus.Int();
		Wait();							// first: the next request is not missed
		if (st != KErrNone)
			return;
		TBuf8<64> req;
		if (iReq.Get(req) != KErrNone)
			return;
		TLex8 lex(req);
		TUint tid = 0;
		if (lex.Val(tid, EHex) != KErrNone || tid != (TUint)RThread().Id())
			return;
		lex.SkipSpace();
		TPtrC8 seq = lex.Remainder();
		HBufC8* b = HBufC8::New(KMaxOut);
		if (!b)
			return;
		b->Des().Append(seq.Left(16));
		b->Des().Append('\n');
		TOut o(b);
		TExceptionHandler old = User::ExceptionHandler();
		User::SetExceptionHandler(FaultHandler, KExceptionFault);
		GUARDED(Dump(o), (o.End(), o.Tag('X'), o.Str8("fault"), o.End()));
		User::SetExceptionHandler(old, old ? KExceptionFault : 0);
		RProperty::Set(KAgentCat, KKeyResp, *b);
		delete b;
		}
	RProperty iReq;
	};

class CUiProbeLib : public CEikLibrary2
	{
public:
	static CUiProbeLib* NewL() { return new CUiProbeLib; }
	~CUiProbeLib() { delete iProbe; }
	void InitializeL() { Attach(); }
	CArrayFix<TFileName>* ResourceFileArrayL() const { return NULL; }
	CArrayFix<TCreateByTypeFunction>* ControlFactoryArrayL() const { return NULL; }
	void PostAppUiInitializeL() { Attach(); }
private:
	void Attach()
		{
		if (iProbe || !CActiveScheduler::Current())
			return;
		iProbe = new CUiProbe;
		if (iProbe)
			iProbe->Start();
		}
	CUiProbe* iProbe;
	};

static const TImplementationProxy KTable[] = { IMPLEMENTATION_PROXY_ENTRY(KImplUid, CUiProbeLib::NewL) };

EXPORT_C const TImplementationProxy* ImplementationGroupProxy(TInt& aCount)
	{
	aCount = sizeof(KTable) / sizeof(KTable[0]);
	return KTable;
	}
