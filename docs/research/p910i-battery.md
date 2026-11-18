# P910i: battery level in the status bar

Static reading only (no runs). Tags: [V] read from the code, [I] inferred.
ROM: eka1/p910i/core/ROM.bin, VA = 0x50000000 + offset.

## Answer

The level is measured by the GSM side and sent to the phone as the OSE indication **0xC39C**. Send
it unasked with a level in percent and "not charging":

```
# battery: OSE 0xC39C {level %, charger}: ereq -> batpwr.lsu -> event 0x10202 -> LSBLib (status bar)
# level 0..100 (else dropped), charger byte 0 = not charging, 1 = charging [V encoding, I meaning]
unasked 5000 C39C 64 00
unasked 30000 C39C 64 00
unasked 90000 C39C 64 00
```

The repeats are there because the status bar subscribes late (when the UI is up) and it is not known
whether the event server or eose replays an old value to a late subscriber [I]. A later 0xC39C with
the same level is harmless (LSBLib ignores an unchanged level [V]).

No hardware path: the kernel side (pwrnotif.ldd, EPower) does not carry the level [I: batpwr uses
pwrnotif only through `DoControl(0)` / `DoControl(3)` with the "BatPwrWrapper" active object, and
LSBLib calls no HAL]. No request from the phone asks for the level [V for batpwr: its only
request is ereq 0x508CB4D9 = swbp stub 0x4397, already answered].

## Chain

1. **GSM side -> eose**: frame `00 len 00  00 9C C3  LL CC  00 00`; eose delivers 0xC39C to every
   session subscribed to it (0xC39C is also in eose's 28-signal keep list 0x508A60EC) [V, see
   p910i-gsm-link.md 6.3].
2. **ereq.dll**: `System/Data/Linnea/batpwr.lsu` (code 0x50D74064) builds its own CEReq
   (0x508CCFE9) and subscribes four sub-handlers with its observer at `this+12` (vtable 0x50D75794)
   [V, 0x50D74110]:

   | ereq call | sub-handler | SIGSELECT | signals |
   |---|---|---|---|
   | 0x508CD495 | CEReq+0x3C | 0x508EE4FC | **C39C** C390 C391 C393 C394 |
   | 0x508CD9ED | CEReq+0xA4 | 0x508EE572 | C39B |
   | 0x508CDB11 | CEReq+0x7C | 0x508EE562 | C389 |
   | 0x508CDD4D | CEReq+0xC0 | 0x508EE5AA | C411 |

   The CEReq+0x3C sub-handler (CActive vtable 0x508F14B8, RunL 0x508E823D) decodes with
   0x508E5E3C and calls observer slot +0x30 with (id, err, &decoded); err = 0 if the decode
   succeeded, else 11 [V].
3. **Decoder 0x508E5E3C** [V]: `p = &buffer[4]` (buffer = b0, lenLo, lenHi, payload...), signal =
   LE16 p[0..1], switch on `signal - 0xC390`:

   | signal | decoded.type | other fields |
   |---|---|---|
   | C39C | 0 | `level = p[2]` (DATA[0]), must be <= 100; `charger = p[3]` (DATA[1]) through 0x508CE2C0: 0 -> 1, 1 -> 0, anything else = decode error |
   | C390 | 1 | none |
   | C391 | 2 | none |
   | C393 | 3 | none |
   | C394 | 4 | none |

4. **batpwr observer 0x50D743EC** (via thunk 0x50D753C5) [V], only if err == 0; raises an event
   through eventsvrcli.dll (0x50958981, via 0x50D745A8):

   | type | event uid | 8-byte payload |
   |---|---|---|
   | 0 (C39C) | **0x10202** | byte 0 = level; word at +4 = 1 if DATA[1] == 1, 2 if DATA[1] == 0 |
   | 1 (C390) | 0x10201 | word +8 of a 4-byte buf = 1 |
   | 2 (C391) | 0x10201 | = 2 |
   | 3 (C393) | 0x10205 | = 2 |
   | 4 (C394) | 0x10205 | = 1 |

   0x10201 is also read by soundhandler.dll and LinShell.exe, 0x10205 by soundhandler.dll,
   lcdhandler.asp, bco.app, ConSvr.exe [V: literal search]. Meaning [I]: C390/C391 = charger
   connected / disconnected, C393/C394 = some other battery state (e.g. low battery warning); not
   needed for the level, do not send them.
5. **Status bar: System/Libs/LSBLib.DLL** (used by LSBBATTERY.DLL, the uikon_init status-bar
   control), event handler 0x5074F75C [V]:
   - uid 0x10206 (len 4): `+0x48 = (value == 1)`: selects a coarse 3-step display. 0x10206 comes from
     batpwr's slot-13 observer (0x50D744BC: decoded word 0 -> 2, else 1), on another sub-handler
     (C39B or C411 [I]). Not sent = fine-grained display [I: default 0].
   - uid **0x10202** (len 8): `charging = (word@4 == 1)` -> 0x5074F688 (state 12 if charging, 13 if
     not) [V code, I: 12/13 = charging / not-charging icon]; then the level byte@0 ->
     0x5074F6AC (clamped 0..100, unchanged level ignored): icon step `<=4:1, <=14:2, <=24:3, <=34:4,
     <=44:5, <=54:6, <=64:7, <=74:8, <=84:9, <=94:10, else 11` [V]. Coarse mode 0x5074F724:
     `<=24:1, <=75:6, else 11` [V].
   - uid 0x2D84 (len 8): `+0x4C = (word@4 != 0)` [V], unrelated to batpwr.

## The signal

`0xC39C`, DATA (after the signal number) [V layout]:

| byte | meaning | values |
|---|---|---|
| DATA[0] | battery level | percent, 0..100 (> 100: decode error 11, nothing published) [V] |
| DATA[1] | charger | 0 = not charging, 1 = charging [V mapping to event word 2/1; I which is which: LSBLib takes word 1 as "charging"]; any other value: decode error [V] |

Frame on the link: `00 05 00 00 9C C3 64 00 00 00` (payload[0] and the trailer not read [V for
this path, p910i-gsm-link.md 6.3]).

## Open [I]

- Whether a value raised before LSBLib subscribes is replayed (batpwr calls a Sysagt function
  ported as `SetEventBufferEnabled`, names [I]). Hence the repeats.
- The exact meaning of states 12/13 in LSBLib (charging animation vs plain). If the status bar shows
  a charging icon with DATA[1] = 00, swap to 01.
- C390/C391/C393/C394, C39B, C411 and C389 for batpwr: not decoded further (out of scope).
