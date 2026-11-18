# Nokia 6600: why the time query shows on every boot (rtc.dll and the UEM RTC)

Static reading of `ROM.bin` (rtc.dll 0x5033EDB0, cbus.dll 0x50367860). No runs. [V] = read in
the code, [I] = inferred.

## Main finding

The RTC object has two separate words, and the earlier notes looked at the wrong one:

| field | meaning | set by |
|---|---|---|
| `+0x0C` state (9 / 1 / 2) | 9 = normal, 1/2 = clock-alarm wake-up detection | 0x5033F5D8 |
| `+0x3C` **RTC valid** | 1 = the UEM clock kept its time | init 0x5033F098, 0x5033F4B8 (time set), cleared in 0x5033F14C |

- [V] Request 1 (vtable +0x20, 0x5033F3D4) returns `state == 2`. State 2 needs UEM interrupt
  status bit 3 (the RTC alarm) to be pending at init, the clock equal to the alarm time, and the
  time at 00:00. That is the **alarm power-on** path. A power-key start ends in state 9 and
  returns 0. This is correct, not the bug.
- [V] When request 1 returns 0, rtc.dll runs 0x5033F250:
  - `+0x3C != 0`: notifications 3 and 5, a timer, `reg 0x15 |= 8` (acknowledges the alarm),
    `reg 0x16` alarm mask. This is the normal kept-clock start.
  - `+0x3C == 0`: 0x50340BF8 resets the UEM clock. It pulses reg 2 bit 7, sets **reg 4 = 0**
    and `reg 0x15 = 0x0F`, then sends notification 4 (RTC invalid). [I] This is the time query.
- [V] rtc vtable (0x503413E8): +0x0C `State()` returns +0x0C, +0x14 (0x50340A60) `IsValid()`
  returns +0x3C, +0x20 request, +0x28 (0x5033F4B8) sets the time (`+0x3C = 1`, notification 4).

So the board must make **`+0x3C` = 1**. Making `state` = 2 is the wrong goal. Also, setting
`dev1 reg 0x15 = 0x0018` (bit 3 = alarm interrupt) asks for the alarm-wake path.

## How `+0x3C` is computed

- [V] Init (0x5033EF98, second phase): the object starts in state 9. It calls 0x5033F5D8 once,
  sets base time `+0x10/+0x18` to the kernel's current time with microseconds 0 (0x50341184,
  [I] kernel time), then:
  `valid = (reg4 & 0xFF) != 0 && days(reg1) != 0xFF`.
- [V] Request 1 → 0x5033F0C8. It sets `+0x3C = 0` and state 9 if `time1 != time2` or
  `time1 == 0` (TTime `!=` / `==`, ekern 0x5038A198 / 0x5038A17C). Otherwise:
  - base `+0x10 = +0x18 = time1` with microseconds 0.
  - If `time3 != 0` and `trim(time3) >= trim(now)`, then `+0x20 = time3` [I: the alarm time].
  - Then 0x5033F5D8.

So `valid` needs three things:
1. UEM **reg 4 ≠ 0**. [I] This is the RTC calibration byte. The driver writes its default
   0xC0 itself (0x5033FCFC) when it reads 0.
2. UEM **reg 1 (day counter) ≠ 0xFF**.
3. A **non-zero `time1 == time2`** in the stand-in's hand-over. The stand-in passes the rtc's own
   `+0x10`. [I] If the kernel time at rtc init is 0, the hand-over itself clears `+0x3C`.
   Check `[rtc+0x10]` in a run.

## cbus.dll logical → physical registers [V]

The rtc calls the CBus driver at `[rtc+8]`:
- read: vtable +16 `(obj, dev=1, logical, bits, &out)`
- write: vtable +20 `(obj, 1, logical, bits, 0, clearMask, setBits)`

Table at **0x50369034**, 20-byte entries `{device, type, hw reg, shadow idx, bits}`, indexed by
the logical number (1..69, checked at 0x50367A08):

| logical | dev/reg | type | bits | rtc use |
|---|---|---|---|---|
| 1 | 1/0x01 | 0 | 8 | day counter (0xFF = invalid) |
| 2 | 1/0x02 | 0 | 8 | alarm minutes (&0x3F); bit 7 = reset pulse (0x50340BF8) |
| 3 | 1/0x03 | 0 | 8 | alarm hours (&0x1F); bit 7 = alarm enable (0x50340408) |
| 4 | 1/0x04 | 0 | 8 | calibration / "running" byte, must be ≠ 0 |
| 21 (0x15) | 1/0x15 | 3 | 16 | interrupt status; bit 3 = RTC alarm, bit 4 = power key |
| 22 (0x16) | 1/0x16 | 2 | 16 | interrupt mask; bit 3 = RTC alarm |
| 31 (0x1F) | 1/0x1F | 1 | 16 | bit 5 read at 0x50340A44 [I: status] |
| 32 (0x20) | 1/0x11 | 1 | 8 | seconds (&0x3F) |
| 33 (0x21) | 1/0x12 | 1 | 8 | minutes (&0x3F) |
| 34 (0x22) | 1/0x13 | 1 | 8 | hours (&0x1F) |

- [V] Time read (0x50340304): reads seconds, minutes, hours and days, then seconds again,
  repeating until both seconds reads agree (at most 10 tries, else fault 7).
- [V] Field mapping: +0 seconds, +4 minutes, +8 hours, +12 days. Proved by the TDateTime setters
  0x503A3A9C SetSecond, 0x503A3A74 SetHour, 0x503A3A88 SetMinute, 0x503A3AB0 SetMicroSecond.
- [V] The rtc's "now" (0x503406B8) = DateTime(base +0x10) with the RTC's seconds, hours and
  minutes, plus `days` (TTimeIntervalDays). The base date is in RAM (from the hand-over). The UEM
  keeps only the time of day and the days since the base.

## State machine (for reference) [V]

- State 9 branch (0x5033F5FC), [I] alarm wake-up detection:
  1. Clears `reg16` bit 3, reads `reg15`, sets `reg16` bit 3.
  2. If `reg15 & 8`, acknowledges it. Then: hour/minute == alarm hour/minute and
     (days == 0, or days == 1 at 00:00) → state 1.
- State 1 branch (0x5033F718): needs `+0x3C`, `+0x20 != now` (to the minute) and now at 00:00
  → state 2, else state 9.

## Startup.app (question 2) [I]

- Not traced: Startup.app has no plain strings about the RTC or time.
- [I] S60 2nd edition Startup asks DosServer/SysUtil for the RTC status. The DSY reaches the
  kernel side, whose answer is rtc vtable +0x14 `IsValid()` = `+0x3C`.
- [I] Notification 4 (from the invalid path) probably also tells the user side the clock is unset.
- To check cheaply, run a gdb hw breakpoint on 0x50340A60 at the moment Startup decides. The
  caller is the decision path. If `+0x3C` is 1 and the query still shows, look at Startup's
  SharedData keys next.

## Board change (question 3)

In `ngage.c` (CBus model, nokia6600 board; the N-Gage uses the same rtc code):

1. **dev 1 reg 0x04: start at 0xC0** (the driver's own default), not 0. Keep last-written
   semantics: the invalid path writes 0 and the driver writes 0xC0. Mark it written, so that a
   warm restart keeps it. NgageCbus has no reset handler, so the registers survive a guest reset
   [V ngage.c].
2. **dev 1 reg 0x01 (days)**: 0 (≠ 0xFF), which it is already.
3. **dev 1 reg 0x15: keep 0x0010.** Do not set bit 3, which is the alarm wake path. 0x0018 was
   the wrong experiment.
4. **Host time** (optional, for a live clock):
   - Serve dev 1 regs 0x11/0x12/0x13 (read-only, type 1) as the host's local seconds, minutes
     and hours at read time, with reg 1 = 0.
   - Make the stand-in's hand-over pass `time1 = time2 = TTime(host local date, 00:00:00)`
     instead of `[rtc+0x10]`, which may be 0. Give it from the board, e.g. two RAM words the
     board fills, or a NOS-device register.
   - TTime = microseconds since 0001-01-01: `(days_since_0001_01_01 * 86400) * 10^6`. For a
     Unix time, that is `(unix_secs + 62135596800) * 10^6` at midnight local time.
   - `time3` (the third pointer) can stay the same value or be 0 (no alarm).
5. [I] For the clock the user sees, the kernel's system time must also be the host time. rtc.dll
   only reads it at init (0x50341184). If the home screen still shows the old time after (4),
   set the kernel time from the stand-in (the same TTime plus the time of day) before rtc.dll
   initialises.

Expected result: `valid` = 1 at init. The hand-over leaves it at 1, returns 0 (state 9), and
rtc.dll takes 0x5033F250's kept-clock branch (notifications 3, 5), with no RTC reset and
[I] no time query.
