# Nokia 6630 (RM-1): DeviceManagementServer exits with -18 (stall 16)

Static reading only (core `eka1/rm1/core/ROM.bin`, VA = 0x50000000 + offset; Ghidra project folder `/rm1`);
nothing was run. [V] = read in the code at the given address, [I] = inferred.
New Ghidra programs: `rm1_EHWA_50249390.bin` (EHWA.ldd), `rm1_DeviceManagementServerProxy_50221ee0.bin`
(cut into `build/evidence/ghidra-rm1/`).

## Summary

- The -18 **is a leave**: DMS calls `User::Leave(r0)` at **0x5021C794** when its "test server" DSP plug-in
  (TestServerGwc.dll) fails to open; the leave unwinds to E32Main's TRAP (0x50220720), whose result is the
  thread's return value [V].
- The -18 itself is produced in the kernel by **EHWA.ldd 0x5024ADF8**: node control 0x80000007 ("run node")
  refuses a node whose state word (+0x1C) is 0 or 5 [V]. The state is 0 because the "create node" request was
  completed with KErrNone by our own H-rm1-dsp-timeout patch and no DSP ever moved the node on [I]. That is
  why -33 became -18 with that patch.
- EikAppUiServer does not hit the same call: it is a **client** waiting for DMS to start
  (DeviceManagementServerProxy 0x50221F50: process Logon + rendezvous); a server that dies before its
  rendezvous hands its exit reason to the client [V], which leaves with it [I].
- Proposed fix: one word in DMS, `rm1_patch(0x5021C790, 0xD0012800, 0xE0012800)` (the `beq` over the leave
  becomes `b`).

## 1. Where the -18 comes from

Call chain, top down:

| address | what | mark |
|---|---|---|
| 0x5021AF54 -> 0x50220770 | E32Main: reads the 4-byte command line (0x502206A4), then 0x50220720 | [V] |
| 0x50220720 | `CTrapCleanup::New`, `TTrap::Trap(&r)`, 0x50220640, `UnTrap`; **returns r** (the thread's exit reason) | [V] |
| 0x50220640 | `RThread::Rename` + LeaveIfError, `new CActiveScheduler`, Install, **0x5021C890** (server NewL), 0x502206E4 (rendezvous: `RThread::Open(id)` + `RequestComplete(status, 0)`) + LeaveIfError, `CActiveScheduler::Start` | [V] |
| 0x5021C890 -> 0x5021CA6C | server ConstructL: 0x5021BA00, 0x5021CAF8 (plug-in list from the resource file, asc.dll per entry), **0x5021BF64** (call at 0x5021BF84), 0x5021AFB4, 0x5021CE50, `RLibrary::Load`/`Lookup(1)`, `CServer::StartL` | [V] |
| 0x5021BF64 -> 0x5021C690 -> **0x5021C6C4** | creates the test-server plug-in: stub 0x5022119C -> TestServerGwc export 2 (0x50285110), fills a 0x32C-byte parameter block (two stream blocks: 1, 0x28, 2, 0, -1, 2 + a name), then a virtual call through the helper 0x50220A14 (`bl` at 0x5021C78C) | [V] |
| 0x5021C790..0x5021C794 | `cmp r0, #0` (0x2800); `beq +2` (0xD001); `bl 0x50220A7C` = **User::Leave(r0)** | [V bytes `00 28 01 d0 04 f0 72 f9`] |
| TestServerGwc 0x50285174 (export 1) | the open: Bal ord 9 (stub 0x50285344), ord 21, ord 20, ord 2 (create node), **ord 22** (stub 0x50285374), then Gwc 0x50287FDC(this, 1) whatever ord 22 said; returns ord 22's result | [V]; that the virtual call lands here [I] |
| Bal.dll ord 22 = 0x50285BD8 | `if (this+0x30 == 0) return -18` (0x50285C74), else `DoSvControl(0x80000007, node, &status)`; 0 -> `User::WaitForRequest(status)`, return status; a non-zero result is returned as it is | [V] |
| EHWA.ldd 0x5024A0FC | channel DoControl; case 0x80000007 -> 0x5024ADDC | [V] |
| **EHWA.ldd 0x5024ADDC** | `if (node+0x1C == 0 or == 5) return -18` (**0x5024ADF8 `mvn r0, #0x11`**); else registers the request at node+0x88 and calls the PDD (vtable +0x24) | [V] |

Why this -18 and not another [I, from the code]:
- Bal's own -18 (flag this+0x30 == 0) cannot be reached here: ord 2 (0x5028579C) sets the flag exactly when it
  returns 0, and TestServerGwc calls ord 22 only after ord 2 returned 0 [V].
- EHWAOMAP's -18 (0x50232DBC in 0x50232BF4) maps a DSP mailbox reply status 1; without a DSP there is no
  reply [V].
- EHWA.ldd's other -18 (0x5024A23C, control 0x80000001, same state test) is not on Bal's open path [I].
- bridgeapi, Gwc, TestServerGwc, DMS, BalBuffers, DspMemMapper hold no -18 constant (ARM `mvn #0x11`, Thumb
  `mov #18; neg`, literal) [V scan].
- Ord 2 ends with `DoSvControl(ch, 5)` + `WaitForRequest`; that request is one the PDD's DSP timer now
  completes with 0 (H-rm1-dsp-timeout), so ord 2 "succeeds" with the node still in state 0 [I].

Not confirmed on a run: the node state value at 0x5024ADE4 (expected 0).

## 2. Why EikAppUiServer exits with the same code

- DeviceManagementServerProxy.dll 0x50221F50 [V]: `RProcess::Create(server, command line = own thread id)`,
  `Logon(logonStatus)`, `Resume`, `User::WaitForRequest(rendezvousStatus, logonStatus)`. If the rendezvous is
  still pending, the process has died: it returns **logonStatus = the server's exit reason** (-18).
- DMS completes the rendezvous only at 0x502206E4, after the whole ConstructL [V]. So the client sits in
  WaitForRequest from its first sound call until DMS is up or dead (~57 s here, most of it DSP timeouts [I]).
- Import chain [V stubs]: EikSrvUi.dll (and Cone.dll) -> MediaClientAudio.dll -> MMFDevSound.dll -> the proxy
  (MMFDevSound is the proxy's only user). Which EikSrvUi object opens the sound (key sounds [I]) was not read.
- The 2 s gap between the two exits was not explained (a second start attempt is possible [I]).

So fixing DMS's start fixes this exit too; no patch in EikSrv is needed [I].

## 3. Fixes, ranked

1. **DMS ignores the plug-in's open result** (proposed). `rm1_patch(core, len, "H-rm1-dms-open", 0x5021C790,
   0xD0012800, 0xE0012800)`: bytes `00 28 01 d0` -> `00 28 01 e0` (`beq` -> `b` at 0x5021C792). Touches only
   DMS; same idea as the 9500's "DMS accepts a failed open". ConstructL goes on to the EAP control module
   calls (0x502211FC, 0x50221208), 0x5021AFB4, 0x5021CE50, the library load and `StartL`; the next failure, if
   any, will show the same way (a leave in ConstructL) [I].
2. **Bal ord 22 returns 0**: 0x50285BD8 0xE92D40F0 -> 0xE3A00000 (`mov r0, #0`), 0x50285BDC 0xE24DD008 ->
   0xE12FFF1E (`bx lr`). Two words; of the modules read only TestServerGwc imports ord 22 (Gwc.dll does not)
   [V]. The 9500 stand-in also answers ord 22 with 0. Use it if another caller of ord 22 appears.
3. **The 9500's Bal stand-in** (`bal_standin_ra2.S`, as on the 7710 with `bal_delta` = 0x50285500 -
   0x50925C30): Bal.dll is the same binary relocated, so the export slots (directory 0x50286490 [V]) can be
   pointed at it. More work (the DMS part of H-ra2-bal does not carry over: this DMS is a different binary),
   but it removes every DSP wait, and with it most of the 57 s.

Do **not** patch EHWA.ldd 0x5024ADF8 to return 0: Bal then waits on a request status that nobody registered
(`WaitForRequest` at 0x50285C1x never returns) [V code].

What the phone loses: nothing it has now. No DSP is emulated, so there is no audio either way; with fix 1 or
2 DMS runs with a test-server node that was never started, and later node messages are not answered [I].
Each DSP request still costs one EHWAOMAP timer period, so the start stays slow until fix 3 or a shorter
timer.

## 4. Techniques and traps

- **Signed conditions never match** [V tool, I that this was the cause]: `ngage_sess.py` evaluates a catch
  condition over unsigned register values (`eval(cond, {}, regs)`, regs from RSP hex). `r0 == -18` is always
  false; write `r0 == 0xFFFFFFEE` (or `r0 >= 0x80000000` for "any error"). This would explain "no User::Leave
  with -18": the leave at 0x5021C794 is a plain `User::Leave` (0x500BBE80).
- First step for "thread returned an error, no panic": read E32Main. If it returns a TRAP result (here
  0x50220720), it is a leave by construction; then catch `User::Leave` with `r0 >= 0x80000000` limited to the
  thread, and read lr and the stack. No need to guess the code.
- Scan the suspect modules for the constant before reading code: ARM `mvn rX, #0x11` (mask 0x0FFF0FFF =
  0x03E00011), Thumb `mov rX, #0x12; neg`, literal 0xFFFFFFEE. Eight modules gave four sites here.
- To confirm on a run (one boot): catch 0x5024ADF8 (EHWA.ldd, the -18 itself; read node+0x1C at r4) and
  0x5021C794 (DMS, r0). Both are cold paths, so timing is not disturbed.
- A client exiting with the server's code shortly after it is the start-server pattern (Logon + rendezvous);
  check the proxy DLL before reading the client.
- A request completed "successfully" by a patch (H-rm1-dsp-timeout) moves the failure to the next state check.
  Expect more of these in DMS's ConstructL; a stand-in at Bal's level (fix 3) ends the series.
- Import stubs: Bal.dll uses a third form, `ldr ip, [pc, #4]; ldr ip, [ip]; bx ip; .word slot`
  (0xE59FC004, 0xE59CC000, 0xE12FFF1C) [V]; a scanner that knows only the two usual forms finds none.
- DSClient ordinals in `user-exports.tsv` look shifted for jump-table exports (targets 4 bytes apart); do not
  trust those names [I].
