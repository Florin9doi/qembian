# P1i: minimal SIM behind the fake CCPU (semc.tsy SIM detection)

Static research (R6E30 core image, base 0xf8000000; semc.tsy XIP at 0xf822aa70; OpaClient ISimManager proxy).
Marks: [V] read in the code, [I] inferred. Goal: semc.tsy reports an inserted, ready SIM (no network, flight
mode) so ETel `NotifySecurityEvent` stops completing with `ENoICCFound`.

## 1. OpaClient ISimManager proxy {0303fc15-1275-7c47-b519-587c03c29a51}

Primary vtable 0xf810c6b4, interface vtable 0xf810c71c (subobject +0x80). As for IRadioControl, interface slot
`+0x20 + 4*m` = method m (+0x2c m3 f80f3be6, +0x30 m4 f80f3caa, +0x34 m5 f80f3d7e, +0x38 m6 f80f3e18) [V: each
sender writes its own method number]. Reply parser f80f4436, `switch8` on the pending method `this+0x28` [V]:

| m | in | out reserve | what the reply parser reads (after word 0 = status; nothing more if status < 0) |
|---|---|---|---|
| 3 | `{3, index}` | 4+4+0x18+4+4+0x46 = 114 [V] | word -> out+0; **len-prefixed 8-bit buffer** (max 20) -> out+4; word -> out+0x18; word -> out+0x1c; **len-prefixed 16-bit string** (max 0x21 chars, **length 0 = read error -2**, f80bb40a) -> out+0x20 [V] |
| 4 | `{4, ?}` | var | as m3, then word -> out+0x64, 8-bit buffer (max 0xff) -> out+0x68 [V] |
| 5 | `{5}` | 12 | **one word -> caller's out pointer**; the third word is unused [V] |
| 6, 7 | `{m, word}` / 9 bytes | 12 | one word = object handle -> **binds an interface** (f80b6e20) [V] |
| 8 | 14 bytes | 12 | one word (same case as m5) [V] |
| 9 | 8 | 8 | status only [V] |
| 10 | 8 | 16 | binds an interface (holder vtable +0x20) [V] |

Lengths in the stream are a 4-byte count followed by the bytes, no padding [V f80bb23c/f80bb40a].

## 2. semc.tsy: the SIM-application reader (observer f824644c, parent object P = `this+0x68`)

The observer slot 0 gets `(kind = completed method, iid, iface, status)` [V, as in p1i-semc-tsy-sim.md]:

| completion | handler | TSY then does |
|---|---|---|
| (IShell {991baf67}, 4) = CreateInstance CLSID {c1e7720b}, ISimManager done | f8246416 | **ISimManager m5** (out -> `P+0x170`) [V] |
| (ISimManager, 5) | f824639c | status < 0 or **word 1 == 0 -> `f8246186(P, 0)` = "no SIM"** (`P+0xdc = 2`) [V]. Else index = 0 and **m3(index)** [V] |
| (ISimManager, 3) | f82462a6 | appends the entry to an RArray (`P+0x28`); the entry's **out+0x1c (wire word 4) = application type**: type 1 remembered in `this+0x74`, type 2 in `this+0x78` [V]; next index until `index == count`. Then: a type-2 entry -> **m6(type-2 index)**, else a type-1 entry -> m6(type-1 index), else "no SIM" [V]. Meaning 1 = GSM SIM, 2 = USIM [I] |
| (ISimManager, 6) | f824624c | status >= 0: QueryInterface on the bound object (`P+0xd0`) for **ISimApplication {10bf3803}** into `P+0xe4` [V]. Status < 0 with type 2 tried and a type-1 entry present: m6(type 1) again; else "no SIM" [V] |
| (ISimApplication, 0) = QI done | f82464ba | if the IGsmSimApplication holder (`P+0xe8`) is still empty: QI for **IGsmSimApplication {cd4808e6}** (f82461da); else **`P+0xd8 = 1`, `f8246186(P, 1)` -> `P+0xdc = 1` = SIM ready** (f82461cc) [V] |

`f8246186` writes the state (1 ready / 2 none) at `P+0xdc` and runs the pending-request list (f8245e7a): when ready
every waiting TSY client gets a QueryInterface on the ISimApplication object, else they are completed with an
error [V structure, I that ENoICCFound comes from state 2]. Kind 0 is accepted only with the ISimApplication IID,
so **the object m6 returns must be an ISimApplication**: today's `syborg_ccpu_outif.inc` binds it as ISimManager
("m6 -> ISimManager (own IID) [I]"), which would stall after m6 [V check, I effect].

No ICBSimManager callback is needed on this path [V: the reader waits only on its own completions].

## 3. ICBSimManager (CCPU -> app)

Stub CICBSimManager (OpaClient vtable 0xf810d174, dispatch f80c84a4): **only method 3, one 4-byte argument**
`{03000000, a}` (n = 8), reply reserve 8, then `observer->vtbl[+0xc](observer, a)` [V]. semc.tsy's
`SecurityTSY::CSecurityOpa` (typeinfo f8298034) has `MICBSimManager` as a base [V]; a candidate handler
(f8243e0a, vtable slot +0xc of a CSecurityOpa sub-vtable) treats **a == 0** as "OK" (marks a pending request
state 2 and completes it with 0) and maps any other a to an error [V code, I that this is the MICBSimManager slot].
So if a registered ICBSimManager ever needs an event, send `a = 0` [I]; it is not needed for SIM-ready.

## 4. Table entries for syborg_ccpu.c

`ccpu_opa_values` (word 0 = status; values are per (interface, method), not per input index; one application):

```c
    /* ISimManager m5: word 1 = number of SIM applications; 0 made semc.tsy report "no SIM" (ENoICCFound,
       SecurityApp's "Insert SIM card") [V f824639c] */
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 5, 1, 1, "SIM applications"},
    /* ISimManager m3(index): {status, id, len8=0, w, type, len16=1, "S"}; type 1 = GSM SIM [I meaning, V offset];
       a zero-length label is a stream error in OpaClient (f80bb40a) [V] */
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 3, 4, 1, "SIM app type"},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 3, 5, 1, "SIM app label length"},
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 3, 6, 0x53, "SIM app label 'S'"},
```

Wire output of m3 with these (26 used bytes of 114): `00000000 00000000 00000000 00000000 01000000 01000000 5300 ...` [V layout].
Type 2 (USIM) is what the TSY tries first; use `value 2` if type 1 leads somewhere worse [I].

`ccpu_opa_outif_fix` (replaces the generated guess):

```c
    /* ISimManager m6(index) -> ISimApplication {10bf3803-2f67-a648-8bbe-57c4de35bbd6}: semc.tsy QIs the result for
       ISimApplication and IGsmSimApplication and accepts the QI completion only under the ISimApplication IID
       (docs/research/p1i-minimal-sim.md) [V use, I name] */
    {{0x15,0xfc,0x03,0x03,0x75,0x12,0x47,0x7c,0xb5,0x19,0x58,0x7c,0x03,0xc2,0x9a,0x51}, 6,
     {0x03,0x38,0xbf,0x10,0x67,0x2f,0x48,0xa6,0x8b,0xbe,0x57,0xc4,0xde,0x35,0xbb,0xd6}},
```

QueryInterface answers (BServer type 2) must register a new object with the asked IID (ISimApplication, then
IGsmSimApplication {cd4808e6-1e65-4b40-950d-d4c5ae0dc9a7}) so the holders bind; the TSY tests the
IGsmSimApplication holder (`vtbl[2]`) to tell the first QI from the second [V]. Today's QI path registers an object
per QI ("works today" per p1i-semc-tsy-sim.md) [I that it binds here too].

No callback rows are needed for SIM-ready. Optional, only if a log shows CSecurityOpa waiting:
`ccpu_callbacks` row "after ISimManager m4/m7 (subscription) -> ICBSimManager m3, args {03000000, 00000000}" [I].

## 5. After "ready" (not followed, budget)

- TSY clients then talk to ISimApplication (m3..m24, table sizes in syborg_ccpu_opa.inc) / IGsmSimApplication:
  PIN state, IMSI, service table [I]. Zero outputs mean status 0 and zero values; whether 0 means "PIN not
  required" is not read. Next step on a boot: log ISimApplication/IGsmSimApplication calls after m6 and look for
  a negative status or a new "TSY_MErr" [I procedure].
- Other ISimManager users (an early start-up client, Security.dll's #46 class) will also see one application;
  they get the same m3/m6 answers [I].
- Another semc.tsy class (observer f8270386) handles (ISimManager, 7) then a QI; m7 also binds an interface
  (outif guess ISimManager) [V kind, I purpose].

## Addresses

semc.tsy: reader observer f824644c, m5 call f82463e6, m5 done f824639c, m3 done f82462a6, m6 done f824624c,
QI done f82464ba, ready f82461cc, state write f8246186 (`P+0xdc`), pending list f8245e7a; ISimManager IID
f828b4d4, ISimApplication f828b4a4, IGsmSimApplication f828b4b4, IShell f828b4e4. OpaClient: ISimManager
reply parser f80f4436 (switch at f80f445c), m3 sender f80f3be6, CICBSimManager dispatch f80c84a4 / m3 f80c847c.
