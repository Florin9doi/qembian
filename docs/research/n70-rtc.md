# Nokia N70, N72, N90: where the clock comes from (H-rm84-rtc)

Static reading of the N70's ROM (V 5.1003.3, `eka1/rm84/core/ROM.bin`), the ISI log of a run, and runs of the N70,
N72 and N90 with the reply below (2026-11-26). [V] = read in the code or seen in a run, [I] = inferred.

**N72 (RM-180) and N90 (RM-42)**: the same custom.ldd (size 11468 on all three; the 0x0B path matches the N70's
byte for byte except for relocated calls and literals; the set call at custom.ldd +0x20C4 in each = N72 0x5014EA54,
N90 0x5016E8F4), the same NokiaDSY 0x0B path (the N90's DLL is a different build, 74588 bytes, with the same dispatch
and sb 04 rule), and the same isimsg RTC class (count at frame[11], sub-blocks from [12]) [V]. Each table
(`isi-replies-rm84.txt`, `-rm180.txt`, `-rm42.txt`) has the same line.

## Main finding

The N70 has no clock of its own on the application side: no rtc.dll, no UEM over CBus (the N-Gage family's
path, docs/research/ngage-uem-rtc.md), and no access to the OMAP RTC block (0xFFFB48xx does not appear in the
ROM) [V]. The time comes from the **modem over ISI, resource 0x19 (RTC)**, the same message the N95 uses
(platform/isi/replies-bb5.txt, docs/research/n95-isi-encoders.md 11.1). Two clients send `19 0A` at every
boot and our reply table (`src/qemu-ngage/isi-replies-rm84.txt`) answers neither [V log: `UNANSWERED`]:

| client | ISI object | sends | reads from `19 0B` | effect |
|---|---|---|---|---|
| custom.ldd (kernel side) | 0x40 | `19 0A {00 00}` at start (0x50153638); subscribes to indications `0C 00` and `19 05` [V log, its first message] | sub-block 01: date and time | sets the kernel's clock |
| NokiaDSY.DLL (DosServer) | 0x18 | `19 0A {00 00}` after the start-up reason `23 04` | sub-block 04: RTC state | tells Startup whether the RTC is valid |

Unanswered, the clock keeps the kernel's default and Startup asks for the time and date at every boot.

## `19 0B` layout (frame offsets; data from frame[10])

isimsg's RTC class (vtable 0x50143D64, resource 0x19 at +0x18): sub-block count at frame[11] (vtable +0x10
returns 0x0B), first sub-block at frame[12] (+0x14 returns 0x0C); field 0 = frame[10] (0x50141CBC) [V].
Sub-blocks are `{id, length, ...}` with a byte length, found by walking ids (0x501307A4) [V].

    [10] status        01 = OK
    [11] nsb           02
    [12] sb 01         01 0C TT FF YY YY MM DD hh mm ss xx    (12 bytes)
    [24] sb 04         04 04 VV 00                            (4 bytes)

- **custom.ldd** (0x50153764 -> 0x50153834) [V]: status [10] must be 1. It reads the first sub-block at fixed
  offsets with no id walk, only checking that its id ([12]) is 01, so **sb 01 must come first**. Byte [15]
  (FF) is the format:
  - 6: nothing set
  - 1: time only (hh mm ss; date from the current clock)
  - 2: date only (YYYY big-endian, MM, DD; time from the current clock)
  - anything else (0): date and time
  
  MM and DD are 1-based (the code subtracts 1). It formats `"%04d%02d%02d:%02d%02d%02d.000000"` (TTime::Set
  form, literal 0x5015452C) and passes the TTime to an ekern export (stub 0x50153FFC -> 0x5001F9A4) [V]. The
  time is **universal**: the phone shows it plus the home city's offset [V run: N72, Abu Dhabi as home city;
  the host's local fields sent as UTC moved the clock by the host's offset, UTC showed the right Abu Dhabi time].
  Bytes [13], [14] and [23] are not read [V].
- **NokiaDSY** (handler 0x5054C7CC, dispatch 0x5054C794 for `19 0B/12/1A/1C`) [V]: if status [10] is 1, it finds
  sb 04 by id. Its byte 2 (VV, isimsg field 0x12 -> byte 2, 0x50141D50) set to 1 or 3 means valid; 2, any other
  value, or no sb 04 means invalid. If status is 3, it reports invalid and skips the error path. It posts the
  result to DosServer as event 0x1C. This is the same rule as the N95's NokiaDSY.
- custom.ldd's set is gated [V code, I meaning]. Before parsing, it looks up an object named `"PowerModel"` (kernel
  vtable +0x118, literal 0x50154520) and skips the set if that object's +0x34 returns non-zero. On our board the
  gate passes (0x50153884 reached, then the set at 0x50153A94 with `20261025:222425.000000`) [V run].
- `19 05` (RTC indication) goes through the same parser (0x501537D4 -> 0x50153834) [V], so the modem can push
  a new time later. It is not needed at boot.

## The stand-in

The N95 line, with the date and time taken from the host:

    reply 19 0A 0B 01 02 01 0C 01 00 rtc 00 04 04 01 00   # RTC time read: OK, sb 01 date+time (format 00), sb 04 valid (01)

where `rtc` is a token in `ngage_isi.c` (beside `rq10`/`echo`). It writes 7 bytes, `YYYY(BE) MM DD hh mm ss`, of
QEMU's RTC clock **in UTC** at the moment of the reply: `qemu_get_timedate()` gives local fields with `tm_gmtoff` set
under the launchers' `-rtc base=localtime`, UTC fields with offset 0 under `base=utc` or a fixed date (`NGAGE_RTC`,
taken as UTC as QEMU does), and `mktimegm(tm) - tm_gmtoff` turns either into UTC. One line answers both clients:
each reply goes back to the object that asked.

Tests (`tests/ngage_isi_test.c`): `test_rtc_time_read` (regression rm84_time_query_every_boot: the frame, the
length, the clock read per reply, a year past 255) and `test_rtc_sends_utc` (regression rm180_rtc_is_utc: local
fields turned into UTC across a year and a day boundary, both offset signs).

Runs [V]: on all three the set call gets the host's time (e.g. `20261025:205036.000000` at 20:50 UTC). A second boot
from a kept NAND (N72, N90) goes to the home screen with no Time or Date query, dated in the home city's zone. A
factory-fresh boot still asks for both (Startup's first-boot flow, as on the N-Gage family), and the queries'
fixed defaults (N72: 09:00, 01/01/2007) then hold until the next boot.

## Open

1. A time set by the user is not kept over a restart: the next boot follows the host again. A real modem keeps it
   (an RTC write, `19` messages 0x12/0x1A/0x1C seen in NokiaDSY's dispatch, not answered here).
2. Whether the 6630 (RM-1, isa_if_driver instead of ISC) sends the same request was not checked.
