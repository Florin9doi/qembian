# N95 modem (CMT): why SIMSON never says "no SIM card" (research, 2026-11-21)

Question: the linked phone asks SIM 09/C0 {00 00} and gets C1 `00 4B 00 FF 00 00` (not ready) instead of
`00 06 00 FF 00 00` (no card). What does the emulator have to add? [V] = seen in code (Ghidra,
mcusw_f0100000.bin) or in a probe run (gdb from reset, 15 s); [I] = inferred.

## Result in short

**The card-absent news is not lost; SIMSON never gets far enough to ask for it.** SIMSON's state machine
stays in **state 1** (its first state) for the whole run. State 1 ignores the card-status event (13) and
leaves only on a start-up signal that never comes. The APDU side and the SIM hardware model are fine. A
Retu interrupt or a register value would not fix it. The missing piece is upstream of SIMSON: the call to
`FUN_f01cc57e` (see below), which no code in the image calls directly.

## What the runs showed [V]

- APDU FSM (`FUN_f01c191e`, a state pattern: the state object's method for the event returns the next state):
  state 0x14 --ev7--> 0x15 --ev8 (`FUN_f01c1774`)--> 0x19 --ev8--> 0x19. The two driver 0x13 posts are the two ev8.
- `FUN_f01c1774` (APDU "no card" entry) ends with `FUN_f01c0bd2(obj, 2)`. **`FUN_f01c0bd2` is `bx lr`**
  (an empty stub in this build), so APDU pushes nothing to SIMSON. Nothing is dropped because nothing is sent.
- SIMSON receives exactly two messages: 0x15c2 (data b2=00 b3=04 -> `FUN_f01cfe10` sets S+0x84 = 1) and 0x15c0
  (data `00 c0 0a 0c ff ff`; its handler `FUN_f01cfef8` acts only when b3 == 3 (and b2 == 0 or 0x0e) -> no-op).
  It never receives 0x4816 / 0x4818, the two messages whose handlers are the no-card setters.
- SIMSON state events: only `ev1` once, from start-up `FUN_f01e33a8` (lr f01e33cb), on the state-1 object
  (state no. 1, d8152e18). No further event in 15 s.

## SIMSON state 1 (vtable read at run time) [V]

| event | slot | handler | effect |
|---|---|---|---|
| 1 | +0x0c | 0xf01e2dee | if `[S+0x7c]+0x0c != 0` -> new state, ctor 0xf01e2708; else stay (our run: stays) |
| 13 (no card) | +0x3c | 0xf01e2098 = `bx lr` | **ignored in state 1** |
| 20 | +0x54 | 0xf01e2094 | -> state ctor 0xf01e244c |
| 21 | +0x58 | 0xf01e2de4 | -> state 17 (ctor 0xf01e318e) |
| other | | 0xf01e2086 | default |
| factory | +0x6c | 0xf01e203a | accepts 09, C0, E4 (so C0 gets the "other state" answer 4B) |

Event table (`FUN_f01e2114`, switch at 0xf01e2128): ev1 +0x0c, ev3 +0x18, ev6 +0x24, ev13 +0x3c, ev20 +0x54,
ev21 +0x58, ev24 +0x64 [V].

Ways out of state 1 [V code, I meaning]:
- `FUN_f01cc57e(obj = S+0x7c, mode)`: stores mode at obj+8. mode 2 -> ev21 (state 17). Otherwise, if obj+9 is
  4 or 5 -> `FUN_f01cc566`: obj+0x0c = 1, ev1 -> state 0xf01e2708. **Not called in our run.** The image has no BL
  to it and no literal pointer to it (whole-image scan), so it is reached through a pointer built at run time
  (a subscription callback) [I].
- ev20 posted at 0xf01c9df8 / 0xf01c9e0e (code Ghidra has not made a function) [V]. 0xf01c9df8 was not hit [V]
  (0xf01c9e0e was not watched).

Who posts the no-card events [V]: message 0x4816 -> `FUN_f01cfc78`: msg byte 4 == 0x16 -> ev24 (0x18), else ev13.
Message 0x4818 -> `FUN_f01cfc08`: byte 4 in {5, 6, 0x0a} -> ev3, byte 4 == 8 -> ev6. These go through SIMSON's
message dispatcher `FUN_f01e0c90` (its message list is at 0xf04b9868: 4818 4816 4819 15c2 15c0 0110 310a 310d 0a02).
Both handlers call `FUN_f01e1d1c(S)` (the no-card flag S+0x87 per n95-isi-encoders.md 9.1) and then post the
event. Neither message was seen. Their sender is not identified; 0x48xx is not the APDU task's driver
code path [I].

## What the emulator must add

Exact change: **not determined within budget.** It is not a Retu or SIM-block change [V: the APDU side has
already reached "no card", state 0x19, and SIMSON never consults it]. The expected real-phone sequence [I]:
1. SIMSON starts in state 1 and gets ev1 (done).
2. A start signal from outside: `FUN_f01cc57e(S+0x7c, mode)` with obj+9 = 4/5 (ev1 again with the flag set ->
   state 0xf01e2708), or mode 2 -> state 17.
3. In the next state SIMSON asks the APDU server to activate the card. APDU (no card) answers with 0x4818
   (byte 4 = 8 or 5/6/0x0a) or 0x4816. That calls `FUN_f01e1d1c` (S+0x87 = no card), then ev6/ev3/ev13 lead
   to state 4, and C0 {00} answers `00 06 00 FF 00 00`.

Most likely cause of the missing step 2 [I]: the system/MTC start-up state. SIMSON's 0x15c0 message carries b3 = 0x0c
where its handler expects 3. obj+9 = 4/5 looks like a system state too. So some server (MTC / power-up
sequencing) has not reached the state in which it calls the SIMSON callback. Next probe (cheap, 1-2 runs):
- `rwatch`/`watch *(char*)(<S+0x7c obj>+9)` and `+8` from reset (obj = `[0xd816f0b8+0x7c]` in this layout;
  S = 0xd816f0b8 from the state object's +4), to find the writer and the registration of the callback.
- Find the sender of 0x15c0 with b3 = 0x0c (break on the message builder) to see which state value 3 is
  missing (possibly the same blocker as the phone-side start-up).

Fallback if the blocker turns out to be deep (a stand-in, not a model) [I]: at the first SIM 09/C0 the probe
machine can force the no-card state with a gdb/QEMU hook: call `FUN_f01e1d1c(S)`, then post ev20 or the
state-4 constructor. Better to fix the missing start signal.

Proposed patch to syborg_cmtprobe.c: none yet. A Retu IRQ (IDR bit 0x0800, event 4 card detect) would
only re-run the driver debounce. That ends in APDU state 0x19 again, which we already reach [V], so it
would not change SIMSON's state.

## Follow-up (2026-11-21, second agent): SIMSON is in state 4, not state 1

[V] = Ghidra (mcusw_f0100000.bin) or a probe run (`tools/cmt/probe.py --secs 15 --gdb-from-start`, map =
`tools/cmt/n95-cmt.map` + `ssisendat 9000 00006c090600000707c00000`); [I] = inferred.

**Correction to the section above.** SIMSON does leave state 1 at boot. The start-up ev1 is the start signal:
- The object at S+0x7c is built by `FUN_f01cc544` (called from the SIMSON set-up at 0xf01e1af8): size 0x14,
  +4 = S, +8 = 0, +9 = 0, **+0x0c = 1**, +0x10 = 0xfb [V]. Nothing writes +8 or +0x0c afterwards (gdb
  watchpoints from the constructor to 15 s: no hit) [V]. `FUN_f01cc57e` is never called (hbreak) [V]; it has
  no BL, BLX or literal pointer in the image (brute-force scan of every halfword) [V]. It is not part of the
  start-up.
- State 1's ev1 handler `FUN_f01e2dee` reads +0x0c through `FUN_f01cc5c6`. Run: flag = 1 [V], so it calls
  `FUN_f01e2062(state, 0xf01e2709)`, the **state-4 constructor `FUN_f01e2708`** (gdb: `ST4-CTOR lr=f01e2073`) [V].
  The earlier "stays in state 1" came from the SMETH hook, which logs the *old* state when an event arrives.
- State 4 entry: `FUN_f01e1bca(S, 0)` -> `FUN_f01e036c(S+0x2c obj, 0)` sends internal message 0xc011 (resource
  byte 0x48) with body `tid be 01 00 00` to the 0x48 server (the APDU/SIM-driver task) and waits for `bf`. The
  reply comes (sender `FUN_f01c4a60`, lr f01c4ab5) and is accepted (`FUN_f01e1bca` returns 1) [V].
- State 4 handlers (from the object; event slots decoded from the switch table at 0xf01e2128, `ldr r1,[r4,#off]`):
  ev13 (+0x3c) = `FUN_f01e26ea` -> state 0xf01e26b5 (or state 17 0xf01e318f if `[S+0x7c]` method returns 5);
  ev14 (+0x40) = `FUN_f01e25e6` -> state 0xf01e3015; ev16 (+0x48) = `FUN_f01e25b4` -> state 0xf01e3077;
  factory +0x6c = 0xf01e2621 [V]. Full event-to-slot map: ev1 0c, 2 14, 3 18, 4 1c, 5 20, 6 24, 7 28, 8 2c,
  9 30, 11 34, 12 38, 13 3c, 14 40, 15 44, 16 48, 18 4c, 19 50, 20 54, 21 58, 22 5c, 23 60, 24 64 [V].
- So C0 answers 4B **in state 4**: SIMSON waits for card news from the 0x48 server [V state, I meaning].

### Who would deliver the card news [V]

The 0x48 server sends its indications through `FUN_f01c4b60(sender, id, len, status, 0, 0)`; callers: 0x16 from
`FUN_f01c17ba` (status 0x16 or 0x46) and 0x18 from `FUN_f01c19ae` (status = server byte +0x73), plus 0x15/0x1a
elsewhere. Both 0x16 and 0x18 are sent only on card-present paths: `FUN_f01c1560` (ATR parsing) and
`FUN_f01c1ca6` (an APDU status word 90/91). The no-card state (APDU state 0x16, built by `FUN_f01c1c22`, entry
`FUN_f01c1774`) only calls the empty `FUN_f01c0bd2` and sends nothing [V]. So with no card the 0x48 server is
silent by design, and the no-card news has to reach SIMSON some other way [I].

### The other way out of state 4: ISI SIM 0xE0 from the APE [V]

ISI SIM request table at 0xf04b94f0 ({req | resp<<8, factory}): `e0/e1 -> FUN_f01d6384`. Its handler
`FUN_f01d6310` reads the message's byte 2 (service): 1 -> ev14 (via `FUN_f01d62a2`; if SIMSON is in state 0x0f it
answers status 0x1c instead), 2 -> ev15 (`FUN_f01d62e4`), else -> ev16 (`FUN_f01d6278`); response status at +3.
Run with `ssisendat 8000 00006c090600000708e00100` (E0 service 1) before the C0: **E1 `01 01`** (accepted), then
C0 still `00 4B` [V]. With service 0: E1 `00 FA` (refused), C0 `00 4B` [V]. So E0 alone does not produce "no card".

### What is still missing (not determined within budget)

- Whether ev14 actually moved SIMSON to state 0xf01e3015 after E0 01 (not traced), and what that state waits for
  [open; next probe: SMETH hook at 0xf01e219c after `ssisendat 8000 ...e00100`].
- How a real phone without a SIM gets SIMSON past state 4. Candidates [I]: (a) a guard timer in state 4 (check
  `FUN_f01e36dc(S+0x58)` / `FUN_f01e3976(S+0x48)`, which ev14/ev16 stop: these look like timers started on entry
  or in state 1; if one is started and never expires, the probe's timer/RTC model is the gap); (b) the 0x48
  server's reply to the `be 01` request carrying the card state (byte 3 `=` or 2 is "ok"; another value might be
  the absent card, but `FUN_f01e2708` ignores the result).
- No Retu/cbus, SIM-block (0x20034100) or intc 0xa3..0xa8 change is indicated by the code read so far: the APDU
  side already reaches its no-card state and that state is silent by design [V].

### Proposed change

None proven. The cheapest working stand-in [I, not tested] is a probe-machine hook that, when SIMSON's state
object with number 4 is current (state no. at +9 == 4), posts the 0x48 server's indication 0x18 with status 8:
the SIMSON handler `FUN_f01cfc08` sets the no-card flag (`FUN_f01e1d1c`) and posts ev6 [V code]. In gdb terms
(for a probe run): `hbreak *0xf01e2736` (end of the state-4 entry), then `call FUN_f01e1d1c(S)` + post ev6
with `FUN_f01e20f8(*(S+0x1c), 6)` and store the result at S+0x1c. Before writing such a hook, trace the two
timers above: if state 4 relies on a timeout, the right fix is in the probe's timer model, not a stand-in.
