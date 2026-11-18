# RA-2 (Nokia 9500) ISI replies, part 5: CbsServer's blocking ETel call, SIM replies

Static reading only (objdump of eka1/ra2/core/ROM.bin), no runs. [V] = read in the code,
[I] = inferred. Budget reached; open points at the end.

## What CbsServer waits on (goal 1)

- The stack frame 0x50BF38CF is the return address inside CustomApi.dll **ordinal 71** (export table
  0x50BF3E88, entry 0x50BF3FA0 -> 0x50BF38C5): `RTelSubSessionBase::Blank(0x2713)` through the ETel
  import stub 0x50BF3CBC -> ETel 0x502B5258 [V]. A synchronous IPC 10003 with no arguments; by the
  S60 RMmCustomAPI order this is **StartSimCbTopicBrowsing()** [I name].
- CbsServer calls it from 0x50CA2F10 (import stub 0x50CA7000, literal 0x50CA7008 -> 0x50BF3FA0) [V];
  after it, a GetNextSimCbTopic loop (stub 0x50CA700C) [V]. Result -21 has a special path, any
  other error just returns; the callers 0x50CA33CE / 0x50CA43C8 ignore the result [V]. So **any
  completion unblocks CbsServer** [V]. Frame 0x50C9C5DB (ConstructL area, 0x50C9C13C NewL) is a
  stale return address on the stack scan [I].
- In Mmcustomtsy the request is **09 23 {25}** (the one sent with tid 08) [I: built near
  0x50C320E6/0x50C320F2, `movs #0x25` / `movs #0x23`]. Its reply is handled by:
  - Mmcustomtsy SIM receiver 0x50C31804: resource 09, msg **0x24**, field 0 (= frame[10] = data[0])
    must be **0x25** -> parser **0x50C3211C** [V].
  - 0x50C3211C [V]: looks the tid up among pending requests (`vf+0x28(8, tid, 1)`; no match = dropped);
    status = field 1 = frame[11] = data[1]; **01 = OK**, else error -21; when OK, count = field 7 =
    **frame[12] = data[2]** (IsiMsg 0x500FB9F4 maps field ids 7/8/9/0x14/0x1C/0x1D/0x21/0x22 to offset 12).
    Then 0x50C32F38(count, err): count 0 with no error completes the Blank with **-1 (KErrNotFound,
    "no CB topics on the SIM")**; count > 0 builds a topic list and expects topic reads later.
  - Min length: 3 data bytes (frame >= 13) [V offsets, I panic kind].
- This is exactly the 6600 MMCUSTOMTSY line. **No PhoneTsy start-up step is involved**: answering
  09 23 alone should let CbsServer finish ConstructL, so CbsMcnClient finds the server [I].

## IsiMsg field accessors (why 09 31 panicked USER 22)

- Mmgsmwcdma's `0x50C0CC0C(msg, id)` -> IsiMsg **0x500FB9F4**: byte field by **id**, not by index:
  id 0 -> frame[10], 1/4/0x1B -> [11], 7,8,9,0x14,0x1C,0x1D,0x21,0x22 -> [12], 0x15/0x1A -> [13],
  5 -> [16], 6/0x16 -> [17], 0x17 -> [18], 0x18 -> [19], 0x19 -> [20] [V].
- `0x50C0CC18(msg, id)` -> IsiMsg **0x500FBBCC**: a **Mid(offset, length) of the whole frame chosen by
  field id** (base = frame byte 0, descriptor at msg+4) [V]: 0x24 -> Mid(18, 2*[16]); 0x25 ->
  Mid(18 + 2*[16], 2*[17]); **0x26 -> Mid(16, 60)**; 0x45 -> Mid(12,10); 0x27 -> Mid(12,11);
  0x43/0x29 -> Mid(12,3); 0x42/0x21/0x22 -> Mid(12,4); 0x3F -> Mid(12,30); 0x3E -> Mid(12,50);
  **0x2A -> Mid(12, [12]+1)** [V].
- 09 32 handler (Mmgsmwcdma vtable slot +0xDC = **0x50C09EE0**) [V]: tid must match (`vf+0x28(1, tid, 0)`);
  status = frame[11] = data[1], **01 = OK**; then field 0x21 = Mid(12,4) -> 4 bytes, and at
  0x50C09F7C field **0x26 = Mid(16,60)** -> a 60-byte buffer. So the frame must be **>= 76 bytes**
  (data length >= 66); the N-Gage `len=13` (frame 23) can never satisfy it -> USER 22 [V]. The 9500
  answer is not the N-Gage ALS layout: 4 bytes at data[2..5] + 60 bytes at data[6..65] [V layout,
  meaning I]; status != 01 -> completes with an error from 0x50C0C9A0(9, 0, ..) [V].

## Other SIM replies (Mmgsmwcdma router 0x50C07348, resource 09) [V dispatch]

Resource 09 msg ids handled: 0A (+0xAC), 0C (+0xCC), 10, 14 (+0xD4), 1A (+0xB4), 1E (+0xBC),
22, 24 (to Mmiface -> custom TSY), 2A, 32 (+0xDC), 59 (+0xC4), BB, EF.

- **09 09 {01 00} -> 0A**, handler 0x50C089B8 [V]: no tid check; field 1 = data[1]: 02 or 22/23 ->
  one path (sets obj+24 = 1, then +0x28), any other value -> obj+8 = obj+4 = 1; both notify
  `[this+4]->vf+0xFC` [V]. 01 = OK (oFono SIM_SERV_OK) [I]. Min 2 data bytes.
- **09 1D {2D 00} -> 1E** (IMSI), handler 0x50C0A054 [V]: data[1] must be **01**; field 0x2A =
  Mid(12, data[2]+1) = {len, BCD bytes} copied into a 15-byte buffer; digits: high nibble of
  data[3], then low/high nibbles of data[4..], high nibble F = filler [V]. len 8 = 15 digits
  (more overflows the 15-digit buffer [I panic]). IMSI 001010000000001: `08 09 10 10 00 00 00 00 10`.

## Proposed lines

```
reply 09 23 24 @25 25 01 00                           # MMCUSTOMTSY StartSimCbTopicBrowsing (CbsServer's Blank 0x2713, CustomApi ord 71): parser 0x50C3211C, [0]=25 [V], tid matched [V], [1] 01 OK [V], [2] count 0 -> KErrNotFound = no SIM CB topics [V]; CbsServer ignores the result [V]
reply 09 31 32 len=66 3B 01                           # SIM 09 31 {3B 00 00}: handler 0x50C09EE0 [1] 01 OK, Mid(12,4) + Mid(16,60) -> frame >= 76 [V]; contents zero [I]
reply 09 09 0A 01 01                                  # SIM 09 09 {01 00}: handler 0x50C089B8 reads [1]; 01 = OK path [V path, I meaning]
reply 09 1D 1E 2D 01 08 09 10 10 00 00 00 00 10        # SIM IMSI {2D 00}: handler 0x50C0A054, [1] 01, [2] len 8 + 8 BCD bytes [V]; IMSI 001010000000001 [I]
```

`@25` keeps the 09 23 line for the {25} sub-request only (format of isi-replies.txt).

## Not read (budget)

- 1B 00 {41} (IMEI), 5A 00, DB 01 {...31...31}, 31 30, 02 14 {01 00}: not handled by the Mmgsmwcdma
  router (resources 1..10 only; resource 02 there takes only msgs 1B and A0) [V]. PhoneTsy has a
  resource-0x5A path at 0x50C49072 that tests msg ids 0x49/0x42/0x44 (indications, not a 5A 01
  reply) [V]. Their parsers were not found; leave them silent unless a run shows a waiter [I].
  N-Gage hint for 02 14: `reply 02 14 15 00 00` (data[0] = parameter-set count) [I for the 9500].
- Check in a run: CbsServer leaves the Blank after the 09 24 reply (strace on CbsServer), and the
  09 32 line no longer panics.
