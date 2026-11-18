# Nokia 6600: the TSY's SIM requests behind the "Locked" restart hang

Static reading of the NHL-10 ROM (`eka1/nhl10/output/rom/ROM.bin`, VA = 0x50000000 + offset).
[V] = read in the code at the address given, [I] = inferred. Nothing here was run.

## Where the ISI code is

The 6600's `PHONETSY.TSY` (0x51468980) is only the common TSY; it contains no SIM or SMS parser
(its only message dispatch is the resource-5A indication filter at 0x5146FF10) [V]. The ISI
layers are separate DLLs, each with its own dispatcher (resource, then message id):

| DLL | base | dispatcher | handles |
|---|---|---|---|
| MMGSMWCDMA.DLL | 0x51428fd0 | 0x514306cc | SIM (09): 0A 0C 10 14 1A 1E 22 24 2A 32 59 EF; also resources 06, 08, 0A |
| MMIFACE.DLL | 0x51424890 | (called from the above) | SIM 24 -> 0x51427b58, SIM 10 -> 0x51427d30 |
| MMCUSTOMTSY.DLL | 0x51456370 | 0x5145c954, 0x51456790 | SIM 24 (service 25), 40, 63; 2A (service 37/38) |
| MMSTORAGEGSM.DLL | 0x51448910 | 0x51448f00, 0x51449c72 | SIM 10 12 24 EF, 3E |
| MMSMSGSM.DLL | 0x5143da50 | 0x5143ed64, 0x5143db9c, 0x5143ef10 | SMS (02): 03 04 05 06 08 09 0B 14 16 18; 0C 0D; 15 |

SIM messages are read through isimsg.dll's field getter 0x503456d4 (`Get8bit(field)`), which maps
a field number to a frame offset and indexes the frame [V]: field 0 -> [10] (service type),
1/4/28 -> [11] (status), 7/8/9/20/21/29/30/36 -> [12], 22/27 -> [13], 5 -> [16], 6/23 -> [17],
24 -> [18], 25 -> [19], 26 -> [20], 19 -> [46], 10 -> [72], 11 -> [88] ... So "field 20" is frame
byte 12, not byte 30.

SIM status to Symbian error: MMUTILITY 0x5141fb08 (resource 9 -> table 0x51420e7c, indexed by the
status byte) [V]: 00 -> -1, **01 -> KErrNone**, 02/03 -> -21, 04/05 -> -22, 06/07 -> -36,
08/09 -> -22, 0B -> KErrNone.

## The call sssettings waits for: RMobilePhone::GetALSLine = SIM 09 29 {35 00}

- sssettings 0x51032d44 calls etelmm 0x50632b18 synchronously (the frame 0x50632b35 in the caught
  stack is its return address) with one TInt out value, and treats 2 and 3 as "ALS not there"
  (TMobilePhoneALSLine: 0 primary, 1 auxiliary, 2 unknown, 3 not available) [V]. That is
  `RMobilePhone::GetALSLine` [I from the value set; the IPC constant is 0xC01].
- Its caller 0x51032dc0 first reads a stored value (CommonEngine 0x50a0f771); only when that is
  missing (-1) does it ask the TSY, and it stores the answer afterwards [V]. [I] That stored
  value is why the fresh boot and the restart take different paths.
- The answer is SIM message 2A, handler MMGSMWCDMA 0x51433018 [V]:
  - the transaction id must be the request's (the board copies it);
  - [11] status: if it is not FF, error = map(status); FF -> error 0;
  - [10] must be 35; then line = 3 (not available) unless status == 01, in which case
    **[12] bit 0 set -> 0 (primary line), clear -> 1 (auxiliary line)**;
  - completion: (line, error). [10] == 36 is the write confirmation (error only).
  - Reads stop at frame byte 12: three data bytes are enough.
- So the experiment's `35 01 00 ...` was accepted, as "line 2 in use": the "2" indicator [V from
  the parser]. The four 09 29 requests (tids 06..09) are then more likely four GetALSLine calls
  than retries [I].

## SIM 09 23 {25}: MMCUSTOMTSY item count

- Sent by MMCUSTOMTSY 0x5145d1f0 (message 23, one data byte 25) [V].
- Answer 24 with [10] == 25: handler 0x5145d254 [V]: [11] == 01 -> count = [12], KErrNone;
  anything else -> count 0, error -21. Completion 0x5145e070: count > 0 allocates a list of
  `count` 52-byte items and continues with reads (message 3F service 48 -> 40); count <= 0
  completes at once [V]. What the items are was not traced [I: a custom-API SIM list].
- The same 24 is offered to MMIFACE 0x51427b58 and MMSTORAGEGSM 0x51448f50 / 0x51449cd4, which
  read only [11], [10], [12] as well (phone book store info: [12] = total entries) and match by
  transaction [V].
- `25 01 00` is right and cannot cause an out-of-range read.

## SMS 02 16 {00 00} and 02 1E {00 00}: not resolved

No dispatcher in MMSMSGSM consumes 02 17 or 02 1F [V: the three dispatchers and the jump table
0x5143eda8 above]; no other TSY DLL handles resource 02. A guessed `02 16 -> 17` reply is
dropped, so it neither helps nor produces the USER 21. In this SMS server the ids are not
"request + 1" throughout (05/06 call one handler with a flag 0/1; 0C/0D are the CB pair), so the
right response ids for 16 and 1E are unknown [I]. The senders were not found within the budget.
They are not on the sssettings path.

## Not traced

- SIM 09 09 {01 00}: answer 0A goes to a virtual handler of the MMGSMWCDMA object (vtable +156).
- phonetsy 5A 00 {00 00}; the DSY's 0B 11 {01 00}.
- The USER 21 of the experiment: none of the three guessed replies can cause it (their parsers
  read frame bytes 10..12 only, or nothing). [I] It comes from a later request that the
  "ALS present, line 2" answer led to and whose table reply is too short for an isimsg string
  getter (0x50345800, fixed-length Mid). MMCUSTOMTSY has a 2A handler for services 37/38
  (0x5145778c, reads [11], [10], [12]); sssettings also calls CustomAPI 0x50d07688 (IPC 0x2723)
  synchronously at 0x51032d18, probably the ALS programmable-parameter check.

## Proposed table lines

```
reply 09 29 2A @35 35 FF 00                               # GetALSLine (sssettings' synchronous call): [10] 35, [11] FF = no error mapped and line 3 "not available" (SIM without ALS) [V parser 0x51433018]; 01 + [12] bit 0 = primary, bit 0 clear = line 2
reply 09 23 24 @25 25 01 00                               # MMCUSTOMTSY SIM list count: [11] 01 OK, [12] count; 0 completes at once [V 0x5145d254 / 0x5145e070]
```

Alternative for the first line when ALS should exist with line 1 in use:
`reply 09 29 2A @35 35 01 01`.
