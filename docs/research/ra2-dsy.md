# Nokia 9500 (RA-2): what Starter waits for in DosServer / Nokia.dsy

Static reading of `eka1/ra2/core/ROM.bin` (objdump; no emulator run). [V] = read in the code,
[I] = inferred. Goal: just enough to answer the blocked call from the ISI reply table.

## The blocked chain

| Step | Code | What |
|---|---|---|
| Starter 0x5077b4ec | calls SysUtil export at 0x5009bee4 (via IAT 0x5009cd6c) [V] | |
| SysUtil 0x5009bee4 | DSClient 0x5016986d (connect), 0x50169445 (subsession, type **0x65**), 0x50169461 (fn **300**, TPckg<TInt>), 0x5016948d (fn **301**, TPckg<TInt16>, initialised to 100) [V] | if fn 301 returns -1 the value becomes 100 [V] |
| DSClient 0x5016948c | `SendReceive(0x12D, {&TPckg<TInt16>})` — the frame 0x501694af [V] | |
| DosSrv 0x50167084 | subsession 0x65 -> plug-in factory secondary vtable 0x50175790 slot 3 -> thunk 0x50174c0d -> 0x5016e208: new 0x20-byte object, ctor 0x5016e340, vtable 0x50175638 [V] | 0x64/0x65 = SysUtils/Helper style services [I] |
| Nokia.dsy 0x5016e3dc | **GetSWStartupReason(TInt16&)**: writes 100, opens a logical channel to **`Custom.ldd`** (0x5016e120: LoadLogicalDevice "custom", DoCreate "Custom"), `DoRequest(2, status, TPtr8(value,2))`, `User::WaitForRequest` — return address 0x5016e421 is this frame [V] | so DosServer's own thread is blocked in the kernel driver, not on its ISI object 0x52 |
| (0x5016e37c, fn 300) | same, DoRequest(0, 4 bytes) — already completed [V] | |

The "Nokia.dsy frames" 0x50174e90 etc. are partly stale words (0x50174e90 is the descriptor
`"Custom"`) [V].

## Custom.ldd (0x500aadb0, ARM) is ISA object 0x69

Not EStart: Custom.ldd opens a kernel ISA channel (Isa_kernel_if.dll) as object **0x69** and
subscribes to exactly `15 65, 19 02, 19 0B, 23 03, 23 05` (0x500abb90..0x500abc80) [V].
Accessors (Isa_kernel_if.dll) [V]: resource = byte 3, transaction id = byte 8, message id = byte 9,
"data" descriptor = byte 10 on (length = ISA length - 4).

DoRequest (0x500ab624, requests 0..5) [V]:

| Req | Sends (res msg {data}, trans) | Reply handler |
|---|---|---|
| 0 GetStartupReason | `15 02 {00 00}`, trans 0x96 (MTC_STATE_QUERY) | MTC handler 0x500aca74 — already answered by our `15 02 -> 65` line |
| 1 (u16 arg) | 0x500abf60, res 0x23 [I msg 02] | res-0x23 handler, msg 03: data[0]==0 -> KErrNone, else trans 0x99 -> KErrGeneral |
| **2 GetSWStartupReason** | **`23 04 {00 D4 00 00 00 00 00 00 ...}`, trans 0x9A** (0x500ac0e8; PM-style read of field 0x00D4 [I]) | **0x500acb98** |
| 3 | `19 0A {00 00}`, trans 0x98 (0x500ac288; also sent from 0x500ad6f0/0x500ad8f0) | 0x500acd7c (trans 0x98), not read |
| 4 | 0x500ad828, not read | |
| — | `D1 04 {00}`, trans 0x0F at open (0x500ad90c) and again after a res-**D2** trans-0x0F reply (0x500ad53c -> 0x500ad9b0 parses it) [V]; RTC/time [I] | not on the Starter path [I] |

Receive dispatch 0x500ad4c8 is by resource, then by **transaction id** (byte 8), so replies must
echo the request's transaction id [V] (our board does: ngage_isi.c `m->b[8] = req[8]`).

### Reply that completes request 2 (handler 0x500acb98) [V]

- resource 0x23, message id (byte 9) **0x05**;
- data[0] (byte 10) = **00** (non-zero -> request completes with KErrGeneral, SysUtil then uses 100);
- data[14..15] (bytes 24..25) = **u16 LE SW start-up reason**, copied to the client's 2 bytes,
  request completed KErrNone;
- any other message id with trans 0x9A -> KErrGeneral (-2) (DSY still returns; also unblocks).
- Value: 100 (0x64) = normal start (TSWStartupReason ESWNone; SysUtil's own default) [I strong].

## Reply / ind lines

```
reply 23 04 05 00 00 D4 00 00 00 00 00 00 00 00 00 00 00 64 00   # Custom.ldd req 2 (DSY GetSWStartupReason): data[0] 00 = OK, data[14..15] u16 LE = 100 normal [V layout; bytes 1..13 I]
```

No `ind` is needed for this block. Not needed for Starter's current call [I]: `19 0A` (req 3,
handler 0x500acd7c unread) and `D1 04` (reply arrives as resource D2, trans 0x0F; parser 0x500ad9b0
unread). If something later waits on them, read those two handlers.

## Not done (out of budget)

- The DSY's ISI object 0x52 indication handlers (15 C0, 09 EF, 08 10 layouts for the 9500): not
  read; Starter's block does not go through object 0x52. The 6600 values stay [I].
- The 0x52 subscription bytes: Custom.ldd's own list is plain `res msg` pairs with no count byte
  (buffer created with 10 = its byte count) [V]. Read the same way, the 0x52 list does not split
  cleanly; skipping the first byte gives `17 21, 0A 1E, 1C 12, 1C 15, 09 EF, 09 ED, 08 10, 15 C0,
  14 0B, 14 64, 1C 50, 1C 52, 0B 17`, which has the expected SIM 09 EF/ED, security 08 10, MTC 15 C0
  pairs [I]. Check the DSY's builder before relying on either reading.
