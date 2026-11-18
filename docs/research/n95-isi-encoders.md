# N95 (RM-320) modem ISI encoders, read from MCUSW

Source: the modem (CMT, RAP3G) firmware in the N95 8GB core file `RM-320_35.0.001_PR.C0R`, extracted as a
NOR image (docs/research/modem-firmware-in-flash-files.md 2.2). Goal: authoritative layouts of the ISI
responses/indications the Symbian side parses, to check `platform/isi/replies-bb5.txt`.
Marks: [V] = read in the code or bytes, [I] = inferred. Addresses are Ghidra addresses (base below).
No firmware bytes are reproduced beyond short message layouts.

## 0. Image, base address, how to re-open it in Ghidra

| Item | Value |
|---|---|
| File | `build/n95-cmt/MCUSW@00000200.bin` (8 914 112 bytes; file offset 0 = CMT NOR flash offset 0x200) |
| Ghidra program | `/n95cmt/MCUSW@00000200.bin` (owner's project; query name `MCUSW%4000000200.bin`) |
| Language | `ARM:LE:32:v6`, compiler spec `default` |
| Image base | **0xEFD20200** (file offset 0 = 0xEFD20200; NOR offset 0 = 0xEFD20000; the MCUSW partition, NOR 0x3E0000, = **0xF0100000**) [V] |

Base evidence [V]: 3 650 NUL-delimited strings of >= 12 chars; for a random 3 000 of them, the
number whose address (base + file offset) appears as an aligned 32-bit literal anywhere in the image
was 710 for base 0xEFD20200 and <= 49 for every other 64 KiB-aligned candidate (scan of all 65 536).
The partition then starts on a round address (0xF0100000), consistent with a virtual mapping of the NOR.

Strings available: ~300 source-file names (`mtc_srv.c`, `srvsec_srv.c`, `srv_net.c`, `srv_gss.c`, `em_srv.c`,
`gpds_msghandler.c`, `net_sim.c`, `sim_unpack_msg.c`, `time_handler_req.c`, `test_server.c`, `pn_*.c`
...) and assert texts; ISI *message names* for the phone-facing servers are NOT in the image (no
`SIM_..._RESP`, `NET_..._RESP`), so message ids are read from the encoder constants, and names come
from platform/isi/names.txt.

Traps:
- Raw import has no entry points; auto-analysis alone finds almost nothing. Code is ~99 % Thumb: seed
  Thumb functions at `push {..,lr}` halfwords (0xB5xx) with the TMode context register set to 1
  before disassembling (a script did this here: section 0.1). A function created at an even address
  without TMode decodes as ARM garbage: delete it, clear-flow-and-repair, set TMode, re-create.
- 54 % of the image is 0xFF fill (gaps between partitions' code blocks); ignore functions there.

### 0.1 Function seeding done here (repeatable)

`SeedThumb.java` (inline script, run per 1 MiB chunk): for every halfword `0xB5xx` (Thumb `push {..,lr}`)
not already inside an instruction, set TMode=1 on that address, ArmDisassembleCommand(thumb), CreateFunction.
Result: ~16 700 functions. Setting TMode over a whole range fails with ContextChangeException once
auto-analysis has laid ARM instructions somewhere: set it per address. Functions that start without
`push lr` (leaf helpers, e.g. 0xF0127E4C) must be created by hand the same way (`MkThumb.java`).

Other traps:
- Switch statements call an **ARM** helper with BLX (0xF04963D0, `__ARM_switch8`-style): the byte table
  follows the BLX: `[n][off0..off(n-1)][default]`, target = (BLX address + 4) + 2*off. The decompiler
  shows garbage there (`func_0xf04963d0()` then nonsense stores, `software_hlt`); read the table by hand.
- 0xF0158662 / 0xF0158680 are trace calls (`trace(level, id, &arg)`), not senders; 0xF012C8D6 is assert
  (file string, line). 0xF0113C6C is the plain heap alloc.
- `func_0xf012b622(table_or_arg, op, ...)` is an OS service call; op 0xC003 = start a server task with a
  ROM descriptor `{self, ?, ?, ?, ?, {event, handler}...}`; the `0xC009` entry is the ISI-message handler
  (handler(..., msg_ptr_at_tid, (resource<<8)|msg_id)).

## 1. PhoNet plumbing (how a server builds a message)

| Item | Address | What it does | Mark |
|---|---|---|---|
| `CM_MsgAlloc(n)` | 0xF0127E4C | heap-allocates n+8, zeroes the 8 header bytes, writes u16 length = n+2 at +4, tid = 0; returns a pointer to the **transaction id** (ISI byte 8). So `n` = bytes from tid on: data after the message id = n-2 | [V] |
| `CM_MsgAllocZero(n)` | 0xF0127E96 | same, data zeroed | [V] |
| `CM_SendInd(server_ctx, msg)` | 0xF0125DE4 | fills the header: [0] media 0, [1] receiver dev 0, [6] receiver obj 0 (to subscribers), [2] sender dev = ctx dev \| object bits 9..8, [3] resource = ctx resource (low byte; extended resource 0xE0 also writes the 16-bit id into data bytes [10..11]), [7] sender obj = ctx object low byte | [V] |
| `CM_SendResp(req_info, ?, server_ctx, msg)` | 0xF012B644 | through a function pointer (0xF012B5C0) | [V] call, [I] header copied from the request |
| `CM_SendTo(dev/obj, server_ctx, msg)` | 0xF0125FBE | used for requests a server sends to another server (e.g. MTC -> F0 common msgs) | [I] |
| Server context | RAM 0xD80xxxxx | resource at ctx+4 (u32: res \| ext<<8 \| ...), object at ctx+8 | [V] for MTC (0xD8016EC8) |
| Length field | +4 | written as a native (little-endian) u16 = n+2 (= ISI length: bytes after the length field); swapped to big-endian on the link [I] | [V] write |

Every response of a server is therefore `tid, msg_id, data...` built in a CM buffer; the data layouts
below are "bytes after the message id" exactly as `replies-bb5.txt` writes them.

Server tasks registered with op 0xC003 and an explicit resource (sp argument) [V]:

| Resource | Server start | ISI handler (0xC009) | Context |
|---|---|---|---|
| 0x15 MTC | 0xF01760BC | 0xF0177B18 | 0xD8016EC8 |
| 0x09 SIM | 0xF01E327A | 0xF01E368E | - |
| 0x0A NET | 0xF01F214E | (ctx-based) | 0xD801ACB0 |
| 0x08 SEC | 0xF019EF0E | 0xF01A0AE8 | 0xD801839C |
| 0x3A LIGHT | 0xF017A6E2 | 0xF017B6A2 | 0xD80173C4 |
| 0x47 | 0xF019DB7E | 0xF019DD7A | 0xD80182E0 |
| 0x54 | 0xF017ED16 | 0xF017EEBC | 0xD80174A0 |
| 0x1A | 0xF0279846 | 0xF02798EA | 0xD803E654 |
| 0x48 | 0xF01C0572 | 0xF01C06D6 | - |
| 0x81 | 0xF019EC06 | - | 0xD8018370 |
| 0x01 | 0xF03275DE | 0xF032927E | - |
Servers started with resource argument 0 (several resources or set elsewhere): 0xF018A512, 0xF019C564,
0xF01B88A6 (EM area), 0xF01FB50E, 0xF0210CE2, 0xF014505A.

## 2. MTC (0x15), server `mtc_srv.c`

Request dispatch: handler 0xF0177B18, switch on msg id 0x00..0x18 at 0xF0177BAE (table above).
Responses = request + 0x63, all built in a 4-byte CM buffer (tid, id, 2 data bytes) by
`MTC_Send(id, arg)` 0xF0176AC8 [V]. Status codes from the calls [V values, I names]: 00 OK, 01 FAIL,
02 NOT_ALLOWED, 14 NOT_SUPPORTED.

| msg | name | dir | data after msg id | trigger / values | evidence | mark |
|---|---|---|---|---|---|---|
| 0x64 | MTC_STATE_RESP | resp to 01 | `status, 00` | 00 OK / 02 not allowed (0xF017859E, 0xF01785B2) | 0xF0176AC8 path 0x64..0x78 | [V] |
| 0x65 | MTC_STATE_QUERY_RESP | resp to 02 | `current, target` (2 bytes; RAM 0xD8016E6A, 0xD8016E6B) | target = the state also sent in C0 | 0xF0177BD4 -> ac8(0x65) | [V] layout, [I] which is current |
| 0x66 | MTC_POWER_OFF_RESP | resp to 03 | `status, 00` (00 / 01) | 0xF0178508/0xF017853C | | [V] |
| 0x67 | MTC_POWER_ON_RESP | resp to 04 | `status, 00` (00 / 01) | 0xF01784D2/E4 | | [V] |
| 0x68 | resp to 05 | | `status, 00` (00 / 02) | 0xF017842C/3C | | [V] |
| 0x69 | MTC_RESET_GENERATE_RESP | resp to 06 | `status, 00` (00 / 01) | 0xF0177666.. | | [V] |
| 0x6C | resp to 09 | | `status, 00` | 0xF0177C4C | | [V] |
| 0x6E | MTC_STARTUP_SYNQ_RESP | resp to 0B | `status, 00` (00 OK / 01) | 0xF0177548/3A | | [V] |
| 0x6F | resp to 0C | | `00, 00` | 0xF01783D4 | | [V] |
| 0x70 | resp to 0D | | `status, 00` (00 / 02) | 0xF01782CA/D4 | | [V] |
| 0x75 | MTC_SHUTDOWN_SYNC_RESP | resp to 12 | `00, 00` | 0xF0177CFA | | [V] |
| 0x78 | resp to 15 | | `14, 00` (not supported) | 0xF0177D20 | | [V] |
| 0x79 | MTC_RF_STATUS_QUERY_RESP | resp to 16 | `x, rf` : x = RAM 0xD8016E64, rf = 1 when state is 01 NORMAL, 04 TEST or 05 LOCAL, else 0 (0xF0176CAE) | | 0xF0177D26 | [V] layout+rf rule, [I] x |
| 0x7A, 0x7B | resp to 17/18 | | 0x7B: 6 data bytes (CM_MsgAlloc(8), 0xF017745E) | | | [V] size |
| 0xC0 | MTC_STATE_INFO_IND | ind | `state, action` (2 bytes); action 03 START, 04 READY, 0C, 12 seen | sent on every state change (0xF0176E62 sets state then C0 {state,03}); READY later (0xF01773B2..) | 0xF0176AC8 case C0; raw variant 0xF0178BDA (malloc(12), receiver dev 0xFF, sender obj 0x30) | [V] |
| 0xC1 | ind | | `arg>>4, arg&0x0F` | | 0xF0176AC8 | [V] |
| 0xC2 | MTC_RF_STATUS_INFO_IND? | ind | `rf, action` (rf as in 0x79) | sent with C0 when the RF on/off value changes | 0xF0176F2A | [V] layout, [I] name |
| 0xF0 | COMMON (not identified etc.) | resp | `01, req id, 00, ...` (8-byte buffer at 0xF017730A) | unknown request | | [V] shape |

MTC states (values seen in the RF rule; names as in public mtc headers [I]): 00 POWER_OFF, 01 NORMAL,
02 CHARGING, 03 ALARM, 04 TEST, 05 LOCAL, 0A RF_INACTIVE, 0E SW_RESET.

replies-bb5.txt / modem core check (MTC):
- `reply 15 16 79 00 00` - shape right (2 bytes) [V]; but in NORMAL the real modem answers rf = **01**
  in the second byte: `79 xx 01`. The first byte is a RAM value not identified. Suggest `00 01` [V rf rule].
- syborg_modem_core.c 02 -> 65 `{s, s, 00, 00}`: the real data is 2 bytes `{current, target}`; the extra
  two zero bytes are padding the phone does not read [I]. OK.
- 01 -> 64 `{00 00}`, 0B -> 6E, 03 -> 66, 04 -> 67, 06 -> 69, 12 -> 75 `{00 00}`: match [V] (status 00 OK).
- C0 `{01, 03}` (NORMAL, START) as sent by the modem core: matches [V]; the real modem follows with
  `{01, 04}` (READY) when the transition completes [V calls with action 4].

## 3. SEC (0x08), server `srvsec_srv.c`

Request dispatch: handler 0xF01A0AE8, compares `(res<<8)|id` against constants (base 0x833) [V]:
01->0xF01A0404, 04->0xF01A0990 [I], 07->0xF01A0124, 0A->0xF01A0758, 0D->0xF01A08C4, 11->0xF01A1E52,
13->0xF01A30E8, 15->0xF01A300E, 17->0xF01A12A4, 1B->0xF01A122A, 1F->0xF01A2162, 21->0xF01A2222,
23->0xF01A230C, 25->0xF01A23DC, 29->0xF01A413C, 2B->0xF01A423E, 2D->0xF01A2590, 2F->0xF01A296A,
31->0xF01A25D2, 33->0xF01A2EDE, 34..3D via a switch8 table, 3E->0xF01A4784, 40->0xF01A4824,
51->0xF01A591A, 53->0xF01A3822, F0 (common). **No 0x67 handler**: the 9.3 `08 67` (lock info) is a
later modem's message; this modem answers it through the default path (F0 not-identified) [V absence, I effect].
It also takes PN_SIM responses as a client: 09 1E (READ_IMSI resp), 09 3C (IMSI equal resp), 09 C1
(SIM_STATUS resp), 09 04/06/08 [V], i.e. at start-up SEC sends SIM 1D {2D} (0xF01A1F34), SIM C0 {01}
(0xF01A1F8C), SIM 3B {FA} (0xF01A1DD6) [V].

Packers [V]: `pack4(id, s)` 0xF01A14E6 -> `{s[2], 00}`; `pack4z(id)` 0xF01A15A8 -> `{00, 00}`;
`pack8(id, s)` 0xF01A1838 -> `{s[2], s[3], s[4], 00, 00, 00}`; `pack(0x10, s)` 0xF01A17B8 (indication 0x10).

| msg | name | dir | data after msg id | trigger / values | evidence | mark |
|---|---|---|---|---|---|---|
| 0x02 | SEC_CODE_STATE_OK_RESP | resp to 01 | `x, 00` with x = request byte 11 echoed (the new/queried setting) | request `{type, x}` (2 bytes, read as u16 BE): type 06 (and 01/03/05) answered at once after a local check; types 02 (PIN), 04, 07, 09 go to the SIM first (SIM 05/09 requests) and answer later | 0xF01A0404 | [V] |
| 0x03 | SEC_CODE_STATE_FAIL_RESP | resp to 01 | `cause, 00` [I] | local check failed (0xF01A0606 path) | 0xF01A0724 | [I] |
| 0x10 | SEC_..._IND | ind | 8 bytes `{06, 01/06, 0B/0C, 63, 63, ...}` [V bytes, I meaning: lock on/off event] | after 0x02 for type 06 | 0xF01A06FC | [V] shape |
| 0x12 | SEC_STATE_RESP | resp to 11 | `state, value, flag, 00, 00, 00` (6 bytes); state: 05 = startup OK (value 0x16 when no code query is pending, else ctx+0x0C), 02, 03, 01, 06, 2C, 2D, 07 = code-required states from ctx flags; request byte 12 = 0x1E -> state 00, value ctx+0x77 | 0xF01A1E52 | [V] layout, [I] state names |

replies-bb5.txt / modem core check (SEC):
- `reply 08 01 02 00 00` - [V] shape `{x, 00}`; x is the echo of request byte 11, so 00 is right when
  the TSY asks with x = 0 [I: the N95 request bytes not re-read here].
- modem core `08 11 -> 12 {05 00 00}`: real is 6 bytes `{05, 16, flag, 00 00 00}` in the no-code-pending
  case [V layout]; byte 11 = 0x16 rather than 00. The 9.3 SAStartupService reads [10],[11],[12]: if it
  ever checks [11], use 0x16 [I].
- `reply 08 67 68 01 00 00 00 00` (9.3 phones): not in this modem (no 0x67 handler) - cannot be checked here.

## 4. NET (0x0A), server `srv_net.c` / `cs_net.c` (partial)

Started at 0xF01F214E with resource 0x0A, context 0xD801ACB0; its event table is built in RAM, and the
server body is one large generated state machine (0xF01FB8D4..0xF01FEC9C; the decompiler merges it into
one function with dozens of "PIC construction" warnings). The response encoders for 00 -> 01
(REG_STATUS_GET), 0B -> 0C (RSSI) and 36 -> 37 (RAT) were **not found** by constant search: no
`movs #0x37; strb [x,#1]` exists anywhere in 0xF01E8000..0xF0220000, so the message id is computed
(request id + 1) or comes from a table [V absence]. A response with id 0x01 built at 0xF01FD708
(CM_MsgAllocZero(8): `{01, 00, ...}`) is one candidate [I]. Open point; replies-bb5.txt NET lines
remain unverified by this pass.
ISI entry [V]: ROM descriptor 0xF04B9E68 (copied to RAM by 0xF01F2090), `0xC009` handler **0xF01F3130**:
it stores the message in ctx (0xD801AC28 +0x54) and jumps through a per-state halfword table (SDL style,
"Could not recover jumptable"); state number at 0xD8004796 (u16). Next step: read that table by hand
(base -0xFE0CE5A relative) and follow the state handlers for message 00 / 0B / 36.

### 4.1 NET (run 2): the relay, the sub-block builders, the start-up replies

Correction to the paragraph above [V]: 0xF01F3130 is the NET server's own `0xC009` handler (descriptor 0xF04B9E68:
`{0xC009, 0xF01F3131}`); it stores the message at ctx+0x54 (ctx = 0xD801AC28), the tid at ctx+0x0C,
`(res<<8)|id` at ctx+0x58, sets r7 = 0, r5 = 1, and jumps on the state (s16 at 0xD8004796, 15 states) through
the halfword table at **0xF01F31A4** (target = 0xF01F31A6 + 2*entry; states 0..14 -> 0xF01F39FC, 3A6A, 3EEE,
3F92, 4564, 45D2, 3FF2, 45FC, 460C, 461E, 463C, 465C, 467C, 46CE, 470A). Requests that a state does not take
fall into a common block (0xF01F32E4..0xF01F38C0) that compares `(res<<8)|id` with constants (0xA3D +/- k).
The server is a **relay**: it does not compute registration data. The protocol stack's NET layer (0xF01E6xxx..
0xF01EFxxx, "cs_net") builds the ISI sub-blocks itself from ROM templates and posts them to the relay as
internal messages (`0xF01EFB50(id, 0)`, then `0xF011194A(0x10, msg)`; internal msg: u16 id at +2, nsb at +5,
u16 length at +6, pointer to the sub-blocks at +8) [V]. The relay copies them after `status, nsb`.

Message builder [V]: `NET_New(tid, id, n)` 0xF01F219A = CM_MsgAlloc(n), zero it, [0] = tid, [1] = id; data
after the msg id = n - 2 bytes, all zero unless written.

Sub-block template table at **0xF04B9A3C** (8 bytes each: `id, len, nfields, 0, fieldlist*`) [V]:
`00/8/7 fields`, `09/0x18/14`, `01/8/6`, `0B/8/4`, `0E/0x14/3`, `3D/4/3`, `3E/0x0C/6`, `0F/4/1`. Map tables
(`{in, out}...` pairs ending `FF FF`, then `{1, default}` or `{0, -}`; lookup 0xF01EE89A) [V].

Sub-block 0x00 NET_REG_INFO_COMMON (builder 0xF01EEAA6, 8 bytes when no operator names) [V]:
| off | value | source |
|---|---|---|
| 0, 1 | `00 08` | template |
| 2 | registration status: internal -> ISI by table 0xF04B9AD4: 0->00, 1->01, 2->03, 3->04, 4->05, **5->06**, 6->07, 7->08, 8->09, 9->0A, 0A->0B; default 03 | [V] map; ISI names (00 HOME, 01 ROAM, 03 NOSERV, 04 NOSERV_SEARCHING, 05 NOSERV_NOTSEARCHING, 06 NOSERV_NOSIM, 08 POWER_OFF, 09 NSPS, 0A NSPS_NO_COVERAGE, 0B SIM_REJECTED) [I, public naming] |
| 3 | network selection mode: internal 0 -> **02**, 1 -> 01, else 00 (table 0xF04B9A7C) | [V] map; 02 AUTOMATIC / 01 MANUAL [I] |
| 4 | number of operator-name strings that follow (0..2) | [V] |
| 5 | 00 | [V] |
| 6, 7 | 00 00 when no names (field type 0x13: `00` + name-length 0) | [V] |
With operator names (internal flag +0 == 0, operator code known) the block grows by the UCS-2 names
(`{01, nchars, UCS-2...}` per name, at most 0x28 chars, total <= 0xFC; 0xF01EEA0A) [V].

Sub-block 0x09 NET_GSM_REG_INFO (0xF01EE8FC, 24 bytes, field list `8,7,9,10,11,13,14,15,16,17,23,24,25,26`) [V]:
`09 18`, [2..3] u16 LAC (internal +6), [4..7] u32 cell id (+8), [8..10] operator code (3 BCD bytes, +3),
[11] band flags (+0x0C: bit-coded -> 0/1/2), [12] map(+0x12; table 0xF04B9AC6: 0..4 -> same, default 03),
[13] (+0x0D), [14] map(+0x0F: 0->01, 1->02, 2->03, default 00), [15] (+0x13), [16] (+0x14), [17] (+0x0E),
[18] (+0x10), [19] (+0x11), [20] 00, [21] 00, [22..23] 00 (fill). Field meanings (GPRS support/mode, EGPRS,
DTM, HSDPA/HSUPA, ...) [I]; the TSY reads [13..15] as packet capability (syborg_modem_core.c comment).
It is present unless the internal status is 9 (NSPS_NO_COVERAGE): **so even with no SIM the 0x01/0x02 messages
carry nsb = 2 (sb 00 then sb 09)** [V 0xF01EEC68]. A third sb 0x0F (`0F 04 v 00`) follows when the cell
flags +0x00 == 1 and +0x15 == 1 [V].

Who sets the internal status [V calls, I which event]: 0xF01E6D50(status) builds a "no service" update
(flag +0 = 1, selection mode from 0xD801AAC8[1]); its callers pass 2, 3, 4, **5 (three call sites after
`movs r0,#0x0A` cause, 0xF01E8E60/8F4E/8FBE: the no-SIM path [I])**, 6, 7, 8; 0xF01EF9A8 posts status 3
(-> ISI 04 SEARCHING). Before the stack's first report the relay's own blob (ctx+0x68, 0xF01F2090) is
`00 08 03 00 00 00 00 00`, nsb 1 (NOSERV) [V].

| msg | name | dir | data after msg id | trigger / values | evidence | mark |
|---|---|---|---|---|---|---|
| 0x01 | NET_MODEM_REG_STATUS_GET_RESP | resp to 00 | `00, nsb, sb...` = the cached blob (ctx+0x68) + operator-name sbs from the relay's cache (if any) + optional extra sb (ctx+0x38). Before the stack reports: `00 01 00 08 03 00 00 00 00 00`. No SIM [I status]: `00 02 00 08 06 02 00 00 00 00 09 18 <22 bytes>`. Post-fix: if sb 00 and sb 09 are both present and 0xF01F235E(status) holds, status is rewritten to 00 | 0xF01F3604 -> 0xF01F4D78 -> 0xF01F2A5E | [V] layout, [I] the exact no-SIM status (06) |
| 0x02 | NET_MODEM_REG_STATUS_IND | ind | same as 0x01: `00, nsb, sb...` (tid 0) | sent when the stack's internal msg 0x01/0x12/0x17 arrives and the content changed (flag from 0xF01F2A5E) | 0xF01F3E32 -> 0xF01F4BBE | [V] |
| 0x0C | NET_RSSI_GET_RESP | resp to 0B | `00, 01, 04 04 a b` (6 bytes) with a = ctx+0x17, b = ctx+0x18 (the last NET_RSSI_IND values, 0 0 at start) | only when request byte 11 (`[3]`) == 02 (current cell); other values fall to the default reply | 0xF01F35A6..35D6 | [V] layout (r7 = 0, r5 = 1 set in the handler prologue) |
| 0x1E | NET_RSSI_IND | ind | `a, b` (2 bytes) from the stack (internal msg 8); cached for 0x0C | | 0xF01F3DC6 | [V] |
| 0x37 | NET_RAT_RESP | resp to 36 | request [2] (ISI byte 10) = 0 -> cached RAT blob ctx+0x78: `00, nsb, sb...`; = 1 -> blob ctx+0x80; blob empty (nothing reported yet) -> `02 00` | | 0xF01F3488 -> 0xF01F4DF6 | [V] |
| 0x35 | NET_RAT_IND | ind | `00, nsb, sb..., 00` (one trailing pad byte); the same blob is cached at ctx+0x78 | stack internal msg 0x19 | 0xF01F3C10 -> 0xF01F4CD8 | [V] |
| (sb) 0x2C | NET_RAT_INFO | in 35/37 | 8 bytes: UMTS `2C 08 02 00 00 00 00 00`; GSM `2C 08 01 01 00 00 00 00` | posted with nsb 1 | 0xF01EF884 / 0xF01EF8BA | [V] |
| 0x2C | (ind) | ind | `00, nsb, sb...` copy of an internal blob, also cached at ctx+0x3C (used by 0x2D -> 0x2E) | stack internal msg 0x16 | 0xF01F4D2A | [V] shape |
| 0x2E | resp to 2D | resp | cached ctx+0x3C: `00, 01, <blob>`; none: `0F 00` | | 0xF01F31C2 | [V] |
| 0x08 | NET_SET_RESP | resp to 07 | `x, 00` (x = 02 when ctx+0x15 is 1 or 4, else 00) | | 0xF01F34F8 | [V] shape, [I] meaning |
| 0x06 / 0x0A | resp to 05 / 09 | resp | `0A` (1 byte) | | 0xF01F3558 | [V] |
| 0x10 | resp to 0F | resp | (no data) | | 0xF01F3612 | [V] |
| 0x3B | resp to 3A | resp | `00, ctx+4, ...` (3 data bytes) | | 0xF01F333C | [V] shape |
| 0xF0 | COMMON_MESSAGE (not supported) | resp to any other id, e.g. **40** | `01, req_id, 00, req[2] (only if req is F0/F1/F2), 00, 00` (6 bytes) | the relay has no 0x40 handler (constants in the dispatch: 00, 03, 05, 07, 09, 0B, 0F, 28, 2D, 36, 38, 3A, 3D) | 0xF01F2988 | [V] builder, [I] that no state takes 40 |

Not traced: which of the 15 states let 00/0B/36 through to the common block (a request that arrives in a
state that defers it is answered later); the stack's exact internal status in the "no SIM, emergency camping"
case (5 -> 06 is the best match: three call sites with cause 0x0A) [I].

replies-bb5.txt / modem core check (NET, PN_NETWORK 0x0A):
- `reply 0A 0B 0C 00 01 04 04 00 00`: **matches** [V] (`00 01 04 04 a b`, values 0 until an RSSI IND).
- `reply 0A 36 37 00 01 2C 04 01 00` and core `{00 01 2C 04 rat 00}`: **differs** [V]: sb 0x2C is 8 bytes:
  GSM `00 01 2C 08 01 01 00 00 00 00`, UMTS `00 01 2C 08 02 00 00 00 00 00`. Parsers that read [12] only are
  unaffected; a length check (or an iterator using len 4) sees a different block. Before the stack's first RAT
  report the real answer is `02 00`.
- core 0A 00 -> 01 (+ 02) with SIM: `00 02 | 00 04 00 02 | 09 18 ...`: sb 00 is **8 bytes** in the real modem
  (`00 08 st 02 00 00 00 00`); the core's sb 0x09 (24 bytes) has the real length and LAC/CI/PLMN offsets [V].
  [13],[14],[15] = 1,1,1: [14] in the real modem is a mapped value (01..03) so 1 is a legal value [V map].
- `reply C8 00 01 00 01 00 04 06 00` (Symbian^3 PN_MODEM_NETWORK, another modem): not this modem; if it is
  reused as the N95's no-SIM 0A 00 answer, the real N95 shape is `00 02 00 08 06 02 00 00 00 00 09 18 + 22`
  [V shape, I status 06], or `00 01 00 08 03 00 00 00 00 00` before the stack's first report [V].
- `reply 0A 40 41 01 00 00`: **differs** [V builder / I state coverage]: this modem answers 0A 40 with
  `F0 {01 40 00 00 00 00}` (not supported), never 41.
- `reply 0A E0 E1 ...` (9.3 TSY): no 0xE0 handler in this relay (later modem) - unverifiable here.

## 5. EM / "GSS old id" (0x17), server `em_srv.c`

Started at 0xF01B88A6 (context 0xD801A224), ISI handler 0xF01B92F0: compares `(res<<8)|id` with 0x1715
etc., so this server is resource **0x17** [V]. Requests: 00 -> 0xF01B9CFE, 02 -> 0xF01B91D8, 04 ->
0xF01B9C60, 06 -> 0xF01B90EA, 08..13 via switch8 at 0xF01B9380 (index = id - 8: 08 inline 0xF01B946E,
0A -> 0xF01B9BD4, 0C -> 0xF01B06D2, 0F -> 0xF01B9B58, 11 -> 0xF01B9ADC, 13 -> 0xF01B0F6A), 15 -> 0xF01B102A,
24 -> 0xF01B8EB6, 35 -> 0xF01B8B20; it also receives MTC 64/66 responses (0x1564, 0x1566) [V].

Common shape [V]: request `{mask, 00}`; response id = request id + 1, built by 0xF01B99E2:
`01 (OK), nsb, sub-blocks...`, one 4-byte sub-block `{id, 04, b2, b3}` per set mask bit, in bit order
(helpers 0xF01B99CC `{id,04,v,00}`, 0xF01B99B4 `{id,04,u16 BE}`, 0xF01B9BBE `{id,04,a,b}`).
Mask 0 or an unknown bit -> `02, 00` (failure, 0xF01B9996).

| msg | dir | data after msg id | sub-blocks (mask bit -> sb) | evidence | mark |
|---|---|---|---|---|---|
| 0x01 | resp to 00 | `01, nsb, sb...` | b0 -> `00 04 v 00` (v = 0xF01B250E), b1 -> `01 04 v 00` (0xF01B2514), b2 -> `02 04 u16` (0xF01B51E6(5)), b3 -> `03 04 u16` (0 when MTC state is 4/5/8) | 0xF01B9CFE | [V] |
| 0x09 | resp to 08 | same | b0 -> `13 04 v 00` (0xF01B3F8A), b1 -> `14 04 v 00` (0xF01B36B0(0)) | 0xF01B946E | [V] |
| 0x0B | resp to 0A | same | b0 -> `15 04 v 00` (charger, 0xF01B0844), b1 -> `16 04 07 bars` (07 = max, bars from 0xF01B0874), b2 -> `17 04 u16`, b3 -> `18 04 u16`, b4 -> `25 04 FF FF` | 0xF01B9BD4 | [V] |
| 0x10 / 0x12 | resp to 0F / 11 | `01, 00` (CM_MsgAlloc(8)) | - | 0xF01B9B62, 0xF01B9AE6 | [V] |
| 0x0E | ind | `60 01 ...` (8/12-byte forms) | - | 0xF01B97B6.. | [V] shape |
| 0x02 | failure for any | `02, 00` | - | 0xF01B9996 | [V] |

replies-bb5.txt check (0x17):
- `reply 17 00 01 01 01 00 04 02 00` - [V] shape for mask 0x01. The N95 TSY sends `17 00 {03 00}`
  (n95-isc-isi-protocol-2.md 3.1.4): the real modem then answers **two** sub-blocks:
  `01 02 00 04 v 00 01 04 v 00`. The table's one-sub-block answer is a subset [V]; fine if the parser
  looks sub-blocks up by id, wrong if it expects nsb = 2 [I].
- `reply 17 0A 0B 01 02 16 04 07 07 15 04 01 00` - [V] sub-block contents (`16 04 07 bars`,
  `15 04 v 00`). Real order is by mask bit: sb 0x15 before 0x16, and only the requested ones (the TSY's
  `{02 00}` gets just sb 0x16, nsb 1). Harmless if parsers search by id [I].
- `reply 17 08 09 01 01 13 04 01 00` - [V] shape for mask 0x01 (sb 0x13).

## 6. INFO / phone info (0x1B)

ISI handler 0xF0153082 (the server also serves 0x35 and 0x62 messages: 0x3503, 0x6201 constants) [V].
Dispatch on `(res<<8)|id` [V]: 1B00..1B14 via switch8; 15 -> 0xF0155BEC; 17 -> 0xF0152AB6; 23 -> 0xF0152B5A /
0xF0152F5A; 29 -> 0xF01543B4 (request byte 10 == 1) else 0xF015425C; 2B -> 0xF0154440; 2E -> 0xF0155A54;
30 -> 0xF0155AF8; F0 -> 0xF0153500. Responses go out through CM_SendResp (0xF012B644).

| msg | name | dir | data after msg id | trigger / values | evidence | mark |
|---|---|---|---|---|---|---|
| 0x2A | INFO_PP_READ_RESP | resp to 29 | **echo form** (request [10] != 1): `status, nsb, <request's sub-blocks with values filled>`; sub-block 0x74 = `74, x, len16 BE, x, x, count16 BE, {feature16 BE, value16 BE} * count`; status 00 OK, 01 = a feature unknown (then the message is cut to `01, n`) | request `{mode, nsb, sb 74...}`, every sb must be 0x74 | 0xF015425C | [V] |
| 0x2A | (same) | resp to 29 with [10] = 1 | **read-all form**: `00, 01, 74 00 03 84 01 00 00 DF` + 223 entries `{feature16, value16}` (902-byte message) | | 0xF01543B4 | [V] |
| 0x2F | resp to 2E (table: "product profile bits") | resp | `status, 01, F0, 00, 88, 00, u32 BE flags, 128-byte PP block` = **138 data bytes, 148 on the wire** (CM_MsgAllocZero(0x8C) writes the ISI length n+2 and nothing shortens it: the encoder only fills, CM_SendResp 0xF012B644 forwards the message). Fill `FUN_f01559a6`: flags u32 (NV record 0x4000F, default 3) at [16..19], then 128 bytes; the first two words (at [20..23], [24..27]) byte-swapped to BE; default block 0xF04ABB2C = words 0x0F, 0x01, rest 0. Status 00 when the read succeeds, else 01 and a 4-byte `01 00` message | | 0xF0155A54, called from 0xF01530F8 for 1B2E | [V] layout and length, [I] the NV record's contents on a real phone |

replies-bb5.txt / modem core check (INFO):
- modem core `1B 29 -> 2A` "echo the request's sub-blocks (values 0)": matches the real echo form [V];
  real values come from the product-profile table (0 = feature off) [I].
- `reply 1B 2E 2F len=16 00 01 F0 00 00 10` (before 2026-11-18): **differed** [V]: the real sub-block is id16 `F0 00`,
  **len8 0x88 at [14]**, filler 00 at [15]; the old line had the length in the filler (`00 10`).
- Phone side [V] (2026-11-18): SAFeatureInfoService `FUN_8092313e` (SAServer plugin) finds sub-block 0xF000 with
  ISIMESSAGE `ord18(msg, 12, 0xF000, type 3)`; type 3 = id16 + len8 at +2 (ISIMESSAGE.DLL 0x808563c8 switch: type 0
  id8/len8, 2 id8/len16 at +2, 3 id16/len8 at +2, 4 id16/len16 at +2); it reads `[10]` status == 0, `[11]` nsb != 0,
  flags u32 at sb+4 ([16..19]); flag bit 0 and u32 at sb+8 ([20..23]) != 0 -> variant feature 3; bit 1 and u32 at
  sb+12 ([24..27]) != 0 -> feature 4. Nothing past [27] is read. SAServer's receive (SAIscService) starts with a
  128-byte buffer and grows to the needed length on KErrNoMemory (`FUN_809120c2`), so the full 148-byte message is
  accepted on a real driver (and on ours since H-isc-rx-overflow: N95 session, idle at 15.3 s).
- **Table now** [V layout, I values]: `reply 1B 2E 2F len=18 00 01 F0 00 10 00` = the real layout cut to the whole
  16-byte sub-block the phone reads (len8 = 0x10), flags 0 (features 3 and 4 off, as before; the modem's factory
  default would be flags 3 with words 0x0F / 0x01, i.e. both on: kept off until a feature is known to need them).
  Within the 118-byte cap of `tests/isi_replies_test.py`.

## 7. CALL (0x01): emergency number check

Encoder 0xF0293DCA (gsm_call_timers.c area; the PN_CALL server) [V]: looks the number up
(`0xF0291118(4, 0x28, req+4)`), then CM_MsgAllocZero(len + 12) and sends via CM_SendResp.

| msg | name | dir | data after msg id | evidence | mark |
|---|---|---|---|---|---|
| 0x29 | CALL_EMERGENCY_NBR_CHECK_RESP | resp to 28 | not an emergency number: `00, 00` + 8 zero bytes (12-byte CM buffer); emergency: `01, nsb, sb 03 {03, len, 01, 00, 00, ndigits, UCS-2 digits...}` (+ an optional second sub-block copied from the number table, nsb incremented) | 0xF0293DCA | [V] |

replies-bb5.txt check: `reply 01 28 29 00 00` - matches [V] (status 00 = not emergency, nsb 0; the real
message carries 8 more zero bytes of padding).

## 8. GPDS (0x31), `gpds_msghandler.c` (partial)

| msg | name | dir | data after msg id | evidence | mark |
|---|---|---|---|---|---|
| 0x1A | GPDS_STATUS_RESP | resp (to 19 [I]) | `attach, nctx, u32 BE, u32 BE, b20, b21, cid * nctx` (12 + nctx bytes); attach = ctx+0x4B, b20 = ctx+0x4C, b21 = ctx+0x4D (RAM 0xD803E9E8 block) | 0xF02A0876 (request cid field 0xFE = all) | [V] layout, [I] counters = tx/rx bytes |
| 0x23 | per-context status | resp | `cid, nsb, u32 BE, u32 BE, ... [sb 04 {04, len, 00, n, data}]` | 0xF02A0876 (single cid) | [V] |
| others built here | 03, 07, 0C, 21, 34 (`x, 00`, 4-byte buffers), 01 `{x, 01}`, 2B, 2C (sub-blocks 03 08..), 09/0A/0D/1F/25 (context inds with sb 04/05) | | 0xF02A0456..0xF02A0FA2 | [V] ids/sizes only |

replies-bb5.txt check: `reply 31 19 1A 00 00 00 00 00 00 00 00 00 00 00 03` - 12 bytes = the real length
with nctx 0 [V]; [10] attach 00, [20] 00, [21] 03 (from an N8 capture; on this modem [21] is ctx+0x4D, value
not read here) [I]. `31 30 -> 31` (configuration info) was not located (no constant 0x31 store in
gpds_msghandler.c) - open.

## 9. SIM (0x09) (partial)

Started at 0xF01E327A (resource 0x09), ISI handler 0xF01E368E -> 0xF01E355E: requests are unpacked
(`sim_unpack_msg.c`, 0xF01BF954 / 0xF01BF9A4) into command objects created through RAM function pointers
(ctx+0x1C -> +0x68/+0x6C) and run through their vtables (+0x2C, +0x30). No response message id is a
constant in the SIM area (search for `movs #id; strb [x,#1]` and `id+1` patterns found only unrelated
code), so responses are built generically from the request [V absence, I generic].
What was read [V]: a response builder (0xF01D4638) writes byte 11 (status) = **0x01 OK** or **0x24
(data not available)** after byte 10 (service type) - the `{service, status, ...}` shape the tables use.
SEC is a SIM client at start-up: it sends 09 1D {2D 00} (READ_IMSI), 09 C0 {01} (SIM_STATUS, service 01),
09 3B {FA} (IMSI equal) and handles 09 1E / 09 C1 / 09 3C answers.
Indications sent from this area (CM_SendInd, 8-byte buffers): ids 0xEE `{63, 3C/37/35/3B/39/3A, 06/01/05/08/03, 01}`
and 0xEF `{0D 0C / 05 07 / 02 02 / 96 97 / 97 04 ...}`, 0xE8 `{FA 61}` (0xF01E230A.., 0xF01CC9AE..) [V bytes,
meaning not read].

replies-bb5.txt check: `reply 09 19 1A 2F 24` - shape `{svc, 24}` consistent [V status value 0x24 exists];
the no-SIM answers in syborg_modem_core.c (`{svc, FB}`, from an N8 capture) cannot be checked against this
modem without the command objects - open.

### 9.1 SIM (run 2): state objects, command table, start-up replies

Design [V]: the SIM server (ctx at handler r0; ctx+0x0C = the server object S) keeps a **state object** at
S+0x1C (0x74 bytes; state number at +9; event methods at +0x0C..+0x5C dispatched by 0xF01E2114; request
factory at **+0x6C**, a second hook at +0x68). A request `(res<<8)|id` in the server's range goes:
0xF01BF954 (unpack; table 0xF04B7BB4 only for a few special ids, others generic) -> S+0x1C ->+0x6C
(factory) -> command object -> `+0x2C` execute (0xF01D0154 for full commands) -> response via +0x3C builder
and CM-style buffer 0xF01BFA6A(n) (returns a pointer to the tid; n = tid..end, so data after id = n-2).
Factories by state (ctor -> state no. -> factory) [V]:

| state | ctor | factory | accepts (others -> refusal) |
|---|---|---|---|
| 2 | 0xF01E2284 | 0xF01E2206 | 1F, 01, 05, 07, 3D, 73, D4 {01 01 0083}; else as state 5 |
| 4 | 0xF01E26B4 | 0xF01E2620 | E0; else 09, C0, E4 (0xF01E203A); else refusal FB |
| 5 | 0xF01E281C (base) | 0xF01E2794 | DA, 09, C0, DE, E0, E2, E4, B2 {09 01 0278}, BA {66}; else refusal FB |
| 7 | 0xF01E2B7A | 0xF01E2B0C | 03, 75, E2 {14}; else as state 2 |
| 8 | 0xF01E2D94 | 0xF01E2D8C | **everything** (SIM ready) |
| 9 | 0xF01E2402.. | 0xF01E23B2 | 3D, C0, D4, E0; else refusal **FC** (0xF01D1110) |
| 14 | 0xF01E2FC0 | 0xF01E2F7C | 03, 1D, 3B, BA; else as state 18 |
| 17 | 0xF01E318E | 0xF01E3148 | C0 {02} ; else `{svc, FA}` with the request id (0xF01D1096) |
| 18 | 0xF01E322A | 0xF01E31CC | 03, 1D, 3B, BA; else as state 7 |
| ? | 0xF01E309A / 0xF01E310E | 0xF01E203A / 0xF01E30E4 | 09, C0, E4 / C0, E0; else refusal FB |

Refusals [V]: 0xF01D1188: id = response id from the table below, data `{svc, FB}` (4-byte buffer); request
id not in the table: same id echoed, `{svc, FA}`. 0xF01D1110: `{svc, FC}`. Generic invalid parameters
(the command's +0x38 check fails, e.g. wrong service type): `{svc, FA}` with the response id (0xF01D0112).

Request -> response -> command constructor (table 0xF04B9498, `{req, resp, 0, 0, ctor}`) [V]:
D4->D5 0xF01D0A67, D6->D7, 3D->3E, 01->02, 03->04, 05->06, 07->08, 09->0A, 73->74, 60->61, BA->BB,
**1D->1E 0xF01D2379**, **19->1A 0xF01D492D**, 1F->20, 21->22, 13->14, DE->DF, F0->F0, DC->DD, 2F->30, 31->32,
E0->E1, 23->24, 68->69, 3F->40, D0->D1, BE->BF, 29->2A, 62->63, E4->E5; 33->34, D8->D9, DA->DB (no ctor).
Per-service tables (0xF04B95A0, `{req, resp, count, 0, subtable*}`, subtable `{svc, 0,0,0, ctor}`) [V]:
2B->2C {3A, 39, 02, 01}, 2D->2E {3A, 39, 04, 03}, E2->E3 {10, 14, 00..06, 11, 12, 13}, C8->C9 {76..7B},
6A->6B {01, 02}, BC->BD {53, 52, 54, 55, 01..07}, **3B->3C {FA, 68}**, 6C->6D {01, 02, 03},
**C0->C1 {00, 01}**, 75->76 {01, 02, 03}. A service not in the subtable: `{svc, FA}` [I path].

| msg | name | dir | data after msg id | values | evidence | mark |
|---|---|---|---|---|---|---|
| 0xC1 | SIM_STATUS_RESP | resp to C0 {00} | `00, st, b4, b5, 00, 00` (6 bytes; buffer 8) | state 8 (ready): `00 01 01 b5 00 00`, b5 = 00, 01 (flag bit 7 of a card byte) or 02 (value 4); state 4: st = **06** when the no-card flag (S+0x87) is set, else **4B**, b4 = 00, b5 = FF; any other state: `00 4B 00 FF 00 00` | 0xF01D99E4 (ctor 0xF01D9A52) | [V] |
| 0xC1 | (same) | resp to C0 {01} | `01, x` (2 bytes) with x = S+0x86 | | 0xF01D9A9C (ctor 0xF01D9AAC) | [V] layout, [I] x |
| 0x1E | SIM_IMSI_RESP (READ_IMSI) | resp to 1D {2D ..} | `2D, 01, len, 8 bytes EF_IMSI` (11 bytes) / `2D, 24, 0...` (11 bytes) when the IMSI cannot be read | only service 2D; other service `{svc, FA}`; refused states `{2D, FB}` | 0xF01D2346 (ctor 0xF01D2378) | [V] |
| 0x1A | SIM read (service 2F) | resp to 19 {2F ..} | `2F, 01, 4 bytes` / `2F, 24, 00 00 00 00` (6 bytes, buffer 8) | only service 2F; data via 0xF01CA400 (needs product-profile feature 0x3D = 1, then a 3-byte record) | 0xF01D48FE (ctor 0xF01D492C) | [V] |
| 0x3C | SIM IMSI-equal | resp to 3B {FA} | `FA, 2A` (equal) / `FA, 2B` (not equal) (2 bytes) | 0xF01CA354 compares the stored IMSI | 0xF01D243A (ctor 0xF01D245E) | [V] |
| 0x3C | (same) | resp to 3B {68} | `68, 49 / 4A` | | 0xF01D23DA (ctor 0xF01D23FC) | [V] |
| 0xEE / 0xEF | inds | ind | `63 3C 06 01 x 00` / `0D 0C 06 01 x 00` (6 data bytes), x = 0xF01CE002() | sent together from 0xF01E22EC (with further inds after) on a state entry | 0xF01E22EC | [V] bytes, meaning not read |

No-SIM start-up [I that "no card" = state 4: S+0x87 (set by 0xF01CFC1C / 0xF01CFC80 on card-absent events,
then events 3/6/0x0D/0x18 move the state) is read only by the state-4 branch of SIM_STATUS]: C0 {00} ->
`C1 00 06 00 FF 00 00`; C0 {01} -> `C1 01 x`; every other request but 09, E0, E4 -> `id+1 {svc, FB}` -
the {service, FB} that the N8 capture showed is this modem family's "not allowed in this state" refusal.

replies-bb5.txt / modem core check (SIM 0x09):
- `reply 09 19 1A 2F 24`: **differs for no SIM** [V/I]: with no SIM the N95 modem answers `1A {2F FB}`;
  `2F 24` (+ 4 zero bytes, 6 data bytes) is the SIM-present "not available" answer [V]. Whichever is kept,
  the real length with 24 is 6 data bytes.
- core no-SIM `09 13/23/2D/60/BA/DC -> id+1 {svc FB}`: **matches** the state-4 refusal [V shape; these ids
  are refused in state 4] (BA is accepted only in states 5/14/18 with service 66).
- core no-SIM `09 C0 -> C1 {svc, 06}` (2 bytes): status **matches** (06) for service 00; the real message
  is 6 bytes `00 06 00 FF 00 00` [V]. For service 01 the real data is `01 x` (2 bytes) [V]; for other
  services in state 4 the C0 command is created but the service subtable has only 00/01 -> `{svc, FA}` [I].
- core with SIM `C0 {00} -> {00 01 00 FF 00 00}`: real ready state is `00 01 01 b5 00 00` ([4] = 01,
  [5] = 00/01/02, not FF) [V]. NokiaDSY reads only [11] per the core's comment, so harmless [I].
- core with SIM `1D -> 1E {svc, 01, 08, imsi}`: **matches** [V] (11 bytes).
- `reply 09 3B 3C FA 2A` (N73 line): matches the SIM-present "IMSI equal" answer [V]; with no SIM this modem
  sends `{FA FB}` [I state].
- `reply 09 31 32 3B 24`, `09 62 63 4D 24`, `09 09 0A 97 20`, `09 DE DF ...`: 31->32, 62->63, 09->0A, DE->DF exist
  in the table [V]; their builders were not read.

## 10. Budget reached / open points

Covered with [V] encoders: MTC 0x15 (all responses + C0/C1/C2), SEC 0x08 (02, 12, 10 shape), EM 0x17
(01, 09, 0B, failure form), INFO 0x1B (2A both forms, 2F), CALL 0x01 (29), GPDS 0x31 (1A, 23).
Server map (resource -> start, ISI handler) in section 1.

Not covered / open:
- NET 0x0A (00->01, 0B->0C, 36->37, 40->41): SDL state machine behind 0xF01F3130; needs the per-state
  jump table read by hand. Highest-value next step (the TSY's registration status).
- SIM 0x09: command-object design (sim_unpack_msg.c); only the status byte values 01 / 24 were seen.
  Next step: break the factory pointers (ctx+0x1C -> +0x68/+0x6C) by finding where they are stored
  (init of the SIM server at 0xF01E327A) and read the command vtables.
- GSS (`srv_gss.c`, probably resource 0x32 [I]), SMS (`SMS_Server` string), SS, PN_NAMESERVICE 0xDB
  (`pn_name.c`), GPDS 30->31, audio, test server, time: not attempted.
- Resources 0x3A, 0x47, 0x54, 0x1A, 0x48, 0x81 have handlers listed (section 1) but were not read.
- The server at 0xF0157D12 handles 0x38xx, 0x190D, 0x0203, 0xDC01, 0xF20D (RTC/time area,
  `hal_rtc_if.c` / `time_*.c`): relevant to `19 0A -> 0B` and `19 19 -> 1A`, not read.
- Header/length byte order on the link (native LE u16 written at +4) is [I]: confirm in the SSI driver
  (`ssi_hw.c`) if it matters.

Mismatches found against platform/isi/replies-bb5.txt / syborg_modem_core.c (all [V] unless noted):
1. `15 16 -> 79`: real `{x, rf}` with rf = 01 in NORMAL; table sends `00 00`.
2. `1B 2E -> 2F`: real `00 01 F0 00 88 00 + u32 BE + 128 bytes`, 138 data bytes; table had `00 01 F0 00 00 10`, 16 bytes; now the 18-byte cut `00 01 F0 00 10 00 ...` (closed 2026-11-18, section 6).
3. `17 00 -> 01` for the TSY's mask 0x03: real has 2 sub-blocks (00 and 01); table 1.
4. `17 0A -> 0B`: real answers only requested sub-blocks, in mask-bit order (0x15 before 0x16).
5. `08 11 -> 12` (modem core): real is 6 bytes `{05, 16, flag, 00, 00, 00}` with 0x16 at [11]; core sends `{05 00 00}`.
6. `08 67 -> 68` (9.3 table lines): this modem has no 0x67 handler (later modem message).
Matches: 15 01/03/04/06/0B/12 responses, C0 `{01, 03}`, 15 02 -> 65 (data 2 bytes, extra padding harmless),
08 01 -> 02 `{x, 00}`, 01 28 -> 29 `{00, 00}`, 17 08 -> 09, 1B 29 -> 2A echo form, 31 19 -> 1A length 12.

Tools left in the Ghidra project: `~/ghidra_scripts/SeedThumb.java` (seeding) and `MkThumb.java`
(create Thumb function at an address); helper Python (Thumb mini-decoder, CM_MsgAlloc store tracker) was in
the session scratchpad only: re-create from section 0.1/1 if needed.

## 11. Run 2: coverage and open points

Run 2 added 4.1 (NET) and 9.1 (SIM) above, and 11.1 / 11.2 below. Tooling used (scratchpad only, not kept):
a capstone-based linear Thumb disassembler of 0xF0100000..0xF0500000 (literal pools and BLX-switch tables
annotated), a brute-force BL-caller finder, and a "sub-block header store" pattern search; reading tables
straight from the file (offset = address - 0xEFD20200) was faster than Ghidra for every table here.

### 11.1 Time / RTC (0x19), `time_handler_req.c` (trace group 0x36)

| msg | dir | data after msg id | evidence | mark |
|---|---|---|---|---|
| 0x0B | resp to 0A | OK: `01, 02, sb 01, sb 04` (18 bytes; CM_MsgAlloc(0x14)): sb 01 = `01 0C 01 v2 YYYY(u16 BE) MM DD hh mm ss x` (12 bytes, 0xF014E788), sb 04 = `04 04 v 00` with v = RTC state from 0xF01514EC (value 3 makes it take the time from a second source first). Failure: `st, 00` (2 bytes), st = the error from 0xF0151C22 (0xAA = no answer sent) | 0xF014F356..0xF014F416 | [V] layout, [I] field names after the year |
| 0x1A | resp to 19 | failure: `st, 00`; success built by 0xF014D776 (not read) | 0xF014F67E | [V] failure form |

Check: `reply 19 0A 0B 01 01 04 04 01 00` **differs** [V]: the real OK answer has nsb 02 and the date/time
sub-block 01 (12 bytes) before sb 04; a parser that walks sub-blocks by id still finds sb 04 [I], one that
reads sb 04 at offset 12 does not.

### 11.2 GSS: resource 0x32 on this modem (`srv_gss.c`)

The GSS server (started at 0xF0210CE2 with resource argument 0, descriptor RAM 0xD8004990; it is also a
SIM client: CM_SendTo(9, ...) at 0xF0213BA0) dispatches at 0xF0212950..0xF0212A9E on `(res<<8)|id` [V]:
**32 00** -> 0xF0212640, 32 0B, 32 0F -> 0xF0212B56, 32 12 -> 0xF0212B58, 32 F0 -> 0xF0212030, plus EM
replies 17 16 / 17 18, INFO 1B 06 / 1B 2D, 1C 37 / 1C 39 / 1C 54, 43 06 / 43 09 and E6 8D. So **GSS is
0x32** [V]; there is no 32 30 or 32 11 / 32 19 handler [V absence of the constants]. Default reply for an
unknown id: `F0 {01, id, 00, 00/req[2] for F0..F2, 00, 00}` via CM_SendResp (0xF02115A4) [V].
One builder (0xF0212B1A) sends `0C {00, 08, 8 x u16 BE from ROM 0xF04BC4EC}` (CM_MsgAllocZero(0x14)) [V
bytes, I that it answers 32 0B].
Check: the table's `31 11 12`, `31 19 1A`, `31 30 31` lines are N8 captures; on the N95 resource 0x31 is
GPDS (section 8: 31 19 -> 1A is GPDS_STATUS, length 12 matches). 31 11 and 31 30 were not located in
gpds_msghandler.c - unverifiable here.

### 11.3 Coverage after run 2

[V] encoders now: MTC 0x15 (all), SEC 0x08 (02, 12, 10 shape), EM 0x17 (01, 09, 0B, failure), INFO 0x1B
(2A both forms, 2F), CALL 0x01 (29), GPDS 0x31 (1A, 23), **NET 0x0A (01, 02, 0C, 1E, 35, 37, 2C/2E, 08,
06/0A, 10, 3B, F0; sub-blocks 00, 09, 2C, 0F; status/mode maps)**, **SIM 0x09 (state machine and refusals
FB/FC/FA, request->response table, C1 for services 00/01, 1E, 1A, 3C; EE/EF shapes)**, **RTC 0x19 (0B,
1A failure)**, GSS resource id 0x32 and its request list.
Counts of response/indication records marked [V] in run 2: NET 14, SIM 7 (+ 3 refusal forms), RTC 2, GSS 2.

### 11.4 Still open

- NET: which of the 15 relay states pass 00/0B/36 to the common block (a deferred request is answered later);
  the stack's internal status for "no SIM, emergency camping" (5 -> ISI 06 is the best match) [I]; the
  meaning of sb 09 bytes 11..19; the ctx+0x80 blob (RAT_REQ {01}); 0x3D forwarding (to task 0x0D).
- SIM: proof that the no-SIM state is state 4 (only the C0 code ties S+0x87 to state 4); the 31->32,
  62->63, 09->0A, DE->DF, 33->34 builders; EE/EF/E8 indication meaning; C0 {01} value S+0x86.
- RTC 19 19 -> 1A success form (0xF014D776); PNS_NAME 0xDB (`pn_name.c` is not referenced by an `adr`
  or literal: the name service was not found); GSS 32 00 / 32 0B layouts; SMS, SS: not attempted.

### 11.5 Mismatches found in run 2 (against replies-bb5.txt / syborg_modem_core.c)

7. `0A 36 -> 37` (table and core): sub-block 0x2C is 8 bytes: GSM `00 01 2C 08 01 01 00 00 00 00`, UMTS
   `00 01 2C 08 02 00 00 00 00 00`; table/core send `2C 04 rat 00` [V].
8. core `0A 00 -> 01/02`: sb 00 NET_REG_INFO_COMMON is 8 bytes (`00 08 st mode 00 00 00 00`), core 4 [V];
   sb 09 matches in length and LAC/CI/PLMN offsets [V].
9. `0A 40 -> 41 {01 00 00}`: this modem has no 0x40 handler; it answers `F0 {01 40 00 00 00 00}` [V/I].
10. `09 19 -> 1A {2F 24}`: with no SIM the N95 answers `{2F FB}`; `2F 24` is the SIM-present form and is
    6 data bytes (`2F 24 00 00 00 00`) [V layout, I state].
11. core no-SIM `09 C0 -> C1 {00 06}`: status right, real length 6 (`00 06 00 FF 00 00`) [V].
12. core SIM-present `09 C0 -> C1 {00 01 00 FF 00 00}`: real `00 01 01 b5 00 00` (b5 00/01/02) [V].
13. `19 0A -> 0B {01 01 04 04 01 00}`: real nsb 2 with a 12-byte date/time sb 01 first [V].
Matches in run 2: `0A 0B -> 0C {00 01 04 04 00 00}`; `09 1D -> 1E` (core, 11 bytes); core no-SIM `{svc FB}`
refusals for 13/23/2D/60/BA/DC; `09 3B -> 3C {FA 2A}` (SIM present, IMSI equal); NET_REG IND 02 shape.

### 11.6 N95 device notes (docs/devices/nokia-n95-8gb-rm320/) checked against the modem

- n95-isc-isi-protocol.md (SIM_STATUS row): `C1 {st, 1C}` as the "no SIM" answer: this modem's C0 {00}
  builder only produces 01, 06 or 4B in byte 11 [V]; 1C does not come from it. The later choice `C1 00 06`
  (protocol-2 3.1.8) is the real no-card value [V], real length 6.
- n95-isc-isi-protocol-2.md 3.1.6 / table row `37 00 01 2C 04 01 00`: TSY-side verified; the modem sends
  `2C 08 ...` (mismatch 7) [V].
- 3.1.7 / rows for `0A 00 -> 01` and the `02` IND `00 01 00 04 06 00`: modem sends sb 00 with length 8 and
  (except NSPS_NO_COVERAGE) a second sb 09: `00 02 00 08 06 02 00 00 00 00 09 18 ...` [V shape, I status].
- `0A 0B -> 0C 00 01 04 04 00 00`: matches [V].
- `09 19 -> 1A 2F 24`: TSY maps 24 -> -1 and FB -> -18 (retry once, then -5) per protocol-2; the real
  no-SIM modem sends FB [I state], so the real phone retries once; 24 is harmless and avoids the retry.
- `17 00` / `17 0A` rows: covered in section 5 (sub-block order and count).

## 12. What broke on the phones (2026-11-18 regression)

After the realignment of section 10/11.5 the owner saw the N95 8GB not boot (logo ~15 s late), the 5230 and 5800
black for 20 s+, the E52 not boot; the N8 Delight and 808 (Symbian^3, other table lines) were fine.

- **`1B 2E -> 2F` (section 6) hung the phones in its 138-byte form** [V]. SAServer opens ISC channel 35 (resources 0,
  subscribes `17 21`, `1c 37`, `1c 42`), sends `1B 2E {00 00}` at ~3.3 s and receives with a 128-byte buffer.
  Serial log of an N95 seed session:
  `3.307 ISCPEER: ch 35 send (12): 00 00 00 1b 06 00 00 00 02 2e 00 00` then
  `3.307 ISCPEER: ch 35 message 148 > buffer 128, KErrOverflow`, repeated 1,506,495 times by 156 s, `phone booting
  at 150.1 s`, no home screen. **Cause (closed 2026-11-18)** [V]: not the reply but our ISC stand-in. The real
  NokiaISCDriver.ldd completes a too-small receive with **KErrNoMemory (-4)** and the needed length; SAServer grows
  its buffer only on -4 and re-posts the same buffer on anything else; the stand-in answered KErrOverflow (-9).
  Fixed in `iscpeer.inl` / `iscrx.h` (docs/hacks.md H-isc-rx-overflow, `tests/iscpeer_overflow_test.py`); with the
  138-byte form forced in a session table the N95 now boots to idle in 15.3 s (one `KErrNoMemory` line, then
  `deliver (148)`). The earlier guess "the allocation is not the length the modem sends" was wrong: it is [V, section 6].
  The table carries the 18-byte cut of the real layout (section 6), so no reply relies on buffer growth.
- Every other realigned reply was accepted by the N95 8GB (home screen reached, no panics new to the change, no
  date/time query: the RTC `19 0B` with the host date `07 EA 0B 12 ...` and sub-block 04 second is fine) and by the
  E52 (fresh boot to the region wizard) and the 5230 (see docs/roadmap.md).
- Rule for the next realignment: a reply longer than 118 data bytes needs the receiving client's buffer checked
  first (`ISCPEER: ch N message M > buffer B, KErrNoMemory` in the serial log: the client must grow, as SAServer
  does); `tests/isi_replies_test.py` `SAServerSpinsOnLongProductProfile` still fails on any BB5 table reply over
  118 bytes.
