# Nokia 3230 (RM-51): why a key click blocks Wserv, and the reply that ends it

Static reading of `eka1/rm51/output/rom/ROM.bin` (2026-11-19). [V] = read in the code, [I] = inferred.
Follows `docs/research/rm51-audio-e8.md` (DevSound over ISI resource 0xE8, objects 0x55 / 0x56).

## 1. The key-sound server never holds a message [V]

- Client: `AknAnimDll.dll` (0x50989520). Connect 0x509895D4 = CreateSession("KeySoundServer", 1.1.0);
  0x50989604 = `SendReceive(1, {scancode, 0})` (synchronous), called on EEventKeyDown (event type 3,
  0x5098978E) when the anim is connected (+0x18 = 1). Wserv's return address 0x50989615 is this call.
- Server: `EikSrvUi.dll` (0x509E6420). Session ServiceL 0x509E8C44:
  - function 1: reads arg0 byte / arg1, then **`RMessage::Complete(KErrNone)` at once (0x509E8C58)**,
    and only then looks the sound up (0x509E84F4) and plays it (0x509E8A18). No timer, no observer
    callback completes it.
  - other functions (0..11, table 0x509E8CC4) run under a TRAP and are completed with the trap result
    (0x509E8C96); function 11 = CActiveScheduler::Stop.
- Player object (ctor 0x509E92F8, ConstructL 0x509E9388): `CMdaAudioToneUtility::NewL(observer = this+4)`,
  then tone-utility vfunc +0x34 (prepare, argument this+0x24) [V call, I name]. Observer:
  - prepare complete 0x509E92A4: error 0 -> prepared (+0x18) = 1 and set-volume (own vfunc +0x14);
    error != 0 -> nothing (the sound is just never played) [V].
  - play complete 0x509E92BC: playing (+0x14) = 0 for **any** error [V].
  - Play 0x509E93C0: only if prepared; if playing, own vfunc +0x10 (stop) first; tone vfunc +0x1C
    (priority/volume, args +0x0C, +0x10), tone vfunc +0x4C (Play), playing = 1 [V].
- SharedData key `KeyVolume` (RSharedDataClient GetInt into server +0x74, default 3, with NotifyChange)
  sets the volume of the players [V]; nothing makes the server skip playing.

So a blocked SendReceive(1) means the server thread never got back to its scheduler: it is stuck
**synchronously** inside DevSound while handling an earlier key's play. The "idle" stack is
`User::WaitForRequest` (EUser) looping on WaitForAnyRequest, which looks like the scheduler's wait.

## 2. Where DevSound blocks: play end does synchronous 0x56 requests [V]

Msg **0x2B** on object 0x55 -> 0x5078A2F4(owner, -10). In the tone state it does, in this order:
1. if owner+0x104 != 0 (set once the stream started: the 0x21 / data-block path, 0x5078AF68):
   0x5078BCA4 = **`1F 04 {00 00}`** sent from object 0x56 (message class with resource 0x1F, ctor
   0x503743AC; builder 0x503743E0), then receive into owner+0x178 with a local TRequestStatus and
   **User::WaitForRequest (0x5078BD3A)**. Answered by the existing line `reply 1F 04 06 0D 00`.
2. DoControl(audio.ldd, 2) (0x5078A39E).
3. if owner+0x108 != 0: 0x5078B64C(owner, **0**) (resource 0x0B msg 0x19, async because r1 = 0), then
   0x5078BAE4(owner, **1**): two **`0B 19`** requests (resource 0x0B, class ctor 0x5036F450; builder
   0x5036F4E4 with msg 0x19), data `{h, 06, 01, 00, 00, 00}` then `{h, 01, 00, 00, 00, 00}` (h = the byte
   the earlier `0B 00 -> 01` reply gave, owner 0x56 channel +0x54), **each followed by WaitForRequest**
   (0x5078BBAE, then 0x5078BC70). The received message is never parsed: any frame to object 0x56 ends
   each wait.
4. state 0, policy "stopped", observer ToneFinished(-10) -> tone utility -> server's play complete.

**The rm51 table has no `0B 19` line**, so the KeySoundServerThread stays in WaitForRequest at
0x5078BBAE (LR 0x5078BBB3 on its stack) and every later SendReceive(1) from Wserv waits for ever.
The same block happens with `reply E8 20 2B` alone as long as owner+0x108 is set [I: +0x108 set at
initialisation/policy grant, not traced].

`0B 19` is the BB5 / DCT4 audio "get parameter" request; the BB5 family answers `0B 19 1A 00 00`
(`platform/isi/replies-bb5.txt`, `docs/research/n95-key-tone-path.md` section 3) [V there].
The async copies (0x5078B64C with r1 = 0, and 0x5078AC04 at play start) land in the 0x56 RunL
(0x507873A0): resource 0x0B, msg 0x1A is outside its switch (msgs 1..22) and only re-arms [V].

## 3. Other messages in the same sequence [V unless marked]

| request (from) | when | reply needed | read? |
|---|---|---|---|
| `E8 20` (0x55, async) | Play | `21` (stream start) and/or `2B` (play ended, -10) | no field read |
| `E8 23 {b, b, u16 BE len/2, 00 00}` (0x55, async, 0x5078AF60) | data block after start | none needed; `24` = re-arm only (0x50787102) | - |
| `1F 04 {00 00}` (0x56, **sync**) | play end if stream started; Stop | any frame to 0x56 | not parsed |
| `0B 19 {...}` x2 (0x56, **sync**) | play end (0x5078BAE4 r1 = 1) | any frame to 0x56 | not parsed |
| `0B 19 {...}` (0x56, async) | play start (0x5078AC04), play end (0x5078B64C) | optional | ignored by RunL |
| `E8 2A` (0x56 then 0x55, **sync**) | DevSound Stop | any frame to each object | not parsed |

The 0x23 message is a "data written" notification to the modem, not a request for PCM [I from its
fields and async send]; the modem's 0x1A ("send next block", handler 0x50787098) would drive real
streaming, which a click does not need when 0x2B ends the tone.

## 4. Table lines (order of preference)

```
reply 0B 19 1A 00 00      # DevSound play end: 0x5078BAE4 waits synchronously on object 0x56 for each 0B 19 [V 0x5078BBAE/0x5078BC70]; content not parsed
reply 1F 04 06 0D 00      # (already present) play-end / Stop sync wait on 0x56 [V 0x5078BD3A]
reply E8 2A 2B 00 00      # (already present) Stop
reply E8 20 21            # (already present) stream start
reply+ E8 20 2B 00 00     # (already present) tone ends at once (-10): play complete, playing = 0
reply E8 23 24 00 00      # optional: 0x24 only re-arms the receive [V 0x50787102]
```

Check after the change: KeySoundServerThread's stack holds no return address in MMFDevSound
0x50786E60..0x5078E0C0 (0x5078BBB3 / 0x5078BC75 / 0x5078BD3F / 0x5078BE0F) after a key press.

Cheaper fallback if `0B 19` turns out to be shared with another client that needs real fields: reply
`E8 20` with nothing that starts the stream is not enough (step 3 still runs when +0x108 is set);
the alternative that avoids DevSound entirely is to make prepare fail (the server then never plays,
section 1) [I, no reply found that fails prepare cleanly; not pursued].
