# Nokia 9500 (RA-2): the ISI requests left unanswered at 31 processes

Static reading of `eka1/ra2/core/ROM.bin` (objdump; no emulator run). [V] = read in the code,
[I] = inferred. Goal: just enough to answer (or knowingly not answer) each request from
`src/qemu-ngage/isi-replies-ra2.txt`. Byte numbers: raw message bytes; "data[n]" = byte 10 + n.

## Summary

| Obj | Request | Sender | Reply | Verdict |
|---|---|---|---|---|
| 0x52 | `0B 00 {0A}` tid 01 | Nokia.dsy audio class 0x5016b3b8 | msg 01, data[0] = value | **answer**: DSY waits synchronously |
| 0x66 | `17 0C {0D 5C 00..}` tid 01 | SpeDeServer.exe 0x500b00cc | msg 0D, ignored | silence harmless |
| 0x66 | `17 06 {00 20}` tid 02 | SpeDeServer.exe 0x500b01d4 | msg 07, data {01, n, sb{0F, len, u16 BE}} | **answer**: completes a client request |
| 0x4b | `1B 00 {41}` tid 01 | TSY side (IMEI read, INFO_SERIAL_NUMBER_READ_REQ, target 0x41) | not read | no reply (as on the 6600) |
| 0x4b | `5A 00 {00 00}` tid 00 | TSY side | not read | no reply (as on the 6600) |
| 0x69 | `19 0A {00 00}` tid 98 | Custom.ldd req 3 | msg 0B, parser 0x500acd7c | optional "invalid" reply |
| 0x69 | `D1 04 {00}` tid 0F | Custom.ldd at open | res D2, parser 0x500ad9b0 unread | no reply |

## Object 0x52: Nokia.dsy audio `0B 00`

- Sender: 0x5016b3b8 stores the parameter id in `this+0x80` (first 0x0A, then 0x01) and calls
  0x5016bab0 for each [V]. 0x5016bab0 builds a 1-byte data message (the parameter), message id 00,
  transaction id from the counter at `this+0x24` (1..254), sends it through `this+0x2c`, then calls
  0x5016c584, which sets `this+0x18 = 1` and calls a EUser import on `this+0x1c` [V]: a nested
  `CActiveSchedulerWait::Start` [I strong]. The reply path stops it (0x5016c5a0, flag cleared) [V].
  So the DSY call that queries audio parameters does not return until msg 01 arrives; the second
  parameter (0x01) is only sent after the first reply [V order, I that it blocks DosServer's client].
- Receive: 0x5016b3e0 (resource == 0x0B, switch on message id - 1, table 0x5016b42c) [V]:
  - msg **01** -> 0x5016c1a0: copies the message, reads `IsiMsg+0x243c(msg, 0)` = byte 10 =
    data[0] [V: with arg 0 that accessor returns raw byte 10], stores it at `this+0x41` (param 0x0A)
    or `this+0x40` (param 0x01), then ends the wait [V]. 0xFF in those fields means "not known" [V].
    Minimum length: 1 data byte [V].
  - msg **02** -> 0x5016c21c: re-sends the current query [V] (server-not-ready style [I]).
  - msgs 0x12, 0x13, 0x15, 0x16, 0x1A, 0x1B: other handlers (indications) [V exist; not read].
- Value meaning: not read [I]. The N-Gage table answers this request with `01 00`.

## Object 0x66: SpeDeServer.exe (0x500ae650, Thumb)

Owner: `System/Programs/SpeDeServer.exe` [V: its subscription bytes `17 0E, 17 21, 1C 32, 09 EE`
are in its image at 0x500b2acc, client `SpeDeClient.dll`]. Resource 0x17 = energy management /
battery (the N-Gage table's `17 08`, `17 0A`) [I]. It watches the battery voltage against
**0x0D5C = 3420 (mV [I])** [V constant], a guard for a high-current user (speaker/flash?) [I].

- `17 0C` (0x500b00cc): data[0..1] = u16 BE threshold 0x0D5C, data[2..5] = 0, tid 01 [V].
  Reply **msg 0D**: the dispatch 0x500af990 treats 0x0D as a no-op [V]. Silence is harmless [V].
- `17 06` (0x500b01d4): tid = caller's argument, data {00, 0x20} [V].
  Reply **msg 07** (0x500af9f6) [V]:
  - data[0] must be **01**, else ignored [V];
  - data[1] = sub-block count n; sub-blocks of 4 bytes from data[2]: {id, len (not checked), hi, lo};
    id **0x0F** gives u16 BE at sb+2 = the voltage [V]; last 0x0F wins [V]. Minimum: 6 data bytes
    for one sub-block (TDes8 [] beyond the length = USER 21) [V].
  - Then 0x500aed18(value, tid) [V]: value **> 0x0D5B** = OK: retry counter cleared, per-tid action
    (tid 2: 0x500aeeec(0), 0x500b1cdc, 0x500b0290), pending client request (`this+0x54`, flag
    `this+0x7c`) completed with **KErrNone**, threshold `17 0C` re-sent [V]. Value <= 0x0D5B: first
    time re-sends `17 06`, second time low-battery actions and the client completes with -2 [V].
  - Unanswered: the SpeDeClient request stays pending [V]; what waits on it is not read [I].
- Indication `17 0E` (0x500af9b6): data[0..1] u16 BE; == 0x0D5C -> send `17 06` tid 1, else re-send
  the threshold [V]. Do not send at start-up (it only triggers a re-measure) [I].
- `17 21` indication (0x500afa78) and resource 0x32 (0x500afae0): not needed [V exist].

## Object 0x4b: `1B 00 {41}` and `5A 00 {00 00}`

- `1B 00` = PN_PHONE_INFO INFO_SERIAL_NUMBER_READ_REQ (platform/isi/names.txt), target 0x41 = the
  plain IMEI [I]. The parser was not found inside budget (no `cmp #0x41` near a reply parse in
  Mmgsmwcdma/PhoneTsy/Mmiface; 0x50c41bca in PhoneTsy is an unrelated range switch [V]).
- `5A 00`: the 6600 sends the same from its TSY object and reaches idle with it unanswered
  (docs/devices/nokia-6600-nhl10/bring-up.md; phonetsy's resource-5A code there is an indication
  filter) [V for the 6600, I for the 9500]. No 9500 module loads `mov #0x5A` in TSY code [V scan].
- Neither is in the 6600 table, so both stayed silent there [V]; keep them silent [I]. A wrong IMEI
  layout could panic USER 21; silence only leaves a GetPhoneId-style request pending [I].

## Object 0x69: Custom.ldd

- `19 0A` (req 3, tid 0x98), reply handler 0x500acd7c (ARM) [V]: raw byte 9 must be **0x0B**,
  data[0] == **01**, data[16] == 01 or 03 -> valid: parses raw bytes 16/17 (u16 BE), 18 (-1),
  19.. = a date/time [V reads, I meaning]. Anything else (other msg id, data[0] != 1) takes the
  "not valid" path 0x500acff8 (not read; completes the request without a time [I]). Request 3 is a
  DSY time-style query, not on Starter's path [I]. Silence leaves that DoRequest pending [I].
  An "invalid" answer only reads data[0] [V]; pad to 18 data bytes anyway [I].
- `D1 04` (at channel open, tid 0x0F): reply comes back as resource D2; parser 0x500ad9b0 not read.
  Nothing waits on it at start-up [I]; keep silent.

## Reply lines

```
reply 0B 00 01 01 00                         # Nokia.dsy audio param query {0A}/{01} (0x5016bab0, nested wait 0x5016c584): msg 01, data[0] = value stored at dsy+0x41/+0x40 (0xFF = unknown) [V parser 0x5016c1a0]; value 01 [I, N-Gage's]
reply 17 06 07 01 01 0F 04 0F A0             # SpeDeServer battery-voltage read: data[0] 01, [1] 1 sub-block, sb {0F, len, u16 BE mV}; > 0x0D5B = OK, client completed KErrNone [V parser 0x500af9f6/0x500aed18]; 4000 mV, len byte unchecked [I]
# 17 0C (SpeDeServer threshold 0x0D5C): reply msg 0D is a no-op in 0x500af990 -> no reply needed [V]
# 1B 00 {41} (IMEI), 5A 00 (obj 0x4b): silent on the 6600 too -> no reply [I]
# D1 04 (Custom.ldd open): parser 0x500ad9b0 unread -> no reply [I]
# Optional, only if something waits on Custom.ldd request 3:
# reply 19 0A 0B 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   # data[0] 00 -> "no valid time" path 0x500acff8 [V branch, I effect]
```

No start-up indication for object 0x66 is needed [I]; `17 0E` must not be sent with 0x0D5C unless
a re-measure is wanted [V].
