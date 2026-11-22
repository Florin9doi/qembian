# Haptics over ISI on the 808: the 1C 9E request, the 1C 9F reply, what the plugin reads

Task `haptics-isi-packet` (objective vibra-device), 2026-11-19. Device nokia-808-rm807, firmware
RM-807/113.010.1508/04.01+C00.01. Static reading of two ROFS binaries, decompiled headless into
`build/roms/nokia-808-rm807/RM-807_113.010.1508_04.01+C00.01/ghidra/`:

- `hapticsadaptation.dll.c` (base 0x80f18ee8): the ISI sender/receiver (request builder `FUN_80f195a8`
  at +0x6C0, reply parser `FUN_80f194a0` at +0x5B8, 3000 ms timeout constants in `FUN_80f19184`/`FUN_80f19400`).
- `hwrmhapticsvibepacketizerplugin.dll.c` (base 0x80f0bf08): the packet encoders (vtable at 0x80f0d634,
  slots 2..28) and the reply decoder `FUN_80f0d2e2` (slot 30) with the status map `FUN_80f0d294`.
  The encoders are virtual methods that auto-analysis never reached; they were recovered by a
  post-script that creates Thumb functions at every vtable entry (first decompile stopped at 0x80f0c0c2).

Scope: just enough to answer the request without hanging. No naming of internals beyond the packet.
Marks: [V] read in the decompile, [I] inferred.

## 1. Request: ISI 1C 9E (hapticsadaptation `FUN_80f195a8`) [V]

The argument is the packetizer's packet (a TDesC8 of length L). The ISI message is
`total = align4(L + 0x16)` bytes, zero-filled, then (offsets from the phonet header, byte 0 = media):

| offset | width | value | meaning |
|---|---|---|---|
| 0x03 | u8 | 0x1C | resource |
| 0x04..0x05 | u16 BE | set by the ISI class | phonet length |
| 0x08 | u8 | request object's byte +0xC | transaction id (per request, allocated by the adaptation) |
| 0x09 | u8 | 0x9E | message id |
| 0x0A | u8 | 0 | [I] filler / sub-block area start |
| 0x0B | u8 | 1 | number of sub-blocks [I: the reply parser demands non-zero here] |
| 0x0C | u8 | 0x64 | sub-block id 100 [I] |
| 0x0D | u8 | 0 | filler |
| 0x0E..0x0F | u16 BE | total - 12 | sub-block length (from 0x0C to the end) |
| 0x10..0x13 | 4 x u8 | 0 | (0x10 and 0x12 are written as 0; 0x11, 0x13 stay zero-filled) |
| 0x14..0x15 | u16 BE | L | packet length |
| 0x16.. | L bytes | packet | the packetizer's packet, zero padding to `total` |

The adaptation sends one request at a time per command object (state 0 -> 1) and waits 3000 ms [V constants];
unanswered -> KErrTimedOut (-33) to the haptics server (the N8 note: FeedbackPlayerThread dies, the channel is reopened).

## 2. The packet (hwrmhapticsvibepacketizerplugin, little-endian, opcode u16 at 0) [V layouts, I names]

All integers inside the packet are little-endian (ARM byte stores). `h` = the device handle returned by
0x12 (cached at obj+0x28, -1 after 0x13). Names in the last column are the public HWRM haptics
API operations that fit the argument patterns [I]; the byte layouts are [V].

| op | encoder | bytes after the u16 opcode | len | likely operation |
|---|---|---|---|---|
| 0x12 | `FUN_80f0c116` (slot 2) | [2..5] u32 logical actuator | 6 | OpenDevice (N8 log: `12 00 01 00 00 00`) |
| 0x13 | `FUN_80f0c15a` (slot 3) | [2..5] u32 h | 6 | CloseDevice (N8 log: `13 00 ff ff ff ff` after a failed open) |
| 0x1C | `FUN_80f0c21e` (slot 4) | [2..5] h, [6..9] duration, [10..13] magnitude, [14] style<<4, [15..18] 0, [19..22] attack time, [23..26] attack level, [27..30] fade time, [31..34] fade level | 35 | PlayMagSweepEffect |
| 0x1D | `FUN_80f0c3ac` (slot 5) | as 0x1C but [14] = style<<4 \| 1, [15..18] = effect handle | 35 | ModifyPlayingMagSweepEffect |
| 0x1E | `FUN_80f0c55a` (slot 6) | [2..5] h, [6..9] duration, [10..13] magnitude, [14..17] period, [18] style<<4, [19..22] 0, [23..38] attack/fade as above | 39 | PlayPeriodicEffect |
| 0x1F | `FUN_80f0c714` (slot 7) | as 0x1E but [18] = style<<4 \| 1, [19..22] = effect handle | 39 | ModifyPlayingPeriodicEffect |
| 0x19 | `FUN_80f0c848` (slot 8) | [2..5] u32 B, [6..9] u32 A (call args A,B swapped) | 10 | Pause/Resume/Stop effect family [I] |
| 0x1A | `FUN_80f0c89c` (slot 9) | same as 0x19 | 10 | " |
| 0x07 | `FUN_80f0c8f0` (slot 10) | same as 0x19 | 10 | " (the one a touch feedback ends with, by position: stop) [I] |
| 0x08 | `FUN_80f0c934` (slot 11) | [2..5] u32 | 6 | StopAllPlayingEffects(h) [I] |
| 0x04 | `FUN_80f0c9d4` / `FUN_80f0cb12` (slots 12, 13) | [2..3] u16 data len, [4..7] h, [8..9] u16 effect index, [10] repeat (0 in slot 12), [11..] IVT data | 11+n | PlayEffectIncludeEffectData (+Repeat); refused (empty packet) when n+0x50 > a max at 0x80f0cd70 |
| 0x05 | `FUN_80f0cbfe` / `FUN_80f0cc9a` (slots 14, 15) | [2..3] 0, [4..7] h, [8..9] u16 effect index, [10] repeat (0 in slot 15), [11] 0 | 12 | PlayEffect (+Repeat) from data already loaded |
| 0x17 | `FUN_80f0ccde` (slot 16) | [2..5] u32 | 6 | [I] a query with a u32 answer (see the decoder) |
| 0x18 | `FUN_80f0cd32` (slot 17) | [2..5] u32 B, [6..9] u32 A | 10 | [I] |
| 0x16 | `FUN_80f0cd36` (slots 18, 19) | [2..5] u32, [6..9] u32, [10] data len8, [11..12] u16 (0 in slot 18), [13..] data | 13+n | [I] effect-data upload |
| 0x10 | `FUN_80f0ce9a`/`cef8`/`cfb8`/`d0ba` (slots 20..23) | [2..5] h (slot 20: a cached byte), [6..9] property, [10] type (1 int32, 2 bool, 3 string), [11] value length (4 / 1 / n), [12..] value | 13 / 16 / 12+n | SetDeviceProperty int/bool/string |
| 0x11 | `FUN_80f0d18c`, `FUN_80f0d188` (slots 24..26) | [2] type (1 int32, 2 bool, 3 string), [3..6] h, [7..10] capability | 11 | GetDeviceCapability |
| 0x09 | `FUN_80f0d21a`, `FUN_80f0d216` (slots 27, 28) | [2] type (1 int32, 3 string), [3..6] h, [7..10] property | 11 | GetDeviceProperty |

Slots 32..39 return constants (0xFF, 0x80, 0x40 x3, 0xFF, 0): size limits the server asks the plugin for [V values, I meaning].

## 3. Reply: ISI 1C 9F, what is read before the state machine advances

### 3.1 hapticsadaptation `FUN_80f194a0` [V]

Reads by raw offset (the message id is not compared; the pending request is found by transaction id,
`FUN_80f1922a` matches byte +0xC of the request objects [I: the thunk that extracts ISI[8] is not in the decompile]):

| offset | width | must be | else |
|---|---|---|---|
| 0x0A | u8 | 0 | error -2 (KErrGeneral) handed up, state 3 |
| 0x0B | u8 | != 0 | " |
| 0x11 | u8 | 0 | " |
| 0x14..0x15 | u16 BE | packet length P | |
| 0x16.. | P bytes | the reply packet | handed up as (error, packet) |

Success -> state 2, error 0. Either way the pair (error, packet) is externalised (hwrmhapticspluginservice
ord3 = NewL(error, data), ord12 = ExternalizeL) and delivered to the server with the transaction id
(`FUN_80f19400`). So the adaptation never hangs on a well-formed reply: a failure status completes the
request with -2 at once (what H-haptics-fail does), a success status makes the server decode the packet.

### 3.2 Packetizer decoder `FUN_80f0d2e2` (slot 30) [V]

Called with the reply packet; switches on the u16 opcode at packet[0..1] (so the reply must carry an opcode,
and it is the only thing that tells the plugin which answer this is). For every handled opcode
`status = (s8) packet[2]`, mapped by `FUN_80f0d294` to a Symbian error (0 -> KErrNone; -1 -> -11; -2, -10 -> -18;
-3, 1 -> -6; -4 -> -2; -5, -6, -7 -> -5; -8, -11 -> -21; -9 -> -4; -12 -> -14; other -> 0). The values are the
Immersion VibeTonz status codes [I: from the mapping]. The extracted values are appended to a CDesC8Array that
the decoder returns; its entries are what the server hands to the client.

| reply opcode | reads after the status | appended |
|---|---|---|
| 0x04, 0x05, 0x17, 0x1C, 0x1D | u32 LE at packet[3..6] | 4 bytes (effect handle [I]) |
| 0x12 | u32 LE at packet[3..6], also cached as the device handle (obj+0x28) | 4 bytes |
| 0x13 | nothing; cached handle := -1 | nothing |
| 0x1B | s8 at packet[3] | 4 bytes |
| 0x11 | packet[7] = type: 1 -> u32 at [9..12]; 2 -> u8 at [9]; 3 -> string, length packet[8] clamped to slot 38 (0xFF), at [9..]; other types: status only | 4 / 4 / n bytes |
| 0x09 | packet[7] = type: 1 -> u32 at [9..12]; 3 -> string (length [8], clamp slot 37 = 0x40) at [9..] | 4 / n bytes |
| 0x07, 0x08, 0x10, 0x16, 0x18, 0x19, 0x1A, 0x1E, 0x1F, 0x20 | nothing | nothing |
| any other opcode (including an empty packet) | `User::Leave(-2)` | - |

The reply's [7]/[8] (type/length) sit where the request had h, so request and reply layouts of 0x09/0x11
differ: do not echo the request packet, build the reply.

"Effect done": the plugin has no completion beyond the reply. A play request is finished for the client as
soon as its 1C 9F arrives with status 0 and the 4-byte handle (0x04, 0x05, 0x1C) or with status 0 alone
(0x1E, 0x1F). Nothing waits for the effect's duration; a stop (0x07 family) is a separate request [V: no
other message is read by the adaptation; I: the server's own handling, hwrmhapticsserver not read].

### 3.3 Why "OK with an empty packet" froze the N8 UI [I, consistent with the code]

With status 0 and P = 0 the decoder reads the opcode from an empty descriptor and leaves with -2 (`default`).
A leave inside the server's response handling leaves the client's request pending (the server does not
complete it with the leave code), so EwSrv's FeedbackPlayerThread waits forever. A failure status at 0x0A never
reaches the decoder. The rule for the fake modem: never answer status 0 without a decodable packet.

## 4. Minimal successful reply

A reply the plugin accepts for every request it sends today, in `platform/isi/replies-bb5.txt` syntax (data
bytes start at ISI offset 0x0A; `len=22` pads to the 4-aligned message, 0x0E..0x0F = 0x20 - 0x0C):

```
reply 1C 9E 9F len=22 00 01 64 00 00 14 00 00 00 00 00 07 12 00 00 01 00 00 00   # haptics OK: sb 64, packet 7 bytes {op 0x12, status 0, device handle 1}
```

It answers every opcode as "OpenDevice OK, handle 1": accepted by the decoder for any request (the 0x12 case
caches handle 1 and returns one 4-byte entry `01 00 00 00`), so Open gets handle 1, plays get "effect handle 1",
Close re-caches 1 instead of -1 (harmless, the next Open sets it again), status-only requests get one surplus
entry [I: whether the server tolerates a surplus entry is not read; the decoder itself does]. This is the line
for the next task's first experiment (static table, no code).

The robust answer (for vibra-qemu-device, in the modem core / iscpeer.inl like the 09 29 echo) copies the
request's opcode and builds the packet by case:

| request op | reply packet |
|---|---|
| 0x12 | `12 00 00 01 00 00 00` (handle 1) |
| 0x13, 0x07, 0x08, 0x10, 0x16, 0x18, 0x19, 0x1A, 0x1E, 0x1F | `op 00 00` (status 0) |
| 0x04, 0x05, 0x1C, 0x1D, 0x17 | `op 00 00 NN NN NN NN` (an effect handle; a counter) |
| 0x11, 0x09 | `op 00 00 00 00 00 00 TT LL V..`: TT = request packet[2] (type), int32: LL=4, V = 4 bytes; bool: LL=1; string: LL=n |

Header for all of them: `00 01 64 00 LL LL 00 00 00 00 00 PP` with PP = packet length, LL = total - 12;
the transaction id in ISI[8] must be the request's [I: matching rule, V: every other reply of the core already does it].
Request fields the device should act on: 0x1C/0x1E duration and magnitude (ms, 0..10000 [I: HWRM API range]),
0x04 IVT data (opaque; the stop opcode ends it), 0x07 stop.

## 5. [inferred] list

- Names of the operations behind each opcode (table 2, last column) and of the 0x19/0x1A/0x07/0x18/0x17/0x16 group.
- The reply transaction-id matching (`FUN_80f1922a` compares byte +0xC; the extractor of ISI[8] is not decompiled).
- The status byte being a VibeTonz status (from the error mapping); 0 = success is [V] (maps to KErrNone).
- The server's reaction to a decoder leave (freeze) and to surplus array entries.
- "Effect done" = reply received: [V] for the adaptation/plugin, the server was not read.
- Sub-block id 0x64 / count at 0x0B: structure guessed from the reply parser's checks; values [V].
