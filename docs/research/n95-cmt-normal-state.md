# N95 modem (MCUSW): the 0x0a -> 0x01 (NORMAL) transition, and why no MTC_STATE_INFO_IND arrives

Status: answered (2026-11-21). Program: Ghidra `mcusw_f0100000.bin`. Runtime facts come from
`tools/cmt/probe.py --secs 20` runs (map = tools/cmt/n95-cmt.map + `ssisendat` lines below), end-of-run gdb and
`--gdb-after 1` breakpoint traces. [V] = verified (code read or run), [I] = inferred.

**Result: the transition to NORMAL needs no DSP reply. It completes; the MTC_STATE_INFO_IND {01 0c} is built and
dropped because no APE object has subscribed to resource 0x15 indications. One PN_COMMGR subscription message
(10/10) from the APE makes it arrive.** No fake-DSP lines are needed for this step.

## 1. What the 0x0a -> 0x01 path runs, and where it "stalls"

- MTC server (obj 0x30) gets 15/01 {01 00}. FUN_f0176e62 (state request) stores the target, sends
  `FUN_f0176ac8(0xc0, 3)` (STATE_INFO_IND {01 03}: transition starting) and `(0xc2, 3)`, then arms a delay timer
  (`byte(blk-0x57) * 200`, scaled by FUN_f049be20) and returns [V code]. When it expires (FUN_f0177eca, a
  timer/ack counter at server block +7) FUN_f01780c6 forwards the request to the MTC ctrl task with
  `FUN_f0176ac8(1, state)` [V code; that FUN_f0177eca is the timer path: I]. Measured delay: **2.5 s** (request at
  3.000 s, next activity 5.501 s) [V run]. It is a deliberate wait, not a missing reply.
- Ctrl task (FUN_f0179000 -> FUN_f017917c -> FUN_f0178ab0): current 0x0a != 0, new != 0 -> FUN_f01789f8: stops
  the steps of the old list that are not in the new one (FUN_f01787a8) and (re)creates every step of the new list
  (FUN_f0178988). The list for target 1 is the same as for 0x0a (0..15, 17, 19..25; `+0x18 == +0x1c ==
  0xf04a23e0` in the state block) [V gdb: `step_create` 0..25 hit, no `step_stop`]. No group adds anything new; all
  steps return at once.
- Result `+0x20 = 0` -> state block 0xd8016ed4 = `00 01 01 01` (new = current = target = 1) [V gdb, end of run],
  FUN_f01786e0 sends the internal 15/18 {01} (obj 0x30 -> 0x30) [V], the server's 15/18 handler FUN_f017735a
  (jump table 0xf0177bb3 entry 0x18 -> 0xf0177d3e) sends 15/64 {00 00} (response), then C2 {01 04} and
  **C0 {01 0c}** (state NORMAL, ready) [V run].
- Why the phone never sees it: C0/C2 go through the CM indication path FUN_f0125de4 -> FUN_f0125cd8 ->
  **FUN_f011ec7c** (pn_commgr.c), which sends an indication only to the objects in the subscription table
  (`*(u16*)(0xd800d43c + res*2)` = index into the subscriber list at 0xd800d640, entries `dev<<8 | obj`); with
  no subscriber the message is freed (FUN_f011464a) [V code + gdb: `ind res=15 id=c0 d=01 0c`, nothing sent].
  The 0x0a indication at boot was seen only because the first transition (FUN_f017735a branch "previous state
  byte == 0") also sends a broadcast copy to device 0xff via FUN_f0178bda (lr 0xf0178c47) [V]; later transitions
  do not [V].

## 2. Requests during the transition (no reply needed)

| request (after `12 7c 00`) | sender | waited for? |
|---|---|---|
| `e6 00 06 00 30 01 f0 15 00` (E6/F0 {15 00}) | MTC server, at request time | no [V: transition completes without it; the MTC dispatcher FUN_f0177b1a has no E6 case] |
| `f0 00 06 00 03 00 55 03 05` (F0/55 {03 05}) | obj 0x03 (CS appl) during step re-creation | no [V: ctrl result 0 with no answer] |

After C0 {01 0c} no further DSP request appears in the remaining 14.5 s of the run [V].

## 3. What to add

No `dspreply` lines. The APE side must subscribe to MTC indications, as the real ISA stack does
(PNS_SUBSCRIBED_RESOURCES_IND). Parser FUN_f011ea64 (PN_COMMGR, resource 0x10, receiver object 0 of the modem,
called from the router FUN_f011b902) [V code]:

| id | layout after msg id | effect |
|---|---|---|
| 0x10 | `count`, then `count` x u8 resource | replaces the sender's subscriptions (FUN_f011e9f6 clears, FUN_f011ea3c adds); subscriber = `sender dev (b2) << 8 | sender obj (b7)` |
| 0x12 | `count`, 1 filler byte, then `count` x u32 BE resource (0xe0-extended ids) | same, for 32-bit resource ids (FUN_f011e780) |
| 0x13 | `count`, filler, u32 BE ids (max 4) | stored as a single "other" list (puVar 0xd800...; not needed) |

Probe line (APE peer, length written little-endian as the other `ssisendat` lines):

```
ssisendat 2900 00006c100600001800100115     # 10/10 {01 15}: obj 0x18 subscribes to resource 0x15 (MTC)
ssisendat 3000 00006c150600001801010100     # 15/01 {01 00}: MTC_STATE_REQ NORMAL
```

Result [V run r4]: CMT->APE `15/C0 {01 03}`, `15/C2 {01 03}`, (2.5 s), `15/64 {00 00}`, `15/C2 {01 04}`,
**`15/C0 {01 0c}`**. To get the other servers' indications add their resources to the list (count up to the
message length; e.g. `{03 15 0a 01}` for MTC, NET 0x0a, SIM 0x01 [I resource numbers]).

Linked run: the ISC stand-in already forwards client subscriptions as `KMSubscribe` (iscpeer.inl case 23,
(res, msgid) pairs). Whoever turns that into modem traffic must send one PN_COMMGR 10/10 per subscribing PhoNet
object with the distinct resources of its pairs [I: not checked where KMSubscribe ends on the host side; if it is
dropped there, that is the bug].

## 4. After NORMAL

Nothing more is required for the state itself: no cell-search traffic to the DSP happens on its own within 14 s
[V]. Network search starts when the phone's NET client asks for it (NET server, resource 0x0a) [I]; what the
DSP/L1 must answer for a "no network" result was not researched here.

## Traps

- The 32 kHz counter at 0xffd40490 read from gdb breakpoints moves by a constant per hit (host-time based), so
  it cannot time steps under breakpoints.
- A conditional hbreak on the router send FUN_f011b902 slows the guest so much that the 3 s APE request is lost
  (proxy not yet registered). Use unconditional breakpoints on colder functions, or none.
