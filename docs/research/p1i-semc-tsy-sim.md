# P1i semc.tsy: `TSY_MErr SM:1 -2`

Static reading of `sys/bin/semc.tsy` (core ROM XIP, 0xf822aa70, 550860 bytes, Thumb RVCT) against the
boot log `build/sydb-p1i/serial.log` (10:15). Claims are marked [V] verified in code/log or [I] inferred.

## What the panic means: it is not the SIM

- The panic is in the TSY's start-up framework (namespace `StartUp`), not in ISimManager. [V]
  - `CStartUpStoreImpl::ReportError(sub, err)` (f823267e) stores (sub, err) once and signals event 5;
    the event handler (f822b1b2) formats `"TSY_MErr SM:" + sub` and calls `User::Panic(buf, err)`.
  - "SM" = sub-module. `SM:1 -2` = sub-module 1 reported -2 (KErrGeneral).
- Sub-module 1 is `CNwkSubModule` (the network part of the TSY): its error path f822cf42 calls
  `store->ReportError(1, err)`. [V] (CallControl's `CCcPhoneLineOpaResource` also reports with id 1
  (f825ceaa), but only on its own event 1; the log points to the network path below.) [I]
- CNwkSubModule runs a chain of init steps through `MNwkInitialization::Done(err)` (f822d720,
  state in `this+0x9c`): **any error in step 1 or 2 is replaced by -2** and reported (`SM:1 -2`);
  errors in steps 3/4 are ignored. [V]
  - step 1: `CNwkNetwork` (object at `CNwkSubModule+0x80`) start-up, `StartL` = f823d1f2. [V]
  - step 2: the object at `+0x78` (f823c148). Steps 4, 3 follow (f823e642, f823e078); step 5 = done. [V]
  - if `CNwkNetwork::StartL` leaves, the same `Done(-2)` happens directly (f822d04c). [V]

## CNwkNetwork start-up (step 1), state in `CNwkNetwork+0x20`

All OPA completions arrive at the IRequestObserver slot (f823d7ac) as
`(kind, iid*, ?, status)`; a **negative status at any point returns that status and ends in
`Done(-2)`** (f823d63c / f823d52a). A handler result of -17 means "this state finished". [V]

| state | issued by TSY | completions it accepts (iid, kind) | to finish the state |
|---|---|---|---|
| 2 | CreateInstance CLSID {2b688f65-3221-3644-b956-f6abc47b5ee9}, IID ISystemControlManager {be50d762-...} [V; log 4.857] | (ISystemControlManager, 4): calls the manager again, output interface stored at `+0x84` [V]; log: **ISystemControlManager m4, no args** [V log 4.868] | (IRadioControl {0325662a-d924-3b46-9dcc-c826f31d66c8}, **5**) with status >= 0 [V] (status 0 -> radio flags 1/7, status != 0 -> 3/2 [V]; both finish) |
| | | (IShell {991baf67-...}, 4): another call [V]; (IRadioControl, 6): another call `vt+0x34` [V] | |
| 3 | CreateInstance IID INetworkManager {18b31fdc-...} (f823d5e2) [V] | (INetworkManager, 4), (INetworkManager, 0xd), (IShell, 4): further calls [V] | (INetwork {3e0193be-...}, kind 0) [V] |
| 4 | — | | `Done(0)` -> CNwkSubModule goes to step 2 [V] |

Iids not in this list are ignored (handler returns 0: no error, no progress -> a stall, not a panic). [V]

## Why it panics today

Log (10:15): `call object 4169 ? args 4: 04000000 -> 0 (output size unknown)`, reply `02 00 09 ...` with
**no output bytes**, then the panic 1 ms later. The ISystemControlManager m4 reply carried no
`{status, interface}` output, the client reported a negative status for the request, and state 2
turned it into `Done(-2)`. [I: the mapping "no output -> negative status" is from the timing and the
handler, not traced in OpaClient]

Note: the current `syborg_ccpu_outif.inc` answers ISystemControlManager m3/m4/m5 with an
**ISystemControl** {ac0b8359-...} object ("[I name]"). The TSY state machine never looks at an
ISystemControl iid, so that answer gives no panic but a **stall in state 2** (MNwkInitialization never
completes). [I]

## What the CCPU must answer (offline phone, cheapest path)

1. ISystemControlManager m4 (12 output bytes): `{status 0, IRadioControl object, 0}` i.e. bind a new
   object whose IID is **IRadioControl {0325662a-d924-3b46-9dcc-c826f31d66c8}** (output word 1),
   not ISystemControl. [I: the only interface state 2 acts on after the manager is IRadioControl]
   (Keep m3/m5 as they are until a caller shows up.)
2. The IRadioControl completion with kind 5 and status 0 must reach the TSY. If kind 5 is "method
   completed" [I], it comes from the TSY's own IRadioControl call (IRadioControl m3..m5 4/8, m9 4/12):
   answer every IRadioControl method `status 0` with its declared output size (table already has them).
   If the TSY instead waits for it unasked (radio state notification), the CCPU needs a
   CCPU->app callback (ICBRadioControl / MICBRadioControl, CNwkNetwork implements it at `+0x8`) — not
   implemented yet. [I]
3. State 3 then needs: CreateInstance INetworkManager answered (works today), INetworkManager calls
   answered with outputs (m4 20 bytes etc., in the table), and an INetwork {3e0193be} object
   (INetworkManager returns it [I]) — add `INetworkManager m? -> INetwork` to outif when the log shows
   which method.
4. No SIM state is needed for this panic: ISimManager m5 (12 zero bytes) is not on this path. [V: the
   reporting chain is CNwkSubModule, not a SIM class]

Status words: always 0; any negative status in states 2/3 reproduces `SM:1 -2`. [V]

## Addresses (semc.tsy in core ROM, link = load)

- panic formatter f822b1b2 (string "TSY_MErr SM:" at f822b214), store ReportError f823267e
- CNwkSubModule: error report f822cf42, MNwkInitialization::Done f822d720, step-1 start f822d04c,
  MNwkSim callback f822d79e (also reports `(1, err)` on a non-zero result)
- CNwkNetwork: vtable f829e438, StartL f823d1f2, observer f823d7ac, state 2 f823d630,
  state 3 f823d51e, state-2->3 f823d6f8, next CreateInstance f823d5e2; IID literals at f828ad70 - 0xcc..0x4c
- tooling: raw ELF from the ROM bytes: `llvm-objcopy -I binary -O elf32-littlearm` +
  `llvm-objdump --triple=thumbv5te` (`tools/xip2elf.py` failed on this file: export dir offset).

## Follow-up (2026-11-17, log 10:33): the panic is a CCPU object-table overflow, not IRadioControl

### Root cause [V]

- `syborg_ccpu.h`: `CCPU_OBJECTS 64` (ids 0x1000..0x103f). Every object from **4160** on is created and
  registered with the app, but its IID is not stored, so `bs_call` logs `call object 4170 ? ...
  (output size unknown)` and answers `{9, 0, req, 0}` **without outputs** (8 such calls on the log,
  the first at 4.906 on object 4160).
- The TSY's own chain (BridgeServer client `c0cc706d`): CreateInstance ISystemControlManager -> object
  **4170** (4.923), ISCM m4 `04000000` (4.948) -> answered with 0 output bytes -> the proxy's reply
  read fails -> (ISCM, kind 4) with a negative status -> `Done(-2)` -> `TSY_MErr SM:1 -2` (4.954).
- The IRadioControl calls at 1.06-1.09 (object 4103: m5, then m6 `06000000 04010000 00000000 0000`) come
  from **another client** (`c814706d`, start-up side; same pattern as startuphandler's ISystemControl
  m5 + m7 key 0x100 registration) [I who]. They are answered fine and are not the TSY's.
- **Fix:** raise `CCPU_OBJECTS` (e.g. 1024; 32 bytes per object) and treat an untracked object as an
  error in the log. Regression test: a CreateInstance as the 65th+ object must still get its outputs
  (e.g. `ccpu_object_65_gets_outputs` in `tests/caif_peer_test.c`).

### How completions reach CNwkNetwork [V]

- OpaClient proxies keep a primary and an interface vtable (subobject at +0x80, thunks `subs r0,#0x80`).
  On the interface vtable **slot +0x20+4*m = method m** (IRadioControl f810d35c: +0x34 -> m5 thunk
  f80e6fcd, +0x38 -> m6 f80e708d; INetworkManager f810e0e0: +0x30 -> m4 f80de016, +0x54 -> m13
  f80de66a; ISCM +0x30 -> m4, as on the log). +0x20 is the generic QueryInterface (f80dda32 ->
  f80b6d72, sent as a BServer type-2 QueryInterface).
- The proxy sends the call with `f80b68d8(this, method, observer)`; the reply handler (IRadioControl:
  f80e730c) switches on the pending method (`this+0x28`): cases 3/4/5/7/8 read one word, 6 and 9 read a
  status word and, if >= 0, a second word into the caller's out pointer, case 0 = QueryInterface.
  Then `observer->slot0(obs, kind = method number, iid = proxy+0x34, iface = proxy+0x80, status)`,
  status = `f80b6d0e(this, transport status, word)`: the read word when the transport status is >= 0.
- So **"kind" = the method number of the completed call** (0 = QueryInterface, 4 for CreateInstance
  reported under IShell {991baf67}). No ICB callback is needed on this path. [V structure; I that
  CreateInstance completes as IShell method 4]
- The out arguments are proxies the TSY created beforehand (`[+0x68]` ISCM, `[+0x6c]` IRadioControl,
  `[+0x58]` INetworkManager, `[+0x64]` INetwork, `[+0x74]` INetworkNitz); a call binds them. [V by use]

### The TSY's sequence once its objects are tracked (offline phone, all zeros work)

State 2 (f823d630):

| completion | TSY then sends | CCPU answer |
|---|---|---|
| (IShell, 4) CreateInstance ISCM | ISCM m4 `04000000` | 12 bytes `{0, <IRadioControl object>, 0}` + type-4 registration of IRadioControl {0325662a} (as for object 4103 today) |
| (ISCM, 4) status >= 0 | IRadioControl **m6** `06000000 <key = [+0x70]> 00000000 0000` (14 bytes; key: its ICBRadioControl callback, like 0x104 for the other client) [V shape, I key] | 12 bytes `00000000 00000000 00000000` (word 0 = status >= 0; word 1 -> TSY `+0x84`) |
| (IRadioControl, 6) status >= 0 | sets `+0x80 = 1`, IRadioControl **m5** `05000000` | 8 bytes `00000000 00000000` |
| (IRadioControl, 5) status | 0 -> radio flags (1, 7); > 0 -> (3, 2); < 0 -> panic. Either non-negative value finishes state 2 (-17) | 0 is right (radio-state meaning [I]) |

State 3 (f823d51e; CreateInstance CLSID {10195817-cece-9747-a295-3fa6ef2bae13}, IID INetworkManager):

| completion | TSY then sends | CCPU answer |
|---|---|---|
| (IShell, 4) | INetworkManager **m13** `0d000000 <key = [+0x5c]> 00000000 0000` (14 bytes, likely the ICBNetworkManager {69075e3d} key) [V method, I key] | 12 zero bytes |
| (INetworkManager, 13) | sets `+0x78 = 1`, `+0x98 = +0x9c = 6`; INetworkManager **m4** `04000000` (outs: INetwork proxy, `+0x98`, `+0x9c`) | 20 bytes `{0, <INetwork {3e0193be} object>, 0, 0, 0}` + type-4 registration of INetwork (outif already returns INetwork for m4 [V log 4.858]); words 2/3 -> `+0x98/+0x9c` [I order]; (4, 4) there is rewritten to (6, 6), anything else is kept |
| (INetworkManager, 4) | QueryInterface on the INetwork object for **INetworkNitz {e68fee38-5492-c146-9a8c-c6a248d161a1}** (BServer type 2) | QueryInterface answered status 0 (works today) |
| (INetwork, 0) = that QueryInterface done | state 4 -> `Done(0)` -> CNwkSubModule step 2 (`+0x78`, f823c148) | — |

Any negative status in this table reproduces `SM:1 -2`. Steps 2/4/3 of CNwkSubModule are not read yet
(step 3/4 errors are ignored; step 2 errors give -2 again).
