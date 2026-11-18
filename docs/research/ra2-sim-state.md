# Nokia 9500 (RA-2): where the SIM state comes from ("SIM card not valid")

Static reading of `eka1/ra2/core/ROM.bin` and the ROFx `eka1/ra2/images/RA2n6270.l18/r06-0000069.bin`
(no emulator run). [V] = read in the code/data, [I] = inferred.

## 1. The string and its consumer

- "SIM card not valid" is in **Coverui.r01** (ROFx entry at 0x150da: size 0x688b, address 0x01315948 =
  ROFx offset 0x75948; string at 0x7bc88) [V], in a 3-item array: `Insert SIM card`, `SIM card not
  valid`, `SIM card rejected` [V]. The resource is dictionary-compressed (UID 0x101f4a6b); its id was
  not decoded.
- The array matches the System Agent SIM values: Insert = not present/removed, rejected = rejected,
  "not valid" = the remaining states (init wait / blocked / ...) [I]. The cover UI reads the SA value
  through CuiUiComponents/Ckind/CuiSecurity (CuiSecurity and Ckind reference 0x100052E9) [V refs, I path].

## 2. The chain: Nokia.dsy -> DOS event 0x11 -> Sae.exe -> System Agent

**Sae.exe** (0x5015cc30) function 0x5015d6e8 takes the DOS SIM state 0..7 and sets [V]:

| DOS SIM state | KUidSimCStatus 0x100052E9 | KUidSIMStatus 0x100052C6 |
|---|---|---|
| 0..3, 6 | = state | unchanged |
| 4 | 4 | 1 (not present) |
| 5 | 5 | 2 (rejected) |
| **7** | **7** | **0 (OK)** |

(If the previous state was 1 it re-sets 0x100052E9 = 1 and stops.) The values are the S60 TSACSimStatus
order: 0 InitWait, 1 LockOperative, 2 PinVerifyRequired, 3 PermanentlyBlocked, 4 Removed, 5 Rejected,
6 Blocked, **7 Ok** [V numbers, I names].

**Nokia.dsy** (0x5016a100) raises DosSrv events through DosSrv stubs (0x50165dac + 0x18*k, event
number in r1): 0x50165dad = event 8, 0x50165df5 = **event 0x11 (SIM state)**, 0x50165dc5 = event 9,
0x50165e85 = event 0x17, 0x50165eb5 = event 0x19 (by stride, [I]) [V for 8/9/0x11/0x17].

ISI receive dispatch 0x50171950: resource 09 -> 0x50171990, 08 -> 0x50171df0, 53 -> 0x50171d48 [V].
Byte fields come from IsiMsg 0x500FB9F4 (id 1 -> data[1], 8 -> data[2]) or, for resource 08,
IsiMsg 0x500FB810: default frame[10] = data[0]; msg 0x12: field 6 -> data[1], 7 -> data[2];
msg 0x10: 1 -> data[1], 0 -> data[2], 6 -> data[3], 7 -> data[4] [V].

### Security code state 08 12 (reply to the DSY's 08 11; handler 0x50171e4c) [V]

tid must be a pending 08 11 tid. Then **data[0]** -> 0x50171eb4:

| data[0] | effect |
|---|---|
| 1, 6 | SIM state 7, event 0x17 = 1 |
| 2, 44 | SIM state 2 |
| 3, 45 | SIM state 6 |
| **5** | event 0x17 = 0, **SIM state 7**, sends 53 00 {00} (0x50171cd8), 09 3B (0x50171ba4), 0x50171078, 09 C0 (0x50171b2c) |
| 26 | SIM state 5 |
| 7, 22, 30, 31 | nothing |
| other | error path 0x501718a4 |

then **data[1]** -> 0x50172000: 0x12 -> event 0x19 = 1, 0x13 -> event 0x19 = 0, 0x63 -> nothing,
**anything else -> error path 0x501718a4** [V].

Error path 0x501718a4 [V]: event 0x17 = 2, event 8 = 0, **SIM state 0**, re-sends 09 C0 and **08 11**.

**This is the bug**: our `reply 08 11 12 05 00 ...` has data[1] = 00, so after setting state 7 the
DSY takes the error path, sets state **0 (InitWait)** -> KUidSimCStatus 0 -> "SIM card not valid"
[V chain to SA, I for the string choice]; the re-sent 08 11 gets the same reply, so it probably loops
(look for repeated 08 11 in the serial log) [I].

### Resource 09 handlers (0x50171990: msgs C1, 3C, ED, EF only) [V]

- **09 C1** (reply to the DSY's 09 C0 {00 00}, handler 0x50171a98): tid must match; data[1]:
  **01 -> event 8 = 1** (SIM ready, state unchanged); 1C -> state 5; 06, 24 -> state 4; 4B -> state 0;
  other -> error path. Our `09 C1 {00 01}` is right [V].
- **09 3C** (reply to 09 3B, handler 0x50171c10): tid must match; data[1] 2A -> event 9 = 0,
  2B -> event 9 = 1, other -> error path [V]. Not in our table (sent in case data[0]=5 above);
  unanswered just leaves it pending [V]. Meaning of event 9 [I].
- **09 EF** (ind, 0x501719ec): data[2]: 4 -> state 3 (+ event 8 = 0); 5 -> starts a timer (flag +0x54);
  other -> stops it [V]. Our `05 08 08` (data[2] = 08) is harmless [V].
- **09 ED** (ind): sends 08 11 if none pending [V].
- 09 2E / 09 2C are **not** handled by the DSY's object (no case for them) [V]; whoever sends 2D/2B
  (PhoneTsy [I]) parses them.
- **08 10** (ind, 0x50171e14): only triggers an 08 11 if none pending; contents not read [V].

## 3. Table lines

```
reply 08 11 12 05 13 00 00 00 00 00 00                   # security code state (Nokia.dsy 0x50171e4c): data[0] 05 -> DOS SIM state 7 = OK (Sae: SimCStatus 7, SIMStatus 0) [V]; data[1] must be 12/13/63 else error path -> SIM state 0 + 08 11 resent [V]; 13 -> event 0x19 = 0 [V], meaning (PIN request off) [I]; PhoneTsy reads data[7..8] [V]
reply 09 3B 3C 00 2A                                     # DSY 09 3B (sent on code state 05): data[1] 2A -> DOS event 9 = 0, 2B -> 1 [V]; which one is "normal" [I]
reply 09 C0 C1 00 01                                     # unchanged: data[1] 01 -> DOS event 8 = 1 (SIM ready) [V]
ind 82 52 09 EF 05 08 08                                 # unchanged: DSY reads only data[2]; 4 = state 3, 5 = timer, 08 = no-op [V]
```

## 4. Inner display / Startup

Not read (budget). Startup.app and SysAp.app do not reference 0x100052E9/0x100052C6 directly; SysAp
references 0x100052E3/0x100052F6 [V refs]. Best guess: the S80 start-up waits on the same SIM/security
state (SimCStatus leaving 0 InitWait) [I]; recheck after the 08 12 fix.
