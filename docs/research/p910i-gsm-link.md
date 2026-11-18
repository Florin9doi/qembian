# P910i: the GSM link (EStart -> OSE server -> elh4drv -> UART 2)

Static reading of `eka1/p910i/core/ROM.bin` (VA = 0x50000000 + offset). No emulator runs.
[V] = read from code, [I] = inferred / best guess. Budget-limited research (CLAUDE.md rules):
enough to build a board-side peer, not a full protocol.

## 0. Summary

- `OSE_SEM` does **not** depend on the modem [V]. eose signals it as soon as its server object is
  constructed; the construction makes one synchronous link-sync attempt that times out after 1 s
  and is then retried in the background. With no peer at all, EStart passes the OSE_SEM wait about
  1 s later, as long as the UART exists (no panic from the LDD open).
- The message "EStart didn't recive StartSignal take dump from GSM plat" is a **24 s watchdog**
  that EStart arms **after** OSE_SEM, through `pwrnotif.ldd` [V for the timer, I for the meaning].
  The "StartSignal" is an OSE signal from the GSM side, routed eose -> Linnea plug-in
  (`System/Data/Linnea/startuphandler.lsu`, which opens pwrnotif) -> `pwrnotif.ldd` -> EStart [I].
- Link layer (elh4drv.ldd, "link handler 4") is simple [V]: raw 16550 on UART **unit 2**, sync
  handshake `"L1"` (phone) / `"LA"` (GSM), then length-prefixed frames. Cheap to emulate.
- What is *not* cheap: the OSE signal layer above (startuphandler's expected StartSignal, then
  erigsm traffic). Cheapest path for boot: patch the watchdog arm (section 7) and run the link
  peer only for sync, then grow the reply table.
- **Update (section 6): the StartSignal is decoded.** One frame after sync carrying OSE signal
  **0xC384** (or 0xC389) completes EStart's wait; no OSE emulation needed beyond that.

## 1. Who signals OSE_SEM

EStart (Thumb, image 0x5009c760):
- `0x5009d248` [V]: `RSemaphore::CreateGlobal("OSE_SEM", 0)` (lit 0x5009d29c -> "OSE_SEM"),
  `RProcess::Create("Z:\System\Programs\startosesvr.exe")`, Resume, `sem.Wait()` (no timeout),
  Close. Called from `0x5009d464`.
- startosesvr.exe (216 bytes, Thumb, 0x508a38b0) only calls through an import table [V]; the
  callee is eose's export at `0x508a5a64` (pointer at 0x508a6260 in eose's export area) [I].

eose.dll (Thumb, code 0x508a3a54..0x508a6230):
- `0x508a5a64` [V]: if server "OSE Server" is already found -> return; else open global
  semaphore `OSE_SEM` (lit 0x508a5ae4 -> 0x508a60ac) and call `0x508a59e8(sem)`.
- `0x508a59e8` [V]: install CActiveScheduler, `TRAP(0x508a4f30)` (construct + start server
  "OSE Server"), then **`sem.Signal()` (0x508a5a4c) and `CActiveScheduler::Start()`**. Errors ->
  panic "OSE server panic" (0x508a5ae8, codes 2..5).
- `0x508a51a4` (server ConstructL) [V]: `User::LoadLogicalDevice("Z:\System\Libs\elh4drv.ldd")`
  (KErrAlreadyExists accepted), `RBusLogicalChannel::DoCreate("elh4drv", ver, NULL,
  **unit = 2**, NULL, NULL)`, then `DoControl(0, &6)` = **set speed index 6**.
- `0x508a5170` [V]: creates reader AO (prio 20, `0x508a3b2c`, posts `DoRequest(0 = read,
  buf 1024)`) and writer AO (prio 10, `0x508a3bd8`), then calls `0x508a3c60` (writer sync) inline.
- Writer sync `0x508a3c60` [V]: counter at +0x428. Count <= 14: `DoRequest(1 = write, len 0)`
  plus `RTimer::After(1 000 000 us)`, `User::WaitForRequest(write, timer)`; timer first ->
  `DoCancel(2)`. RunL `0x508a3d5c`: status != KErrCancel -> link up (callback(0));
  KErrCancel -> retry, up to 16 tries, then callback(1) = link failed [V]. Count == 15: write with
  no timer (waits forever) [V].

Hardware: elh4drv `DoCreate` (0x508a1b08) [V]: unit u in 0..5 -> registers at virtual
`0x58001000 + u*0x20` (phys **0x58000000 + u*0x20**), interrupt id `u + 5`, bound by name
`IrqUart{u+1}`. **Unit 2 -> phys 0x58000040, virt 0x58001040, IRQ id 7, "IrqUart3".**
Not the serial bridge, not the 0x18000000 external chip.

## 2. UART programming and link framing

Register use by elh4drv (byte registers, 4-byte stride, 16550 layout) [V]:

| off | use |
|---|---|
| +0x00 | RBR read (RX) / THR write (TX) |
| +0x04 | IER: `|= 7` after create (RDA, THRE, RLS) |
| +0x08 | IIR read, low nibble: 0x2 THRE, 0x4 RX data, 0xC RX timeout; others ignored |
| +0x14 | LSR: bit0 DR (RX loop), bit6 TEMT (polled before re-programming) |

Line set-up `0x508a21dc` [V]: wait LSR.TEMT, then a chain of kernel UART helpers (imports
through EKern's table at 0x5003bxxx) with (unit, divisor), (unit, 0, 3, 0), (unit,1), (unit,3),
(unit,0), ... i.e. divisor + 8N1 + FIFO enable [I: helper meaning]. Divisor table at
0x508a270c: `C0 60 30 18 0C 08 04 02 01` -> index 6 = **divisor 4** (28800 baud at 1.8432 MHz/16,
or 115200 if the clock is 7.3728 MHz) [V table, I baud]. The board can ignore baud/LCR/MCR.
No CTS/RTS or GPIO handshake is read by elh4drv [V: no MSR/MCR reads in the driver].

ISR `0x508a22a4` [V]:
- IIR = 0x4: reads **exactly 60 bytes** from RBR without checking LSR (FIFO trigger 60, a
  64-byte FIFO). IIR = 0xC: reads while LSR.DR. -> **Board: report 0xC whenever RX data is
  pending, 0x4 only with >= 60 bytes** (simplest: always 0xC).
- IIR = 0x2: pushes up to 64 more TX bytes to THR; when the TX buffer is empty and the link is
  synced, completes the write.
- RX ring 2048 bytes; overflow -> driver panic 1.

Sync handshake [V]:
- Write of length 0 while not synced -> driver sends the Pascal string at 0x508a2705:
  **`4C 31` ("L1")**. The write completes only after sync.
- RX while not synced: scans for **`4C 41` ("LA")**, table at 0x508a2708. Found -> synced
  (flag +0x7c = 1, never cleared afterwards), pending write completed.
- So: phone sends "L1" every 1 s until "LA" comes back; reply `4C 41` once.

Frames after sync (RX parser `0x508a1f80`) [V]:
```
off 0      : b0              (frame type / channel?)                [I meaning]
off 1..2   : len, little endian
off 3..    : len payload bytes, then 1 more byte (b_end)            -> delivered to eose:
             b0, lenLo, lenHi, payload[len], b_end  = len + 4 bytes
off len+4  : 1 trailing byte, consumed and NOT delivered            [I: checksum/terminator]
```
- Delivered size = len + 4, must be <= 1024 (else driver panic 3). The parser needs len + 5
  bytes buffered. No CRC check and no acks in elh4drv; anything is up to eose and above.
- TX: client data written raw (<= 1024 bytes per write), no framing added by the driver [V].
  So phone frames have whatever layout eose's clients build; expect the same shape.

eose receive side: corrected in section 6.3 [V]: the LE16 at buffer offset 4 *is* the OSE
signal number (0xC350..0xC50E); "+0x3CB0" is only a bitmap index, and the 28-entry table at
0x508a60ec lists signal numbers as is (0xC424, 0xC3B0, ...). Unmatched frames are
queued until a client subscribes. Clients address "processes" by a byte b in 0x94..0xE3
(table index ~b - 0x1C, 80 entries at +0x310) [V arithmetic, I meaning].

## 3. First messages and replies

| phone sends | GSM reply | effect |
|---|---|---|
| `4C 31` ("L1"), repeated every ~1 s, up to 16 times | `4C 41` ("LA") | link synced; eose writer RunL -> link-up callback [V] |
| frames after sync from Linnea plug-ins / erigsm (not read) | ? | [I] |

The StartSignal itself: an OSE signal from GSM, layout not decoded (would need
startuphandler.lsu + pwrnotif controls 14/44/69/84 = probable notifier codes [I]). Out of
budget; see section 6.

## 4. Interrupts and status the board must provide

- IC line for interrupt id 7 (`IrqUart3`) raised while (IER & 1 and RX pending) or (IER & 2 and
  THR empty). docs/research/p910i-soc-map.md has the IC1/IC2 numbering; id 7 is the kernel id [V].
- IIR priority: RX (0xC) before THRE (0x2); IIR bit0 = 1 when nothing pending.
- LSR: bit0 = RX pending, bits 5/6 = 1 (TX always empty: the peer absorbs bytes instantly).

## 5. Proposed board peer (style of nokia9500_xbus.c)

```c
/* p910i_gsmlink.c: 16550 at phys 0x58000040 (unit 2), stride 4, IRQ "IrqUart3" (id 7) */
enum { LS_UNSYNC, LS_SYNC };
typedef struct { uint8_t ier, lcr, rx[2048]; int rh, rt; int state; uint8_t tx[1100]; int tn; } GsmLink;

static void rx_push(GsmLink *s, const uint8_t *p, int n);   /* append, raise irq */
static uint8_t iir(GsmLink *s) {
    if ((s->ier & 1) && s->rh != s->rt) return 0xC;          /* RX timeout: driver reads while DR */
    if (s->ier & 2) return 0x2;                              /* THR empty */
    return 0x1;
}
static void on_tx_byte(GsmLink *s, uint8_t b) {
    s->tx[s->tn++] = b;
    if (s->state == LS_UNSYNC) {
        if (s->tn >= 2 && s->tx[s->tn-2] == 'L' && s->tx[s->tn-1] == '1') {
            rx_push(s, (const uint8_t *)"LA", 2); s->state = LS_SYNC; s->tn = 0;
        }
        return;
    }
    /* synced: frame = b0, lenLo, lenHi, payload[len], b_end, trailer  (len+5 bytes) [I] */
    if (s->tn >= 3) {
        int len = s->tx[1] | s->tx[2] << 8;
        if (s->tn == len + 5) { log_frame(s->tx, s->tn); reply_table(s); s->tn = 0; }
    }
}
/* reply_table: match on b0 + LE16 signal at offset 4; first entries to be filled once the
   phone's first frames are logged (do NOT guess: log them in a run). */
```
Mind: THRE interrupts with IER bit1 set fire continuously; that matches the driver (it only
writes while it has TX data, and clears nothing). If that storms, raise THRE once per write burst.
Also accept "L1" again after sync (eose writes len-0 only while unsynced; harmless).

## 6. StartSignal

Static reading (second pass). EStart = `0x5009c760`, ereq.dll = `0x508c12b0`, swbp.dll =
`0x508a6280`, eose.dll = `0x508a39f0`.

### 6.1 What EStart waits for

`0x5009d464..0x5009d5dc` (after OSE_SEM) [V]:
- `ereq.dll` init (`0x508ca49d`), then an object `r6` (16 bytes: CActiveScheduler subclass, vtable
  `0x5009ef38`, + an ereq observer interface at `r6+4`, vtable `0x5009ef44`; `+8` = "started" flag,
  `+12` = watchdog CPeriodic) is installed as the active scheduler.
- `r7 = CEReq::NewL` (`0x508ccfe9`); **two subscriptions with the observer `r6+4`, id 4**:
  `0x508cdd99` (sub-handler at CEReq+0xC4) and `0x508cdb11` (sub-handler at CEReq+0x7C).
- pwrnotif `DoControl(4,&flag)`; if flag != 0 -> 24 s CPeriodic watchdog (section 7) [V]. Whether
  the flag is 1 on our board depends on pwrnotif (not re-checked) [I: yes, since the message
  "didn't recive StartSignal" is the known symptom].
- `CActiveScheduler::Start()` (`0x5009d5b8`). The only things that can run: the watchdog and the two
  ereq sub-handlers. No pwrnotif notification, no property, no startuphandler.lsu involved at
  this stage [V]: the *.lsu plug-ins are loaded only **after** the StartSignal (6.4).

Observer interface (49 slots, all default stubs that panic except two) [V]:
- slot 29 (`vptr+0x7C`) and slot 48 (`vptr+0xC8`) -> thunks `0x5009e661` / `0x5009e64d`:
  `this -= 4; if (flag == 0) StartUp(this)` (`0x5009df94`). Arguments are ignored. Second and later
  calls do nothing (flag set) [V].

### 6.2 ereq sub-handlers -> which OSE signal

Both sub-handlers are CActives (`0x508ea1b0` / `0x508ea1f4`, vtables `0x508f15a8` / `0x508f15c0`)
holding a SIGSELECT pointer at +0x288 and the swbp session at +0x28C [V]. On the first subscriber
they call swbp `0x508a6f09` (= OSE-server fn 3, "subscribe sigsel") and `0x508a6f49` (= fn 5,
"receive", into a 0x244-byte buffer at +0x3C, iStatus at +4) [V].

SIGSELECT arrays are u16 `{count, sig...}` [V: swbp `0x508bb254` builds the descriptor as
`2*count+2` bytes]:

| sub-handler | sigsel at | signal | RunL | observer slot called |
|---|---|---|---|---|
| CEReq+0xC4 (`0x508cdd99`) | `0x508ee5ae` | **0xC384** (50052) | `0x508e8824`, payload not parsed (`0x508e6170` always ok) | 48 |
| CEReq+0x7C (`0x508cdb11`) | `0x508ee562` | **0xC389** (50057) | `0x508e88c0`, decodes payload byte at buf[6] (`0x508da9a8`); result only changes the error arg | 29 |

RunL calls the observers when `iStatus == KErrNone` [V], so **either signal starts EStart**. 0xC384 is
the simpler one (no payload decoded). Both are in eose's 28-entry "keep" list.

### 6.3 eose receive path (frame -> client)

- Reader AO RunL `0x508a3b98` hands the **whole delivered buffer** (b0, lenLo, lenHi, payload[len],
  b_end) to the server's receive observer (`0x508a5f58` -> `0x508a52ac`) [V].
- `0x508a52ac` [V]: needs length > 4; `sig = LE16 at buffer[4..5]` (= payload[1..2]).
  If `0xC350 <= sig <= 0xC50E` -> `0x508a534c` (signal delivery). Otherwise it is a control frame
  keyed by **buffer[0] (b0)**: `> 0x83` -> `0x508a5494`, `== 0x82` -> `0x508a5578`, `== 0x83` ignored
  [V branch, I meaning]. So b0 is irrelevant for signals in the 0xC350.. range.
- Delivery `0x508a534c` [V]: for each of 16 sessions, if the session's bitmap (+0x6D, bit index
  `sig - 0xC350`, set by fn 3) has the signal: complete its pending fn-5 receive by writing the
  whole buffer into the client's descriptor, or queue a copy if none is pending. Then, if the
  signal is in the 28-entry list at `0x508a60ec` (bitmap at server+0x2D8), it is also kept in the
  server's list at +0xB0 [V], presumably for sessions that subscribe later [I].
- The "+0x3CB0" of section 2 is only the bitmap index (`sig + 0x3CB0 = sig - 0xC350` mod 2^16);
  signal numbers travel as is [V].
- b_end and trailer: never checked (elh4drv drops the trailer, eose ignores b_end) [V for the
  paths read]. payload[0] is not read on this path [V]; its meaning (receiver/process byte?) [I].

The 28 signals of `0x508a60ec` ({28, ...}): C424 C3B0 C3B4 C3B5 C3B9 C3BD C441 C442 C351 C352 C3BA
C358 C389 C39C C38F C455 C454 C38A C3CA C384 C411 C394 C3A8 C3EE C3EF C390 C391 C3E5 [V values,
names unknown].

### 6.4 Frame to send, and when

```
00 03 00   00 84 C3   00   00
b0 len=3   payload    b_end trailer
           [0]=00, sig=0xC384 LE
```
- Delivered buffer `00 03 00 00 84 C3 00` (7 bytes): length > 4, buffer[4..5] = 0xC384 [V].
- b0 = 0x00, payload[0] = 0x00, b_end = 0x00, trailer = 0x00 [I: not checked on this path, any
  value works]. Alternative: `00 04 00 00 89 C3 00 00 00` (0xC389, payload[3] = 0) [I value of
  payload[3]].
- When: unsolicited; the phone sends no request for it (matches the run: nothing after sync). EStart
  subscribes right after OSE_SEM, i.e. after the 1 s link sync, so send it **a little after
  "LA"** (e.g. 0.5-1 s; the first "L1" retry loop and EStart's subscription are in the same
  window). Sending it twice is harmless for EStart (flag) [V]; eose keeps a copy of listed
  signals for late subscribers [I], so early sending probably works too. Recommendation: send
  once ~1 s after sync and once more ~3 s after sync.

What happens next (`0x5009df94`, StartUp) [V calls, I meanings]:
1. watchdog CPeriodic deleted, flag = 1;
2. housekeeping `0x5009e570` / `0x5009d17c` (files `C:\boom`, `C:\SMTmpOn`,
   `C:\system\data\AudioCtrlSilentModeOn.xyz`, `c:\system\data\radio`) [I];
3. starts **"LinneaStartUpSvr"** with plug-in directory `Z:\System\Data\Linnea\` and pattern
   `*.lsu` (`0x5009df00`) -> loads batpwr.lsu, gsmbuttoneventhandler.lsu, startuphandler.lsu
   (all XIP in ROM: `0x50d74000`, `0x50d7e230`, `0x50d84760`) [V strings, I loader]. Failure ->
   restart 7, "EStart Failed to start StartUpHandler, re-flash PDA, format drive".
4. new active scheduler (`0x5009df10(-100)`, Install, Start) [I].
The *.lsu plug-ins then talk to the GSM side over ereq/OSE: the next frames the phone sends should
appear on the link after this signal; log them and grow the reply table (section 5).

Board peer change (section 5): after sending "LA", schedule `rx_push(s, start_sig, 8)` with
`static const uint8_t start_sig[] = {0x00,0x03,0x00, 0x00,0x84,0xC3, 0x00, 0x00};` at
+1 s and +3 s (QEMU timer). If 24 s later the watchdog message still appears, check with a
breakpoint at `0x508a52ac` (frame reaches eose) and `0x5009df94` (EStart started).

### 6.5 Frame layout, corrected after the run; first request and its reply

[V by run]: the StartSignal frame of 6.4 works. EStart's StartUp ran at 1.89 s, and then
appsvr/Sysagx/eventsvr started. Then startuphandler.lsu (inside appsvr) blocks on a synchronous
OSE receive (swbp `0x508bb3f8`, server function 1, which is a blocking receive with a sigsel).
The phone sends `62 02 00 E3 98 43`.

**Frame layout (both directions)** [V code + run]:
```
[0]    dst pid (byte)      phone->GSM: GSM-side process (0x62); GSM->phone: Symbian-side pid (0x94..0xE3)
[1..2] len, LE16 = OSE signal size in bytes, including the 2-byte signal number
[3]    src pid (byte)
[4..]  OSE signal: signo LE16, then len-2 data bytes
GSM->phone only: 1 extra trailer byte (elh4drv parser needs len+5; value unchecked)
```
This replaces the "b0 / payload / b_end" reading of section 2. In the StartSignal frame,
`00 03 00 00 84 C3 00 | 00` is dst 0, len 3, src 0, signal C384 + 1 data byte, then the trailer.
Phone->GSM frames carry no trailer (6 bytes = len+4 [V by run]).

Routing of GSM->phone frames in eose `0x508a52ac` [V]: signo in 0xC350..0xC50E goes to every
subscriber of that signal, and dst is ignored. Otherwise, when dst > 0x83, it goes to the session
whose pid = dst (`0x508a5f10`), matched against that session's pending receive for the signo
(`0x508a496c`) (`0x508a5494`). dst 0x82 is another control path, and 0x83 is dropped.

**Request seen**: `62 | 02 00 | E3 | 98 43`. That is signal **0x4398**, size 2 (no data), from
Symbian pid 0xE3 to GSM process 0x62. It comes from the swbp RPC stub `0x508b0c10` [V]:
`alloc(2, 0x4398)`, `send(pid = *(0x508bf529) = 0x62)`, then `receive(sigsel 0x508bff64 =
{1, 0x4F4E})`. The stub is reached by ereq `0x508d8ed8`, which startuphandler calls at
`0x50d849b2`. The stub skips the send when the le32 at `0x508ea880` is 1; it is 0 in ROM.

**Reply the GSM side must send** (answer to that frame):
```
E3 | 04 00 | 62 | 4E 4F | VV | SS | 00
dst  len=4   src  signo   val  stat trailer
```
- signo **0x4F4E** [V sigsel]. dst = the request's src (0xE3) [V: dst selects the session;
  echo the request's byte, don't hard-code it]. src = 0x62 [I, not read].
- Fields read [V `0x508b0c7c`, I offsets relative to the signo]: `sig[2]` = VV, the value
  returned to the caller; `sig[3]` = SS, the status. `0x508d9bb0` maps SS 0..9 one-to-one, and
  **0 = OK** (with any other value ereq returns an error). `0x508da9a8` accepts VV 0..4; anything
  else gives -11.
- startuphandler at `0x50d849b6`: VV == 2 sets its flag (+0x1C) = 1 [V]. The meaning of 2 is not
  known (maybe a non-normal start-up reason such as charger or alarm [I]). Recommended
  **VV = 0, SS = 0**; try VV = 1 if start-up takes a strange path.
- So: `E3 04 00 62 4E 4F 00 00 00`.

**General peer rule**: parse phone frames as `dst, len16, src, signo16, data[len-2]` (no
trailer). Answer with `src, len16, dst, reply signo16, data, 00`. Keep a table signo -> reply.
First entry: `0x4398 -> 0x4F4E {VV=0, SS=0}`.

Next waits [I, not read]: startuphandler then calls ereq `0x508cb4d9(obj, 3, observer)`
(`0x50d849e2`), which is probably another request answered by an asynchronous signal. It also
subscribes through ereq `0x508cdcb5` (id 1) at construction. Log the next phone frames.
batpwr.lsu and gsmbuttoneventhandler.lsu were not read.

### 6.6 The next four requests (after the start-up logo)

[V by run]: the 0x4398 -> 0x4F4E reply worked. Four phone frames are still unanswered. All four
come from swbp.dll stubs of the same shape as `0x508b0c10`:
- `alloc(size, signo)`, fill in the request fields, `send(pid byte from the table at
  0x508bf4xx/5xx)`.
- If the le32 mode word passed in is 1, the stub returns 1 without waiting. The ereq caller then
  sets up an asynchronous receive instead. It is 0 here, so every stub waits.
- Otherwise the stub does a synchronous `receive(sigsel)`, then a per-stub decoder. 0 means OK
  (mapped by ereq `0x508d9bb0`).
Reply data offsets below are signal offsets: sig[0..1] = signo, data starts at sig[2].

| request (from the run) | swbp stub (ereq caller) | waits for | decoder reads | reply line |
|---|---|---|---|---|
| 0x0A3C, size 4, sig[3] = param (`30 04 00 E3 3C 0A 00 4B`) | `0x508a93ec` (ereq `0x508c39ac`) | **nothing**: send-only stub, returns 0 [V] | - | none |
| 0x42CC, size 8: sig[2..5] = le32 from the caller's struct+4, sig[6], sig[7] = two byte args | `0x508ad790` (ereq `0x508c7cee`, `0x508c7d8e`) | sigsel `0x508bfb4c` = {1, **0x2134**} | `0x508ad830`: signo must be 0x2134 (else status 9); **sig[6] = status**, 0 = OK [V]; sig[2..5] not read | `reply 42CC 2134 00 00 00 00 00` |
| 0x4FB0, size 7, no fields written | `0x508ac554` (ereq `0x508c73b4`, `0x508c7428`) | sigsel `0x508bfac4` = {1, **0x5014**} | `0x508ac5c0` copies sig[2..6]; always OK [V]. ereq `0x508c73a8` passes (le32 sig[2..5], byte sig[6]) on to `0x508c7b40` [V]; meaning unknown [I] | `reply 4FB0 5014 00 00 00 00 00` |
| 0x13A0, size 0x18, **no fields written** | `0x508ac2c8` (ereq `0x508c6b9c`) | sigsel `0x508bfa94` = {1, **0x13A1**} | `0x508ac330` copies sig[2..23] (22 bytes) to the caller; always OK [V] | `reply 13A0 13A1` + 22 x `00` |

Notes:
- 0x13A0: the stub fills nothing after `alloc(0x18, 0x13A0)` [V]. So the
  `32 00 00 00 "AccessTyp..."` in the request is **stale heap memory**, not a parameter name
  (probably left over from CommDb's "AccessType"). It is not a GDFS read by name. The reply is a
  22-byte block with unknown meaning; all zeros is the neutral guess [I]. It looks like a
  23-character string request (ereq `0x508c6b70` sets up a 23-byte descriptor) [I]. If start-up
  shows an empty name or rejects zeros, try ASCII text padded with 00.
- 0x0A3C needs no reply. sig[2] = 00 and sig[3] = 0x4B are the caller's byte [V stub writes only sig[3]].
- Request pids: 0x30 (0A3C), 0x61 (42CC), 0x3A (4FB0, 13A0) come from swbp's pid table
  (`0x508bf4f7`, `0x508bf528`, `0x508bf501`) [V]. Reply dst = the request's src (E1/E2/E3).
- Convention seen so far: a reply signo is often request+1 (13A0 -> 13A1), but not always
  (4398 -> 4F4E, 42CC -> 2134, 4FB0 -> 5014). For any new request, read the sigsel literal of its
  stub. To find a stub: search swbp (`0x508a6280..0x508c12b0`) for `alloc(size, signo)` =
  `bl 0x508a6ddc` with r1 = signo. The sigsel is the literal loaded right before
  `bl 0x508a6db4`, and the decoder is the `bl` right after it.

## 7. Cheapest way past the 24 s watchdog (fallback; section 6 is the real fix)

EStart after OSE_SEM (0x5009d4c2..0x5009d5d6) [V]: loads/opens `pwrnotif` (LDD), does
`DoControl(4, &flag)` with flag preset 1; **if flag != 0** it creates a CPeriodic (priority -20)
with 0x016E3600 us = **24 s** and callback `0x5009e5c9`, which deletes the timer and calls the
restart routine `0x5009e478(2 000 000, 8, "EStart didn't recive StartSignal ...")`.
The timer is deleted when the StartSignal arrives through pwrnotif [I].

Options:
1. Patch EStart `0x5009d53a`: `bne.n 0x5009d564` (`13 D1`) -> `nop` (`C0 46`): never arms the
   watchdog [V instruction, I that nothing else then waits]. One-line code patch, recommended
   for the bring-up.
2. Make pwrnotif's control 4 return 0 (driver-side), same effect.
3. Real fix later: log the frames the phone sends after "LA" and answer startuphandler's
   power-on/StartSignal OSE signal; needs a look at startuphandler.lsu (out of budget).
