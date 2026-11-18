# Nokia 9500 (RA-2): the DSP bridge hang at DeviceManagementServer start-up

Static reading only (core ROM `eka1/ra2/core/ROM.bin`, VA = 0x50000000 + offset); nothing was run.
[V] = read in the code at the given address, [I] = inferred.

The stack is TI's DSP/BIOS Bridge: the PDD has the strings `RMS_createNode`, `RMS_executeNode`,
`_SHM_BEG`, `TIOMAP1510SHM_maxMessages`, `Lv2_DSP_MMU`, `XLv1_DSPMailBox1` and a COFF dynamic loader
(0x507fb90c..0x507fc280) [V]; status codes are the Bridge's (0x8000 = DSP_SOK, 0x80008008 =
DSP_EFAIL, 0x8000801b) [V for the values, I for the names].

## Modules [V]

| module | header | code | exports |
|---|---|---|---|
| BridgeApi.dll | 0x507d9190 | 0x507d91f4, 0x6c6c | 35, dir 0x507dfdd4 |
| Ehwa.ldd ("Hwa") | 0x507dfe80 | 0x507dfee4, 0x283c | 1 |
| Ehwaomap.pdd ("Hwa.Omap") | 0x507e2740 | 0x507e27a4, 0x19f1c | 1 |
| Bal.dll | 0x50925c30 | 0x50925c94, 0xf9c | 28, dir 0x50926bc0 |
| DeviceManagementServer.exe | 0x50935090 | 0x509350f4 | - |

Who imports what [V, import-veneer scan of the core]: DeviceManagementServer uses Bal 12, 16, 26, 27;
Gwc.dll, Dasfgwc.dll and the codec plug-ins (RawGwc, AacDecGwc, Mp3DecGwc, AmrDecGwcTI) use only Bal;
only Bal, Balbuffers.dll (BridgeApi 4, 21, 22) and DspMemMapper.dll (BridgeApi 29-33) use BridgeApi.

## 1. What DeviceManagementServer waits for

Call chain [V]: DMS -> Bal export 12 (0x50925d54, `new` 0x38 bytes + ConstructL 0x50925cec) ->
0x50926830 -> BridgeApi export 7 (0x507dc1b4) via the veneer 0x50926b0c. Export 7 is "load and start
the DSP" (a PROC_Load + PROC_Start pair):

| site | call | code | args (r2, r3) |
|---|---|---|---|
| 0x507dc200 | DoControl(int,void*,void*) (EUser 354) | 0x4000000c | 0, 0 |
| 0x507dc328 | DoSvControl(int,void*,void*) (EUser 377) | 0x4000000b | &count, list of 0x54-byte entries (from the DCD registry: 0x10 + 0x20 bytes copied per entry) |
| 0x507dc3d8 | DoSvControl | 0x40000001 | buffer with the whole DSP image file (RFs/RFile read of the name passed in, mode 2), &size |
| 0x507dc494 | DoSvControl | **0x40000002** | 0x5c-byte start block, 0 |

The return address 0x507dc498 is after the 0x40000002 call [V]. On EKA1 a DoSvControl runs
DLogicalChannel::DoControl in the kernel server and the client waits for it (the EUser wait at
0x500d8fc4) [I: EKA1 Sv semantics]. So DMS waits for "start the DSP" to finish in the kernel server.

Path: LDD DoControl 0x507e04cc: codes 0x40000000..0x40000017 go to the PDD's vtable +0x30 [V]; the PDD
dispatch 0x507e3db8 is a 24-entry table at 0x507e3dc8 (index = code - 0x40000000) [V]:

| code | PDD handler | what [V code / I meaning] |
|---|---|---|
| 0x40000000 | 0x507e3e3c | copy 0x200 bytes from state+0x438 to the client (info) [V] |
| 0x40000001 | 0x507e3e60 | load image (0x507e84e4), state [+8] = 2 on success [V]; COFF loader [I] |
| 0x40000002 | 0x507e3ee0 | start: 0x507e8254 (args), 0x507e85c4 (board start), then [+8] = 1 [V] |
| 0x4000000b | 0x507e40d4 | register the 0x54-byte list [V] |
| 0x4000000c | 0x507e3f58 | set an in-use flag ([state+0xc] = 1); -21 if already set [V] |
| 0x40000003/4 | 0x507e4a80 | not supported [V] |
| others | table above | node/stream/memory ops; not needed for the hang |

Codes 0x80000000..0x80000015 are handled in the LDD itself (table 0x507e0ca4) and 0xc0000001..4 go
through BridgeApi exports 23-26, 34, 35 [V].

BridgeApi control map (export -> code) [V, from the call sites]: E13 DoCreate("Hwa") + Ctl 0x40000000;
E2 Sv 0x40000017; E4 Sv 0x40000007; E20 0x40000008; E22 Sv 0x40000009; E21 0x4000000a;
E3 0x40000015/0x40000016; E30 Sv 0x4000000d; E29 Sv 0x4000000f, 0x40000010, Sv 0x40000012;
E31 Sv 0x40000011; E32 Sv 0x4000000e; E33 0x40000013; E23-26, 34, 35 0xc000000x. The node exports
(E5, 6, 8-12, 14-19) work on in-process objects; their DSP traffic was not traced (out of scope).

## 2. Where the start blocks: an endless poll on the DSP's boot handshake

0x507e85c4 (board start) [V]:
1. needs state [+8] == 3 (image loaded), else 0x8000801b;
2. 0x507f3e88: entry point from the loader;
3. 0x507f4f98 -> 0x507f5fe8 (WMD board start), then state [+8] = 4.

0x507f5fe8 [V]:
- writes a 3-halfword C55x boot branch (0x6a00 | entry>>16, entry low, 0x2020) at the DSP reset
  vector through 0x507e788c (DSP memory write), unless the entry equals the default;
- `ARM_RSTCT1` (base [obj+0x10], halfword +0x10 = 0xFFFECE10) bit 2 DSP_RST cleared (0x507e7480);
  halfword 0 written to [obj+0x20]+0x1c (MPUI DSP_MPUI_CONFIG at 0xFFFEC91C [I]) (0x507e71c8);
  RSTCT1 bit 2 cleared then set (0x507e74a0);
- 0x507e63f0: `strh 0xffff, [SHM+2]`, SHM = [shmobj+0x64] (set at 0x507e56f0 from the `_SHM_BEG`
  symbol, with the 0x298-byte SHM control block after it) [V];
- RSTCT1 bit 1 DSP_EN cleared then set (0x507e74e4): DSP runs [V];
- **0x507f60c8..0x507f60d4: `do { r0 = (s16)[SHM+2]; } while (r0 != 0);` with no timeout** [V]
  (0x507e6400 reads the two bytes). The DSP's boot code clears this word to say "booted" [I, same as
  the Linux tiomap bridge's SHM sync word].

No DSP runs, so the kernel server spins here forever [I: the 0x40000001 load came back, so the hang is
after it; confirm with the kernel server thread's PC in 0x507f60c8..0x507f60d4 or 0x507e6400..0x507e6414].
A spinning kernel server also starves lower-priority threads [I].

Nothing else in this path waits for the DSP: after the poll the function returns 0x8000, the PDD sets
its states and returns to the client [V for 0x507e85c4/0x507e3ee0; I that 0x507e2df4 (a notify) does
not block].

## 3. Is a mailbox model enough?

For start-up: yes, and it is not even the mailbox. The only reply the start needs is the SHM sync
halfword going to 0. Everything after start (node create/execute/delete, streams) is the Bridge's
RMS protocol: commands in SHM message queues plus mailbox interrupts to `RMS_createNode` etc. in the
DSP image, replies carrying DSP node environment pointers and stream buffers moving through SHM.
Faking that at the board means writing an RMS server in C: **heavy, not recommended** [I].
Whether node calls time out cleanly without a DSP was not checked [I: the TI Bridge waits on
replies with the node's timeout].

The SHM is in memory the PDD sets up at run time (DSP MMU, `Lv2_DSP_MMU`), so the board does not know
its address statically [V that it comes from a symbol + run-time mapping, I that it is SDRAM]. A
board-side "DSP that clears the sync word" would need the `_SHM_BEG` DSP address from Dsp_image.dof
translated through the DSP MMU TLB entries the PDD wrote: possible, but more code than the patch below
for the same effect.

## 4. Recommendation

**Now (DMS, wserv):** one-word core patch, as a hack (H-ra2-dsp-boot): replace the poll's back branch.

```c
/* H-ra2-dsp-boot: no DSP; Ehwaomap.pdd's board start polls the SHM sync word (DSP clears it when
 * booted) with no timeout. Skip the loop: the start returns DSP_SOK as if the DSP had booted. */
#define RA2_DSP_SYNC_LOOP 0x507f60d4u   /* bne 0x507f60c8 */
static void ra2_dsp_boot_patch(void)
{
    uint8_t b[4];
    address_space_read(&address_space_memory, RA2_CORE_PHYS(RA2_DSP_SYNC_LOOP),
                       MEMTXATTRS_UNSPECIFIED, b, 4);
    if (ldl_le_p(b) != 0x1afffffbu) {          /* wrong firmware: leave it */
        warn_report("nokia9500: DSP sync loop not found");
        return;
    }
    ra2_put32(RA2_DSP_SYNC_LOOP, 0xe1a00000u); /* mov r0, r0 */
}
```
Call it next to `ra2_doc_standin()`. Test: read back the word (like `eka1/test_ra2_doc_standin.py`).

**Later (audio clients), if node calls hang or fail badly:** stub one level up, not at the mailbox.
Best seam [I]: Bal.dll (28 exports, the only DSP API of DMS, Gwc, Dasfgwc and the codec plug-ins),
with Balbuffers/DspMemMapper's BridgeApi imports (4, 21, 22, 29-33) as the second set. Alternative:
the PDD's DoControl (vtable +0x30, table 0x507e3dc8, 24 codes) answering each code with success and
fake handles. Neither was mapped here: that needs a second brief (arguments per export/code).

## 5. Bal.dll stand-in (second brief, static reading)

### What DeviceManagementServer waits on at Bal 0x50925fac [V]

0x50925fac is the return from `User::WaitForRequest(TRequestStatus&)` (EUser 1210, veneer 0x5092690c)
inside **Bal export 2** (0x50925ecc, "create node"), called from Dasfgwc export 3 (0x5092a524, call at
0x5092a590). The sequence in Bal E2:

1. BridgeApi E10 on this+0x24 (in-process) -> out word this+0x2c;
2. `RBusLogicalChannel::DoSvControl(4, &TBuf8<20>, &TBuf8<4>)` (EUser 377) on the "Hwa" channel at
   this+4: in = {args[1], args[0], [this+0x2c], args[2], args[3]} (args = Bal E2's r1, 4 words),
   out = node handle -> this+0x14; the channel handle (12 bytes at this+4) is copied to this+0x18;
3. `DoSvControl(5, handle, &status)` with status at sp+0x10, then **WaitForRequest(status)**;
4. status != 0: DoSvControl(7, handle) (free); status == 0: [this+0x30] = 1; returns status.

Ehwa.ldd small codes (switch 0x507e03d0, 0..10): code 4 -> 0x507e0598 (node allocate: 0xc8-byte node,
PDD vtable +0x18) [V]; code 5 -> 0x507e0810: finds the node, stores the client's TRequestStatus* in
node+0x7c, writes it pending (PDD vtable +0x54) and kicks the node's worker (node+0x28) [V]; the status
is completed when the DSP answers the RMS create [I]. No DSP, so the status stays pending forever.

A synchronous success at the Bal level is enough for this call: Dasfgwc only tests E2's return
(0x5092a594: r0 == 0) [V]. But the very next call is **Bal E22** (0x5092a5a4), which does
`DoSvControl(0x80000007, handle, &status)` + WaitForRequest (0x50926330..0x50926344) [V] and would
hang the same way (or return -18 if E2 did not set this+0x30). So E2 and E22 are the start-up minimum
[V for the call order in Dasfgwc E3]; the rest of the node exports are needed by Gwc and the codec
plug-ins (audio) [I that they come later].

Patching the LDD/PDD instead would mean completing code 5 and every 0x8000000x code with fake node
state in the kernel: larger and closer to an RMS model. The Bal seam is smaller [I].

### Bal object (0x38 bytes, Bal E12 NewL) [V: ctor 0x50925ca0, ConstructL 0x50925cec]

| offset | what |
|---|---|
| +0 | vtable 0x50926bb4 (dtor 0x50925da4: Bal E23, E28, BridgeApi E17/E27/E18) |
| +4 | BridgeApi E1 object = "Hwa" RBusLogicalChannel (+8 bytes); DoCreate by BridgeApi E13 |
| +0x14 | BridgeApi E28 object: +0x14 node handle, +0x18 channel copy |
| +0x24 | BridgeApi E19 object (in-process node bookkeeping), +0x2c its out word |
| +0x30 | node created (E2 sets, E22/E23 test) |
| +0x34 | DSP started (0x50926758 sets, E28 clears) |

Node calls use `DoControl(0x800000xx, handle, args)` (EUser 354) on +0x18; "wait" ones pass
args = {&status, param} and then WaitForRequest(status) [V].

### Exports [V: code addresses and calls; users from an import-veneer scan of the core]

Users: DMS 12, 16, 26, 27; Dasfgwc 2, 9, 22, 26, 27; RawGwc/AacDecGwc/Mp3DecGwc/AmrDecGwcTI 2, 9, 20,
21, 22; Gwc 3, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15, 18, 19, 23, 28. Nobody imports 1, 6, 17, 24, 25.

| ord | addr | does | stand-in |
|---|---|---|---|
| 1 | 0x50926608 | DoControl(chan, 9, 0x88-byte buf): processor info -> r1 struct | keep |
| 2 | 0x50925ecc | create node (above): DoSvControl 4, 5 + **wait**, 7 | **return 0** |
| 3 | 0x50926120 | (this, TRequestStatus& r1, buffer r2 (UID at [r2+8]: 0x101faf43 -> BridgeApi E23; 0x101faf45 -> code 0x80000010 + local wait, then *r1 = 0x80000001 pending; else 0x80000010 async with r1 in the message), r3, [sp]) | complete r1 with 0, return 0 |
| 4 | 0x50926580 | DoControl(0x80000011, handle, r1) sync | return 0 |
| 5 | 0x509262ac | 0x80000015 {&st, r1} + wait | return 0 |
| 6 | 0x509265f8 | DoControl(chan, 8, r1) | keep |
| 7 | 0x509262a4 | return this+4 (processor channel) | keep |
| 8 | 0x509264ac | 0x80000003 sync, 0x10c bytes of node attributes -> *r1 on success | zero 0x10c bytes at r1, return 0 [I zeros acceptable] |
| 9 | 0x50925ea4 | ensure DSP started (0x50926758) + BridgeApi E12(this+0x24, [r1+8], r1), in-process | keep |
| 10 | 0x509265e0 | 0x80000006 sync | return 0 |
| 11 | 0x509265a0 | 0x80000005 {r1, r2} sync | return 0 |
| 12 | 0x50925d54 | NewL(const TDesC& image): load "EHWA"/"EHWAOMAP", BridgeApi E13 attach, E7 load+start | keep (needs H-ra2-dsp-boot) |
| 13 | 0x50925fe8 | as 3 with 0x8000000e (BridgeApi E25 for 0x101faf43); r1 = TRequestStatus& | complete r1 with 0, return 0 |
| 14 | 0x50926560 | DoControl(0x8000000f, handle, r1) sync | return 0 [I: r1 may be an out buffer left as is] |
| 15 | 0x50926598 | BridgeApi E34(this+0x14) -> DoControl 0xc000000x | return 0 [I] |
| 16 | 0x5092629c | BridgeApi E14(this+0x24), in-process | keep |
| 17 | 0x5092646c | 0x80000009 + wait | return 0 |
| 18 | 0x50926578 | BridgeApi E35(this+0x14) -> 0xc000000x | return 0 [I] |
| 19 | 0x50926240 | 0x80000014 {&st, r1} + wait | return 0 |
| 20 | 0x50925e54 | BridgeApi E15(this+0x24, r1, 1, 0xb0-byte copy of *r2), in-process [I] | keep |
| 21 | 0x50925e04 | same with 0 | keep |
| 22 | 0x50926308 | if !+0x30: -18; DoSvControl(0x80000007, handle, &st) + **wait**; on -2 also 6 + wait, 7 | **return 0** |
| 23 | 0x509263b8 | if +0x30: DoSvControl(6) + wait, DoSvControl(7) (delete node) | return 0 |
| 24 | 0x5092642c | 0x80000008 + wait | return 0 |
| 25 | 0x50926504 | 0x80000000 {&st, r1} + wait | return 0 |
| 26 | 0x509266c0 | BridgeApi E3(chan, 1, 0) -> DoControl 0x40000014 (PDD 0x507e4240) | keep [I no DSP wait] |
| 27 | 0x509266d0 | BridgeApi E3(chan, 2, 0) -> 0x40000015 (PDD 0x507e4268: reads an SHM halfword) | keep |
| 28 | 0x509266e0 | stop: BridgeApi E6, E2 (Sv 0x40000017), Free LDD/PDD | keep |

The stubbed E2 leaves +0x30 = 0, so the real dtor's internal call to E23 (0x50925dbc) does nothing
[V 0x509263c8]. Kept exports touch only the processor channel and in-process objects; BridgeApi
E10/12/14/15 issue no DoControl/WaitForRequest in their first 0x60 bytes [V, partial check].

### Proposed stand-in (H-ra2-bal) [I: not built or run]

Placement: the bodies of E2, E13, E3 (0x50925ecc..0x50926240, 0x374 bytes) are reached only through
the export directory (no internal `bl` into that range [V]), so the board copies the blob to
0x50925ecc and rewrites export-directory entries (0x50926bc0 + 4 * (ord - 1)) to the blob's labels,
checking each old value from the table above first. Importers read the slot at call time (veneer
`ldr ip,[pc,#4]; ldr ip,[ip]; bx ip`) [V], so no importer changes.

```asm
@ Bal.dll stand-in (H-ra2-bal): no DSP; node and stream calls succeed and do nothing.
        .equ    RQCOMPLETE_SLOT, 0x500effac     @ EUser ord 926 User::RequestComplete(TRequestStatus*&, TInt)
        .text
        .arm
        .global _start
_start:
        .global bal_ok                          @ ords 2,4,5,10,11,14,15,17,18,19,22,23,24,25
bal_ok: mov     r0, #0
        bx      lr

        .global bal_attr                        @ ord 8 (this, 0x10c-byte out)
bal_attr:
        mov     r2, #0
        mov     r3, #0
1:      str     r2, [r1, r3]
        add     r3, r3, #4
        cmp     r3, #0x10c
        blo     1b
        mov     r0, #0
        bx      lr

        .global bal_async                       @ ords 3, 13 (this, TRequestStatus& r1, ...)
bal_async:
        stmfd   sp!, {r1, lr}                   @ [sp] = the TRequestStatus* variable
        mov     r0, sp
        mov     r1, #0                          @ KErrNone
        ldr     ip, =RQCOMPLETE_SLOT
        ldr     ip, [ip]
        mov     lr, pc
        bx      ip
        ldmfd   sp!, {r1, lr}
        mov     r0, #0
        bx      lr
        .ltorg
```

The sketch assembles (armv4t, 84 bytes at 0x50925ecc: bal_ok 0x50925ecc, bal_attr 0x50925ed4,
bal_async 0x50925ef4) [V].

Board side (nokia9500.c, next to `ra2_dsp_boot_patch`): check the old directory words (ord 2 =
0x50925ecc, 3 = 0x50926120, 13 = 0x50925fe8, 22 = 0x50926308, ...), copy the blob to 0x50925ecc, write
the new slots. Slot 0x500effac (EUser export directory 0x500ef138 + 4 * 925) holds 0x500d68a4 [V];
the name comes from the 7.0 bmarm euser def, whose ordinals match the other imports seen here
(WaitForRequest 1210, DoSvControl 377, DoControl 354) [V for those, I for 926].

Open points [I]:
- E3/E13 complete at once with KErrNone: a codec that feeds buffers in a loop will spin through them
  (silent playback at CPU speed). If that is a problem, keep the TRequestStatus pending (never
  complete) for E3 and complete only E13, or complete with KErrNotSupported (-5).
- Balbuffers.dll (BridgeApi 4, 21, 22 -> PDD 0x40000007/0x4000000a/0x40000009: DSP memory) and
  DspMemMapper.dll (BridgeApi 29-33) still go to the PDD; they were not checked for DSP waits.
- What ASC1510/DMS do after Dasfgwc E3 returns was not traced; the stale BridgeApi frames
  (0x507da398, 0x507df5e0) are likely from Bal E9's in-process BridgeApi E12 call.
