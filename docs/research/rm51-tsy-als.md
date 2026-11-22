# Nokia 3230 (RM-51): the SIM request 09 0F {0B 00 00} behind the Phone app's GetAlsLine block

Static reading of `eka1/rm51/output/rom/ROM.bin` (VA = 0x50000000 + offset). [V] = read in the code at
the address given, [I] = inferred. Nothing here was run.

## Path

- `vmbx.dll RVmbxNumber::GetAlsLine` (0x50ee245c) sets the out value to 1, calls the sssettings
  export at 0x5135df31 with (this+0xE4, 2, &value) synchronously (RSSSettings::Get(ESSSettingsAls) [I]),
  and copies the value back only when it is 2 [V]. It ignores the call's error, so **any completion
  unblocks the Phone app** [V].
- The SIM ISI code is not in PHONETSY.TSY but in `MMGSMWCDMA.DLL` (0x507b66b0..0x507c6034), as on the
  6600 (docs/research/6600-tsy-sim-replies.md). Its message dispatcher 0x507bef4c switches on the
  resource (table 0x507bef80, res 9 -> 0x507befbe) and then the message id [V]. Message 0x10 goes to
  MMIFACE 0x507b54bc (a phonebook-read handler, service 09/49) [V], which is not this one; the
  0F/10 pair with service 0B is handled inside MMGSMWCDMA:
  - request builder 0x507c1bb8 (tid, service, u16 location): data = {service, 00, location appended
    as one TChar} sent as message 0F [V]. The location is how `0B 00 00` becomes `0B 00 00 01` [I on
    the exact encoding].
  - **reply handler 0x507c1c7c** [V] (follows the builder; reached for SIM 10 replies).

## isimsg field getters used (isimsg.dll)

Get8bit 0x50372124: field 0 -> frame [10], 1 -> [11], 5 -> [16], 6 -> [17] [V].
Get16bit 0x503721f8: field 29 with message id 0x10 -> two bytes from frame [12] [V; byte order I].

## Handler 0x507c1c7c

1. [10] (service) must be 0B or 6D; anything else -> forwarded to MMIFACE 0x507b54bc [V].
2. [11] = status; tid = the frame's transaction id; a virtual check (vtable+0x24, args 1, 25, tid)
   must accept it, else the reply is dropped [V] (the board copies the tid, so it matches [I]).
3. status == 01 and a target object present [V]:
   - counter (this+0x6C) = Get16([12..13]); a 4-byte record buffer (this+0x68) is created, [0] = tid,
     [1]++ (records), [2]++ unless [16] == 01 and [17] == 01;
   - counter++, then the **next request is sent** (vtable+0xD4: tid, service 0B, counter) = the
     second `0B 00 00 01` [V]. So status 01 means "record present, read the next one": answering 01
     every time makes the TSY loop (it re-asks location [12..13]+1) and never completes.
4. status != 01 [V]:
   - counter == 0 (first reply): error = MMUTILITY map 0x507ad530(9, 0, status) (res-9 switch 0x507af370,
     status 1..0x63, most values give a negative error, e.g. 09..13 -> -2) [V partly; the exact
     KErrNone status not found, I];
   - counter != 0: the collected buffer is handed on (0x507bdea4) and error = 0 [V];
   - completion through vtable+0x154 with that error; counter and buffer reset [V].
   Either way the IPC completes, which is all vmbx needs.

Bytes read: [10], [11]; on status 01 also [12..13], [16], [17] (frame >= 18). On the ending path only
[10], [11] [V]. Keep the 23-byte frame (len=13) so no getter can run past the end.

## Proposed table line

```
reply 09 0F 10 len=13 0B 00                               # 3230 SIM service 0B record read (MMGSMWCDMA builder 0x507c1bb8, handler 0x507c1c7c): [10] must be 0B, [11] 01 = record present -> TSY asks location [12..13]+1 (endless with a fixed reply); [11] != 01 = no more records -> completes (first reply: mapped SIM error, after a 01: KErrNone) and unblocks vmbx GetAlsLine [V]
```

The second request {0B 00 00 01} needs no different answer: with `0B 00` the first request already
completes (with a mapped error that vmbx ignores [V]); if the table keeps `0B 01` for the first
request, the second must get a status other than 01 (the current table cannot tell the two apart, so
use status 00 for both). [I] why the old `0B 01` run showed only two requests and no third: probably
the record buffer/target check; not traced.
