# 5800 (S60 5.0) accssy.dll: what the accelerometer stand-in must do

Research note (2026-11-17), mock-level, static reading only. Firmware: 5800 RM-356 31.0.101, core XIP
`Sys/Bin/accssy.dll` at 0x807a6d28 (text 0x2a48). Disassembly: `tools/xipdis.py core-flat.img core.json
accssy.dll` (Thumb). [V] = read in the firmware code (address given), [I] = inferred.

**The 5800 accssy speaks a different protocol from the N8/Belle one** (docs/devices/nokia-n8-rm596/accelerometer.md):
no streaming request, no CenRep chip check; the samples come through a **Publish & Subscribe property** whose
category/key the driver hands out. Every request is synchronous (DoRequest + User::WaitForRequest). No
DoCancel is imported. Imports are only EUser, sensrvutil, ECom, DRTAEABI, DFPAEABI, SCPPNWDL: **no CenRep,
no HAL, no FeatureManager** [V].

## 1. Why our DoCreate never runs [V]

Open function 0x807a8f18:
```
if (iHandle > 0) return KErrInUse;
User::LoadLogicalDevice("accelerometerldd.ldd")    // KErrNone or KErrAlreadyExists, else return it
User::LoadPhysicalDevice("accelerometerpdd.pdd")   // same; on error FreeLogicalDevice("accelerometer")
DoCreate(_L("accelerometer"), TVersion(1,0,0x718), KNullUnit (-1),
         &_L("accelerometer.pdd"), NULL, EOwnerProcess)   // on error Free both devices
```
- Unit is **KNullUnit = -1** (0x807a8f74 `subs r3, r1, #1`, r1 = 0), not 0 as on the N8.
- The kernel (kernel/eka/kernel/device.cpp 711) accepts -1 with KDeviceAllowUnit (`iUnit >= KMaxUnits` is
  false), then calls the named PDD's `Validate(aUnit = -1, info, LDD's iVersion)` (device.cpp 619).
  **Our PDD returns KErrNotSupported for aUnit != 0**, so ChannelCreate fails with -5 before the LDD's
  DoCreate; the caller LeaveIfErrors (0x807a6e5c) and the SSY dies quietly. Root cause [V statically].
- Version: the kernel passes the LDD's own iVersion (2.9.0xffff) to the PDD, so major 1..2 passes; the
  client's 1.0.0x718 is seen only by our DoCreate, which does not check it. No change needed there.

## 2. Controls (DoControl, synchronous) [V numbers and call sites; meanings I]

| # | a1 | a2 | caller | stand-in answer |
|---|----|----|--------|-----------------|
| 5 | TDes8* TBuf8<168> | – | right after open (0x807a6e66), LeaveIfError | write the 168-byte **driver info block**, KErrNone |
| 6 | TInt* channel (0, 2, 4) | TDes8* TPckgBuf<12> | start of a channel (0x807a8516 / 8790 / 89ae), Leave on error | write `{TUint32 category, TUint32 key, 0}`; define that P&S property (§4); KErrNone |
| 0 | – | – | stop XYZ channel (0x807a859a, 0x807a7cfc) | stop publishing; KErrNone |
| 2 | – | – | stop wake-up channel (0x807a8814, 0x807a7d40) | KErrNone |
| 4 | – | – | stop double-tap channel (0x807a8a3c, 0x807a7d1e) | KErrNone |

### Control 5 info block (168 bytes) [V 0x807a6d98]
Only three words are read; each, if non-zero, registers one channel with the sensor server:

| offset | channel registered | channel type | data size | driver channel # |
|--------|--------------------|--------------|-----------|------------------|
| 0x10 | XYZ axis | 0x1020507E | 24 | 0 |
| 0x2c | wake-up | 0x1020507F | 16 | 2 |
| 0x78 | double tap | 0x10205081 | 16 | 4 |

Zeros (what our N8-style WriteInfo/WriteZeros gives without an info block) = **no channel at all**.
[I, from the N8 lesson "the server rejects a list missing any static channel of accssy's ECom resource"]:
set all three. The block length is not checked [V].

## 3. Requests (DoRequest, all synchronous: complete at once) [V numbers; contents not decoded]

| # | a1 | when | stand-in |
|---|----|------|----------|
| 7 | TDesC8* 16 bytes (control object +184) | once before the first channel use (flag +52), 0x807a70ae, 0x807a7dc8 | KErrNone (non-zero -> Leave) |
| 9 | none (DoRequest(9, status)) | deconfigure: destructor (0x807a8fa0) and before re-sending 7 | KErrNone |
| 0 | TDesC8* 20 bytes (axis config) | XYZ start after control 6 (0x807a855a), and on property change (0x807a7eb0) | KErrNone; start publishing |
| 2 | TDesC8* 20 bytes | wake-up start (0x807a87d4), 0x807a80b0 (no a1) | KErrNone |
| 4 | TDesC8* 28 bytes | double-tap start (0x807a89fc, 0x807a7fd2) | KErrNone |

Anything else: KErrNotSupported. The config blobs need not be decoded (accepted and ignored) [I].

## 4. Sample delivery: P&S, not a request [V 0x807a84e6 start, 0x807a8412 RunL]

```
DoControl(6, &TInt(0), &pckg12)               // pckg payload: +0 category UID, +4 key
RProperty::Attach(TUid(cat), key, EOwnerThread)
RProperty::Subscribe(iStatus)
DoRequest(0, s, &TPtrC8(cfg, 20)); WaitForRequest   // != KErrNone -> Leave
RunL: if (iStatus != KErrNone) Leave;
      TBuf8<20> b; LeaveIfError(prop.Get(b));        // byte-array property (EInt -> KErrArgument -> Leave)
      t = TTime::HomeTime();                           // the SSY stamps the sample itself
      x = (TInt8)b[0]; y = (TInt8)b[1]; z = (TInt8)b[2];   // b.AtC: length < 3 panics USER 21
      ... buffer as TSensrvAccelerometerAxisData {t, x, y, z}; Subscribe again
```
- Property: **byte array**, at least 3 bytes, signed 8-bit x, y, z. Each Set completes the subscriber,
  so publish on a timer (every 40 ms, current vector, also when unchanged) while channel 0 is started
  (request 0 .. control 0).
- Scale [I]: the 5800 chip is a LIS302DL (8-bit, ±2 g, 18 mg/digit, 1 g ~ 56). orientationssy uses only
  ratios; games may assume ~56 per g. Host vector G = 1000 -> `clamp(v * 56 / 1000, -128, 127)`.
- Category/key are the driver's choice [V: the client takes them from control 6]. Read policy must
  pass (sensorserver). Kernel side: `RPropertyRef::Attach(cat, key); Define(RProperty::EByteArray, KPass,
  KPass, 20)` (as src/standins/bootprops), `Set(TPtrC8(xyz, 3))`. Proposed: category 0x1020507E (the
  channel type uid, no clash known [I]), key = driver channel number (0, 2, 4). Wake-up/double-tap
  properties are defined but never published (no event).

## 5. Differences from the N8 protocol (our stand-in)

| | N8 / Belle (implemented) | 5800 |
|---|---|---|
| DoCreate unit | 0 | **KNullUnit (-1)**: PDD Validate must accept it |
| version asked | 2.0.0x861 | 1.0.0x718 (not checked by anyone) |
| info block | control 0, 248 bytes | **control 5, 168 bytes**, words 0x10/0x2c/0x78 |
| control 0 | info block write | **stop XYZ** (no args) |
| controls 2, 4 | get config / – | **stop** wake-up / double tap |
| control 6 | – | **P&S category/key for a channel** |
| requests | 1 sync start, 3 stream, 5/6/7 pending | **0/2/4 sync start (cfg blob), 7 sync cfg, 9 sync deconfigure** |
| samples | request 3, 24-byte records | **P&S byte array, 3 x int8**, time from HomeTime |

Overlapping numbers (controls 0, 2; requests 2, 4, 7) mean different things, so this must be a
**declared variant**, not guessed from arguments.

## 6. Proposed changes

Profile (models/nokia-5800-rm356/model.toml):
```toml
[[standins]]
name = "accelerometer"
rom_name = "accelerometerldd"
kind = "ldd"
family = "common"
config = { protocol = "pns" }        # 5800 accssy: control 5/6, sync requests, samples via P&S int8 (docs/research/5800-accssy.md)

[board.accelerometer]                # QEMU syborg-sensor; control 5 block, 168 bytes
info = "10=01000000,2c=01000000,78=01000000"   # XYZ, wake-up, double tap channels present [V 0x807a6d98]
```
(If `info` needs the length to reach 0x7c, the existing INFO register path already pads with zeros up to
KRegInfoLen.)

accelerometerpdd.cpp:
```cpp
virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
	{ return (aUnit == 0 || aUnit == KNullUnit) && aVer.iMajor >= 1 && aVer.iMajor <= 2 ? KErrNone : KErrNotSupported; }
// 0: N8/Belle accssy; KNullUnit: 5800 accssy (DoCreate unit -1) [V 0x807a8f74]
```

accelerometer.cpp (sketch; `iPns` from `BoardConfig("accelerometer.protocol") == "pns"` in DoCreate):
```cpp
// controls
if (iPns) switch (aFn) {
	case 5: return WriteInfo(a1, 168);                       // info block, client max length
	case 6: { TInt ch; ThreadRawRead(a1, &ch, 4); if (ch != 0 && ch != 2 && ch != 4) return KErrArgument;
	          TInt r = DefineProp(ch);                         // RPropertyRef iProp[ch]: Attach(0x1020507E, ch),
	          if (r != KErrNone) return r;                     //   Define(EByteArray, KPass, KPass, 20), KErrAlreadyExists ok
	          TUint32 p[3] = { 0x1020507E, (TUint32)ch, 0 };
	          return Kern::ThreadDesWrite(iClient, a2, TPtrC8((TUint8*)p, 12), 0, KChunkShiftBy0, iClient); }
	case 0: iRunning = EFalse; iTimer.Cancel(); return KErrNone;   // stop XYZ
	case 2: case 4: return KErrNone;                           // stop wake-up / double tap
	default: return KErrNotSupported; }
// requests: complete every one at once
if (iPns) {
	if (aReq != 0 && aReq != 2 && aReq != 4 && aReq != 7 && aReq != 9) return KErrNotSupported;
	if (aReq == 0) { iRunning = ETrue; if (!iTimer.IsPending()) iTimer.OneShot(NKern::TimerTicks(KSampleMs)); }
	Kern::RequestComplete(iClient, aStatus, KErrNone); return KErrNone; }
// Sample(), pns: if (!iRunning) return; read host x,y,z as today; TInt8 b[3] = scale(v, 56/1000);
//   iProp[0].Set(TPtrC8((TUint8*)b, 3)); re-arm the timer.  Close: iProp[*].Delete(); Close().
```
- KMaxReq must reach 10 only if the request-status array is reused; in the pns variant no request is kept.
- Sample() runs in a DFC (thread context), so RPropertyRef::Set is allowed there [I].

## Open points
- Whether the sensor server requires all three channels (ECom resource accssy.rsc static channel list
  not read) [I: set all three].
- Meaning of the 16/20/28-byte config blobs and of payload word +8 of control 6 (not read by accssy) [I].
- Exact count-per-g that games (Raging Thunder GR) expect; 56 is the LIS302DL nominal value [I].
