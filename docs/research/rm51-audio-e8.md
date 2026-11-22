# Nokia 3230 (RM-51): DevSound ISI resource 0xE8 (objects 0x55 / 0x56)

Static reading of `eka1/rm51/output/rom/ROM.bin`. [V] = read in the code, [I] = inferred.
Frame offsets are ISA_IF frame offsets (msg id at [9], data from [10]).

## Client module

- `System/Libs/MMFDevSound.dll` (0x50786E60, 29280 bytes, Thumb) talks ISI itself: it imports
  isaapi.dll (#1 ctor, #8 Open, #13 CancelReceive, #14 Receive, #16 Send, #17 subscribe-list,
  #18/#19 close) and isimsg.dll through stubs at 0x5078D684..0x5078D7B4 [V]. No HW-device plug-in,
  no separate audio server. It also loads `AUDIO.LDD` and uses the audio policy proxy [V].
- Two channel classes [V]:
  - object **0x55**: ctor 0x50786F00, Open(0x55,1) at 0x50786F40, send wrapper 0x50786FD0 (arms the
    receive, then sends), RunL 0x50787020. DevSound keeps it at `this+0x12C`.
  - object **0x56**: ctor 0x50787210, Open(0x56,1) at 0x50787250, send 0x50787340, RunL 0x507873A0,
    subscribes `{1F 06}` (0x507872E0). DevSound keeps it at `this+0x128`.
- Neither RunL checks the resource on object 0x55: it switches on the msg id only [V].

## Object 0x55 receive switch (RunL 0x50787020, table 0x50787050, ids 0x1A..0x2B) [V]

| msg | handler | what it does |
|-----|---------|--------------|
| 1A | 0x50787098 | if owner+0xF4 == 0: 0x5078B0B4 (next data block) |
| 1B | 0x507870AC | owner error -2 (0x5078A520) |
| 1D | 0x507870B8 | reads field 4 byte, 0x5078B204 |
| **21** | 0x507870F2 | no data read; ch+0x54 = 1; 0x5078AA90(owner, 0, 1): DoControl(audio.ldd, 1), policy/stream start |
| 24 | 0x50787102 | re-arm receive only |
| 26 | 0x5078710A | ch+0x54 = 0; field 0 byte -> 0x5078AA90(owner, b, 0) |
| **2B** | 0x5078715A | 0x5078A2F4(owner, **-10 = KErrUnderflow**): end of play |

Requests seen in the code (builder isimsg#27 `(tid, msg, data)`): 0x19, 0x1C, 0x1E, **0x20**
(0x5078BE2C), 0x23 / 0x27 (data blocks, 0x5078AF60 / 0x5078B018), **0x2A** (0x5078BD54).

## `E8 20` (play start) [V]

- Sender 0x5078BE2C: data `82, mode (2/3), u16 0x15E0, two u16 sizes, 00, 0x32 or 0x46`, async on 0x55.
- Answer: msg **0x21**, no field read, any length. Starts the stream (LDD enable + policy).
- Msg **0x2B** at any time ends the play: 0x5078A2F4 in state 4 (tone) calls 0x5078A984(this,0,0)
  (with r1 = 0 it sends nothing [V]), state = 0, tells the policy "stopped" (0x5078AA80, state 10), and
  calls the observer's vfunc +12 (ToneFinished/PlayError) with -10 [V for the calls, I for the name].

## `E8 2A` (stop) from 0x55 and 0x56: synchronous [V]

- DevSound Stop 0x50788588 (state switch), playing branch 0x507885F2: sets owner+0xF8 = 1, then
  0x5078BCA4 (object 0x56) and 0x5078BD54 (object 0x55) back to back (0x50788606 / 0x5078860C).
- 0x5078BD54: builds `E8 2A` (no data), CancelReceive on the channel, Send, then isaapi Receive into the
  channel buffer with a **local TRequestStatus** and **`User::WaitForRequest`** (0x5078BE0A); afterwards
  only `state = 5`. The received message is never parsed: **any** message to object 0x55 ends the wait.
- 0x5078BCA4: same pattern on object 0x56 (WaitForRequest at 0x5078BC70) [V]; message built with
  isimsg#24 (args 0, 4, {00 00}) [I how that becomes the `E8 2A` frame seen from 0x56].
- Stop is called from 0x50787D02 / 0x507887F8 (tone-sequence play when state != 0/4) and others [V];
  0x50787D02 being InitializeL is [I].

So with a tone still "playing" (0x20 never answered), the next Initialize/Play calls Stop, which blocks
the thread in User::WaitForRequest until 0x55 (and 0x56 before it) get a message. [I] The
KeySoundServerThread is blocked here, not in a scheduler wait: look for return addresses
0x5078BE0E / 0x5078BC74 (and 0x5078860A / 0x50788610) on its stack.

## Object 0x56 (RunL 0x507873A0) [V]

- res 0x0B, msg 01 (0x50787440): one byte through isimsg#119(field 0) -> ch+0x54; no success check.
  The byte is sent back later as the data of the close request (0x5078A984, r1 != 0, again a
  synchronous send+WaitForRequest on 0x56). msg 02: owner error -13.
- res 0x1F: msg 02 re-arm + flag; msg 06: byte at field 2 == 0x0D -> owner OK, else -13; msg 03: -13.
- So `reply 0B 00 01 01 00` fits: msg 01 is accepted with no check; the byte (01) is only echoed [V];
  which frame byte field 0 is ([10] by the isimsg default) is [I].

## Table lines

```
reply E8 2A 2B 00 00     # DevSound Stop: sync WaitForRequest on 0x55/0x56, content ignored [V 0x5078BD54]
reply E8 20 2B 00 00     # play start answered as "play ended" (KErrUnderflow): tone completes at once [V 0x5078715A]
# alternative: reply E8 20 21  -> stream starts, tone never ends; the next Stop then needs the 2A reply
```

`2B` to `2A` on object 0x56 lands in a sync receive too (ignored). If the table routes the reply to the
requesting object, one line covers both. Cheapest clean "no audio": the two lines above (every tone
finishes immediately with KErrUnderflow, which MMF treats as normal end of play [I]).
