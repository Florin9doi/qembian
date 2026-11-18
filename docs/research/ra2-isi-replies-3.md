# Nokia 9500 (RA-2): LightServer's ISI requests (CuiWs light-client connect = KErrNotFound)

Static reading of `eka1/ra2/core/ROM.bin` (objdump, Thumb; no emulator run). [V] = read in the code,
[I] = inferred. Raw byte numbers as in `src/qemu-ngage/isi-replies-ra2.txt`; "data[n]" = byte 10 + n.
On this phone the ISA length at [4..5] is **little-endian** (LightServer reads `[5]<<8 | [4]`, and the
logged frame `00 00 00 1c 0a 00 ...` has 10 bytes after [5]) [V].

## Summary

| Obj | Request | Sender | Reply | Verdict |
|---|---|---|---|---|
| 0x67 | `3A 03` tid 02 `{00 01 03 04 35 00}` | **LightServer.exe** 0x50096a7c (set brightness) | msg 04 (or 06), >= 1 data byte | **answer**: LightServer blocks in `User::WaitForRequest` |
| 0x65 | `1C 2D` tid 01 `{80 82 49 5F 65 78 ..}`, subscription `{1F 00 00}` | not LightServer; owner not found | - | leave silent |

Later LightServer requests on the same path (not yet seen): `3A 05` (0x50096978 / the branch at
0x50096b30, only when `this+0x98 == 1`), `3A 03` tid 03 (0x50096874), `15 14` tid 04 (0x50096d30).

## Object 0x67 is LightServer (not 0x65)

- The blocked stack: 0x50096167 = return from `bl 0x50096a7c` in the server's start-up (0x5009615c,
  args (this, saved level, 1)); 0x50096d1b = return from `bl 0x500966e0` at 0x50096d16 inside
  0x50096a7c; 0x50096711 = return from `bl 0x50097c38` (EUser `User::WaitForRequest`, IAT 0x500f041c)
  inside 0x500966e0 [V].
- 0x50096a7c builds, in the send buffer `this+0x7c` (+4 = the TDes8): [3] 0x3A, [4..5] 0A 00,
  [6] 0x30, [8] tid 02, [9] msg 03, [10..15] `00 01 03 04 <level byte> 00` with the level byte from
  the table at 0x50098220 + level (0x35 in the log), sent with IsaApi 0x50102054 (stub 0x50097ddc),
  then calls 0x500966e0 once [V]. That is exactly obj 0x67's frame, so **0x67 = LightServer** [V].
- obj 0x65's `1C 2D` is built nowhere in LightServer (no 0x1C/0x2D constants; its only ISI
  resources are 0x3A and 0x15) [V]. Its data bytes `49 5F 65 78` = "I_ex", the middle of
  "..._TI_execute", a symbol string found only in the DSP plug-ins (Dsp_image.dof, *dyn.dll55l) [V];
  the request probably carries a stale buffer from the audio/DSP side (DMS, Bal) [I]. Owner not
  found within budget: IsaApi importers are EStartavalon, ASC1510, GenDll, Dataport.csy, Nokia.dsy,
  H4PlusHci, StStarter, MmfIsaTone.exe, Emon.exe, PhoneTsy, EInfoServer, SpeDeServer, LightServer,
  four codec plug-ins, Linda.gdr [V scan]; none of MmfIsaTone, EInfoServer, StStarter, GenDll, Emon
  has 0x1C/0x2D immediates [V]. Resource 0x1C is "PN_ACCESSORY?" in platform/isi/names.txt [I].
  Leave it silent: it does not hold LightServer [V], and a guessed reply layout could do harm [I].

## The receive: 0x500966e0 (one message, synchronous)

- `IsaApi 0x50102314` (stub 0x50097dcc: receive into `this+0x78`, status on the stack), then
  `User::WaitForRequest` [V]. **Any** message to object 0x67 ends the wait; the function does not
  loop [V]. Receive error -> returns -2 [V].
- [3] == **0x3A**: TPtr8 over data[0..len-4); msg id (IsiMsg 0x500f5be8, stub 0x50097e00) **04 or 06**
  -> reads data[0] and drops it; returns 0 [V]. Anything else for 0x3A -> returns 0 [V].
  Minimum: 1 data byte for msg 04/06 (data[0] on an empty TPtr8 = USER 21) [V].
- [3] == **0x15**, msg **0x77**: data[0] == 00, data[1] == 01, data[2] == 02, data[4] == 00 and
  (data[5] & 1) -> returns 1, else 0 [V]. Minimum: 6 data bytes [V].
- The return value of the 3A path is ignored by 0x50096a7c [V]; data[0] meaning not read
  (status, 00 = OK) [I].

## Light requests (resource 0x3A, PN_LIGHT [I])

- `3A 03` (0x50096a7c tail, 0x50096874): data `{00, 01, 03|02, 04, level, 00}` = set target(s)
  brightness [I]. Reply **msg 04** (= id + 1, the 0x3A path accepts 04) [V accepted, I pairing].
- `3A 05` (0x50096a7c head when `this+0x98 == 1`, 0x50096978): data[0..] `05 05 00 00 00 05 03 04 lvl`
  style [V bytes, I meaning]. Reply **msg 06** [V accepted, I pairing].

## MTC `15 14` (0x50096d30, tid 04)

- Sent from 0x50097190 only when HAL attribute 0x39 (Hal stub 0x50097e9c) reads 0 [V]; the result
  picks LightServer state 3 (reply returns 1) or 1 (returns 0); HAL != 0 gives state 2 [V]. HAL 0x39
  meaning (cover/flip?) [I]. Reply **15 77** (MTC replies = id + 0x63) [V parser]; returning 0 is
  the plain choice [I].

## Reply lines for src/qemu-ngage/isi-replies-ra2.txt

```
reply 3A 03 04 00                 # LightServer (obj 0x67) set brightness: receive 0x500966e0 accepts res 3A msg 04/06, reads only data[0] (>= 1 byte, else USER 21) [V]; 00 = OK [I]. Unanswered = LightServer never registers (CuiWs KErrNotFound) [V]
reply 3A 05 06 00                 # LightServer 3A 05 (0x50096978, when this+0x98 == 1): same parser [V], msg 06 pairing [I]
reply 15 14 77 00 00 00 00 00 00  # LightServer MTC 15 14 (0x50096d30): parser 0x500966e0 needs 6 data bytes [V]; this content returns 0 = state 1 [V], meaning [I]
```

Left silent: obj 0x65 `1C 2D` and its subscription (not LightServer [V], owner unknown [I]).
