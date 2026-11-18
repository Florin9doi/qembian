# P1i start-up: what APS waits for after the OPA calls

Sony Ericsson P1i R6E30 (`se-p1i`), static reading of the core XIP image (startuphandler.aps 0xf81d2a50,
ccpustorage.dll 0xf8154608, OpaClient.dll 0xf80b5e60). Follows `p1i-opa-methods.md` and
`docs/devices/se-p1i/ccpu-link.md`. [V] = read in the disassembly, [I] = inferred. The research budget ran
out before the last link (the parameter-store chain), so its end is marked [I].

## 1. Start-up handler state machine (CStartupHandler, startuphandler.aps)

Objects [V]: `CSystemControlWrapper` (created f81d2b58, owns an ISystemControl proxy at +0x18 and an
`OPAIF::CICBSystemControl` callback stub at +0x1c, from OpaClient #96 / #123), `CStarter` (f81d3e72, an
active object), the callback observer from f81d449a. If `this+0x12c == 0`, a `CDummySystemControlWrapper`
replaces the real one (no OPA).

The ISystemControl proxy's interface vtable (f810da50) maps the slots to methods [V]: slot 10 -> m3,
11 -> m4, 12 -> m5, 13 -> m6, 14/15 -> m7 (two overloads), 16 -> m8.

State `this+0x14` [V]:

| event | code | effect |
|---|---|---|
| CreateInstance of ISystemControl done (the request observer, f81d3af8: completion of "method 4" on {991baf67-...} -> observer slot 0) | f81d3228 (value 0) | state 0 -> 1, calls ISystemControl **m5** (wrapper f81d3ac6) |
| m5 reply (f81d3af8: method 5 + ISystemControl IID -> observer slot 1 (value)) | f81d3400 | `reason = value` (negative -> 6), state 1 -> 2; f81d33cc: handles the reason (f81d32d8: reads the start-up reason P&S key, reason 2/3/5 = other modes), registers the callback object with ISystemControl **m7** (`07000000 00010000 00000000 0000`: word 1 = 0x100 = the callback object's key, [I] the key) |
| `MStarterObserver` slot 0 (CStartupHandler+0xc, f81d3424) | f81d3424 | if state 2: **f81d3248** = start-up continues (RProperty::Set, reason 2 -> f81d47fe, IMEI/COPS handler); state = 3 |

So the boot already got through m5 and m7 (both on the log). **The start-up handler then waits for
CStarter, not for a CCPU callback** [V: the only call of f81d3248 from state 2 is the MStarterObserver
slot]. ICBSystemControl callbacks go to a separate observer and are not on this path [V for the
dispatch, I that none is needed].

m5 output word 1 = the start-up reason. 0 is accepted (normal boot) [V: only < 0 is replaced, by 6]; the
values 2, 3 and 5 pick special modes [V that they branch, I which: 2 is probably charger/alarm].
Keep 0.

## 2. CStarter: waits on a CCPU parameter read (ccpustorage)

CStarter::RunL (f81d407c, state at +0x1c, switch) [V]: starts processes and threads, loads `LCDCTRL`
(f81d3f78), sends it control 13 with two bytes of the buffer at +0xd0 (f81d3fc0), then calls f81d3ecc
with the next state 5: if the starter has ccpustorage (`+0x24`), it creates `CCcpuParameterCtrl`
(ccpustorage #15) and issues an **asynchronous parameter read** (ccpustorage #11: name, `TDes8` +0xd0,
`TRequestStatus`) and waits. This is exactly where the boot stops (LCDCTRL control 13, then nothing).

ccpustorage #11 -> f8154c54 [V]: stores the request; it is sent (f8154b22) only when
`CCcpuParameterControl+0x30` (ready) is set; when `+0x34` (failed) is set it completes -2 (KErrNotFound),
otherwise it **stays queued** until ready. Readiness comes from the control's own initialisation
(RunL f8154fcc, a switch) [V for the steps seen]:

1. CreateInstance IParameterStorageManager (object 4101, done);
2. IParameterStorageManager **m3** (vtable slot 11, f8154ff2) with the request observer; nonzero return
   -> failed (+0x34);
3. after m3 completes: CreateInstance CLSID {56462d90-8ec4-e440-a968-650fcb512c42}, IID
   **IRegistryManager** {a323d9dd-...} (f8155006, slot 10 of the object at +0x1c) = object **4102** on
   the log;
4. a later state stores ready = 1 (f815502e) and issues queued reads (f8154b22) [V], reads go through
   IRegistryManager / IParameterStorage {536081a8-...} [I].

Nothing is called on 4102, so the control never reaches ready (or reaches it with nothing queued) [I].
Two candidates, cheapest first:

- **a.** the CreateInstance of 4102 is answered on the wire but its completion does not reach the
  control's TRequestCallback (check on a boot: does APS's CreateInstance request for 4102 complete, and
  with which value; `sydb strace` on BServer function 3) [I];
- **b.** step 3 or 4 also waits for the m3 result delivered to a callback (m3's output word 1 goes out
  through f80b6e20 / op 0x1F9 as `{callback key, value}`: if the control's next state needs a nonzero
  handle there, our zero stalls it) [I].

## 3. CCPU -> app callback calls (type 8 to the app)

ICBSystemControl stub, `OPAIF::CICBSystemControl` (vtable f810e6cc), incoming dispatch f80c91ba [V]:
reads the input stream (+0x30): word 0 = method, must be **3** (the only method; anything else is
ignored); f80c9188 reads **word A, then word B** and calls the app's `MICBSystemControl` slot 3 with
`(A, B)`; then f80b69ba(this, 12) (reply/cleanup with 12 [I]). In startuphandler that is f81d3b8c ->
callback observer (`wrapper+0x14`) slot 0 `(A, B)` [V]. The other ICB* stubs follow the same pattern
(header = method from 3, then fixed words) [I].

Packet the CCPU would send, mirroring the app's own calls (`{8, P, req, obj, n, w0, args}`) [I for the
words 1 and 3]:

```
08000000  <P: the app's proxy (word 1 of the app's type-4 registration)>  <req'>
<obj: the callback key, 0x100 for ICBSystemControl (word 1 of the m7 input)>
<n = 12>  03000000  <A>  <B>
```

and the app answers `{9, status, req', outlen}` + outputs (status 0, 12 bytes per f80b69ba [I]).
Not needed for the current stop (section 1), so not proposed yet; A/B meanings are unknown.

## 4. Proposal (offline phone, smallest set)

1. Keep the current answers: ISystemControl m5 -> `{9,0,req,8}` + `{0, 0}` (status 0, reason 0 =
   normal), m7 -> 12 zero bytes. [V that start-up passes these.]
2. On the next boot, trace APS around object 4102 (BServer IPC: is the CreateInstance request completed
   and does APS then call anything): if the request is never completed -> fix the CreateInstance path for
   a second CreateInstance from the same session (candidate a).
3. If it completes and nothing follows, give IParameterStorageManager m3 a nonzero word 1 (e.g. 1, a
   "handle"/session id) and retry (candidate b).
4. Once the control is ready, CStarter's read goes out as a call on 4102 (IRegistryManager, methods in
   `p1i-opa-methods.inc`) with the parameter name (UTF-16 path like `/PLATFORM/...` [I]); answer it with
   status 0 and a 2-byte value (CStarter feeds bytes 0 and 1 to LCDCTRL control 13 on the next round and
   then continues) or with an error status: f81d3ecc's fallback state (12) runs when ccpustorage is absent
   [V]; whether a failed read reaches it too is [I].
5. Fallback if the chain is long: answer IParameterStorageManager m3 with a negative status (e.g.
   0xFFFFFFFB) so the control sets failed (+0x34) and every read completes -2 at once [V for the -2
   path in f8154c54; I that CStarter carries on after -2].
