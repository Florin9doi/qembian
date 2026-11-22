# Surface update server: the client opcodes and what completes a notification (808)

Scope: Nokia 808, RM-807 113.010.1508 04.01+C00.01. Binaries: `Sys/Bin/surfaceupdateclient.dll`
(XIP, base 0x80dda348) and `Sys/Bin/surfaceupdateserver.dll` (XIP, base 0x80dda8a8). Evidence:
`build/roms/nokia-808-rm807/RM-807_113.010.1508_04.01+C00.01/ghidra/SurfaceUpdateClient.dll.c` and
`.../ghidra/SurfaceUpdateServer.dll.c`; the dispatcher and the vtable targets the headless export
missed (ServiceL 0x80ddb8da, notification RunL 0x80ddb318, session dtor 0x80ddb45e, provider RunL
0x80ddb068, handlers 0x80ddb792/0x80ddb8bc) were decompiled with a second headless run that creates
functions at the vtable entries (the session vtable at 0x80ddbd88: slot 7 = ServiceL); that export is
`.../ghidra/SurfaceUpdateServer.dll.full.c`, the post-script `.../ghidra/CreateThumbFunctions.java`
(`analyzeHeadless ... -postScript CreateThumbFunctions.java 0x80ddb8db ... -postScript
ExportDecompiled.java OUT.c`). Addresses below
are absolute; module-relative = absolute - 0x80dda8a8 (server) / - 0x80dda348 (client).
No EPL source for this component is checked out locally; the names are the EPL names from memory and
are [inferred]. Everything else is read from the code [verified] unless marked.

Registry: component `surface-update`, task `surface-update-opcodes` (objective hosted-compositor).

## Client object (RSurfaceUpdateSession, 6 words)

| word | set by | meaning |
|---|---|---|
| +0 | ord7 (Connect) | session handle; ord7 = CreateSession("!SURFACE_UPDATE_SERVER", version 1.4.1), -11 if already connected |
| +1 | ord2 | TRequestStatus* for NotifyWhenAvailable |
| +2 | ord3 (arg 1) | TRequestStatus* for NotifyWhenDisplayed |
| +3 | ord4 (arg 2) | TRequestStatus* for NotifyWhenDisplayedXTimes |
| +4 | ord3 (arg 2) | TTimeStamp* (4 bytes, written back by the server) |
| +5 | ord4 (arg 1) | the X count |

ord9 = constructor (zeroes the six words), ord6 = Close. ord2/3/4 only record pointers; nothing is
sent until ord1 (SubmitUpdate). ord10/ord11 complete every recorded status with KErrNotSupported (-5)
and return -5: not implemented on this firmware, not a cancel. Every entry panics (category at
0x80dda6cc, reason 27) when the session is not connected.

## Opcode table

The server dispatcher (ServiceL, 0x80ddb8da) switches on `iFunction - 3` through the RTABI byte-table
helper at 0x80ddba44 (5 cases, table at 0x80ddb8e6). Opcodes 0..2 and > 7 hit the default case:
`RMessagePtr2::Panic(client, 1)` (bad request).

| opcode | client entry | server handler | IPC args (client -> server) | reply / completion |
|---|---|---|---|---|
| 3 NotifyWhenAvailable | ord1, when +1 set | 0x80ddb8bc -> common 0x80ddb8bc..0x80ddb8da: session slot +0x30, batch state 2 | none (async, client status = +1) | completed by the compositor's "consumed" status: see Completion |
| 4 NotifyWhenDisplayed | ord1, when +2 set | 0x80ddb8c6: slot +0x34, state 3 | arg0 = TDes8 (flags 4), 4 bytes: the TTimeStamp to write back | completed by "displayed"; the server writes the 4-byte timestamp into arg0 first (0x80ddb238: TBuf8<4> + RMessagePtr2::Write(0)) |
| 5 NotifyWhenDisplayedXTimes | ord1, when +3 set and count >= 1 | 0x80ddb8d0: slot +0x38, state 4 | arg0 = TInt count | completed by "displayed X times"; count < 1 is answered KErrArgument (-6) by the client itself (and by the SubmitUpdate handler if it got through) |
| 6 SubmitUpdate | ord1 (synchronous, after 3/4/5) | 0x80ddb740 -> 0x80ddb684 | arg0 = TDesC8 8 bytes (two zero words from ord1; the server never reads it); arg1 = TDesC8 16 bytes TSurfaceId; arg2 = TInt buffer; arg3 = TDesC8 rect list (16 bytes per TRect, only when the region has > 0 rects); flags 0x36 / 0xC36 | KErrNone right after the hand-off; buffer < 0 -> Leave(KErrArgument) -> ServiceError completes with -6 [inferred: default CSession2::ServiceError] |
| 7 CancelAllUpdateNotifications | ord5 (synchronous) | 0x80ddb41c then RMessagePtr2::Complete(0) | none | every pending notification message of this session completed with KErrCancel (-3); then the opcode-7 message itself with KErrNone |

Server-side checks for a surface id of all zeros: none (the client refuses it with KErrArgument
before sending and completes the recorded statuses with -6).

## Server objects (for reading the code)

- CSurfaceUpdateServer (0x9c bytes): +0x68 RPointerArray per screen (receiver interface or NULL),
  +0x80 RPointerArray of (priority, interface) pairs sorted by InsertIsq on the priority (0x80ddaf3c),
  +0x98 the provider (CSurfaceUpdateServerProvider, a CActive in the server thread).
- MSurfaceUpdateServerProvider::Register(TInt aScreen, CBase* aReceiver, TInt aPriority)
  (secondary vtable 0x80ddbdf8 slot 0, thunk 0x80ddaf2c) is cross-thread: it stores the three
  arguments, completes the provider's own request with 1, waits on a semaphore and returns the result
  (+0x28). In the server thread (RunL 0x80ddb068) 0x80ddaf3c asks the receiver
  `CBase::Extension_(0x10286498, &out, NULL)`; a NULL `out` is KErrArgument (-6), OOM is -4; a NULL
  receiver unregisters the screen. Terminate() (slot 1) is empty.
- CSurfaceUpdateSession (0x40 bytes): +0x18 RPointerArray of batches, +0x30/+0x34/+0x38 the batch
  waiting for the next SubmitUpdate for Available/Displayed/DisplayedXTimes, +0x3c -> server +0x80.
- CUpdateReceiverNotificationBatch (0x60 bytes): +4 the RMessage2 (0x28 bytes copied), +0x2c state
  (1 free, 2 available, 3 displayed, 4 displayed X times), +0x30 RPointerArray of the per-receiver
  notifications still outstanding, +0x50 "a receiver succeeded", +0x54/+0x58 best timestamp,
  +0x5c best status so far (KRequestPending when idle).
- CUpdateReceiverNotification (0x2c bytes, CActive): +4 iStatus (the TRequestStatus handed to the
  compositor), +0x1c the TUint32 timestamp the compositor writes, +0x24 "owner deleted, self-delete",
  +0x28 the batch.

## Completion (the contract)

1. ord1 sends opcodes 3/4/5 for the recorded statuses (each one asynchronous, the client's own
   TRequestStatus is the IPC status), then opcode 6 synchronously. A notify opcode arriving while the
   same slot still holds a batch whose message is pending (two NotifyWhenX without a SubmitUpdate in
   between) completes all three pending batches with KErrCancel and panics the client with reason 4
   (0x80ddb8bc: 0x80ddb4b0 + 0x80ddb822).
2. SubmitUpdate (0x80ddb684 -> 0x80ddb612 -> 0x80ddb520), always for every registered receiver (the
   sixth argument is 0 = all; the client's screen argument is never transmitted): per receiver, one
   CUpdateReceiverNotification per armed batch is created and SetActive (CActiveScheduler::Add order:
   available, displayed, displayed-X-times); if one cannot be allocated that batch's message is
   completed with KErrNoMemory (-4). Then the receiver interface is asked `vtable[3](0x10286498,
   &out, NULL)` (gcebe: SuExtension) and `out->vtable[2]` (ContentUpdated) is called with
   `(const TSurfaceId&, TInt aBuffer, const TRegion* (NULL when no rects), TRequestStatus* aConsumed,
   TRequestStatus* aDisplayed, TUint32* aTimeStamp, TRequestStatus* aDispXTimes, TInt* aCount)`;
   each status pointer is NULL when that notification was not armed, aTimeStamp is NULL unless
   aDisplayed is set, aCount is NULL unless aDispXTimes is set (the count is the client's arg0 of
   opcode 5). The slots +0x30/+0x34/+0x38 are cleared: the next notify starts a new batch. The
   opcode-6 message is completed with KErrNone after that; a compositor that completes the statuses
   synchronously inside ContentUpdated (gcebe does) therefore has the client see opcodes 3/4/5 complete
   before or together with opcode 6 [inferred ordering; not observed on the phone this time].
3. When the compositor completes a status, the notification's RunL (0x80ddb318) runs 0x80ddb238 in
   the server thread: the notification is removed from the batch, its status is ranked (0x80ddab98:
   KErrNone best, then KErrOverflow (-9), then -222.., anything else, KErrNotReady-class; the best
   seen is kept in +0x5c) and the batch's IPC message is completed:
   - Available (state 2): when the last receiver has answered, with the best status.
   - Displayed (state 3): on success from the first receiver of the batch (the highest-priority
     one, index 0) immediately, otherwise when the last receiver has answered; before completing, the
     best timestamp (+0x58, from the receiver's +0x1c) is written to the client's arg0.
   - Displayed X times (state 4): same rule as Displayed, no timestamp.
   The batch then goes back to state 1 (free) and is reused by the next notify of that session.
4. A completion whose message is no longer pending (cancelled, session closing) only frees the batch.

## Cancellation and destruction

- The client call that cancels is ord5 (CancelAllUpdateNotifications): opcode 7 completes every
  pending notification message of the session with KErrCancel (-3), then the client completes any
  recorded-but-not-yet-submitted status with KErrCancel too (0x80dda46e). The per-receiver
  notifications already handed to the compositor stay outstanding; the compositor's later completion
  only frees the batch (item 4). Nothing is cancelled on the compositor side.
- Session close (Disconnect = CSession2 default -> dtor 0x80ddb45e): the same KErrCancel sweep, then
  each batch is deleted (0x80ddb364): an outstanding notification is marked "self-delete" (+0x24 = 1)
  and deletes itself in RunL; the compositor may still complete it later.
- Surface destruction: the server knows nothing about surfaces; a notification for a destroyed
  surface is completed only if the compositor completes it (gcebe: synchronously, so never pending)
  [inferred].
- The notification's DoCancel panics (reason 4): the server never cancels a status it gave to the
  compositor, so a replacement compositor must complete every status it is given, exactly once.

## What this means for src/gcebe

gcebe's 4-slot MCompositionSurfaceUpdate vtable (ApiVersion, InternalVersion, ContentUpdated with
the 8 arguments above, Extension) matches the server's use: slot 3 is called with
(0x10286498, &out, NULL) before every ContentUpdated and must return >= 0 with `out` set (a negative
result leaves `out` NULL and the server calls through NULL). gcebe completes consumed, writes the
timestamp, then completes displayed and dispX, all with KErrNone, which is the order the server
expects. The statuses belong to CActive objects in the server thread: completion must target that
thread (RequestComplete on the server thread's handle), which gcebe already does.

## [inferred] items

- The EPL names (RSurfaceUpdateSession::NotifyWhenAvailable etc., CUpdateReceiverNotificationBatch
  fields) and the opcode names: the server's RTTI only gives the class names
  (CSurfaceUpdateServer/Session/Provider, CUpdateReceiverNotification, CUpdateReceiverNotificationBatch).
- The client-observed order "3/4/5 complete before 6 returns" with a synchronous compositor: from the
  code paths, not from a sydb run (the acceptance's cross-check was skipped on instruction).
- ServiceError on the SubmitUpdate leave (KErrArgument) being the default CSession2 one (vtable slot 8
  is euser 0x804c814b).
- The meaning of the status ranking's two table-driven entries (-222 and one other negative code).
- ord8 of the client (not in the export: a private helper, probably IssueRequestComplete).
