# Reading modem memory / per-phone flash over ISI (N95 8GB, MCUSW / RAP3G)

Goal: find an ISI service the Symbian side can send to the real N95 modem to dump the per-phone NOR
areas (so we can load them into the emulated modem whose image, built from stock flash files, has these
areas erased). Needed in particular by `ST_SECURITY_TEST`, which wants the protected-application (PA)
directory at modem VA `0xefdd0000` = NOR `0xb0000` (docs/research/n95-cmt-emulation.md §7).

Source: Ghidra program `mcusw_f0100000.bin` (base `0xf0100000`, raw `build/n95-cmt/mcusw_f0100000.bin`,
file offset = addr − 0xf0100000). [V]=read in code/bytes, [I]=inferred. Function addresses are MCUSW VAs.

## 0. Bottom line (production answerability)

- **There is NO raw "read arbitrary address" ISI peek service that a production phone answers.** The
  firmware's only arbitrary-address readers are the fault core-dump (`"Interrupted mode stack dump from
  address 0x%08X"` f0100e3c, `"Core dump follows"` f0100778) — triggered by a crash and emitted to the
  STI/trace port, not requestable over ISI. [V strings, I: no ISI entry]
- The modem exposes memory only as **logical stores read by id**, through three servers:
  1. **PN_PERMANENT_DATA**, resource `0x23`, req `0x04` → resp `0x05` — permanent-data blocks by id.
     **Production-answered** (the phone reads PD during every boot). [V repo: platform/isi/names.txt,
     replies-bb5.txt line 41; I: MCUSW handler not located, likely served by the PMM/BB5 core]
  2. **NVD server** ("(c) Nokia NVD Server" f015ead0, `nvd_req.c` f015f284) — NVD blocks by owner id
     (radio calibration/tuning). **Production-answered** (radio needs it at every boot). [V]
  3. **Monitor server** ("Monitor server, (c) NMP" f04abdbd), resource **`0x38`** — the HW-config /
     certificate / SIMlock / product-config records that physically live in the PA area (NOR
     0xa0000–0xe0000, directory at 0xb0000) and PMM. **Writes are R&D-mode gated** (see §2); the record
     directory list is ungated, but protected-content reads are almost certainly gated too. [V]

So: **raw flash dump by address = NO. Per-phone data by logical id (PD/NVD) = YES. PA/cert protected
content via res 0x38 = effectively NO on a production phone.** The PA directory `ST_SECURITY_TEST`
needs is therefore *not* remotely readable from a stock phone — extract it from the owner's flash/core
offline (build/evidence) instead, or bypass the security test. (Details and the recommended dump plan
in §5.)

## 1. NOR → VA mapping [I, cheap check]

MCUSW maps the modem NOR read-only at **VA = NOR_offset + 0xefd20000** (boot trace "R/O offset
0xEFD20000"; the MCUSW partition NOR 0x3E0000 → 0xF0100000 matches, encoders doc §0). Arithmetic
confirms the PA directory: `0xefdd0000 − 0xefd20000 = 0xb0000`. The literals `0xefd20000`/`0xefdd0000`
are **not** present as 32-bit constants in MCUSW (searched) — the mapping is computed from the NOR base
(NOLO sets the MMU; MCUSW gets the base from NOLO hw_conf records), so this stays [I] from the trace +
arithmetic, not a verified table read. Target NOR areas to dump (owner's list):
`0x60000–0x7ffff` per-phone records · `0xa0000–0xe0000` PA area (dir 0xb0000) · `0xf00000–0xffffff` PMM.

## 2. Monitor server — resource 0x38 (mon_trace.c / mon_ftd.c) [V]

Task name "Monitor server, (c) NMP". Server descriptor at `f04abdd8`/`f04abdf0` declares it owns PMM
keys `0xc1` and `0x0b` for **resource 0x38** (bytes `c1 00 24 00 38 00 …`, `0b 00 24 00 38 00 …`), plus
a `0xC009` ISI-handler entry. Message dispatcher **FUN_f0157d12**: it masks the incoming word to 16 bits
= `(resource<<8)|msgid` and compares against constants (all high byte `0x38`), then on success replies
with `FUN_f012b644(ctx, 0xc008, …)`. Handlers all operate on PMM records via the permanent-memory API
(`FUN_f01aa0fe` write, `FUN_f01aa48a` read, `FUN_f01aa586` delete, `FUN_f01aa5c6` enumerate), keys
`0xc1`, `0x0b`, `0xef`. Message map (msgid = low byte; request → response id) [V]:

| msgid | handler | resp id | what it does |
|---|---|---|---|
| 0x00 | FUN_f0158b08 | 0x01 | write 0xDA HW-config/cert block (sub-blocks type 02/03 = two 0x60-byte fields); **gated** |
| 0x03 | FUN_f0158e66 | 0x04 | read/modify the 0xDA block (key 0xc1); **gated** |
| 0x0c | FUN_f015cb32 | 0x0d | status query |
| 0x0e | FUN_f015c8c2 | 0x0f | **record-list get** (req byte[2]='`'=0x60, byte[3]=count; resp = 4-byte entries, which ids exist) |
| 0x17 | FUN_f015d1ee | 0x18 | **key-list get** (req byte[2]='b'=0x62, byte[3]=count, then count sub-blocks {idHi,idLo,len,…}); ungated |
| 0x19 | FUN_f015926e | 0x1a | set bits in key 0xc1 sub-record |
| 0x1b | FUN_f0159356 | 0x1c | clear key 0xc1 sub-record |
| 0x1f | FUN_f0159446 | 0x20 | clear bitmask (key 0xc1, sub 1) |
| 0x21 | FUN_f01594d2 | 0x22 | read bitmask (resp bytes 4..7 = 32-bit value) |
| 0x23 | FUN_f015d3d2 | — | build a multi-record list (reads many records into one response) |
| 0x27 | FUN_f01593ba | 0x1e | set bitmask |
| 0x2d | FUN_f0156c42 | — | enumerate key 0xef product-data record ids (resp = 4-byte id entries) |

Full msgid→handler index table at `f04c27c0` (pairs {u16 idx, u16 msgid}).

**Security gate** [V]: the write/modify handlers (FUN_f0158b08, FUN_f0158e66) begin with
`if (FUN_f01a3a9c(0xfffffffe) == 9) status = 6;` i.e. refuse. `FUN_f01a3a9c(0xfffffffe)` (f01a3a9c)
queries a privilege bit via `FUN_f01a319a(0)` and returns **8 if the R&D/local privilege is set, 9 if
not** → on a stock phone (bit clear) it returns 9 and the operation is refused with status 6. The read
list-gets (0x0e/0x17) don't call this gate, but they return only *which* records exist, not protected
content; reading the 0xDA cert/HWconfig content goes through the gated path.

## 3. NVD server (nvd_req.c) [V]

"(c) Nokia NVD Server" (f015ead0). Init/PNS handler FUN_f015ea3c (answers PNS version query msg 0x02 →
0xf0/03, name "NVD Server"). Block-read request handler **FUN_f015f0e8**: takes a list of up to 0xff
2-byte block/owner ids from the request, builds an internal request (msg `0xf0`/`0x10` per owner) to the
flash/NVD manager (`FUN_f0125e86`/`FUN_f015fb42`), and returns the block bytes. These are per-phone
radio-calibration/tuning blocks. Read by owner-id, not address. Production-answered.

## 4. PN_PERMANENT_DATA — resource 0x23 [I, from repo]

Already exercised by the Symbian TSY and documented in the repo: `platform/isi/names.txt` (`res 23
PN_PERMANENT_DATA`) and `platform/isi/replies-bb5.txt` line 41:
`reply 23 04 05 len=18 00 01 01 10   # PN_PERMANENT_DATA read`. So the APE sends **resource 0x23, req
msgid 0x04**, the modem replies **msgid 0x05** with an 18-byte payload (`00 01 01 10 …` = status +
block-id/len + data in the canned fake). The exact request sub-block layout was not read out of MCUSW
(the res-0x23 handler was not located — likely in the BB5 core/PMM layer, not the mon_* region), so the
precise address/offset/length fields of req 0x04 are **[inferred]**: {transId, 0x04, blockId(2), offset?,
length?} returning {transId, 0x05, status, blockId, length, bytes…}. This is the production read path.

## 5. Recommended way to dump the three areas

Reality: you cannot pull raw NOR offsets `0x60000`/`0xa0000`/`0xf00000` by address over ISI from a stock
phone. Map each area to its logical service instead:

- **NOR 0x60000–0x7ffff (per-phone records)** and **0xf00000–0xffffff (PMM)** → **PN_PERMANENT_DATA
  (res 0x23, req 0x04)**: enumerate block ids and read each with req 0x04 → resp 0x05. One request per
  block; a block is typically ≤ a few hundred bytes, so one message each. Production-answered.
- **Radio calibration** (if needed) → **NVD server**: one read request can carry many owner ids.
- **NOR 0xa0000–0xe0000 (PA / cert / SIMlock)** → **Monitor server res 0x38**: msg 0x17/0x0e enumerate
  the record directory (ungated), but the protected 0xDA content (what `ST_SECURITY_TEST` validates) is
  behind the R&D privilege gate (§2) and a production phone replies status 6. **Do not expect to dump
  the PA area over ISI from a stock phone.** Get it from the owner's full flash image / core dump
  (build/evidence) offline, or run the emulated security test in a bypass mode.

### Tested example (not run; erased image + answering state needs the full APE handshake)

A live probe was not run: the per-phone data is erased in our image, so reads return empty, and reaching
the modem's answering state needs SOS-proxy registration + SOS-media activation by the APE peer
(docs/research/n95-cmt-emulation.md §5), not a one-shot `probe.py` boot. To try it once that state is up,
add to `tools/cmt/n95-cmt.map` and run `python3 tools/cmt/probe.py --secs 8 --map FILE --out DIR`
(watch `DIR/access.log` "ISI CMT->APE" lines):

```
# res 0x38 key-list get (msgid 0x17, 'b'=0x62, count 1, one {idHi,idLo,len=04,..} sub-block)
ssisendat 3000 00006c381b001b006200010101040000
# PN_PERMANENT_DATA read (res 0x23, req 0x04) — req sub-block layout [inferred], adjust block id
ssisendat 3200 00006c23080018000204<blockIdHi><blockIdLo>00
```
(header = media00 rdev00 sdev6c res len(2BE) robj sobj tid id data; robj/sobj to the server object.)

## 6. Open / to verify
- Exact req-0x04 sub-block layout and the res-0x23 handler location in MCUSW (search the BB5 core/PMM
  layer; not in the mon_* region).
- Whether res-0x38 msg 0x0e/0x17 content reads (vs id-only) exist and are gated.
- Whether the PA directory can be obtained from the owner's core file offline (recommended fallback).
