# P1i start-up: CStarter's parameter reads and what follows them

Sony Ericsson P1i R6E30 (`se-p1i`), static reading of startuphandler.aps (code f81d2ac8), ccpustorage.dll
(f8154608) and EStart.exe (f80af9a8), plus the serial log of the boot at 416173c
(`build/sydb-p1it/serial.log`). Follows `p1i-startup-callbacks.md` and `p1i-bserver-fn3.md`.
[V] = read in the disassembly or the log, [I] = inferred.

## Summary

- The reads are CStarter's LCD/HAL set-up. Each is a `{id, size}` pair from a table in startuphandler,
  sent by ccpustorage as IParameterStorage **m11 (size query) then m5 (read)**, wire id = **id | 0x100000**.
- **Empty answers are fine** [V]: every consumer copes with a short or empty buffer, and every error path
  goes to the same next state. Our replies need no change to get past CStarter.
- After the last read (0x6d5) CStarter needs **no CCPU traffic**: it waits for `system_animation_player.exe`
  to exit, then tells the start-up handler, which does `RProperty::Set(0x10203113, 0, 0)`. **EStart**
  waits for that property (since 1.289 s on the log) and then starts the rest of the boot. That never
  shows on the log, so the stall is on this local path, not on the CCPU. Which link breaks is a runtime
  question (section 4).

## 1. CStarter::RunL (f81d407c), states at +0x1c [V]

`RunL` starts with `User::LeaveIfError(iStatus)`. `RunError` (f81d41e6) sends states 4..11 to 12 and
re-runs the others. A failed read therefore goes to the same place as a successful one.

| state | action | next |
|---|---|---|
| 1 | start process (name from resource), Logon | 2 |
| 2 | ccpustorage #5 `/ifs/settings/acoustic/handheld.apf`, 10000 | 3 |
| 3 | read **{0x6c9, 2}** | 4 |
| 4 | load `lcd_ctrl` / open `LCDCTRL`; failure -> read {0x6d5,4} -> 11. Ok: **ctl 13** (a1 = byte 1, a2 = byte 0 of the 0x6c9 data, 0 if missing); read **{0x6aa, 8}** | 5 |
| 5 | open LCDCTRL again (failure -> 12); **ctl 12 (0, &data8)**; read **{0x6ab, 8}** | 6 |
| 6..9 | ctl 12 (1..4, data of 0x6ab..0x6ae); read 0x6ac..0x6af (8 bytes) | 7..10 |
| 10 | ctl 12 (5, data of 0x6af); close LCDCTRL; `HAL::Get(69)`: ok and != -1 -> 12, else read **{0x6d5, 4}** | 11 |
| 11 | if data length >= 4 and value <= 99: `HAL::Set(69, value)` | 12 |
| 12 | f81d3e9c: if `system_animation_player` (+0x2c, started by the `DspBootThread` thread at construction) still has ExitType Pending: **Logon and wait for its exit** | 13 |
| 13 | `iObserver->slot0()` (MStarterObserver -> CStartupHandler f81d3424) | 0 (idle) |

The `{id, size}` table is at f81d5a44 (`aa06 0800`, `ab06 0800` ... `c906 0200`, `d506 0400`) [V]. The log
matches it exactly: ctl 13, then ctl 12 with 0..5, then the 0x6d5 pair [V].

## 2. IParameterStorage m11 / m5

- ccpustorage's send (f8154b22) adds `1 << 20` to the id [V], so `0b000000 c9061000` = m11, id 0x1006c9.
- m11: input `{11, id}`, output 12 bytes `{status, w1, w2}` [V sizes]. w1 is the parameter's size [I].
- m5: input `{5, id, w}`, where w = 0 on the log, presumably m11's w1 or an offset [I]. Output is variable,
  with a reserve of 524 = 4 + 8 + 512 [V size]. The layout is probably `{status, length, data...}` [I].
- Our answers (m11 -> 12 zero bytes; m5 -> status 0 and no output bytes) complete each read. The next
  LCDCTRL control follows each m5 reply within 3 ms [V log]. CStarter then works with an empty buffer:
  ctl 13 gets 0/0, ctl 12 gets an empty 8-byte buffer, and state 11 skips `HAL::Set` [V].

Parameter meanings and safe values for an offline phone. All of them can stay empty.

| id | size | used as | default |
|---|---|---|---|
| 0x6c9 | 2 | LCDCTRL ctl 13 args (byte 1 -> a1, byte 0 -> a2) [V] | `00 00` (current) |
| 0x6aa..0x6af | 8 each | LCDCTRL ctl 12 (index 0..5, 8-byte block): LCD init/calibration [I] | empty; our LCDCTRL stand-in ignores it |
| 0x6d5 | 4 | `HAL::Set(EAttr 69, v)` if v <= 99 (a 0..99 level, e.g. backlight/contrast [I]) | empty (skip), or `32 00 00 00` = 50 |

If real data is ever wanted [I]: m11 -> `00000000 <N> 00000000`; m5 -> `00000000 <N> <N data bytes>`.
Check on one read first that the bytes land in CStarter's `TBuf8<8>` at +0xd0.

## 3. After the last read: the start-up handler and EStart [V]

- CStarter state 13 -> CStartupHandler f81d3424: if its state is 2 (ISystemControl m5 already answered,
  which happened at 1.272 s on the log) -> **f81d3248**:
  `RProperty::Set(TUid 0x10203113, key 0, 0)`, the IMEI/COPS handler when the reason is 2, and
  f81d4a14 if +0x140 is set. The return value of `Set` is ignored.
- EStart (f80b1458): f80b1316, then f80b12d0 (start a process, wait for its exit), then f80b1264:
  `Define(0x10203113, 0, EInt)` with read policy always-pass and write policy one capability (5 =
  WriteDeviceData [I numbering]), `Attach`, `Subscribe`, then a synchronous message 8 to `!APS[10200b14]`
  (on the log: 0.773 sent, 1.270 completed), then **f80b11c8: `WaitForRequest` on the subscription, then
  `Get`**. Value 0 -> f80b0d3a: a `RProperty::Set`, `RProcess::Create` + Resume, `RFs::DriveList` (the
  rest of the boot). Nonzero -> EStart returns.
- On the log nothing from EStart follows 1.289 s, and no Loader request comes from EStart. So either
  CStarter never reaches state 13, or the `Set` does not wake EStart.

## 4. What to check next (runtime, cheap)

1. `system_animation_player` alive? (`sydb ps`). It ran at 1.21 s: MPEG4DECDRV ctl 8 returned -10, its
   channels and file sessions closed at 1.217 s, so it looks like it exited [I]. If it is still alive,
   CStarter waits in state 12 (Logon). Make the MPEG4DECDRV stand-in complete or fail so that it exits.
2. Hardware breakpoints in APS (`sydb dbg`): f81d4192 (state 11), f81d41cc (state 12), f81d41d6 (state 13),
   f81d3248 (`Set`). The last one hit tells which link breaks.
3. P&S 0x10203113 / key 0 after the stop (defined? value?). If f81d3248 ran but EStart sleeps, check the
   `Set` return value in r0 at f81d325a (-46 = APS lacks WriteDeviceData under our platsec).
4. Unrelated to this path, still open: at 1.318 s APS sent op 0x1F4 with IID {85385620-...}
   (**IUsbChargingControl**) + `0a000b00`. BServer turned it into a link-1 **type 2** message, which the CCPU
   model logs as "not answered". APS's request for it is never completed [V log]. It runs on another
   subsession, so it does not block CStarter [I]. If step 2 shows CStarter done and APS still idle, answer
   this message next (`p1i-bserver-fn3.md`, op 0x1F4 handler f81a43ee).

## 5. CCPU changes proposed

None needed for the parameter reads: keep m11 -> 12 zero bytes and m5 -> `{9, 0, req, 0}`. Optionally, for
realism only, add 0x6d5 -> 4 bytes `32000000`, using the format in section 2 [I]. The stall is past the CCPU.
