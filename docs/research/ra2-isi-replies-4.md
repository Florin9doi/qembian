# RA-2 (Nokia 9500) ISI replies, part 4: Telephone's NULL [this+0x20]

Static reading only (objdump of eka1/ra2/core/ROM.bin), no runs. [V] = read in the code,
[I] = inferred. Budget reached; open points at the end.

## Who creates PhoneEngineGsm [this+0x20]

- The object is CPEPhoneModel-like (PhoneEngineGsm.dll): ctor 0x50c5e684 stores vptrs 0x50c62cc0 (+0),
  0x50c62ccc (+12, interface at +4 = the message interface, slot +0x14 = HandleMessage 0x50c5ec70
  via thunk 0x50c6225c) and 0x50c62ce8 (+16, interface at +0x10 = the starter interface) [V].
- ConstructL 0x50c5e6d4: `new(32)` + PhoneEngineBase 0x50c53930 = CPEActiveStarter (CActive, priority 0,
  `iStarter = this+0x10` at +24, step counter at +28) stored at +20, then started [V].
- Starter RunL 0x50c539b4: `if (iStarter->StepL(iStep)) { iStep++; complete self }` [V]. RunError
  0x50c539a0 calls iStarter slot 0 = 0x50c5f24c, which sends message 0x2329 (9001) through
  0x50c5ec5c and clears +20 [V]. The normal end (step 13) sends the same 0x2329 [V]. So 0x2329 =
  "construction finished" [I], and **a leave in any step ends construction early with the same
  message** [V path, I meaning].
- StepL 0x50c5f26c (steps 0..13, jump table 0x50c5f288) [V]:
  | step | creates | callee |
  |---|---|---|
  | 0 | - | 0x50c5f3e0 (local init) |
  | 1 | +0x3c | 0x50c5f5cc, then its vfunc +12 (0) |
  | 2 | +0x50 | DSClient.dll+0x18bd (RDosServer side) |
  | 3 | +0x24 | Callhandling.dll+0x3df1 (NewL) |
  | 4 | +0x38 | SecurityClient.dll+0x95 |
  | 5 | +0x28 | Audiohandling.dll+0x21c1 |
  | 6 | +0x2c | Loghandling.dll+0x335 |
  | 7 | +0x30 | Contacthandling.dll+0x1bd |
  | 8 | +0x44 | PhoneEngineBase.dll+0x4b9 |
  | 9 | +0x4c | 0x50c60948 (local, arg 0) |
  | **10** | **+0x34, then +0x20** | **Networkhandling.dll+0x3071 (NewL), then 0x50c59ad8(..all members..) -> +0x20** [V] |
  | 11 | +0x48 | 0x50c60948 (arg 1) |
  | 12, 13 | - / +0x74 | |
- HandleMessage 0x50c5ec70 uses +0x20 without a NULL check (0x50c5f134 for 0x307A) [V]. With a leave
  in step 0..10, the UI gets 0x2329 and goes on sending messages to a half-built engine -> the data
  abort [I, matches the symptom]. All steps are synchronous, so +0x20 can be NULL at 70 s only when a
  step left [I].
- The step just before +0x20 is Networkhandling NewL: ConstructL 0x50c72de8 connects RTelServer,
  loads the TSY, opens the phone and RMmCustomAPI (errors there **panic** reason 12 via 0x50c74f64,
  not leave [V]), then builds its monitors (0x50c728f8, 0x50c73a3c, 0x50c73d10, 0x50c73be0, ...) [V].
  A leave from those (a network-status read completing with an error/timeout) is the best candidate
  [I, not traced]: of the unanswered requests only **0A 00 (NET_MODEM_REG_STATUS_GET)** is a network
  request, and it is sent twice (tids 05 and DE, a retry) [I].
- Cheapest confirmation in a run: `sydb catch` on Leave in Phone's thread, or a breakpoint on
  0x50c539a0 (starter RunError) and print `[starter+28]` = the failing step [I].

## NET_MODEM_REG_STATUS_GET_RESP (0A 01): the 9500 parser

- PhoneTsy hands messages to Mmgsmwcdma's router 0x50c07348 (switch on resource 1..10, table
  0x50c0737c; resource 0x0A -> 0x50c07484, switch on msg id, table 0x50c0749c) [V].
- msg **01** -> vfunc +0xf8 of vtable 0x50c0e360 = **0x50c096bc** [V]:
  1. builds a network message object (IsiMsg 0x500fa77c, vtable 0x500ffa88) over the frame [V];
  2. looks the **tid** up among pending requests (`[this+4]->vf+0x124()` -> `vf+0x28(1, tid, 0)`);
     no match = message dropped [V]. So the reply must carry the request's tid [V].
  3. status = frame[10] = **data[0]** (IsiMsg 0x500fa82c, id 0x35, msg id != 0x0E -> offset 10) [V];
  4. parse 0x50c0974c, then notify `[this+4]->vf+0x174(status)` [V]; 00 = OK [I].
- Parse 0x50c0974c [V]: sub-block **count = frame[11] = data[1]** (vtable slot 0x500fa968: 11, or 15
  for msg ids 0x24/0x29); count 0 -> nothing parsed, returns [V]. First sub-block at **frame[12] =
  data[2]** (0x500fa9a8) [V]. Sub-blocks are `{id, len (incl. header), ...}`, found by id with
  0x500f5ae4 (returns 0 when found, -1 not found) [V].
  - sub-block **0x09** (NET_GSM_REG_INFO in oFono naming [I]) found -> reads fields 0x47, 0x3e, 0x11,
    0x12, 0x10, 0x0e, 0x06, 0x0f, 0x48 and MCC/MNC digits (each digit must be <= 9) [V ids; field
    offsets I: the accessor 0x500fa9e8 maps ids to offsets, default 2, 0x11 -> 5 for some blocks];
  - else loops other sub-blocks: id 0x0A (cmp at 0x50c0991a) with fields 0x20, 0x05 (offset 3), 0x43 [V].
- The N-Gage line `0A 00 01 00 01 00 04 04 00` gives a sub-block id 00, which this parser does not
  look for; harmless (falls to the loop, no 0x0A) [I].

Proposed lines (pick the first; the second only if the first leaves registration "unknown" and that
matters):

```
reply 0A 00 01 00 00                              # NET_MODEM_REG_STATUS_GET_RESP: handler 0x50c096bc (Mmgsmwcdma) needs the request's tid [V]; data[0] status 00 OK [V read, I value]; data[1] sub-block count 0 -> parse 0x50c0974c skips [V]
# reply 0A 00 01 00 01 09 10 00 00 00 00 00 00 00 00 00 00 00 00 00 00   # alternative: one GSM reg info sub-block {09, len 16}, all zero (MCC/MNC digits 0 pass the <= 9 check) [I offsets]
```

## The other unanswered requests (not traced, budget)

No 9500 parser read. The frame offsets are the same absolute bytes as on the N-Gage ([11] = data[1]),
so the N-Gage/6600 lines are usable hints only [I]:
- `09 31 {3B 00 00}` -> N-Gage `reply 09 31 32 len=13 3B 01`; `09 3D {FA 00}` -> `reply 09 3D 3E len=42
  FA 01 31 31 32 ...`; `09 23 {25}` -> `reply 09 23 24 @25 25 01 00` (already a 6600 MMCUSTOMTSY line).
- `09 09 {01 00}`, `09 1D {2D 00}`, `02 16`, `31 19` (N-Gage: `reply 31 19 1A len=12`), `31 30`,
  `DB 01`, `1B 00 {41}`, `5A 00`: not checked. None of them is in the step chain shown above [I];
  leave silent until a run shows which step leaves.
