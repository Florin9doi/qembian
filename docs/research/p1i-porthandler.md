# P1i: c32's port handler panics E32USER-CBase 40 (porthandlersvr.dll)

Static reading of SE's `porthandlersvr.dll` (ROFS1 `Sys/Bin`, code at 0xee53d000 on our boots; offsets below
are from that base). Tags: [verified] read in the code / ROM manifest, [inferred] not checked on a run.

## Summary

The port handler's `CCable` (RTTI `6CCable`, vtable 0xee5440b8) is built inside a TRAP; its `ConstructL`
starts a USB personality (`RUsb` request pending, `SetActive`) and then creates a `CEdbPort` (RTTI `8CEdbPort`)
whose `ConstructL` does `User::LoadPhysicalDevice(_L("EUART"))` and leaves on any error but -11. Our hybrid ROM
has no `euart.pdd` (only our Syborg serial PDD as `EUART1.PDD`), so the load fails, the leave unwinds the
cleanup stack, `CCable` is destroyed with its `RUsb` request still pending, and `~CActive` panics CBase 40.

**Fix:** put our Syborg serial PDD into the kernel side a second time as `\sys\bin\euart.pdd` (P1i only).
Its factory name equals EUART1's, so whichever loads second gets -11, which this code accepts.

## The call chain [verified]

| offset | what |
|---|---|
| +0x40c | thread start-up, under TRAP (+0x444): `RCommServ::Connect`, then `CCable::NewL` (+0x1b54) |
| +0x1b54 | `CCable::NewL`: `User::AllocZL(0x7c)`, ctor (+0x1b22, `CActive(-20)`, `RUsb` ctor = usbman ord 21 at +0x44), `CleanupStack::PushL`, `ConstructL` |
| +0x1a5c | `CCable::ConstructL` (below) |
| +0x212c | `CEdbPort::NewL` (`AllocZL(0x140)`, ctor +0x20d8, PushL, vtable slot 0x50 = `ConstructL`) |
| +0x2178 | `CEdbPort::ConstructL`: `LoadPhysicalDevice("EUART")`, error != 0 && != -11 -> `User::Leave`; then `LoadLogicalDevice("ECOMM")` (same rule) |
| +0x1980 | `~CCable`: deletes members +0x28/+0x24/+0x20/+0x2c/+0x30/+0x38/+0x34, `RHandleBase::Close(iUsb)`, usbman ord 23 (`~RUsb`), `~CActive` **without `Cancel()`** -> CBase 40 |

The scanned stack frames +0x2191 (return from the `User::Leave` at +0x218c), +0x215d, +0x1b0f, +0x1b75, +0x42b,
+0x457 match this exactly; +0x19f7/+0x1a03 are `~CCable` D1/D0 called by the cleanup stack.

`CCable::ConstructL` (+0x1a5c), in order [verified]:
1. `CActiveScheduler::Add(this)`; `iUsb.Connect()` (usbman ord 20), leave on error. Works on our side.
2. `CRepository::NewL(uid)`, `Get(key 1)` -> personality id (default 1 if the key is missing), at +0x6c.
3. P&S publisher (+0x38, +0x4330: `RProperty` at +0x20).
4. usbman ord 33 `(iUsb, personality, iStatus)` (= `RUsb::TryStart(TInt, TRequestStatus&)` [inferred from the
   arguments]) + **`SetActive()`**. On our null USB controller this request stays pending [inferred from the
   bring-up notes], which is fine as long as nobody deletes the object.
5. `CUsbPort` (RTTI `8CUsbPort`) at +0x24: CSY `ECACM`, port `ACM::1`, internal `MUXCOMM::3`. Constructed fine.
6. **`CEdbPort` at +0x2c: CSY `ECUART`, port `COMM::4`, internal `MUXCOMM::4`. Its `ConstructL` leaves here.**
7. A timer/callback object at +0x28 (+0x3108).

So the destroyed-active object is `CCable` and its pending request is the `RUsb` start (step 4); USB/RUsb itself
is not at fault, and nothing is wrong with ECACM/ACM::1, the bearers or CAIFMUX for this panic.

The `UsbBearer.ph` / `BtBearer.ph` / `IrBearer.ph` strings are not on this path (not needed for the fix; not
examined further).

## What our side lacks [verified]

- P1i core ROM: `sys/bin/euart.pdd` (0xf83b11e0) **and** `sys/bin/euart1.pdd` (0xf8036f60), `ecomm.ldd`
  (`build/firmware/SE-P1i/R6E30/CDA162024-70/core-files.txt`, `kernel-drivers.txt`).
- Our hybrid kernel side (`hybrid-manifest.txt`): `EUART1.PDD` (= `_syborgmv_eserial.pdd`, Syborg serial) and
  `ECOMM.LDD`; **no `euart.pdd`**. `LoadPhysicalDevice("EUART")` -> KErrNotFound -> leave.
- `kernel.missing_drivers = []` in `models/se-p1i/model.toml` is therefore wrong: `euart.pdd` was missing.

## Proposed change

`models/se-p1i/model.toml` (the stand-in mechanism already writes
`device[VAR]= $REL/_$BP_<file>.<ext> \sys\bin\<rom_name>.<ext>`, scripts/build-standins.sh, so a baseport
binary under another ROM name needs only a flag that keeps it out of the stand-ins' bld.inf):

```toml
# H-p1i-euart-alias: SE's port handler (c32exe, porthandlersvr.dll CEdbPort) loads the PDD "EUART" and leaves on
# KErrNotFound, destroying its active CCable (E32USER-CBase 40). Our Syborg serial PDD serves it under that name too
# (same factory name as EUART1.PDD: the second load returns -11, which the port handler accepts).
[[standins]]
name = "euart"
kind = "pdd"
file = "eserial"            # _syborgmv_eserial.pdd, the baseport's serial PDD (also our EUART1.PDD)
rom_name = "euart"
baseport = true             # built by the baseport, not src/standins: no bld.inf line
role = "the PDD name SE's port handler loads (EUART), served by the Syborg serial PDD"
```

`tools/device.py`:

```python
def standins_bldinf(prof):
    mmps = [standin_mmp(s['name']) for s in prof.get('standins', []) if not s.get('baseport')]
```

Check every other place that resolves a stand-in to `src/standins/<name>` (`device.py check`, the hosted
`device_standins` in `tools/hosted/htest.py` / `device-bundle.py`) and skip `baseport = true` entries there too.
Regression test (`tests/device_py_test.py`, e.g. `test_p1i_euart_alias_baseport_standin`): `standins_oby` of
se-p1i contains `device eserial pdd euart`, `standins_bldinf` has no `euart` line. Add H-p1i-euart-alias to
docs/hacks.md.

Cheaper alternative if the flag is unwanted: a one-line `device[...]` entry appended to `extra.oby` for this
device. Putting the PDD in ROFS instead would not help: it must be on the kernel side.

## Risks after the fix [inferred]

- `CEdbPort` uses `ECUART` port `COMM::4`. Syborg serial `DoCreate` accepts units 0..3 only
  (`syborg_serial.cpp`, `default: KErrNotSupported`). Construction does not open the port (it only builds two
  helpers, +0xeca, and leaves -2 if a helper is missing), so a later open of `COMM::4` would fail with -5 inside
  the `CEdbPort` signal path, not at start-up. If that turns into a new panic, give the serial PDD a fifth unit
  (a dummy one with no hardware) or answer it from a stand-in.
- The `RUsb` request (step 4) stays pending forever on the null controller: harmless while `CCable` lives.

## The start-up stall after the power menu [inferred]

- The panic is in the port handler's own thread, after its `!PortHandler` server started (`CServer2::StartL`
  +0x168) and after `RThread::Rendezvous` (+0x274), so c32 itself is not left waiting for it. Whether c32exe
  dies with it depends on the thread being process-critical (not checked).
- After the panic the `!PortHandler` server is gone and the P&S values the port handler publishes
  (`RProperty::Set` at +0x236c / +0x237e, keys 3 and 4 of its category; +0x438e for the USB personality) are
  never set. A start-up component that connects to `!PortHandler` or waits on one of those properties would
  stall: plausible but not proven. First check after the fix: does the progress bar move on; if not, look for
  clients blocked on `!PortHandler` / c32 (`sydb strace PortHandler`) before other theories.
