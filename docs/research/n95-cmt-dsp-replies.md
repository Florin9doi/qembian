# N95 modem (MCUSW): what MTC's 0 -> 0x0a start-up waits for, and which DSP replies matter

Status: partial (2026-11-21, research budget used up). The blocking point is now known exactly; the DSP
reply layouts are NOT yet known. Program: Ghidra `mcusw_f0100000.bin`. Runtime facts come from
`tools/cmt/probe.py --secs 15 --gdb ... --gdb-cmds` (end-of-run gdb, not from start: see "Traps").

[V] = verified (code read or memory dump), [I] = inferred.

## 1. How the MTC transition runs (mtc_ctrl_t.c + cm_group.c)

- FUN_f0178ab0 (mtc_ctrl_t.c) starts a transition. From current state 0 it calls FUN_f01790e6, which
  takes the step list for the target state, `list = *(0xd8003fc4 + (target-1)*4)`, and for each step id
  calls FUN_f0178988(step) until 0xffff or until a step fails [V].
- FUN_f0178988(step) = `cm_group` create + start: FUN_f0128aee(0, step) then FUN_f0128f7a(group). The group
  object goes into state block `+0x24 + step*4`; `+8` (u16) is the current step index; `+0x20` the
  result [V].
- **Both phases are synchronous inside the MTC task** [V]:
  - FUN_f0128aee: allocates the group object (0x20 bytes, `+0x10` = group descriptor, `+0x14..` one state
    byte per CM), sends COMMGR (resource 0x10) **0x80** to every CM's object (FUN_f0128a66, state byte = 5),
    then FUN_f012899c(obj, 1) loops on the MTC task's receive (FUN_f0112870) until every CM answered **0x81**
    (state byte 1; 0x83 -> 2, 0x86 -> 3, 0x88 -> 4, 0x8a -> 0). A COMMGR response with result byte
    (offset 10) != 0 asserts "pResp->result == CM_MTC_OK". A non-COMMGR message is offered to the hook
    `*DAT_f0128dec`; if the hook refuses it, the group start is abandoned (returns 2) [V].
  - FUN_f0128f7a: 0x82 to every CM, FUN_f0128ed4 waits for **0x83** (result byte 10 == 1 = failure),
    then 0x84 (no answer awaited), group `+4` = 1 [V].
- The CM side (cm_commgr.c, FUN_f0126fce, the 0x80/0x82 handler in each server task): calls every
  registered appl callback (FUN_f012b686(appl, event)); if every appl finished synchronously it replies at
  once (FUN_f0124e04 / FUN_f0125884), otherwise the appl replies later itself (client state +0xe = 2/3) [V
  code; which appls defer: I].

Group descriptors (table 0xf04a2378, `{id, cm list *, nCm}`, CM = `{u16 object, *ids, u8 nIds}`) [V]:

| group | CM objects [CM ids] |
|---|---|
| 0 | 0x16 [2a,28,29,39,30,27], 0x11 [24], 0x13 [2c], 0x12 [2d,2f,3c,3d] |
| 1 | 0x15 [3a,3b], 0x12 [32,38] |
| 2 | 0x14 [2b], 0x0f [37] |
| 3, 4, 7 | empty |
| 5 | 0x0e [36], 0x12 [34], 0x0d [35] |
| 6 | 0x16 [33,68] |
| 8 | 0x18 [43], 0x19 [44] |
| 9 | 0x1f [03], 0x22 [47], 0x1a [25], 0x1b [6b], 0x19 [6a] |
| **10** | **0x17 [3e,69], 0x1c [45]** |
| 11 | 0x1e [41,3f,40,42] |
| 12 | 0x1d [46] |
| 13 | 0x12 [31] |
| 18 | 0x12 [2e] |

Step lists per target state (read at run time from 0xd8003fc4) [V]:

| target | steps |
|---|---|
| 0x08 (the state reached before) | 0, 1, 2, 5 |
| **0x0a** | 0..15, 17, 19..25 |
| 0x04 | 0,1,2,3,5..13,17,18 |
| 0x0d | 0,1,2,3,4,15 |

So the old target 8 never needed groups 6..13; target 0x0a does.

## 2. Where 0 -> 0x0a stops [V]

End-of-run dump at 15 s (state block 0xd8016ed4): new = target = 0x0a, current 0, `+8` = 10,
`+0x24[0..9]` set, `+0x24[10]` = 0, `+0x20` = 0. Group 9's object (0xd80f86c8) has `+4 = 1` (fully started).
Group 10's object (found by searching the heap for its descriptor 0xf04a26e8) is at 0xd80f8758 with
state bytes **05 05**: the 0x80 went to both CMs (object 0x17 CM ids 3e,69; object 0x1c CM id 45) and
**neither answered 0x81**. MTC sits in FUN_f012899c (group 10 create).

Consequences:
- MTC does **not** wait for any reply to its own DSP requests (15/7A, E6/48, E6/F0) for this transition
  [V for the switch: in FUN_f0177b1a, resource 0x15 message ids > 0x19 go to the default case (table
  0xf0177bb3 has 0x1a entries); the only other (resource, id) pairs it dispatches are 0x0000, 0xdc01 and
  0x0a0e (table 0xf04b195c) plus 0x0c/xx, 0x3a/23, 0x77/02. That these requests are irrelevant to the
  transition: I, because groups 0..9 complete without any DSP reply].
- None of the DSP requests come from group 10's CM ids (senders seen: 0x24, 0x29, 0x30, 0x3a, 0x2b (F3/1D),
  0x43, 0x03, 0x4a, 0x4b, 0x25, 0x26) [V, access.log]. So group 10's appls do not wait on a reply to a
  request of their own [I]; they wait either on another local server or on an unsolicited DSP
  indication, or their task is blocked before it reads the 0x80 [I].
- The DSP requests that appear only on the 0x0a path, right after group 9 starts (t = 0.868 s), are the
  candidates [V that they appear; I that they matter]. Their senders are group 9 CMs (0x03 in object 0x1f,
  0x25 in object 0x1a) and non-CM objects 0x4a/0x4b/0x26:

| request (res/id, from obj) | bytes (after media 12, rdev 7c, sdev 00) | note |
|---|---|---|
| F0/0E from 0x03 | `f0 00 06 00 03 00 0e 00 00` | resource 0xF0 [I: DSP common/control] |
| F0/13 from 0x03 | `f0 00 06 00 03 00 13 40 01` | |
| ED/5A from 0x03 | inline MDI entry `ed007c12 03000400 04005a00` | inline (flags 4) |
| ED/0A from 0x4a | `ed 00 0e 00 4a 00 0a 08 38 08 38 00 01 00 02 06 00` | |
| ED/09 from 0x4b | `ed 00 0c 00 4b 00 09 55 55 00 4b 00 4c 55 55` | registers objects 0x4b, 0x4c [I] |
| FD/06 from 0x26 | inline `fd007c12 26000400 04000600` | |
| FD/05 from 0x25 | inline `fd007c12 25000400 04000500` | |

## 3. Reply table: not established

Not reached in budget: the response parsers (message ids, layouts, success values) for resources
0xED, 0xFD, 0xF0, 0xF2, 0xE6, 0x15/7A. The earlier generic "id + 1, status 0" test was rejected with
`f0 01 <id>` [V, earlier run], so blind replies are not an option.

## 4. Next steps (cheap, in this order)

1. **Which of the two group-10 CMs is pending, and on what** [the decisive check]. At the end of a run
   (not `--gdb-from-start`: see Traps), dump the cm_inst of objects 0x17 and 0x1c: the client list
   `param_1[1]`, 0x10 bytes per client, `+0xc` u16 CM id, `+0xe` state (2/3 = appl pending). Or put a
   single hbreak on FUN_f0125884 (the CM response send) with a `$pc`-only print: whichever of 0x17/0x1c
   never reaches it is the one; then `bt`-style walk from its saved stack.
2. Find the task blocked: the appl callback of CM 0x45 / 0x3e / 0x69 (FUN_f012b686's first argument in
   that task) and decompile its create event (0xc012 is the event code seen for create [V from a trace
   of FUN_f012b686 r1]).
3. Only then read the parser of the DSP message it waits for (likely resource 0xED or 0xFD, both new
   on this path [I]).

## 5. Proposed fake-DSP code (skeleton; reply bytes still to be filled)

A table-driven hook for `dsp_drain_m2d()` in `syborg_cmtprobe.c`, covering both long (flags 0x06) and
inline (flags 0x04) messages, so each reply is added as one row once its layout is known:

```c
/* one row per DSP request the MCU needs answered (docs/research/n95-cmt-dsp-replies.md) */
typedef struct {
    uint8_t res, id;                 /* request resource and message id */
    uint8_t rlen;                    /* reply data length (bytes after msg id) */
    uint8_t rid;                     /* reply message id */
    uint8_t data[24];                /* reply data after the message id */
} DspReply;

static const DspReply dsp_replies[] = {
    /* { 0xed, 0x0a, N, 0x0b, { ... } },   [to fill: section 4] */
};

static void dsp_reply(const uint8_t *msg, uint32_t n)        /* msg = media..data, as in the log */
{
    for (unsigned i = 0; i < ARRAY_SIZE(dsp_replies); i++) {
        const DspReply *r = &dsp_replies[i];
        if (n < 10 || msg[3] != r->res || msg[9] != r->id) {
            continue;
        }
        uint8_t b[40];
        uint16_t len = 4 + r->rlen;                           /* robj, sobj, tid, id + data */
        b[0] = 0x12; b[1] = msg[2]; b[2] = 0x7c; b[3] = r->res;
        b[4] = len >> 8; b[5] = len;
        b[6] = msg[7]; b[7] = msg[6]; b[8] = msg[8];          /* objects swapped, transaction echoed */
        b[9] = r->rid;
        memcpy(b + 10, r->data, r->rlen);
        dsp_send(b, 6 + len);
        return;
    }
}
```

Call it from the flags 0x06 branch (`dsp_reply(msg, len)`), and for flags 0x04 rebuild the 12-byte
message from the entry words (`w0` = media/rdev/sdev/res, `w1` = len/robj/sobj, `w2` = tid/id/data0)
and call the same function.

## Traps

- `probe.py --gdb-from-start` with hardware breakpoints shifts guest time a lot (the DSP started at 2.0 s
  instead of 0.565 s, and the run stopped at group 8 instead of group 10): use end-of-run gdb for state,
  and from-start breakpoints only for ordering.
- The response messages 0x81/0x83 are not sent through the router send FUN_f011b902 (only the 0x80/82/84
  requests are seen there) [V]; tracing replies needs another hook (FUN_f0125884).
