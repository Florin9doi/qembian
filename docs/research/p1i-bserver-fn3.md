# P1i BServer IPC function 3 / op 0x1F9: why it completes -6 and what the CCPU must send

Sony Ericsson P1i R6E30 (`se-p1i`), static reading of `BServer.dll` (core XIP 0xf81a2d88) and
`OpaClient.dll` (0xf80b5ed8). Follows `p1i-opa-methods.md`, `p1i-startup-callbacks.md` and
`docs/devices/se-p1i/ccpu-link.md`. [V] = read in the disassembly, [I] = inferred.

## Summary

Op 0x1F9 is not a callback message. It is **"bind subsession w0 to remote object w1"**: the client
(OpaClient) uses it when a call's reply returns an interface pointer. Before the call, the proxy queues
an object of its own (a fresh proxy subsession for the returned interface); after the reply it reads
output word 0 (status) and word 1 (K) and sends `{0x1F9, 8, 0}` + `{handle of that subsession, K}`.
BServer refuses it with -6 when **K is not a key of its remote-proxy table**, i.e. not a value BServer itself
returned in a type-5 answer to a CCPU-initiated type-4 registration. Our CCPU answers those methods with
K = 0 (or 1 in the `ccpu_opa_values` experiment), so the bind fails, the proxy stays unbound, and
ccpustorage's control never gets its IParameterStorage object.

**Fix**: answer an out-interface method the way CreateInstance is answered: create a CCPU object, register
it with BServer by our own type 4, wait for BServer's type 5 (local id L), then send the type-9 reply
with output `{0, L, 0}`.

## 1. BServer: function 3 -> op dispatch [V]

- Session ServiceL f81a36ee: `-29` if the bridge failed (`+0x13c` / `+0x124`), then a switch on
  `RMessage2::Function()`. Functions 3, 4 and the default go to f81a37e0: `Int3()` (subsession handle)
  must be nonzero and found in the session's `CObjectIx` (f81a31d8 -> `CObjectIx::At`), else **-6**;
  found -> subsession handler f81a45da.
- Subsession f81a45da: -29 if the bridge failed; function 3 reads arg 0 (`{op, in len, out len}`) into
  `+0xe0`, then `op - 0x1F4` through a switch8 (f81a466e):

  | op | handler | note |
  |---|---|---|
  | 0x1F4 | f81a43ee | |
  | 0x1F5 | f81a4308 | |
  | 0x1F6 | f81a42e4 | |
  | 0x1F7 | f81a40c4 | the normal call (type 8 to the CCPU) |
  | 0x1F8 | f81a3e9e | |
  | **0x1F9** | **f81a4212** | bind (this document) |
  | 0x1FA | f81a3d4e | |
  | other | f81a46c2 | **-6** |

  Function 4 -> f81a454a, 5 -> f81a45b4, other functions -> **-6**.

## 2. Op 0x1F9 handler f81a4212 [V]

```
f81a3e2a      read the input (arg 1) into the subsession's HBufC8 (+0xac); a leave -> that error
input         Left(8) -> { w0, w1 }
w0 == 0                                              -> -6
CObjectIx::At(session, w0) == NULL (f81a31d8)        -> -6   (w0 = a subsession handle of this session)
manager(+0xa4)->slot 9 (lookup, f81a6ae6) of w1 == 0 -> -6
f81a41f6(subsession(w0), proxy):  subsession+0x94 = proxy; subsession+0xa8 = 0;
                                  proxy->vtbl[3](proxy, subsession+0x1c, subsession+0xa0)   (attach)
complete 0 (f81a3bb2, completion through the OPA encoding f81a4b0c: -6 -> 0x8000FFFA)
```

The manager lookup (vtable at f81ad204, slot 9 = f81a6ae7; thunk vtable f81ad274 slot 9 = f81a6b1b):
`RArray` at manager+0x10 of 28-byte entries `{key, proxy, CCPU object, IID[16]}`, found by **entry word 0
== w1**, returns `proxy + 0x1c`. Entries are added only by slot 8 = f81a6a3e, which is called from the
CCPU type-4 handler f81a514e (`{4, P, req', 1, IID, obj}`): it creates the proxy, stores
`{proxy, proxy, obj, IID}` and answers `{5, status, req', proxy->vtbl[1]()}` [V]. That type-5 value is the
key: our CreateInstance answer `{1, 0, req, L}` with L from that type 5 is found by the same lookup
(f81a5644) and works on the boot [V]. So **w1 must be an L from a type-5 answer, never the CCPU object id**.

The same bind runs inside BServer after a CreateInstance completes (f81a3a16: lookup of the type-1 value,
`-6` on a miss, then f81a41f6) [V]: 0x1F9 is the explicit form for interface pointers in call outputs.

## 3. OpaClient: who sends 0x1F9 [V]

f80b6e20(proxy, a, b) sends the 8-byte `{a, b}` and waits. Callers are the proxies' reply handlers
(vtable slot 4). Pattern, e.g. IParameterStorageManager f80e4032 (method 3):

```
status = read word 0;  if status < 0 -> error path
obj    = pending[0] (RArray +0x44: the object the caller passed to m3, appended before the call, f80e3fbc)
K      = read word 1
f80b6e20(this, obj->vtbl[1]() /* its subsession handle */, K)      -> BServer op 0x1F9
obj->vtbl[7](K)                                                    (stores the remote id)
```

The m3 input is the header only (`03000000`); the object goes nowhere on the wire: the CCPU creates the
target, the client binds its own proxy to it. The earlier note "{callback key, value}" was wrong: w0 is a
subsession handle (`13 00 14 00` = 0x00140013, a CObjectIx handle, index 0x13, instance 0x14 [I]), w1 is K.

Reply handlers with this pattern (f80b6e20 callers, class from RTTI) [V], those on the current start-up
in bold: **IAudioDeviceManager m3**, **IBatteryDeviceManager m3**, **IParameterStorageManager m3**,
**ISystemControlManager m3, m4, m5**, ISimManager (two methods), AccessoryControl(Manager), AudioDevice,
CallManager, CellBroadcastManager, ClockManager, CopsManager, FileSystemManager, IndicationDeviceManager,
InternetAccountManager, MobileTerminalManager, NetworkManager, PhonebookManager, PlayerManager,
SmsManager, SystemConnectorManager, UserInputDeviceManager, VoiceCall, ... (about 80 sites; 25 more in
generated helper code without a vtable owner). ISystemControl's handler (f80fac86) has none: m5/m7 need no
bind [V].

## 4. CCPU-side change (packet bytes)

For a call `{8, P, req, obj, n, w0=m, ...}` on an (interface, method) in the out-interface table:

1. Allocate a CCPU object O (`next_obj++`), record its IID (the returned interface, table below).
2. Send our request 4 (36 bytes, as for CreateInstance), word 1 = **P from the call** [I: same field as in
   CreateInstance; f81a514e passes it to the proxy creation]:
   ```
   04000000 <P> <req'> 01000000 <IID of the returned interface, 16 bytes wire order> <O>
   ```
3. BServer answers `05000000 <status> <req'> <L>`. Status 0 -> reply to the app's call:
   ```
   09000000 00000000 <req> <outlen=12>   00000000 <L> 00000000
   ```
   (outputs: word 0 status 0, word 1 = L, word 2 reserved). Status != 0 -> reply `{9, 0, req, 12}` +
   `{0x80004005 (E_FAIL-like, negative), 0, 0}` so the client takes its error path [I value].
4. Later calls on that object arrive as type 8 with word 3 = O [I, as for CreateInstance objects]; give them
   outputs from `ccpu_opa_methods` with O's IID.

Out-interface IIDs for the start-up (wire bytes) [I: by name; the class of the queued object was not
traced]:

| call | returns | IID wire bytes |
|---|---|---|
| IParameterStorageManager m3 | IParameterStorage {536081a8-124b-eb46-87ed-72c8a2f8930b} | a8 81 60 53 4b 12 46 eb 87 ed 72 c8 a2 f8 93 0b |
| IAudioDeviceManager m3 | IAudioDevice {e6df237e-7c7b-1046-bc46-9c3f1aaa5c3d} | 7e 23 df e6 7b 7c 46 10 bc 46 9c 3f 1a aa 5c 3d |
| IBatteryDeviceManager m3 | IBatteryDevice {03beabf7-bf5f-2b4a-bfa3-3dcdd00b9d42} | f7 ab be 03 5f bf 4a 2b bf a3 3d cd d0 0b 9d 42 |
| ISystemControlManager m3/m4/m5 | unknown [I] | use the manager's own IID until known |

BServer does not check the IID on the bind path [V for f81a4212; I for the proxy's attach slot 3], so a
wrong IID should only matter for later QueryInterface calls.

Proposed code (syborg_ccpu.c), reusing the CreateInstance slot:

```c
/* (interface, method) whose output word 1 is an interface pointer (OpaClient reply handler -> f80b6e20) */
static const struct { uint8_t iid[16]; uint16_t method; uint8_t out_iid[16]; } ccpu_opa_outif[] = {
    /* IParameterStorageManager m3 -> IParameterStorage */
    {{0x45,0xd4,0xd3,0x68,0xd6,0xa0,0x46,0xb0,0x88,0xeb,0xa7,0x95,0xab,0xdc,0xfe,0x5c}, 3,
     {0xa8,0x81,0x60,0x53,0x4b,0x12,0x46,0xeb,0x87,0xed,0x72,0xc8,0xa2,0xf8,0x93,0x0b}},
    /* IAudioDeviceManager m3 -> IAudioDevice */
    {{0xb5,0x09,0xb5,0xf7,0xe2,0xf9,0x44,0xa5,0x97,0x3b,0x2e,0xdb,0x78,0xf5,0xbb,0xb3}, 3,
     {0x7e,0x23,0xdf,0xe6,0x7b,0x7c,0x46,0x10,0xbc,0x46,0x9c,0x3f,0x1a,0xaa,0x5c,0x3d}},
    /* IBatteryDeviceManager m3 -> IBatteryDevice */
    {{0xce,0xbc,0x6d,0x33,0xa7,0x23,0x48,0xd7,0x8a,0x40,0xb6,0x20,0x6f,0xb4,0x7f,0x8e}, 3,
     {0xf7,0xab,0xbe,0x03,0x5f,0xbf,0x4a,0x2b,0xbf,0xa3,0x3d,0xcd,0xd0,0x0b,0x9d,0x42}},
    /* ISystemControlManager m3, m4, m5: returned interface unknown, the manager's IID for now */
};

/* SybCcpuCreate gets: uint32_t kind (0 CreateInstance, 1 call), outlen. In bs_call, before the normal
 * answer: if (iid, method) is in ccpu_opa_outif, take a create slot (kind 1, app_req = req, outlen),
 * object = next_obj++ with out_iid, and send { 4, rd32(m + 4) (P), our_req, 1, out_iid, object }.
 * In bs_registered, kind 1: bs_send_out(.., s->app_req, s->outlen, ..) with output word 0 = status ? a
 * negative error : 0, word 1 = status ? 0 : local. */
```

Tests (tests/caif_peer_test.c): a call on an IParameterStorageManager object, method 3, must produce our
type 4 with the call's P and the IParameterStorage IID, and no type 9 until the type 5; after
`{5, 0, req', L}` the type 9 carries `{0, L, 0}`; a type 5 with nonzero status gives a negative word 0.

## 5. Expected effect on the start-up [I]

The bind succeeds (0x1F9 completes 0), ccpustorage's control continues (IRegistryManager CreateInstance
already happens) and should reach "ready"; then CStarter's queued parameter read goes out as a call on
the IParameterStorage / IRegistryManager object (p1i-startup-callbacks.md section 4, step 4). Check on a
boot: `sydb strace BServer` shows function 3 completing 0 after IParameterStorageManager m3, and the CCPU
log shows calls on the new object.

## 6. Full table (generated)

`src/qemu-syborg/hw/arm/syborg_ccpu_outif.inc`: 85 (interface, method) pairs, from every proxy's reply
handler (vtable slot 4): method dispatch decoded (switch8 on `this+0x28` or a cmp chain), each method's
code walked to the f80b6e20 calls, with the output reads before them [V for the method -> bind mapping;
generator `outif.py`/`reply2.py`/`gen_outif.py` in the research job's `tmp/opa3/`]. Returned interface:
[V] only for IParameterStorageManager m3, [I name] from the interface names (IXManager -> IX), else the
proxy's own IID [I].

Output layouts that are not `{status, K}` [V]:

- IVoiceCall m10, ISpeechRecognitionSession m6: `{status, K1, K2}`: two queued objects, bound in order
  (words 1 and 2).
- ISmsFinder m3..m7: `{status, u8 present, K}`: present 0 = the queued object is released, no bind;
  K at byte offset 5 (the stream reads are unaligned [I]). IInterfaceList m11 reads the same
  `4, 1, 4` sequence [I same meaning].
- ISimManager m10: `{status, K, ...}` but binds a sub-object (its vtable +0x20 handle) [V].

Methods that queue an object but never bind (ISystemControl m7, ICopsManager m5, IToneManager m3,
IVibratorDevice m3, ...: callback registrations) need nothing. 25 more f80b6e20 calls sit in the ICB*
callback stubs (CCPU -> app calls whose inputs carry interfaces, e.g. f80c2cf8 reads `{4, 4, 1}`): they
matter only once the CCPU makes such callbacks.
