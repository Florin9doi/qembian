# Nokia 3230 (RM-51): Nokia.dsy audio requests `0B 23` and `0B 11 {01 00}`

Static reading of `eka1/rm51/output/rom/ROM.bin` (Nokia.dsy at 0x508386D0, 44764 bytes, Thumb).
[V] = read in the code, [I] = inferred. Frame offsets are ISA_IF frame offsets (msg id at [9], data
from [10]). isimsg.dll accessors are named by what they read, not by their export names (the export
table of isimsg.dll is not in `analysis/user-exports.tsv`).

## Where the replies go

- Resource switch [V]: 0x5083E70E (`subs r0, res, #2; cmp #0x51`, table at 0x5083E724). Resource
  0x0B there goes to 0x5083E8D0, which handles msg 0x12 / 0x17 for a *different* DSY object (it is not
  the audio class below).
- Audio class receiver [V]: 0x50839D94, `res == 0x0B`, then a jump table on the msg id, range 01..1B
  (table at 0x50839DE4). Handled ids: 01 -> 0x5083AA94, 02 -> 0x5083AF5C, 12 -> 0x5083A800,
  13 -> 0x5083AA34, 15 -> 0x5083AB48, 16/1B -> 0x5083A7C4, 1A -> 0x5083AB90. **No 0x24.**

## `0B 23 {00 00}` (tid 4): no reply is consumed [V]

- Sender 0x5083AE9C (via 0x5083AF1C, called from the AO method 0x50839964 that sets state 9 and
  `SetActive`): data = {arg, 00}, tid stored at this+0x60, the AO's TRequestStatus stored at this+0xCC,
  pending flag this+0x84 = 1.
- Nothing completes this+0xCC: the only code touching +0x84 / +0xCC is the constructor (0x50839A78)
  and the sender. The tid-based completer 0x5083ACD0 checks slots +0x68/+0x70/+0x74/+0x78/+0x80 only,
  not +0x84 (tid +0x60). Msg 0x24 is outside the receiver's 01..1B table, so a `0B 24` would be dropped.
- Conclusion: the 3230 DSY never consumes an answer to `0B 23`; leave it unanswered (as harmless as the
  6600's unanswered request) [V for "not consumed", I for "harmless": the state-9 AO just stays active].
  A `0B 13` with tid 4 would only reach the AsyncStop path below, not this slot.

## `0B 11 {01 00}` (tid 9, sent twice): the one that blocks [V code, I who calls]

- Sender 0x5083A210 (called through 0x50839388 -> `[this+0x28]`): data {01, 00}, **fixed tid 9**,
  sends, then 0x5083AF40: if this+0x20 == 0 set it to 1 and `CActiveSchedulerWait::Start` (this+0x24);
  after the wait it returns the u16 at this+0x54 to the caller (*r1). A second call while the first
  waits does not wait (this+0x20 already 1) and returns the cached value at once.
- The wait is stopped only by 0x5083AF5C (`AsyncStop` if this+0x20): called by msg 02, by msg 13 whose
  tid matches neither this+0x5C nor this+0x59, and at the end of the msg 12 handler when its local
  flag r9 is set.
- Msg 12 handler 0x5083A800, subtype 01 branch (0x5083A98E):
  - [10] subtype (isimsg field 3, default data offset 10) = 01 (07 is accepted too);
  - [11] sub-block count != 0 (else nothing is done);
  - first sub-block (from [12]): sb[0] = 02 (checked with `== 2`), sb[2] = sub-block id = 00 for subtype
    01 (08 for subtype 05, 01 for 06, 02 for 05/02); u16 value at sb+4 (isimsg u16 field 25: offset 4;
    byte order BE as on the 6600 [I]);
  - stores the value at this+0x54; r9 = (this+0x38 != 0) **before** setting this+0x38 = 1; if the async
    slot (+0x6C pending, tid +0x59, sender 0x5083A290) matches, writes the value to its result and
    completes with 0 (or -3 if cancelled).
  - if r9: AsyncStop.
- Consequence [V]: the *first* subtype-01 reply only fills the cache (this+0x38 0 -> 1) and does **not**
  end the wait; the second one does. The phone sends the request twice while hung (the second call
  returns at once because a wait is already active), so answering both requests ends the first wait.
  From then on every reply stops the wait at once (this+0x38 is never cleared).
- Msg 13 (0x5083AA34) for tid 9: not the async tids -> AsyncStop without a value (cache stays 0);
  if it matched the async subtype-01 slot it would complete that with -2 [V]. Not recommended.

Reply (data from frame [10]): subtype 01, 1 sub-block, sb {02, len 08, id 00, 00, u16 0000, 00 00}:

    reply 0B 11 12 @01 01 01 02 08 00 00 00 00 00 00      # frame length 20

The value 0000 is [I] (meaning of subtype 01 unknown; on the 6600 subtype 05 carried 0000 as well).

Also missing from the 3230 table but present in the 6600's: the subtype-05 line (same parser here,
0x5083A87C: [10] 05, sb id 08, u16 -> this+0x40):

    reply 0B 11 12 @05 05 01 02 08 08 00 00 00 00 00

## Who waits

- DosServer's audio call into the DSY (0x50839388 -> 0x5083A210) is synchronous through a nested
  `CActiveSchedulerWait`; the RMessage of the DosServer client is completed only after it returns [I].
  The key-sound server (inside MMF init, the thread that Menu's `CAknKeySoundSystem::NewL` waits on) is
  the likely client [I, not traced through DosSrv.dll/MMF].
- Risk [I]: if a boot path sends `0B 11 {01 00}` only once before anything else stops the wait, one reply
  is not enough (first reply only caches). Then the modem would need to send the 12 twice (the table
  answers one reply per request) or send an unsolicited `0B 02`.
