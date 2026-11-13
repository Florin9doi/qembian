# P1i CCPU link: what the fake CCPU must answer

The P1i's user side reaches the modem CPU (CCPU, Ericsson) through
`BServer.dll` (SE "BridgeServer", a CPM of the comms root server) -> `caifmux.csy` (C32 module `CAIFMUX`)
-> `bridgedrv.ldd` (kernel side, our stand-in `src/standins/bridgedrv`). Static reading of R6E30 (XIP
addresses in the core image); [V] = read in the disassembly or seen on a boot, [I] = inferred. Linux
`net/caif` (cfctrl/cfmuxl) has the same packet layout and is used for naming only (no code copied).

## bridgedrv.ldd (client: caifmux.csy) [V]

- `User::LoadLogicalDevice("BRIDGEDRV.LDD")` (f818580e; KErrNone or -11 accepted), `DoCreate("BridgeDRV",
  TVersion(1,0,0x431), unit, no info)` (f8188ec8). Units from `Z:\private\101F7989\caif.ini`
  (`DECM_PORT=2, DECMCCPU_PORT=1, VEI_PORT=1, VEICCPU_PORT=2`): units 1 and 2, one channel each.
- Request 0 = read (`TDes8*` ~16 KB, `TInt*` max): one packet per completion, the descriptor length is the
  packet length (no serial framing). Request 1 = write (`const TDesC8*`): one packet.
- Control 0 = configuration (`TBuf8<16>` words {4|7,1,0,1}), result ignored; 1/2 = cancel read/write [I];
  3 = idle hint from a CTimer when no read or write happened since the last tick (f818532c), result ignored.

## CAIF link layer [V]

Byte 0 = link id (0 = control). Control: byte 1 = command, 0x20 = response, 0x10 = error, low nibble
0 LINK_SETUP, 1 LINK_DESTROY, 3 ENUM, 9 MODEM_SET (name [I]). Data link: byte 1 = 0x00 data, bit 7 = flow.

| caifmux writes | reply |
|---|---|
| ENUM `00 03 phy` (retried 10 times) | `00 23 phy` (handler f818636a reads only the error bit) |
| LINK_SETUP `00 00 type prio|phy endpoint [connid x4 when type 1 and chtype 0x10]` (one per opened C32 port) | `00 20 <payload> <channel id>` (parser f8186458: id at payload index 4, or 8 with connid) |
| MODEM_SET `00 09 ..` / LINK_DESTROY `00 01 ..` | `00 29 ..` / `00 21 ..` |
| on a VEI link of chtype != 1: `ch 00 "AT*SECAIF=<n>\r"` | `ch 00 "\r\nOK\r\n"` (any OK/ERROR, f8186836) |

Never send a CCPU-initiated control request or responses 2, 4..8: caifmux panics (104..110). A port counts as
up when its channel is connected; BServer waits for DSR on both its ports (MUXCOMM::1, ::2, both DECM links
on unit 2) and panics `CDecmLinkMgr 13400-err` on a failure (13429 = -29), or -33 after 22 s [I].
Unit 1 (VEI, AT) gets a LINK_SETUP only when someone opens a VEI port (later: semc.tsy [I]).

## BridgeServer messages (payload after the 2 CAIF bytes) [V]

`{ TUint32 type, word1, word2, ... }`; BServer reads `Left(20)` and `Left(24)` of every incoming payload, so
a message to it must have at least 24 bytes. Types: requests 0,2,4,6,8,10,13,15; replies 1,3,5,7,9,11,14,16;
12 special; > 16 ignored (dispatcher f81a6e40). A reply is matched to its pending request by word2.

| BServer sends | the CCPU answers |
|---|---|
| 13 RemoteCreateThreadId `0d000000 <proxy> <id>` (CProxyCCPU's first request, f81a639c) | 14: `0e000000 <status 0> <id>` + zeros to 24 bytes (`CProxyShell` state 2 -> 3). Word 1 is the CCPU's error (f81a8454 -> observer f81a39c2): nonzero flags the bridge failed and every CreateSubSession completes -29 |
| 4 `04000000 <proxy> <id> 01000000 <IID> <n>` (registers interface n: one per proxy, n = 1, 2, ...) | 5: `{5, status, id, value}` (handler f81a4fc8: status and value go to the observer) [V layout, I meaning] |
| 0 `00000000 <proxy> <id> <CLSID> <IID>` (CreateInstance) | 1: `{1, status, id, value}` (handler f81a5644): value must be a key of BServer's proxy table (f81a6ae6: RArray of 28-byte entries {object, object, key, IID}, created only by f81a6a3e); a miss completes the client 0x80030000 |
| 17 (one-way, f81a6360) [I] | nothing |
| Invoke (OPA calls on an object) | not decoded yet |

BServer's error encoding [V]: a TInt error x becomes `x & 0x8000FFFF` (f81a4b0c: -29 -> 0x8000FFE3); a
remote status x becomes `0x80000000 | (x & 0x3FF) << 16` (f81a4af6: 3 -> 0x80030000).

## The fake CCPU (2026-11-13, night)

The CCPU is a host model now: `src/qemu-syborg/hw/arm/syborg_ccpu.c`, part of the modem core (QEMU device
and hosted board), reached through port 0 of the syborg-modem device (TX kind 6 = a packet from bridgedrv
unit N, RX kind 4 = a packet for it); `bridgedrv` only forwards and keeps the 2 ms delivery latency. A
CCPU change needs a QEMU build, not a ROM. Test: `tests/caif_peer_test.c`.

Incoming requests from the CCPU [V dispatcher f81a6e40]: even types (and 13, 15) are requests; 0 goes to
the handler at `this+0x94`, 2 to `+0x98`, 4 and 6 to `+0xa4`, 8 to `+0x9c`, 10 to `+0xa0`; 12, 13 and 15
from the CCPU are dropped.

CreateInstance, as answered now [V on a boot]:

| step | message |
|---|---|
| app -> CCPU | `{0, proxy P, req, CLSID, IID}` |
| CCPU -> app | `{4, P, req', 1, IID, obj}`: BServer (f81a514e) creates a proxy for `obj` through slot 0x20; word 1 must be P (the new proxy looks it up via its owner, f81a74a4; another value: leave -4, answer status 12) |
| app -> CCPU | `{5, 0, req', local proxy}` |
| CCPU -> app | `{1, 0, req, local proxy}`: f81a5644 finds the proxy, the client gets its interface |

Calls: `{8, P, req, obj, n, w0, ...}` with n = the byte count after it, w0 = 3, 4, 5 or 7 [I: the method
number], then arguments; reply `{9, status, req, w3}` (f81a61c2 hands the whole message on: outputs would
follow). Seen on the start-up (all answered `{9, 0, req, 0}`):

| object | CLSID (module that names it) | calls |
|---|---|---|
| 4096 | {65bd320f-21d4-6a4e-bc4a-b5000d0e52ba} (startuphandler.aps) | w0 5; w0 7, 00010000 00000000 0000 |
| 4097 | {20b6a200-e258-ce4a-8542-218c995eec60} (ccpustorage.dll, ASRhandler.aps) | w0 3 |
| 4098 | {e8658abf-6c40-054e-bdeb-43b9e15a66f0} (BAPhandler.aps, semc.tsy) | w0 3 |
| 4099 | {c1e7720b-ba0b-2e41-8857-657aae9a15eb} (semc.tsy) | w0 5 |
| 4100 | {2b688f65-3221-3644-b956-f6abc47b5ee9} | w0 4 |
| 4101 | {fa5b516a-0146-6a48-9353-798b35b0f12b} (ccpustorage.dll) | w0 3 (twice) |

The app's own type-4 registrations (IIDs {a02c3e11-...}, {27717ee3-...}, {fb52f7db-...}, {30650cd7-...})
are probably its callback objects (OpaClient has `OPA::ICB*` interfaces: ICBSimManager, ICBSmsManager, ...)
[I]; the CCPU never calls them yet.

### Call outputs [V, 2026-11-13]

APS's IPC to BServer for a call is `{method id, input length, output length}` + inputs (e.g. `{0x1F7, 4, 12}`,
input `03000000`: method 3 on object 4101, 12 output bytes expected); the type-8 packet carries only the
inputs. Word 3 of the type-9 reply is the output length and must match what the caller expects:
answering `{9, 0, req, 12}` + 12 zero bytes to every call made calls that expect no output fail
0x8000FFFA (-6) at once. With `{9, 0, req, 0}` (current) the 12-byte call completes without data, APS's
next call (`{0x1F9, 8, 0}`, input `13 00 14 00 6e 00 69 00`) is refused inside BServer with -6, APS loads
LCDCTRL, calls its control 13 and then waits. So the CCPU must know each method's output size: the
interface definitions (below) are the next step.

## Next

Name the interfaces (OpaClient's RTTI: `N3OPA12ICallManagerE` ...; IIDs are compared in each proxy's
QueryInterface, e.g. {ac0b8359-...} at f80fabc0 inside OpaClient export 97, which startuphandler.aps calls)
and the methods, then give the calls their outputs and send the callbacks APS waits for. Start-up still stops
at 2.7 s with a black screen.
