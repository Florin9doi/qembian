# 5800: DisplayDriver control 44 and the Raging Thunder GR crash

Research note (2026-11-17), static reading only, firmware RM-356 31.0.101 (core ROM). Scratch
disassembly: real `d_display.ldd` (ARM, code 0x8012a6f8), `r_display.dll`, `PostingSurfaceFactory.dll`,
`RagingThunderGR.exe` (ARM/RVCT, code 0x80efc958). Claims are marked [verified] (read in the code)
or [inferred].

**Summary: control 44 is not the cause.** The game crashes because the sensor framework lists no
accelerometer channel and the game then uses an uninitialised register as the channel pointer.
Control 44 is a 2-word "set" that the real driver stores and gives back through control 45.

## 1. Control 44 / 45 (real 5800 d_display.ldd)

- [verified] User side: `r_display.dll` ord71 = `DoControl(44, a1)` and ord70 = `DoControl(45, a1)`. Both
  are thin wrappers. The only importer is `PostingSurfaceFactory.dll`, which reaches them through a vtable
  of r_display thunks (slots at 0x807a5054/0x807a5058), not through direct calls.
- [verified] Kernel side: the control switch at 0x80136c5c (`ctl - 3`, cases 3..45). Case 44 is at
  0x801376b8:
  `ThreadRawRead(client, a1, buf, 8)`. If that returns 0, then `buf[0], buf[1]` -> per-screen
  state (`screens[cur] + 0x328 / +0x32c`). It returns the read error, or 0. **Exactly 2 words are
  read and nothing is written back.**
- [verified] Case 45 (0x80137700): `ThreadRawWrite(client, a1, &screen+0x328, 8)` returns the
  2 stored words.
- [inferred] What it means: a per-screen 2-word setting (an offset or origin pair). WSERV passes
  {0,0}, and so does the game's DSB path (the 3rd and 4th words in our log come after the 8 bytes
  the driver reads). The N95 driver's 43/44/45 (freeze, unfreeze, count; nokia-d_display.md) are
  different controls under the same numbers.
- Our stand-in already does what matters: it returns 0, like the real driver. It does not keep the
  value for ctl 45. Proposed code is below. It is a fidelity fix, not the crash fix.

## 2. The crash: no accelerometer channel

The game function at 0x80f34948 (`RagingThunderGR.exe+0x37ff0`) [verified]:

```
TSensrvChannelInfo info;  info.iChannelType = 0x1020507E      // accelerometer XYZ axis data
RArray<TSensrvChannelInfo> list;
finder = CSensrvChannelFinder::NewLC();
finder->FindChannelsL(list, info);                             // vtable +12
if (list.Count() > 0)
    r7 = CSensrvChannel::NewL(list[0]);                        // r7 set ONLY here
CleanupStack::PopAndDestroy();
if (r7 == 0) return 0;                                         // r7 is uninitialised when Count()==0
obj = new(44) CListener(r7);                                   // ctor 0x80f347e0
```

- [verified] The constructor (0x80f347e0) calls `channel->OpenChannelL()` (vtable +12) at
  0x80f34844. That is the faulting `ldr pc,[r2,#12]`, with r2 = the vtable word of whatever stale value
  r7 held. Next comes `StartDataListeningL(this+4, 1, 1, 0)` (vtable +24).
- [verified] r7 is not cleared on the `Count() <= 0` path (0x80f349ac `ble 0x80f349c4`). This is a game bug
  that the real phone never hits, because the real phone always has the channel.
- [inferred] There are no channels because the 5800 profile has no accelerometer stand-in.
  `accelerometerldd.ldd/accelerometerpdd.pdd` are in `missing_drivers`, so the 5800's `accssy.dll`
  cannot open its driver and registers no channel. The 5800 accssy strings are `accelerometerldd.ldd`,
  `accelerometerpdd.pdd`, channel `accelerometer`, PDD name `accelerometer.pdd`, with classes
  CAxisChannel, CWakeUpChannel and CDoubleTapChannel (no threshold channel, unlike the N8) [verified
  strings]. The game does not use GLES, EGL or M3G. Its graphics are CDirectScreenAccess +
  `CDirectScreenBitmap::NewL` (ScDv) [verified imports], so the missing GPU is not the problem.

## 3. Fixes

A. Proper fix: give the 5800 an accelerometer. Add `src/standins/accelerometer` to the 5800 profile
under the 5800 file names (`[[standins]] name = "accelerometerldd"`, `file = "accelerometer"`, plus
`accelerometerpdd`) and a `[board.accelerometer] info` block. [inferred] The 5.0 accssy protocol
(control 0 info block, request numbers, record layout, chip id in its cenrep) is not checked against
the N8 one. It needs its own short pass, and any difference goes in as a declared
`[[standins]] config` variant. The game then also gets tilt steering.

B. Cheapest stop-gap: a one-word game patch that keeps r7 = 0 when no channel is found. With it,
the game skips the sensor and runs without tilt.

```
# models/nokia-5800-rm356/code-patches.txt (new file; wire it into the profile like the 701's)
RagingThunderGR.exe+0x38050  000050e3  007050e2   # cmp r0,#0 -> subs r7,r0,#0: r7 = 0 when the sensor finder lists no accelerometer (game reads an uninitialised r7) [H-rtgr-noaccel]
```
[verified] The instruction is at 0x80f349a8 = `e3500000` (`cmp r0, #0`, r0 = Count()). `subs r7, r0, #0`
sets the same flags as the cmp. With Count()==0, r7 becomes 0 and the following `cmp r7,#0; beq` skips the listener.
With Count()>0, r7 is overwritten by `CSensrvChannel::NewL`. [inferred] The rest of the game tolerates
a NULL listener (the function's return value is 0 on that path in the original too).

C. ctl 44/45 in d_display (fidelity only):

```cpp
// ctl 44 a1 -> 2 words: stored per screen (the real driver keeps them at screen+0x328), nothing written back
// ctl 45 a1 <- the 2 words stored by ctl 44
static TUint32 Ctl44Words[2];
...
case 44:
	{
	TUint32 w[2];
	TInt r = Kern::ThreadRawRead(iClient, a1, w, sizeof(w));
	if (r == KErrNone) { Ctl44Words[0] = w[0]; Ctl44Words[1] = w[1]; }
	return r;
	}
case 45:
	return Kern::ThreadRawWrite(iClient, a1, Ctl44Words, sizeof(Ctl44Words));
```
On the N95 (`d_display.partial`-style board config), 43/44/45 are freeze, unfreeze and count, and 45
returns a count by value. Gate the 5800 meaning behind a declared config, for example
`d_display.ctl44 = "pair"`, and keep the N95 behaviour as the default.

## Also seen

The black screen after the crash (bring-up.md "Seen on the way") is a separate question: does WSERV
release a dead DSA/DSB client? It was not looked at here.
