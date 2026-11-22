# N95 8GB modem (MCUSW): PN_PERMANENT_DATA (ISI 0x23) and the PMM record store

What we need to answer WlanServer's `23 04` read: the request and response layouts, an ungated ISI
write, and the PMM flash format, so that a record can be put into the NOR image at 0xf00000.
[V] = verified in Ghidra (`mcusw_f0100000.bin`) or on the emulated modem (`tools/cmt/probe.py`);
[I] = inferred.

## 0. Where the server is

- Perm server ISI dispatcher: **FUN_f015dc88** [V]. It switches on `(resource<<8)|msgid` (0x23xx). The
  common-message handler (0xF0) is FUN_f015dbe8, which holds the "(c) Nokia Perm Server" string. Even
  msgids 0x00-0x14 and 0x28-0x2e go through a switch8 table at f015dca4/f015dcd0. Odd and unknown ids
  go to f015ded2 (error reply).
- Message map [V] (handler, then the PMM API call):

| req | resp | handler | PMM API | meaning |
|---|---|---|---|---|
| 0x00 | 0x01 | f015e5e2 | FUN_f01aa044 (group, buf, len, &index) | create a record at a new index |
| 0x02 | 0x03 | f015e546 | FUN_f01aa0fe (group, index, buf, len) | **write** (whole record) |
| 0x04 | 0x05 | f015dace | FUN_f01aa48a (group, index, offset, len, buf) | **read** |
| 0x06 | 0x07 | f015e4de | FUN_f01aa586 (group, index) | delete one record |
| 0x08 | 0x09 | f015e480 | FUN_f01aa58a (group) | delete the whole group [I] |
| 0x0a | 0x0b | f015e414 | FUN_f01aa5c6 (group, idx) | enumerate |
| 0x0c,0x0e,0x10,0x12 | +1 | f015e39c, e33a, e2ce, e262 | aa652, aa60e, aa6a4, aa6d2 | group queries (size/count) [I] |
| 0x14 | 0x15 | f015e1e0 | FUN_f01aa59a | global query [I] |
| 0x26, 0x32, 0x40 | | f015e174, f015de74, f015da22 | | not looked at |
| 0x44 | 0x45 | inline, FUN_f01aa8d8 | | status byte |

Every handler rejects a group >= 0x16b (363) with status 0x04 [V].

## 1. Read: 23 04 -> 23 05

The request bytes, counted from the ISI transaction id (`tid 04 01 39 00 00 00 00 00 00 00 00 00 00 02 67`) [V, f015dace]:

| off | bytes | meaning |
|---|---|---|
| 0 | tid | |
| 1 | 04 | msgid |
| 2-3 | `01 39` | **PMM group id** 0x0139 (BE), < 0x16b |
| 4-5 | `00 00` | **record index** 0 (BE) |
| 6-7 | `00 00` | padding |
| 8-11 | `00 00 00 00` | byte offset in the record (BE u32) |
| 12-15 | `00 00 02 67` | length = **0x267 = 615 bytes** (BE u32, at most 0x400; more than that gives status 0x41) |

So WlanServer reads **group 0x139, index 0, all 615 bytes**. The 16 bytes in the old reply table were wrong.

Response [V, f015dace + emulator]:
- error: `tid 05 STATUS 00` (4 bytes).
- success: `tid 05 00 01 | 00 00 00 00 | SBLEN(BE32) | DLEN(BE32) | data, zero-padded to 4`. Here
  SBLEN = DLEN + pad + 12. Seen on the emulator for 615 bytes: `00 01 00 00 00 00 00 00 02 74 00 00 02 67 <615 bytes> 00`.

Status codes (PMM API results) [V unless marked]:

| code | meaning | where |
|---|---|---|
| 0x00 | OK | |
| 0x04 | group id >= 0x16b | f015dace |
| 0x05 | index out of range (group 0x139 has a single index) | seen on the emulator for index 1 |
| 0x0a | record shorter than offset+len | FUN_f01ae4b2; seen on the emulator for a 16-byte record read with len 0x267 |
| 0x0f | slot state 3..7 (busy or invalid) [I] | jump table in FUN_f01ae3ca |
| **0x11** | **no such record**: the slot state is 0 or 1 (never written) | FUN_f01ae3ca table at f01ae3ea, `movs r4,#0x11` at f01ae3fa |
| 0x13 | write-protected group | FUN_f01aa0fe / aa044 |
| 0x14 | len != 0 with a NULL buffer | FUN_f01aa48a |
| 0x16 | NULL data with len != 0 | write |
| 0x41 | length > 0x400 | f015dace |

The real phone's `05 {11 00}` means that **group 0x139 index 0 is not in the PMM**, because our NOR's PMM area is erased.

Lookup path [V]: FUN_f01aa48a -> FUN_f01ae3ca -> FUN_f01ab51e. That function fills the slot state from RAM tables
(`DAT_f01ab604` per-group first slot plus index, `DAT_f01ab60c` slot state, `DAT_f01ab610` slot -> page),
which are built by the mount scan at boot.

## 2. Write over ISI: ungated for group 0x139 [V]

**23 02 (write)**, counted from the tid: `tid 02 | group BE16 | index BE16 | 00 00 | len BE32 | data[len]`.
The response is `tid 03 00 01 05 04 XX 00` (status 0, one sub-block 05/len 4) [V].
**23 00 (create)**: `tid 00 | group BE16 | len BE32 | data`. The response is `tid 01 00 02 {03 04 idxHi idxLo} {05 04 XX 00}`.

Gate [V, FUN_f01aa0fe]: the group's config byte +8 (12-byte entries, ROM table 0xf04b5f1c, indexed by
internal group through the RAM map at 0xd80188a0, FUN_f01ac22e/FUN_f01ac196). If that byte is 0,
FUN_f01a2b8e also checks a 3-entry protected list at f04b5b9c: groups **0x0007, 0x0001, 0x0135**. That
check depends on the security state. Every other group is writable. No R&D or MTC state check sits on this path.

Tested on the emulated modem [V]: `ssisendat 3200 00006c231e0000180202013900000000000000100011223344556677889900aabbccddeeff`
-> status 00, and the read-back returned the 16 bytes. A write has no offset, so the 615 bytes must go in one
ISI message (0x275 bytes of ISI payload). probe.py's `ssisendat` buffer holds only 256 bytes
(`CmtLater.m[256]`, syborg_cmtprobe.c), so a 615-byte write cannot be tested that way today. The real
APE path (SSI_MAXMSG 4096) can carry it, for example sent once from the phone side or from a stand-in.
Writes go to the pflash, which probe.py opens with `snapshot=on`, so **they are lost at power-off**. For
a persistent record, write it into the NOR image (section 3).

## 3. PMM flash format [V: the dump after a boot, plus a synthetic image accepted by the firmware]

The PMM covers NOR 0xf00000-0xffffff (1 MB). Physically it is at that address. The MCUSW VA guess
0xefd20000+off is **wrong** (gdb cannot read 0xf0c20000). Use `monitor pmemsave 0xf00000 0x100000 "file"`.

Pages: seven 128 KB pages at 0x00000-0xc0000, then four 32 KB pages at 0xe0000, 0xe8000, 0xf0000 and 0xf8000 (offsets within the PMM).
Two categories: large pages (records of groups 0x2, 0x8, 0xb, 0xc, 0x32, 0x36, 0x6b, 0xc1, 0x142, 0x162)
and small pages (groups 0x134, **0x139**, 0x155, 0x164).

Page layout:
- header (12 bytes): `f0 f0 03 00  ff ff ff ST  00 00 00 00`, where ST = `f0` spare (one per
  category: 0x00000 and 0xe0000), `cc` active (0x20000 and 0xe8000 after the first boot), `fc` formatted and free.
- trailer: the page's last 2 bytes are `f0 f0`.
- records start at page+12, packed, 2-byte aligned. An all-0xff 16-byte header ends the page.

Record (16-byte header, little-endian):

```
u8  state   0xff = current, 0xcc = superseded (an older copy; a rewrite appends a new copy and clears bits)
u8  type    0x30
u16 group   e.g. 0x0139
u16 index
u16 0xffff  (always ff ff in every record seen; no CRC) [I]
u32 len     data length
u32 total   16 + len rounded up to 2 (the step to the next record)
u8  data[len], pad 0xff
```

No checksum anywhere [I: none of the 160 records written by the firmware has one, and the firmware
accepted a synthetic image]. Erased state is 0xff, and state changes only clear bits.

**Insert function**, verified: the firmware mounted the image instead of reformatting it, kept the record
and added its own defaults; `23 04 0139` then returned status 00 and the 615 bytes:

```python
import struct
PMM = 0xf00000
def _rec(group, index, data, state=0xff):
    tot = (16 + len(data) + 1) & ~1
    return (struct.pack('<BBHHHII', state, 0x30, group, index, 0xffff, len(data), tot) + data).ljust(tot, b'\xff')
def pmm_format(recs_small=(), recs_large=()):
    """A 1 MB PMM area: formatted pages; recs_* = [(group, index, bytes)]."""
    a = bytearray(b'\xff' * 0x100000)
    pages = [(o, 0x20000) for o in range(0, 0xe0000, 0x20000)] + [(o, 0x8000) for o in range(0xe0000, 0x100000, 0x8000)]
    st = {0x0: 0xf0, 0x20000: 0xcc, 0xe0000: 0xf0, 0xe8000: 0xcc}
    for o, sz in pages:
        a[o:o+12] = b'\xf0\xf0\x03\x00\xff\xff\xff' + bytes([st.get(o, 0xfc)]) + b'\0\0\0\0'
        a[o+sz-2:o+sz] = b'\xf0\xf0'
    for base, recs in ((0x20000 + 12, recs_large), (0xe8000 + 12, recs_small)):
        p = base
        for g, i, d in recs:
            r = _rec(g, i, d); a[p:p+len(r)] = r; p += len(r)
    return bytes(a)
# nor_img[PMM:PMM+0x100000] = pmm_format(recs_small=[(0x139, 0, wlan_615_bytes)])
```

Only do this to an erased PMM (the stock image). To add a record to an already formatted area, append
after the last record of the right category's active page. Test harness: a scratch wrapper patched
`nor.build` and ran probe.py unchanged; see the commands in section 6.

## 4. What group 0x139 holds

The size is fixed by the group config: ROM entry `67 02 00 00 01 00 00 00 00 ...` = max size 0x267 and one
index [V, table at 0xf04b5f1c, the entry with size 0x267; the mapping to 0x139 is I, but it matches the
read length and the status-05 limit of one index]. That makes it **WLAN tuning/calibration data (615 bytes)**,
not a 16-byte MAC block [I]. The modem never reads it; it only stores it for the APE, so the layout is
in WlanServer or the WLAN driver on the Symbian side, not in MCUSW. Stand-in: 615 bytes that
WlanServer accepts. Start with zeros. If WlanServer still fails, read the layout from its parser
(the next research step, on the Symbian side). A MAC address, if it is in there, is probably the first 6
bytes [I, unverified].

## 5. IMEI (INFO 1B/00 sub-block 0x41)

Not investigated (budget). The IMEI is likely PA_ESN / PA area, not PMM [I].

## 6. Reproduce

- Read: `ssisendat 3000 00006c231200001801040139000000000000000000000267` (status 11 on a stock NOR).
- Write 16 bytes: see section 2. Read 16: `...0139 00000000 00000000 00000010`.
- PMM dump: probe.py `--gdb PORT --gdb-cmds F --gdb-after 8`, with F = `monitor pmemsave 0xf00000 0x100000 "OUT"` / `continue`.
  The file name must be quoted, or HMP parses the path as part of an expression.
