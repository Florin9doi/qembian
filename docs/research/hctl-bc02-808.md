# H4+ HCTL → BC02 transport contract (Nokia 808, Belle FP2 113.010.1508)

Scope: what `h4plushciv2_hctl.dll` requires of the `BC02` logical device, enough to decide how an emulated
Bluetooth controller can be attached later. **Static analysis only** — no run, no QEMU change, no patch
removal. BC02.LDD was inspected only to confirm it is an ordinary EKA2 LDD; its handlers were **not** recovered
(see "Not recovered").

> **Review 2026-11-17** (byte-level spot checks of §2-§4, §9A, §14 confirmed: strings, TVersion, unit 4, the
> TCommConfigV01 writes, the LDD vtable at 0x801F73C8). The document grew by passes and later sections correct
> earlier ones; the stale statements are now fixed in place or marked. For a mock, §3 (as corrected), §4, §9A-§9C
> and §14.4 are what matter; §11-§16 (the LDD's internal completion pump) go beyond what a replacement PDD needs
> and are not required reading. The registry contract `hctl-bc02-transport` was re-synced from these sections.

## 1. Binary identity

| | |
|---|---|
| module | `Sys/Bin/h4plushciv2_hctl.dll` (XIP, in the 808 core) |
| uid3 | `0x2001cb65` |
| unpacked code | **7,468 B**, code base **0x8086de98**, one export (`ord1`, ECom `ImplementationGroupProxy` at +0x270) |
| identity | sha1 of the code section `2cc0afdc9761b6c7df55cba06480f9b7b84aa177`; index crc32/size in `imports.json` |
| Ghidra program | `build/roms/nokia-808-rm807/RM-807_113.010.1508_04.01+C00.01/ghidra/h4plushciv2_hctl.dll.elf` (ARM:LE:32:v8, image base 0x8086de98) |
| imports | 66 resolved from this firmware + `Nokia_Symbian3_SDK_v1.0` (euser 59, `dso`-named); `hctl_base_ord15`, `hctl_base_ord17` stay **unnamed** (no `.dso`) |

All offsets below are **module-relative** (= ELF address − 0x8086de98).

## 2. Channel open (module +0x14A8, `H4pHctl_OpenChannel_BC02`, entry 0x8086F340)

```c
TVersion v(TInt8 1, TInt8 0, TInt16 0x21D);     // 4 bytes; e32cmn.h:2056  (iMajor:iMinor:iBuild)
ch.DoCreate(name = "BC02" /*TLitC16 @0x8086F4A4*/, &v, aUnit, aDriver = NULL, aInfo = NULL, aType = 0);
```

- **Logical device name `BC02`**, version **1.0 build 0x21D (541)**.
- **Unit is 4** on the bring-up path: `FUN_8086E94C` (+0xB4) does `movs r1,#0x4; bl OpenChannel`, then
  `ConfigureAndWaitReady(1)` (120 000 baud); on failure it calls `DoControl(9)`. A second caller,
  `FUN_8086E70C` (+0x874), opens with **unit 0** — both exist, and which one runs first depends on the
  caller, so the unit is *not* a constant of the transport.
- **Device names are now identified** (`FUN_8086E6D4`, +0x83C): `User::LoadPhysicalDevice(TPtrC16("BC02_UART1"))`
  at 0x8086E6F2 (string at **0x8086EAC8**) and `User::LoadLogicalDevice(TPtrC16("BC02"))` at 0x8086E6F6
  (string at **0x8086EAE0**). So the PDD is `BC02_UART1.PDD` (shipped as `bc02_uart1.PDD`) and the LDD is
  `BC02.LDD`.
- TVersion layout corrected: `TInt8 iMajor; TInt8 iMinor; TInt16 iBuild` (local header `e32cmn.h:2056`), i.e. a
  4-byte struct. The halfword pack/`ldr` sequence at 0x8086F354-0x8086F368 **is** a correct TVersion, not a
  suspicious packing.

## 3. Controls, requests, cancels (verified immediates from disassembly)

| call | number | arguments | meaning | evidence |
|---|---:|---|---|---|
| `DoControl` | **0** | `TBuf<0x34>` (52 B) out | `EControlConfig` — read `TCommConfigV01` | +0xA10 |
| `DoControl` | **1** | same 52 B block in | `EControlSetConfig` — apply, starts the UART | +0xA10 |
| `DoControl` | **3** | – | `EControlSignals`: returns the signal mask; HCTL polls **bit 0 = `KSignalCTS`** (d32comm.h:81) | +0xA10 |
| `DoControl` | **4** | `(0x10, 0)` then `(0, 0x10)` | `EControlSetSignals(set, clear)`: **set `KSignalRTS` (0x10)** after CTS, **clear RTS** in the type-6 handler | +0xA00, +0xBA8 |
| `DoControl` | **9** | – | teardown/flush (called after a failed open and from the destructor) | +0x14EC |
| `DoControl` | **0xB** | (a1 == 0) | abort the pending receive; no caller inside this DLL | +0xCDE |
| `DoRequest` | **0** | `(TRequestStatus*, buffer)` | **receive** into the buffer at obj+0x20 | +0xCF6, issued at +0xED8 |
| `DoRequest` | **1** | `(TRequestStatus*, TDesC8*, &len)` | **transmit**; `len = *des & 0x0FFFFFFF` | +0xD16 |
| `DoRequest` | **2** | `aStatus=&[sp+4]`, `a1=0 (r3)`, `a2=2 ([sp])`, then `User::WaitForRequest` | synchronous, returns the request's result word; **meaning unknown** | +0xCBE |
| `DoCancel` | mask **1** | – | cancels **request 0 (receive)**; channel from obj+0x2C (`H4pHctl_CancelReceive`, 0x8086EEF6) | +0xD0A |
| `DoCancel` | mask **2** | – | cancels **request 1 (transmit)**; channel from obj+0x24 (`H4pHctl_CancelTransmit`, 0x8086ECCA) | +0xD32 |

**`DoCancel` takes a bitmask, not a request number** (`RBusLogicalChannel::DoCancel(TInt aMask)`); bit N is
request N. Confirmed against BC02.LDD, whose `DoCancel` (module +0x3BE8) branches per bit: mask 1 completes
request 0 with `KErrCancel` (-3) and clears the receive state, mask 2 completes request 1, mask 4 request 2,
mask 8 request 3. The **receive** is therefore cancelled with mask 1 through `H4pHctl_CancelReceive`; the
earlier note's "DoCancel(1) cancels request 1" was wrong.

### Config block (control 0/1) — partially recovered

Written before control 1 (`+0xA10`): a constant `0x80000000` in one field, `3` in the next, two zero fields, and
`0x1204` in a later field, plus the baud rate selected by the caller:

| selector | baud |
|---|---|
| 1 | 120000 |
| 2 | 921600 |
| 3 | 4000000 |

The **field offsets inside the 52-byte block are not established** (the decompiler shows stack locals only;
they would need the disassembly of `0x8086E8A8` traced into the `TBuf` data pointer). This is the main gap for a
PDD replacement.

## 4. Framing (derived from code, not from the name "H4+")

**Transmit** (`H4pHctl_Transmit`, +0xDC4): the caller's descriptor has the **H4 packet type in byte 0**; the whole
descriptor goes to request 1 with its length. Before sending, the upper layer is notified with kind `2` when
`byte0 == 0x01` (HCI command) and kind `0` otherwise; the notification receives the descriptor **minus byte 0**.

**Descriptor slicing** (important when reading offsets): the receive buffer arrives whole, and the demux works on
`Mid(1)` — the **H4 type byte is dropped**. The type-6 branch slices **again** (`Mid(1)`, dropping the BC02
sub-opcode) before calling the handler. So an offset *k* inside the type-6 handler refers to **original buffer
byte k+2**; that is how `pkt[10]`/`pkt[11]` below map to original +12/+13.

**Receive** (`H4pHctl_DemuxRecv`, +0xEFA), switch on byte 0 of the *original* buffer:

| type | handling | evidence |
|---:|---|---|
| 2 | ACL: `len = LittleEndian::Get16(&pkt[3])` — **little-endian** (`es_sock.h:1507`), total `len+4`, handed up as kind 1 | +0xF0E |
| 3 | SCO: whole packet, kind 1 | +0xF48 |
| 4 | Event: `code = pkt[1]`, `plen = pkt[2]`, `Left(slice, plen+2)` → **forwarded as kind 3 first**, and only afterwards does `code == 0x10` trigger an extra `-44` report | +0xF4E |
| 6 | BC02 transport packet (slice dropped one more byte): baud/vendor handshake | +0xBD0 |
| 7 | transport packet, upper object at +0x30 notified, state cleared | +0xC70 |
| 8 | transport packet handed to the second upper object (+0x30) as kind 1 | +0xF7C |

Corrections to the earlier reading: `LittleEndian::Get16` is **little-endian**, and HCI event **0x10 is Hardware
Error** (Command Complete = 0x0E, Command Status = 0x0F). The packet is **not** withheld: forwarding to the upper
layer happens before the 0x10 test, so the event is delivered and a `-44` status follows it. Types 6/7/8 are
**not** HCI — they are UART/transport level.

**Baud-rate handshake — driven by HCTL, not by BC02.LDD** (`H4pHctl_OnTransportType6`, +0xBD0): on a type-6 packet it cancels **a CTimer**, not the
receive object — `CActive::Cancel(*(obj+0x18))`; that timer is armed with **4 000 000 µs (4 s) right after a
post-init transmit** (`FUN_8086E9EE`, +0xB56: `ldr r1,[0x8086EAF8]=4000000; ldr r0,[obj+0x18]; CTimer::After`).
Then `DoControl(4, 0, 0x10)`, store slice bytes at index 10/11 (original +12/+13) in obj+0x34/+0x35, and
`ConfigureAndWaitReady(obj, 3)` → re-apply the config at 4 Mbaud and poll control 3 again.

## 5. Ownership, completion, cancellation, errors

- **Buffer ownership**: the transmit descriptor comes from the caller of `Transmit`; this DLL stores a second
  pointer at obj+0x20 and drops the async request into the LDD. Whether the LDD copies the bytes or the buffer
  must stay valid until completion is **not established** (needs the BC02.LDD `DoRequest` handler).
- **Completion**: three distinct things are conflated by the interface and must not be:
  1. *transport write completion* — request 1's `TRequestStatus`;
  2. *controller events* — type 4, forwarded as data; code `0x10` (**Hardware Error**, §4; not Command Status)
     additionally produces a `-44` report after the event is delivered;
  3. *unsolicited controller events* — type 4 with any other code, passed up as data.
  Only (1) is observable here; (2) and (3) are the caller's business.
- **Cancellation**: `DoCancel` takes a bitmask (§3): mask 1 cancels the receive (request 0, `KErrCancel`), mask 2
  the transmit (request 1). What happens to a pending receive at `Close` is unknown.
- **Errors seen in this DLL**: `KErrTimedOut` (`-0x21`) when CTS never appears within ~1 s (1000 × 1 ms sleep),
  `-14` = **KErrInUse** when transmitting on a closed transport (`e32err.h`), `-44` on a Hardware-Error event,
  `-5` = KErrNotSupported from the baud selector for any other value. In BC02.LDD request 1 returns
  `-0x12` = **KErrNotReady** when a transfer is already queued.

## 6. Not recovered (blockers, with the exact place to look)

| question | where |
|---|---|
| `DoControl` 0/1 block layout (52 B): every field's offset, width, meaning | `BC02.LDD` handler for control 0/1 (one exported function, +0x0 of the LDD) |
| `DoControl` 4 semantics (`0x10` bit), 9, 0xB | `BC02.LDD` control dispatch (`ekern` 72 imports → `DoControl` switch) |
| request 0/1/2 semantics on the kernel side, buffer pinning/copying | `BC02.LDD` `DoRequest` handlers |
| which device names `LoadLogicalDevice`/`LoadPhysicalDevice` are given | two call sites near module +0x80x in this DLL |
| the LDD↔PDD interface (`bc02_uart1.PDD`), power/wake before bytes flow | `bc02_uart1.PDD` (needs egpio/dma/karap/PRM — the same class that blocked the accelerometer PDD) |
| controller vendor initialisation requirements | `h4plushciv2_initialisor.dll` (44,640 B) — deliberately out of scope here |

## 6a. BC02.LDD boundary (module offsets from code base 0x801F1A88)

Identity: `Sys/Bin/BC02.LDD`, uid3 `0x101FB3F8`, **code 23,012 B at 0x801F1A88**, sha1
`ec181a36c6d734fcee9015e02869d3fa8b552347`, **one export: ord1 at module +0xA54 (0x801F24DC)** — the factory,
i.e. it is *not* at offset zero. ARMv5 ARM code; Ghidra program `BC02.LDD.elf` (146 functions).

**Factory (+0xA54)** (verified): `DBase::operator new(0x38)`, `memclr(0x34)`, `DObject` ctor, vtable
`0x800BBE88` stamped, `TVersion(1, 1, 0x21D)`. So the LDD is version **1.1 build 541** while HCTL asks for
1.0/541 — the LDD validates version and unit in its `Create` path (not audited in detail).

**Control dispatch — `BC02_Channel_DoControl` at module +0x44B0 (0x801F5F38)**:

| fn | verified behaviour | PDD call |
|---:|---|---|
| 0 | `Kern::ThreadDesWrite(aThread, a1, TPtrC8(ch+0x18, 0x34), 0, 0x40000000)` — returns the **52-byte config block** from the channel's cached copy | – |
| 1 | `Kern::ThreadDesRead(... , 0x34)` into a zeroed buffer, then `BC02_Channel_ApplyConfig` | – |
| 2 | fills a `TBuf<0x2C>` and `Kern::ThreadRawWrite`s it to the client | vt[+0x24] |
| 3 | returns `(*(pdd vt[+0x14]))()` — the status word HCTL polls for bit 0 | vt[+0x14] |
| 4 | rejects `a1 & a2` with -6, else `pdd vt[+0x18](pdd, aSet, aClear)` and `ch[0x154] = (ch[0x154]|aSet) & ~aClear` | vt[+0x18] |
| 5 | internal state query | – |
| 6 | `pdd vt[+0x2C](pdd, value)` | vt[+0x2C] |
| 7 | returns `ch[0x1A4]` | – |
| 8 | internal helper | – |
| 9 | `BC02_Channel_Stop(ch, 1)`, `ch[0x27] = 0` (guarded: other functions first run `(*ch)[+0x40]`) | – |
| 0xA | `pdd vt[+0x30](pdd, 1)` | vt[+0x30] |
| 0xB | `ch[0x26] = 1`; `a1 == 0` ⇒ abort the pending receive | vt (indirect) |

**Cancel — `BC02_Channel_DoCancel` at +0x3BE8 (0x801F5670)**: `aMask` bit N = request N.
mask 1 ⇒ clears RX bookkeeping (+0x1B8..+0x1C8), cancels the objects at +0xD0/+0xB0, completes **request 0** with
`KErrCancel`, `pdd vt[+0x10](pdd, 1)`; mask 2 ⇒ clears TX bookkeeping (+0x1D0..+0x1F0), `(*(ch+0x28) vt[+0x2C])`,
cancels +0x110/+0xF0, completes **request 1**, `pdd vt[+0x10](pdd, 2)`; mask 4 ⇒ request 2; mask 8 ⇒ request 3.

**Requests**: request 1 (`+0x48B0` region, `param_2 == 1`) copies the client descriptor with
`Kern::ThreadRawRead`/`ThreadDesRead` into a kernel ring buffer (fields +0x1CC..+0x1F4, buffer ptr +0x204) and
returns `-0x12` = **KErrNotReady** if a transfer is already queued; request 0 is the receive side; request 2 exists (mask
4 cancels it) but its handler was not reached in this pass; request 3 likewise. **Request 2's meaning is
unknown** — HCTL uses it synchronously with `a1=0, a2=2`.

**Config block**: 52 bytes (`0x34`). Cached at channel+0x18 (what control 0 returns) and copied to channel+0x60
by `ApplyConfig`; four derived words from config[+0x10] land at channel+0x158/+0x15C/+0x160/+0x164. **Field-by-field
layout is not recovered**, and no local serial-driver header was matched yet — the HCTL side writes `0x80000000`,
`3`, `0`, `0`, `0x1204` into *some* of these fields (their HCTL-side offsets are themselves unresolved, see §6).

**Six observed PDD call slots** (vtable slots into the object at channel+0x94, arguments in brackets; this is what the LDD calls, not a complete interface definition):
`+0x10(1|2)` on cancel, `+0x14()` status, `+0x18(set, clear)` masked flags, `+0x24(TBuf<0x2C>)` query,
`+0x2C(value)` and `+0x30(1)`. Channel object also holds an object at +0x28 with its own vtable (`+0x2C` called on
TX cancel). Execution context: all of these run in the LDD's channel-request context (thread of the requesting
client); the PDD is expected to complete asynchronously for the data path.

## 7. Recommended implementation boundary

Evidence supports **original BC02.LDD + a replacement PDD** as the next experiment, with these caveats.

*For:*
- `BC02.LDD` is a plain EKA2 logical device: imports are `ekern` only (72 references: `DBase`/`DObject`/`DFactory`,
  `Kern::ThreadDes*`, `Kern::RequestComplete`, `BTrace`), one export (the factory), and the control/request/cancel
  surface above is fully expressed in LDD terms. Nothing Nokia-platform specific appears in its import table.
- The PDD boundary is small and explicit: six vtable calls (+0x10, +0x14, +0x18, +0x24, +0x2C, +0x30) plus
  whatever the data path uses, and `BC02_UART1` is a *named* physical device that the user-side HCTL loads
  (`User::LoadPhysicalDevice("BC02_UART1")` at 0x8086E6F2 in h4plushciv2_hctl.dll, not in the LDD), so a
  replacement PDD only has to be installed under that name.
- Keeping the LDD keeps the serial-channel plumbing out of our code. The baud switch and the type-6 handshake are
  driven by the HCTL (§4) and must be answered by the emulated controller: the config (EBpsSpecial +
  iSpecialRate), CTS within 1 s, and the type-6 packet within 4 s of the post-init transmit.

*Against / unknown:*
- "Imports only ekern" is not proof it loads: the LDD uses kernel-internal APIs (`Kern::ThreadDesWrite`,
  `ThreadRawRead`, `RequestComplete`, `DObject`/`DFactory` layouts, vtable `0x800BBE88`) whose **ABI must match
  our EPL kernel**, and it links against symbols by ordinal from *this* firmware's ekern. That needs a symbol/ABI
  check before anything else.
- `bc02_uart1.PDD` pulls in `egpio`, `dma`, `karap`, `powerresourcemanager` — the platform-driver class that
  already blocked the accelerometer PDD. A replacement PDD must therefore answer only the six calls above and
  must not need DMA or GPIO; whether the LDD tolerates a PDD without DMA is **unknown**.
- The controller side still needs the transport-level handshakes: the baud switch driven by a type-6 packet
  (bytes at +12/+13), whatever `DoControl(4)` latches, and the 4-second watchdog. Those must be implemented by the
  emulated peer, so "original LDD retained" does **not** make the controller handshake disappear.
- The 52-byte config block is not fully decoded, so a replacement PDD cannot be written correctly yet: the LDD
  *derives* four words from it (channel+0x158..+0x164) and passes them to the PDD.

Verdict: **viable in principle, blocked on (a) an ekern ABI/symbol check for BC02.LDD, (b) the 52-byte config
layout, (c) request 2's meaning.** If (a) fails, the fallback is a replacement LDD implementing the HCTL contract
from §3–§4 directly (no PDD), which loses the UART framing but needs no kernel-internal ABI.

## 7a. Smallest follow-up test

Load `BC02.LDD` in a minimal kernel-side probe (or just link-check it against `ekern.exe.sym`): open
`BC02_UART1`, `DoCreate` unit 4, `DoControl(0)` and check that a 52-byte block comes back and that `DoControl(3)`
returns without a PDD (expect a defined error, not a fault). That single test decides ABI viability — the one
assumption the recommendation rests on.

## 8. Artifacts

- Ghidra program with 17 names and 10 plate comments (`h4plushciv2_hctl.dll.elf`), saved.
- Bulk decompilation: `ghidra/h4plushciv2_hctl.dll.c` (144 functions).
- Registry: component `bt-hctl` (binary + sha1), contract `hctl-bc02-transport`, claims 22-27, task
  `h4plushciv2-hctl-contract` (left **claimed/unfinished**, blockers recorded).
## 9. Source-to-binary comparison (supersedes the guesses in §3–§5 where they differ)

Source: `external/oss.FCL.sf.os.kernelhwsrv/kernel/eka/include/d32comm.h` +
`d32comm.inl` (Symbian^3-era `DComm` / `RBusDevComm` contract). Every table row below was checked against the
firmware handlers; where the firmware only *resembles* the source, it says so.

### A. Config table — `TCommConfigV01`, 52 bytes (0x34)

`sizeof` agreement: the header's field list lays out to 0x34 with 4-byte alignment, the LDD's control 0 writes
exactly `0x34` bytes and `ApplyConfig` copies `0x34`. The HCTL values were read from the store instructions in
`FUN_8086E8A8` (+0xA10).

| off | width | source field | value written by HCTL | binary access | conf |
|---:|---:|---|---|---|---|
| 0x00 | 4 | `TBps iRate` | **`EBpsSpecial` = 0x80000000** (`d32public.h:88`) | store | verified |
| 0x04 | 4 | `TDataBits iDataBits` | **3 = `EData8`** (`d32public.h:35`) | store | verified |
| 0x08 | 4 | `TStopBits iStopBits` | **0 = `EStop1`** | store | verified |
| 0x0C | 4 | `TParity iParity` | **0** (`EParityNone`) | store | verified |
| 0x10 | 4 | `TUint iHandshake` | **0x1204** = `KConfigObeyCTS(0x04) | KConfigFreeDTR(0x200) | KConfigObeyRTS(0x1000)` | store | verified |
| 0x14 | 4 | `TUint iParityError` | not written | – | – |
| 0x18 | 4 | `TUint iFifo` | not written | – | – |
| 0x1C | 4 | `TInt iSpecialRate` | **120000 / 921600 / 4000000** | store | verified |
| 0x20 | 4 | `TInt iTerminatorCount` | not written | – | – |
| 0x24 | 4 | `TText8 iTerminator[4]` | not written | – | – |
| 0x28..0x2B | 1 each | `iXonChar`, `iXoffChar`, `iParityErrorChar` + pad | not written | – | – |
| 0x2C | 4 | `TSir iSIREnable` | not written | – | – |
| 0x30 | 4 | `TUint iSIRSettings` | not written | – | – |

The `iRate = EBpsSpecial` / `iSpecialRate = <baud>` pairing is what makes this mapping self-consistent
(`d32comm.h:161`: "Special data rate, not listed under TBps. Use this, when iRate is set to EBpsSpecial").

### B. Request table — standard `DComm` numbering, verified in both modules

| req | source name (`d32comm.h:578-592`) | HCTL use | BC02.LDD handler | conf |
|---:|---|---|---|---|
| 0 | `ERequestRead` | receive into obj+0x20 | receive side; cancel bit 0 completes it | verified |
| 1 | `ERequestWrite` | transmit descriptor, `len = *des & 0x0FFFFFFF` | copies the client descriptor into a kernel ring buffer; `-0x12` (KErrNotReady) if one is queued | verified |
| 2 | `ERequestBreak` | synchronous `a1=0, a2=2`, waits, returns the result word | completes on cancel bit 2 | **number matches Break; the HCTL call arguments do not** (`Break(aStatus, TInt aTime)` takes one value) - meaning unknown |
| 3 | `ERequestNotifySignalChange(aStatus, TUint& aSignals, TUint aSignalMask)` | not used by HCTL | cancel bit 3 completes it | plausible, unverified |

Cancels: `ERequestReadCancel=1`, `WriteCancel=2`, `BreakCancel=4`, `NotifySignalChangeCancel=8` — exactly the
four bits BC02.LDD's `DoCancel` (+0x3BE8) branches on.

### C. Control table

| ctl | source name (`d32comm.h:598+`) | firmware behaviour (BC02 +0x44B0) | conf |
|---:|---|---|---|
| 0 | `EControlConfig` | writes 0x34 bytes from `ch+0x18` | verified |
| 1 | `EControlSetConfig` | reads 0x34 bytes, `ApplyConfig` (copies to `ch+0x60`) | verified |
| 2 | `EControlCaps` | fills a **`TBuf<0x2C>`** = 44 bytes = `TCommCapsV01` (11 × `TUint`) from PDD `vt[+0x24]` | verified size, caps content unknown |
| 3 | `EControlSignals` | returns PDD `vt[+0x14]()`; HCTL tests `KSignalCTS` | verified |
| 4 | `EControlSetSignals(aSet, aClear)` | rejects `aSet & aClear` with -6, else PDD `vt[+0x18](set,clear)` and `ch[0x154] = (ch[0x154]|set)&~clear` | verified |
| 5 | `EControlQueryReceiveBuffer` | `FUN_801f72e0` | plausible (returns a count), unverified |
| 6 | `EControlResetBuffers` | PDD `vt[+0x2C](value)` | plausible, unverified |
| 7 | `EControlReceiveBufferLength` | returns `ch[0x1A4]` | verified |
| 8 | `EControlSetReceiveBufferLength` | `FUN_801f554c(ch, a1)` | plausible, unverified |
| 9 | **`EControlMinTurnaroundTime`** (LDD enum, `d32comm.h:614`); `EControlFlowControlStatus` is 9 only in the *client* enum at `d32comm.h:834` | firmware: `FUN_801f526c(ch,1)` sets `ch[0x9C]=3` and calls port `vt[+0xC](1)`, then `ch[0x27]=0` → **stops the channel**, not a turnaround-time getter | **DIFFERENT** — source declaration and firmware instructions both cited; do not apply either name to BC02 |
| 10 | **`EControlSetMinTurnaroundTime`** (`d32comm.h:618`) | firmware: PDD/port `vt[+0x30](1)` and falls through into 0xB | matches the *shape* (set one value) but unverified |
| 11 | **`EControlDebugInfo`**, `#ifdef _DEBUG_DEVCOMM` only (`d32comm.h:621`) | firmware: `ch[0x26]=1`; `a1 == 0` ⇒ abort the pending receive | **does not match** DebugInfo (which fills a `TDes8`); Nokia-specific abort |

### D. Differences between the source implementation and this binary

1. **Control 9 is `EControlMinTurnaroundTime` in the LDD enum of this source, and the firmware stops the
   channel anyway** (verified instructions above). The earlier note's "9 = EControlFlowControlStatus" came from the
   *client* enum (`RBusDevComm`) and was an unfounded transplant.
2. **Controls 10 and 11 exist in the LDD enum** as `EControlSetMinTurnaroundTime` and, only under
   `_DEBUG_DEVCOMM`, `EControlDebugInfo`. The firmware's 0xA looks like a "set one value" (fits 10) and 0xB aborts
   the pending read (does **not** fit 11), so 0xB is Nokia-specific.
3. **Control 2 returns `TCommCapsV01`** (0x2C) rather than a device-specific block — consistent with `DComm`.
4. **Requests, cancel bits and controls 0–8 match the source exactly**, with the same argument shapes
   (`EControlSetSignals(TAny* set, TAny* clear)`, `EControlConfig(TAny* config)`), so the *read/write/break/
   signal* contract is the standard serial one; only the control numbering tail and the PDD differ.
5. HCTL's `iHandshake = KConfigObeyCTS|KConfigObeyRTS|KConfigFreeDTR` means the LDD/PDD is expected to honour
   RTS/CTS flow control — a replacement PDD must implement it, not just the byte pipe.

### E. PDD identity and boundary

| | |
|---|---|
| loaded file | `BC02_UART1` → `BC02_UART1.PDD` (shipped as `bc02_uart1.PDD`), loaded by the user-side HCTL via `User::LoadPhysicalDevice(TPtrC16 "BC02_UART1")` at 0x8086E6F2 (not by the LDD) |
| logical channel name | `BC02` (`DoCreate` name, HCTL +0x14A8) |
| registered physical-device name | **`BC02.Rap`** — length-prefixed ASCII at PDD module **+0x675C** (0x801F1874 = `08 00 00 00`, text at 0x801F1878), referenced once from `FUN_801EB5B8` (0x801EB5F0) as the `DObject::SetName` argument. A second record at +0x6768 holds length 9, `BT_driver` (purpose unknown) |
| PDD module | 26,856 B code at 0x801EB118, uid3 `0x101FB3F7`, sha1 `c44a3028…43d0`, **5 exports**: ord1 = factory (+0x458), ord2..5 = data (module +0x677C..0x6788) |
| factory | `DBase::operator new(0x30)` → 48-byte `DPhysicalDevice`, `DObject` ctor, vtable `0x800BC180`, `TVersion(...)` (values not yet read) |
| PDD → LDD callbacks | not established in this pass: the RX path needs the PDD's completion callback, which lives behind the `DComm` channel objects at `ch+0x28`/`ch+0x94`, not in the six control slots |
| buffer ownership | TX: the LDD copies the client descriptor into a kernel ring buffer (+0x1CC..+0x1F4, ptr +0x204), so **the client buffer need not outlive the request**; RX: the LDD owns a ring buffer and copies out on read |
| context | controls/requests execute in the requesting client's kernel-side channel context; the data path is expected to complete from the PDD's DFC/interrupt context — **not demonstrated** for this PDD |
| DMA | `bc02_uart1.PDD` imports `dma`(6) — whether the LDD tolerates a PDD without DMA is unknown |

### F. Staged checks (replaces the earlier "open without a PDD" probe)

1. **Static ordinal/signature audit** (no runtime): resolve every `ekern` ordinal BC02.LDD imports against
   `ekern.exe.sym` of this firmware and compare the class layouts it assumes (`DObject`/`DFactory`/`DLogicalDevice`
   sizes, vtable slots `+0x40`, `DComm`-style channel members). *Proves*: the imports can bind. *Does not prove*:
   that the layouts match our kernel's build.
2. **Factory/load check**: load `BC02_UART1.PDD`+`BC02.LDD` under our kernel and call the LDD factory (ord1 only),
   expecting either a `DLogicalDevice*` or a *defined* error. *Proves*: import binding and the DObject/DFactory
   layout are close enough to construct the device. *Does not prove*: that a missing/failing PDD is an ABI
   problem — a factory failure can equally be a missing export, a UID check or a version check.
3. **Channel open/config check** (only after the PDD creation contract is known): `DoCreate` unit 4, `DoControl(0)`,
   and `DoControl(3)`. *Proves*: the config block round-trips and signals are readable. *Does not prove*: RX/TX,
   cancellation or flow control.

## 10. Status of the acceptance criteria

Satisfied: A (channel/version/unit/device names), B (control/request/cancel numbers and arguments), F (H4 framing),
C (config block, now fully mapped), and the standard-vs-Nokia comparison.
Not satisfied: **D (buffer ownership/lifetime at the PDD boundary — LDD side only), E (completion and close
semantics at the PDD boundary), G (power/wake beyond `KSignalRTS`/`KSignalCTS`), H (vendor-init dependencies)**,
request 2's meaning, controls 5/6/8, and the PDD callback/DFC path.

Blockers, in the order they bite:
1. the PDD → LDD completion callback and ring-buffer contract (needs the `DComm`-style channel objects at
   `ch+0x28`/`ch+0x94`, and `bc02_uart1.PDD`'s `RX`/`TX` methods);
2. `ekern` ABI/symbol compatibility for the kernel-internal APIs the LDD uses;
3. DMA tolerance (the PDD imports `dma`).

## 11. One transmit path, end to end (request 1 = ERequestWrite)

All offsets module-relative; BC02.LDD code base 0x801F1A88, channel object pointer = `ch`.

| # | module / address | operation | object / field | caller → callee | evidence |
|---:|---|---|---|---|---|
| 1 | LDD +0x5188 `0x801F6C10` | request dispatch, case 1 | `ch+0x9C` busy state; `ch+0x204` status slot; `ch+0x1E0` descriptor | `DoRequest` entry → this | `cmp [ch+0x9C],#2` → `-0x12` (KErrNotReady, `e32err.h`); `ch[0x204]==0` required |
| 2 | LDD +0x5188 | client length word | `Kern::ThreadRawRead(aThread=ch+0xA0, aLenPtr, &len, 4)` | LDD → ekern | call at 0x801F6CE2; `len` = the client's `TInt` (HCTL passes `a2=&len`) |
| 3 | LDD +0x5188 | store request state | `ch+0x1E0 = aDes`, `ch+0x204 = aStatus` (4-byte slot holds the pointer) | → `FUN_801F669C` | stores at 0x801F6CE8-0x801F6CF0 |
| 4 | LDD +0x4C14 `0x801F669C` | arm the port for TX | port object `ch+0x94`, `FUN_801F22C8(port, 2)` | LDD → port | call at 0x801F66A2 |
| 5 | LDD +0x4C14 | read transmit-side state | `(*(ch+0x28) vt[+0x14])()` then `FUN_801F5858(ch, value)` | LDD → object at +0x28 | 0x801F66A6 / 0x801F66AE |
| 6 | LDD +0x4C14 | flow-control gate | `(ch[0x158] & ~ch[0x154]) == 0` else `-0x1D` | local | 0x801F66B2-0x801F66C6 |
| 7 | LDD +0x4C14 | copy client bytes into the kernel ring buffer | ring `ch[0x1CC]` (base), tail `ch[0x1D0]`, head `ch[0x1D4]`, wrap `ch[0x1D8]`; `Kern::ThreadDesRead(aThread, aDes, TPtr8, offset)` | LDD → ekern | 0x801F2D00-0x801F2D20 |
| 8 | LDD +0x4C14 | **start physical transmission** | `(*(ch+0x28) vt[+0x10])()` (no arguments) | LDD → object at +0x28 | call at 0x801F2DD4 (jumptable-adjacent indirect call) |
| 9 | LDD +0xDDC `0x801F2864` | **complete the client** | `Kern::RequestComplete(aThread=ch+0xA0, &ch+0x204, result)` then **`ch[0x204] = 0`** | TX-done handlers (callers 0x801F3370, 0x801F34B0, 0x801F2D08, ...) → helper | helper case 1, 0x801F29xx |
| 10 | LDD +0x3BE8 `0x801F5670` | **cancel that write** (mask 2) | clears `ch+0x1D0..ch+0x1F0` (incl. the descriptor at +0x1E0), `(*(ch+0x28) vt[+0x2C])(value)`, cancels the objects at +0x110/+0xF0, `FUN_801F2864(ch,1,KErrCancel)`, then port `vt[+0x10](2)` | `DoCancel` → LDD | instructions at 0x801F5700-0x801F576C |
| 11 | LDD +0x37E4 `0x801F526C` | stop the port | `ch+0x9C = 3`; `port(ch+0x94) vt[+0xC](aMask)` | stop path (control 9) → port | 0x801F37E4-0x801F37F8 |

**Double completion is prevented by the status slot itself**: the dispatcher refuses a second write while
`ch[0x204] != 0` (`-0x12`), and the completion helper zeroes `ch[0x204]` immediately after
`Kern::RequestComplete`. Cancellation therefore completes the request once with `KErrCancel`, and any later port
callback that would complete it again finds the slot empty. A late TX-done after cancel also has its ring-buffer
state cleared (step 10), so the pump has nothing to send.

**Buffer ownership (verified)**: the client descriptor is *copied* into the LDD's ring buffer at step 7, so the
client's buffer need not outlive the request; the length is read separately at step 2. The LDD's ring buffer is
the only buffer the PDD can touch, and it belongs to the LDD.

### Required replacement-PDD behaviour demonstrated by this path

Only these are demonstrated, and only for TX:
1. accept a *start transmission* call with **no arguments** (step 8) — the LDD has already put the bytes in its
   ring buffer and expects the PDD to fetch them through the DComm-style port (whose own calls are the
   `+0x14/+0x18/+0x24/+0x2C/+0x30` slots observed from the control handlers);
2. report TX-done so that the LDD's TX-done handler can call the client's `TRequestStatus` (step 9);
3. accept a stop/abort for the TX direction on cancel (steps 10, 11) and not complete the request itself;
4. tolerate the request being completed by the LDD while the PDD still holds a reference to the ring buffer — i.e.
   it must stop touching the buffer once aborted.

### Exact remaining break in the chain

Steps 8 and 9 are indirect calls through the object at **`ch+0x28`** (vtable slots `+0x10` and `+0x2C`). See §12:
that object is an **embedded sub-object of the channel**, not a heap pointer, so the "PDD channel vs port"
question is answered; what is still unknown is the *entry* that delivers "TX space available / sent" from the PDD
into the port's queue objects (the six `ord1205` objects created at +0xB0..+0x130 and +0x298), which is what
eventually drives the completion pump.

### Optional bounded ABI check (TX path only)

| ekern import | evidence | status |
|---|---|---|
| `Kern::ThreadDesRead(DThread*, const TAny*, TDes8&, TInt, TInt)` | import present in BC02.LDD, called at 0x801F2D18 with 5 register args | **resolved**, signature matches the call shape |
| `Kern::ThreadRawRead(DThread*, const TAny*, TAny*, TInt)` | called at 0x801F6CE2 | **resolved**, signature matches |
| `Kern::RequestComplete(DThread*, TRequestStatus&, TInt)` | called at 0x801F29xx with `&ch+0x204` | **resolved**; the *object layout* `TRequestStatus` at +0x204..+0x20B is **unverified** against our kernel's `DThread`/`Kern` build |
| `Kern::ThreadGetDesMaxLength` (RX path only) | called in `FUN_801F69A0` | resolved, not used by TX |

The check proves the *ordinals resolve and the call shapes agree*; it proves nothing about class layouts
(`DObject`/`DFactory`/`DThread`) inside our kernel — that remains the ABI risk.

## 12. Channel creation, execution context and the completion pump (answers the §11 break)

`FUN_801F4C24` (module **+0x319C**, body to 0x801F50B7) is the channel-creation function. Verified statements:

| what | evidence |
|---|---|
| capability gate | `Kern::DoCurrentThreadHasCapability(1 /*ECapabilityCommDD*/, "BT Driver Permission Denied")`; failure returns `-0x2E` |
| version gate | `Kern::QueryVersionSupported(TVersion(1,1,0x21D), aClientVersion)`; failure returns `-5` (KErrNotSupported) |
| **execution context** | `DLogicalChannel::SetDfcQ(dfcq = *(ch+0x20) + 0x34)` then `TMessageQue::Receive(ch+0x30)` — channel work runs on that **DFC queue**, not on the client thread |
| six queue objects | `ord1205(ch+0xB0, ch+0x2C)`, `+0xD0`, `+0xF0`, `+0x110`, `+0x130`, `+0x298` — created against the queue at `ch+0x2C`; their callbacks were **not** examined |
| `ch+0x94` | 0xC4-byte **port object**, `DBase::operator new(0xC4)` → `FUN_801F1E98(+0x410)`; a `DPowerHandler` with vtable `0x801F73A8`, three `TDfc`s on the queue `ch+0x2C`: callbacks `+0x384` (stop: clears `port+0xC0`, `Fun_801F526C(ch,0)`), `+0x328` (power-up: port `vt[+0x14]`, `PowerUpDone`), `+0x2C8` (power-down: `PowerDownDone`), plus one object registered via `ekern ord1193` |
| **`ch+0x28`** | **a pointer field, not an embedded object** (corrected). Assembly is `p = *(void**)(ch+0x28); *(void**)(p+8) = ch;` then `vt = *(void**)p; call vt[slot]`. This is `DLogicalChannelBase::iPdd`: the local kernel source (`kernel/eka/drivers/ecomm/d_comm.cpp:314`) does exactly `((DComm *)iPdd)->iLdd=this;`, and `iLdd` sits at **+8** of the PDD's `DComm` object → so `p` **is the PDD-side `DComm` channel object** and `+8` is its `iLdd` back-pointer. Slots called on it: `+0x10` start TX (no args), `+0x14` state read, `+0x18` set/clear flags, `+0x24` caps fill, `+0x2C` abort, `+0x30` set value, `+0x54`/`+0x58` used by the pump/construction |
| buffers | RX ring 0x400 at `ch+0x1CC` (size at `+0x1D8`); receive bank `Kern::Alloc(0x8000)` at `ch+0x220` with `+0x224 = base+0x4000` and quarter marks at `+0x1A8/+0x1AC/+0x1B0`; `Kern::Alloc(0x2000)` at `+0x228`; `Kern::Alloc(0x200)` at `+0x22C`; `ch+0x1A4 = 0x4000` (what control 7 returns) |
| **completion pump** | `FUN_801F30A4` (module **+0x1A1C**) calls `(*(ch+0x28) vt[+0x54])()`, `FUN_801f72e0`, `FUN_801F2E18`, and completes request 1 via `BC02_CompleteRequest(ch,1,...)` at 0x801F3370 / 0x801F34B0 — so the client's `TRequestStatus` is completed **on the DFC queue**, from the port's receive/status path |
| who enters the pump | 0x801F3538, 0x801F3600 (port code), **0x801F54A8** (`+0x3A68`, called by `ApplyConfig` and control 6 ResetBuffers), **0x801F6BBC** (inside `BC02_RxIssue`, +0x4F18) |

So the remaining unknown is now a **call registration**, not an object identity: which callback of the six queue
objects at `ch+0xB0/+0xD0/+0xF0/+0x110/+0x130/+0x298` (or the `ord1193` object in the port) is invoked by the port
when a PDD write completes, and therefore what a replacement PDD must call to make step 9 happen. Evidence needed:
the callbacks passed to `ord1205` for those six objects (they are constructed at +0x319C..+0x3200 and were not read
back), plus `ekern ord1193`'s identity.

### What this changes for a replacement PDD

- The PDD must complete its write **asynchronously** and the LDD will finish the client request on the DFC queue —
  a PDD that completes from the client's thread would violate the LDD's assumptions about that queue.
- Because the channel is a `DPowerHandler` and the port's power DFCs stop/restart the port via
  `FUN_801F526C(ch, mask)`, a replacement PDD has to survive `PowerUp`/`PowerDown` sequences even before data flows.
- The 16 KiB (`0x4000`) receive bank and the 1 KiB TX ring are allocated with `Kern::Alloc`, i.e. **kernel heap**
  owned by the LDD — a replacement PDD must not free or repurpose them, and must tolerate them being reset by
  control 6 (`ResetBuffers`) and by the restart path `FUN_801F54F0` (+0x3A68).

## 13. Object model corrected, and the exact remaining edge

**Object model (verified, with the source cross-check):**

| thing | what the code says | source analogue |
|---|---|---|
| `ch+0x28` | **pointer** to the PDD channel object; the code writes `*(*(ch+0x28)+8) = ch` and calls `(**(void**)*(ch+0x28)+slot)()` | `DLogicalChannelBase::iPdd`; `((DComm*)iPdd)->iLdd = this` with `iLdd` at +8 (`d_comm.cpp:314`) |
| `ch+0x94` | **separately allocated** 0xC4-byte `DPowerHandler` object (ctor `BC02_PortCtor` +0x410) owning three TDfcs | `iPowerHandler = new DCommPowerHandler(this)` (`d_comm.cpp:318`) |
| `ch+0x20` | another pointer whose `+0x34` is used as the DFC queue | should be `((DComm*)iPdd)->DfcQ(aUnit)`; **the pointer differs from `iPdd`, so one of the two offsets is mis-attributed** — open |
| channel vtable | *(superseded by §14.1: the table is 0x801F73C8, 22 slots; DoCancel/DoControl/DoRequest are slots 11/12/13 - the addresses below are right, the slot numbers are not)* table **0x801F73F4**, 11 entries: slot 0 dtor 0x801F50B8, **slot 8 (+0x20) = DoCancel 0x801F5670, slot 9 (+0x24) = DoControl 0x801F5F38, slot 10 (+0x28) = DoRequest 0x801F6C10** | matches `DLogicalChannelBase`'s cancel/control/request triple → the LDD is an ECOMM-style `DComm` LDD, as the create sequence already suggested |

The create sequence in §12 maps one-to-one onto `DChannelComm::DoCreate` (`d_comm.cpp:258-330`): capability
`ECapabilityCommDD` → `QueryVersionSupported` → `SetDfcQ` → `iMsgQ.Receive()` → **TX buffer +
`iTxFillThreshold = size>>1`** → RX buffer `Alloc(rxsize<<1)` with the error buffer behind it → quarter/half
thresholds → **`((DComm*)iPdd)->iLdd = this`** → `PddCheckConfig`/`FailSignals`/`HoldSignals` → power handler +
`DoPowerUp()`.

**The PDD→LDD calls exist and are visible.** In `bc02_uart1.PDD` the calls go through `*(pdd+8)` (its `iLdd`) at
vtable offsets **+0x30, +0x34, +0x38 (3 args), +0x3C, +0x48** (PDD call sites 0x801F…1718, 2021, 2466, 4717, 1730
in the decompilation). On the *channel* table 0x801F73F4 those offsets land past the 11 real slots and resolve to
unrelated table/constructor entries (0x801F7358 has no instructions, 0x801F2524 is a constructor), which means the
object `iLdd` points to is **not** the channel base table I found — it is another class in the same image whose
vtable I did not locate.

**Exact remaining edge (do not summarise, this is the missing evidence):**

| | |
|---|---|
| caller | `bc02_uart1.PDD`, e.g. instruction at PDD 0x801F…2466: `ldr r0,[pdd,#8]; ldr r1,[r0]; ldr r2,[r1,#0x38]; blx r2` with three args |
| callee | unknown: `*(void**)(pdd+8)` → vtable slot **+0x38** |
| object | the LDD object whose address the LDD stored into the PDD's +8 (`iLdd`); the store site is the LDD's `*(*(ch+0x28)+8) = ch` in `BC02_ChannelCreate` (+0x319C) → so `iLdd == ch` |
| what is missing | the **channel's real (full) vtable**: `ch`'s vtable must have ≥19 entries (offset +0x48), but the only table I located at 0x801F73F4 has 11 and the tail does not continue coherently. Either the channel class stamps a longer table elsewhere in the image (not found by my consecutive-word scan because of interleaved RTTI/negative entries), or the object at `ch` is a sub-object of a larger class whose vtable starts before it |
| evidence needed | the vtable pointer actually stored into the channel at construction (the store is in ekern's `DLogicalChannelBase` ctor, so it must be read from the LDD's data — grep the image for a table containing 0x801F5670/0x801F5F38/0x801F6C10 **and** code at +0x30..+0x48) |

`ekern ord1205` / `ord1193` could not be named from this checkout: the profile's KABI/`.sym` resources do not
cover those ordinals and the ekern export table does not carry names. What is verified about them: `ord1205(object,
queue)` is called six times in `BC02_ChannelCreate` and the objects were already constructed with `TDfc`
callbacks, so it is a **queue assignment** (`TDfc::SetDfcQ`-shaped), not a constructor and not an enqueue;
`ord1193` takes an object and a callback (`LAB_801f1d14`) plus a queue and was **not** resolved — it may be a
timer or another DFC-like constructor, and that is unproven.

### Provenance note

The interrupt/DFC question the brief raised is answered for the *LDD* side only: the LDD completes the client on
its DFC queue, so a PDD that completes from ISR context must enqueue a DFC (or the queue-assignment shape above
would be pointless). **Not established**: which of the six `ord1205` objects or the port's `ord1193` object a
given PDD completion path uses.

## 14. Channel vtable located, and one complete TX completion chain

### 14.1 The channel vtable (derived from the constructor store, not from Ghidra labels)

`BC02_ChannelCtor` at **0x801F3604** (+0x1B7C) does `ldr r1,[0x801F4460]` where the literal **0x801F4460 =
0x801F73C8**, then `str r1,[r0,#0]` — so **vptr = 0x801F73C8**. The table runs to 0x801F741C and is terminated by a
0 at 0x801F7420: **22 slots**. The word at 0x801F73C4 is the RTTI pointer for it (→ 0x801F734C, a typeinfo record
whose vptr is 0x800BD094). `0x801F73F4` is **slot 11**, as stated.

| slot | offset | target | kind |
|---:|---:|---|---|
| 0 | +0x00 | 0x801F3830 | destructor (stamps the same vptr) |
| 1 | +0x04 | 0x801F398C | deleting destructor |
| 2–8, 10 | +0x08..+0x20, +0x28 | 0x8009CF9C, 0x8009CFA4, 0x8009CFB0, 0x8009CFB8, 0x8009CFC4, 0x8009081C, 0x80090880, 0x800907CC | **ekern ROM** implementations of inherited `DObject`/`DLogicalChannelBase` virtuals |
| 9 | +0x24 | 0x801F4C24 | `BC02_ChannelCreate` |
| 11 | +0x2C | 0x801F50B8 | channel helper (dtor tail) |
| **12** | **+0x30** | 0x801F3EB4 | PDD callback: RX status/error+indication handling |
| **13** | **+0x34** | 0x801F46AC | PDD callback: **give the PDD the next TX bytes** (memcpy out of the ring) |
| **14** | **+0x38** | 0x801F4868 | PDD callback: RX data byte (into `ch+0x220` buffer, index `ch+0x19C`) |
| **15** | **+0x3C** | 0x801F49F0 | PDD callback: RX error byte (same buffer, error side) |
| **16** | **+0x40** | 0x801F4B5C | PDD callback: wrapper → `FUN_801F4B00` |
| **17** | **+0x44** | 0x801F4B9C | PDD callback: sets `ch[0x9C] = 4` (transport state) |
| **18** | **+0x48** | 0x801F52C4 | PDD callback: **TX progress / completion** |
| 19 | +0x4C | 0x801F5670 | `DoCancel` |
| 20 | +0x50 | 0x801F5F38 | `DoControl` |
| 21 | +0x54 | 0x801F6C10 | `DoRequest` |
| 22 | +0x58 | 0 | end |

**Corrections to the previous message's offsets** (measured from the bytes at 0x801F73C8..): the three dispatch
slots are **+0x4C / +0x50 / +0x54**, not +0x50/+0x54/+0x58; the PDD callback 0x801F4B9C sits at **+0x44**, not
+0x48; and there is one callback the message did not list, **+0x40 = 0x801F4B5C**. Everything else matches.

`FUN_801F2524` is confirmed a **destructor**, not a constructor: it stamps `PTR_FUN_801f7428` (= 0x801F2524, i.e.
the logical-device vtable's own dtor), calls **`TDynamicDfcQue::Destroy(obj+0x34)`** and then
**`DLogicalDevice::~DLogicalDevice`**.

Constructor-established DFCs also check out from the ctor assembly: `adr r1,0x801F35C4; add r0,r0,#0xB0;
bl 0x801F1BDC` (TDfc ctor for `ch+0xB0`), then `adr r1,0x801F3584; add r0,r0,#0x20` (`ch+0xD0`), matching the
list 0xB0→0x801F35C4, 0xD0→0x801F3584, 0xF0→0x801F2DD8, 0x110→0x801F2B00, 0x130→0x801F29E4, 0x298→0x801F27E4.

### 14.2 One complete TX completion chain (PDD → LDD → client)

| # | where | operation | object / args | evidence |
|---:|---|---|---|---|
| 1 | LDD `DoRequest` +0x5188 | request 1 accepted | `Kern::ThreadRawRead(aThread=ch+0xA0, aLenPtr, &len, 4)`; `ch+0x1E0 = aDes`; `ch+0x204 = aStatus` | call 0x801F6CE2, stores 0x801F6CE8 |
| 2 | LDD `BC02_TxPump` +0x4C14 | copy client bytes into the ring, then start the PDD | `Kern::ThreadDesRead` into `ch+0x1CC` (head `ch+0x1D4`, wrap `ch+0x1D8`); then **`(*(ch+0x28) vt[+0x10])()`** | 0x801F2D00-0x801F2DD4 |
| 3 | **PDD** | PDD fetches data | calls `iLdd->vt[+0x34]` = **0x801F46AC** with a destination buffer | `bc02_uart1.PDD` call site with slot +0x34 |
| 4 | LDD **+0x2C24** (`0x801F46AC`) | **fill the PDD buffer from the ring** | `memcpy(dst, ch[0x1CC] + ch[0x1D4], n)`, advances `ch[0x1D4]`, wraps at `ch[0x1D8]` | body: `+0x1CC` base, `+0x1D4` head, `+0x1D8` size |
| 5 | **PDD** | PDD reports progress/completion | calls `iLdd->vt[+0x48]` = **0x801F52C4**, **no arguments** | PDD call site with slot +0x48 |
| 6 | LDD **+0x383C** (`0x801F52C4`) | gate on "write in flight" | `if (ch[0x1C4] == 0) return;` then compares `ch[0x1BC] + ch[0x250]` against the client length `ch[0x1C0]` | body, first two tests |
| 7 | LDD **+0x383C** | **completion is executed inline, in the PDD caller's context** | clears `ch[0x1C4]`, calls **`BC02_RxStatusPump(ch)`** (0x801F30A4) — no DFC is enqueued on this path | call at 0x801F5338 region |
| 8 | LDD pump +0x1A1C | finishes the client request | completes request 1 through `BC02_CompleteRequest(ch,1,result)` at 0x801F3370 / 0x801F34B0 → `Kern::RequestComplete(aThread, &ch+0x204, result)` then **`ch+0x204 = 0`** | helper +0xDDC |

So the LDD's six DFCs are **not** on the TX-complete path: step 7 runs on whatever context the PDD called from.
An interrupt-context PDD would therefore have to defer *itself*; the LDD does not do it for it. (This supersedes the
earlier "the PDD must complete asynchronously / on the LDD's DFC queue" reading.)

### 14.3 Cancel and late completion

`DoCancel` mask 2 (+0x3BE8): clears `ch+0x1D0..+0x1F0` (including the descriptor `ch+0x1E0`), calls
**`(*(ch+0x28) vt[+0x2C])(value)`** = PDD abort at channel-vtable... no: on the *PDD* object, slot +0x2C, then
cancels the objects at `ch+0x110`/`ch+0xF0`, completes request 1 with `KErrCancel` through the same helper (which
zeroes `ch+0x204`), and calls port `vt[+0x10](2)`.

A **late** PDD callback after that finds `ch[0x1C4] == 0` (step 6) and returns — no second completion. If a PDD
were to call slot +0x34 after cancel it would read the cleared ring head/tail, i.e. copy nothing; the client status
slot is already `0`, and `BC02_CompleteRequest` is only reached through the pump's `ch[0x1C4]` gate.

### 14.4 What a replacement PDD must provide (now demonstrated, not inferred)

1. **Start**: accept `vt[+0x10]()` with **no arguments** after the LDD has buffered bytes (step 2).
2. **Pull**: call `iLdd->vt[+0x34](dst)` to obtain the next `n` bytes, and honour the wrap (`ch+0x1D8`); the LDD
   copies, so the PDD's buffer is its own.
3. **Report**: call `iLdd->vt[+0x48]()` with **no arguments** when bytes have left the UART (or an error occurred);
   the LDD will complete the client inline, so the call must be made in a context where `Kern::RequestComplete`
   is legal (i.e. not in an ISR unless the thread object is completed via a DFC first).
4. **Abort**: accept `vt[+0x2C](value)` on cancel and stop touching the ring.
5. **Never complete the client itself**, and never call +0x48 from two contexts concurrently — `ch[0x1C4]` is a
   plain flag, not a lock.
6. Optional/other callbacks it may drive: +0x30 (RX status/indication), +0x38/+0x3C (RX data/error byte),
   +0x40 (wrapper), +0x44 (state → `ch[0x9C]=4`).

## 15. TX/RX completion split (corrects §14.2 steps 7–8 and the role of slot +0x48)

### 15.1 The completion helper's index argument, per call site (assembly)

`BC02_CompleteRequest(ch, aWhich, aResult)` (+0xDDC): `aWhich == 0` → `Kern::RequestComplete(aThread, &ch+0x208, r)`
then `ch+0x208 = 0`; `aWhich == 1` → the same with **`&ch+0x204`**; 2 → break; 3 → notify-signal-change.
So **`ch+0x208` is the receive status, `ch+0x204` is the transmit status** (this confirms the correction and the
earlier read of the dispatcher).

| call site | index passed | disassembly |
|---|---:|---|
| 0x801F3370 | **0** | `mov r1,#0x0; cpy r0,r4; bl 0x801F2864` |
| 0x801F34B0 | **0** (via r2) | `cpy r1,r2; cpy r0,r4; bl 0x801F2864` |
| 0x801F49A8 | **0** | `mov r1,#0x0 …` |
| 0x801F6A54, 0x801F6ACC | **0** | `mov r1,#0x0 …` |
| **0x801F2CFC** | **1** | `mov r1,#0x1; cpy r0,r4; bl 0x801F2864` (result in r6) |
| **0x801F5804** | **1** | inside `DoCancel` (mask 2) → completes the write with `KErrCancel` |
| **0x801F67D0** | **1** | inside `BC02_TxPump` → completes the write when `Kern::ThreadDesRead` fails |
| 0x801F7014 | **2** | break |
| 0x801F2A84 | **3** | `mov r1,#0x3; b 0x801F2864` (tail call) |

**Therefore** the receive/status pump `BC02_RxStatusPump` (0x801F30A4) completes the **receive** request at
0x801F3370/0x801F34B0, not the write. §14.2 steps 7–8 are wrong on that point and are superseded by this section.

### 15.2 The actual write-completion routine: 0x801F2B40

`0x801F2B40` (+0x10B8) is a **jump target, not a call target** (no `bl` reaches it):

* `0x801F67F4` inside `BC02_TxPump` — conditional jump ("descriptor fully consumed / nothing left to copy");
* `0x801F2E14` inside the **`ch+0xF0` DFC callback** `FUN_801F2DD8` — unconditional jump ("bytes have gone out").

Its body reads `ch+0x1E0` (the write descriptor) and, on the completion path, does at 0x801F2CFC:
`mov r2,r6 (result); mov r1,#1; mov r0,r4 (ch); bl BC02_CompleteRequest` → completes **request 1** against
`ch+0x204`. So **this routine is the single place where a successful write is completed**, entered either from the
LDD's own copy loop or from the TX DFC.

### 15.3 `slot +0x48` is an RX-side callback, and its deferral is conditional on context

`BC02_PddTxProgress` (0x801F52C4, channel-vtable **+0x48**) has two exits:

* **inline**: `ch[0x1C4] != 0` and `ch[0x1BC] + ch[0x250] < ch[0x1C0]` and `ch[0x218] > 0` →
  `ch[0x1C4] = 0; BC02_RxStatusPump(ch);` → **completes request 0**;
* **deferred**, 0x801F5448..0x801F5460:
  ```
  801f5448  bl   NKern::CurrentContext()
  801f544c  cmp  r0,#0
  801f5450  add  r0,r4,#0xD0            ; &ch[0xD0] = a TDfc
  801f5454  bne  0x801F5460
  801f5458  bl   TDfc::Enque            ; (0x801F1B3C)
  801f545c  b    return
  801f5460  bl   TDfc::Add              ; (0x801F1AD4)
  ```
  i.e. **`TDfc::Add` when already in a DFC/interrupt-like context, `TDfc::Enque` otherwise** — the standard
  "run it now if legal, else queue it" idiom. The `ch+0xD0` DFC (`FUN_801F3584` → body at 0x801F3500) runs the
  receive pump and then, when `pdd->vt[+0x54]()` reports a non-zero status, clears `ch+0x1B8/+0x1BC/+0x1C0` and
  completes **request 0** with `ch[0x1C8]`.

So slot **+0x48 belongs to the receive side**, and §14's "replacement PDD reports TX progress with +0x48" is
superseded. The write side's PDD→LDD call which leads to 0x801F2B40 is **not yet identified**: the candidates left
on the channel vtable are **+0x40 (`0x801F4B5C` → `FUN_801F4B00`)** and the PDD status query
`iPdd->vt[+0x54]()` that both the `ch+0xB0` and `ch+0xD0` callbacks issue.

### 15.4 Cancel and late callbacks

`DoCancel` mask 2 completes the write with `KErrCancel` at **0x801F5804** through the same helper (so `ch+0x204`
is zeroed), clears `ch+0x1D0..+0x1F0` and calls `iPdd->vt[+0x2C](value)` (abort). Two distinct late cases:

* **late callback while idle**: `BC02_TxPump`'s completion path is only reachable through the copy loop or the
  `ch+0xF0` DFC; with the descriptor/head/tail cleared the routine has nothing to complete and the status slot is
  0, so no second completion happens — **inferred from the cleared state, not verified instruction by instruction**;
* **late callback after a new write starts**: *unknown*. `ch+0x204` is a plain pointer slot, not a generation
  counter, so a stale DFC that jumps into 0x801F2B40 could in principle complete the *new* write early. Whether
  0x801F2B40 re-checks ownership (e.g. `ch[0x1C4]` or the descriptor pointer) was **not** verified — this is the
  one place where a double/early completion is plausible and needs the next pass.

### 15.5 Method caveat found this pass

Ghidra's function boundaries in `BC02.LDD.elf` are **wrong**: `get_function_by_address(0x801F2CFC)` answers
`801f669c` (an address *above* the query) and treats 0x801F2B40..0x801F6850 as one function. Callers in this
section were therefore derived from **xrefs, jump targets and ARM prologue scans** (`stmdb sp!,{…,lr}` at
0x801F2B40), not from Ghidra's function objects. Any later pass must either fix the boundaries or use the same
method.

## 16. The verified write-success chain (supersedes §15.2/§15.3 conclusions)

### 16.1 0x801F2CFC is the FAILED ThreadDesRead path, not success

Inside the routine at `0x801F2B40` the copy loop does `Kern::ThreadDesRead`; on failure it traces `0x81000C` and
falls into `0x801F2CFC`: `mov r2,r6` (the error), `mov r1,#1`, `mov r0,r4`, `bl BC02_CompleteRequest` — i.e. it
completes the write **with the read error**. §15.2's "single place a successful write is completed" is wrong.

### 16.2 Success: `0x801F2A88`, entered from the `ch+0x110` DFC

```
801f2abc  mov  r5,#0
801f2ac0  str  r5,[r4,#480]      ; ch+0x1E0 = 0   (write descriptor cleared)
801f2ac4  str  r5,[r4,#484]      ; ch+0x1E4 = 0
801f2ac8  str  r5,[r4,#488]      ; ch+0x1E8 = 0
801f2acc  ldr  r0,[r4,#516]      ; ch+0x204 = the TRANSMIT TRequestStatus pointer
801f2ad0  cmp  r0,#0
801f2ad4  movne r2,#0            ; result  = 0
801f2ad8  movne r1,#1            ; index   = 1 (write)
801f2adc  movne r0,r4
801f2ae0  blne BC02_CompleteRequest        ; -> Kern::RequestComplete(&ch+0x204, 0) + zero ch+0x204
801f2ae4  str  r5,[r4,#496]      ; ch+0x1F0 = 0
801f2ae8  ldr  r0,[r4,#148]      ; ch+0x94 = the port object
801f2af0  ldr  r2,[r1,#16]       ; port vt[+0x10]   (tail call)
801f2af8  mov  r1,#2             ; direction 2
801f2afc  bx   r2
```
So `0x801F2A88` clears the write state, completes **request 1 with result 0** (only if `ch+0x204 != 0`) and then
stops the port for direction 2. It is called from `0x801F2B00` (the **`ch+0x110` DFC callback**, which is just
`FUN_801f2a88(ch)`) and is also the tail target of `0x801F4AC8` (alternate scheduling path).

### 16.3 The two schedulers

`0x801F7290(ch)` / `0x801F726C(ch)` are the same shape: `NKern::CurrentContext()` → `if in context: TDfc::Add(
&ch+0x110 | +0xF0)` else `TDfc::Enque(...)`:

| scheduler | DFC | callback | body |
|---|---|---|---|
| `0x801F7290` | **`ch+0x110`** | `0x801F2B00` | → `0x801F2A88` → **write completed with 0** |
| `0x801F726C` | `ch+0xF0` | `0x801F2DD8` | jumps into the refill routine `0x801F2B40` |

Cancel cancels **both** scheduled DFCs: `0x801F57EC` (`add r0,r4,#272` = `ch+0x110`, `TDfc::Cancel`) and
`0x801F57F4` (`add r0,r4,#240` = `ch+0xF0`, `TDfc::Cancel`), then completes the write with `mvn r2,#2` (-3 =
`KErrCancel`) at `0x801F5804`.

### 16.3 Decision logic inside the PDD's data-pull callback (LDD slot **+0x34**, `0x801F46AC`)

Tail of the routine (0x801F47F0..0x801F4864), all verified from assembly:

```
801f47f0  ldr  r0,[r4,#492]      ; ch+0x1EC  ("last chunk handed to the PDD" flag/counter)
801f47f4  cmp  r0,#0
801f47f8  ldrne r0,[r4,#484]     ; ch+0x1E4, ldrne r1,[r4,#488] = ch+0x1E8
801f4800  cmpne r0,r1
801f4804  bge  0x801F4824        ; still more to send -> fall through to completion logic
801f4808  bl   0x801F72FC        ; = ch[0x1D0] - ch[0x1D4] (bytes buffered in the TX ring, wrap by ch[0x1D8])
801f4810  ldr  r1,[r4,#476]      ; ch+0x1DC = fill threshold (ring size >> 1)
801f4814  cmp  r0,r1
801f4818  movlt r0,r4
801f4820  blt  0x801F726C        ; buffered < threshold  -> SCHEDULE REFILL (ch+0xF0) and return
801f4824  mov  r0,r4
801f4828  bl   0x801F72FC        ; buffered again
801f482c  cmp  r0,#1
801f4830  popgt {...,pc}         ; >1 buffered -> NEITHER (return)
801f4834  ldr  r0,[r4,#492]      ; ch+0x1EC
801f4838  cmp  r0,#0
801f483c  popeq {...pc}          ; no pending last chunk -> NEITHER
801f4840  mov  r0,#0
801f4844  ldr  r1,[r4,#488]      ; r1 = ch+0x1E8
801f4848  str  r0,[r4,#492]      ; clear ch+0x1EC
801f484c  ldr  r0,[r4,#40]       ; r0 = ch+0x28 = iPdd  (the PDD object)
801f4850  ldr  r1,[r0]           ; iPdd->vtable
801f4854  ldr  r1,[r1,#72]       ; iPdd vtable slot +0x48
801f4858  blx  r1                ; call the PDD, r0 = iPdd, r1 = ch+0x1E8
801f485c  mov  r0,r4
801f4864  b    0x801F7290        ; schedule COMPLETION (ch+0x110)
```

So the three outcomes are:
1. **refill** — `ch+0x1EC == 0 || ch[0x1E4] >= ch[0x1E8]` and buffered `< ch[0x1DC]` → schedule `ch+0xF0`;
2. **neither** — buffered `> 1`, or `ch[0x1EC] == 0`;
3. **completion**: buffered ≤ 1 and `ch[0x1EC] != 0` → clear `ch[0x1EC]`, **call the PDD's own `vt[+0x48]`**
   with `(iPdd, ch+0x1E8)`, then schedule `ch+0x110` → `0x801F2B00` → `0x801F2A88` → client completes with 0.

### 16.4 Do not confuse the two +0x48 slots

| | table | direction | what it is |
|---|---|---|---|
| **PDD slot +0x48** | `iPdd`'s vtable, `ch+0x28` → `*(iPdd) + 0x48` | **LDD → PDD**, `blx` at 0x801F4858 with `(iPdd, ch+0x1E8)` | called just before completion: the LDD hands the PDD the last-chunk length and asks it to wind the transfer down |
| **LDD slot +0x48** | channel vtable 0x801F73C8 + 0x48 = `0x801F52C4` | **PDD → LDD**, called by the PDD | receive-side progress (`BC02_PddRxProgress48`), completes request 0 |

### 16.5 Verified success chain (one line)

`DoRequest(1)` → `BC02_TxPump` (ring fill + `(*(ch+0x28) vt[+0x10])()`) → **PDD pulls** via LDD slot `+0x34`
(`0x801F46AC`) → tail logic picks completion → **calls PDD `vt[+0x48](iPdd, ch+0x1E8)`** → schedules `ch+0x110`
(`0x801F7290`) → DFC callback `0x801F2B00` → `0x801F2A88` → `Kern::RequestComplete(&ch+0x204, 0)` + `ch+0x204 = 0`
→ port `vt[+0x10](2)`.

### 16.6 Cancel, and what stays unknown

Cancel (mask 2): `TDfc::Cancel(ch+0x110)` at 0x801F57EC, `TDfc::Cancel(ch+0xF0)` at 0x801F57F4, write completed with
`-3` at 0x801F5804, PDD aborted via `iPdd vt[+0x2C]`, `ch+0x1D0..+0x1F0` cleared.
**Still unknown:** whether a DFC that is already *running* (not merely queued) when cancel happens can reach
`0x801F2A88` and complete a *new* write — `ch+0x204` is a plain pointer slot and `0x801F2A88` only tests it for
non-zero, so a generation/ownership check is not visible there. Would need the DFC-cancel semantics (`TDfc::Cancel`
vs executing) traced in ekern, which is outside this module.
