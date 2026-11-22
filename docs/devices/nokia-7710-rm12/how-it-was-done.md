# Nokia 7710: how it got to the Desk, and what we learned

The Nokia 7710 (RM-12, Series 90, Symbian 7.0s, EKA1, TI OMAP1510) went from the flash files to its S90 Desk
in one session (2026-11-18) with four stalls. The stall record is [bring-up.md](bring-up.md); the workarounds are
in [../../hacks.md](../../hacks.md) "Nokia 7710". This page is for the next phone that shares a board with one
already done.

## 1. What carried over from the 9500, and how

| Piece | How much changed |
|---|---|
| Container | the 9500's Phoenix A2 extractor; one fix: the sub-header's tag is a fixed 20-byte field (data at +56) |
| SoC, bootstrap | identical OMAP1510 literals; QEMU's OMAP model and patches 0003-0017 unchanged |
| Board | `nokia9500.c` became a board for two phones: a `Ra2Phone` description per phone (images, partitions, stand-in addresses, devices) |
| Storage | the TrueFFS API stand-in, assembled at the 7710's DiskOnChip.dll (same thunk words, new addresses) |
| Modem | the XBUS peer unchanged; the reply table copied, then two corrections (CheckCrc byte, object numbers) |
| DSP bridge | Ehwaomap, BridgeApi, Bal, Dasfgwc, DeviceManagementServer: the same binaries **relocated**: one delta per module applied to the 9500's patch addresses, each old word checked |
| Kernel words | ported by code match (Kern::Fault export 102) and literal voting (current thread, containers) |
| New | the digitiser (exyin.dll) and the 5x6 key matrix (ekeyp.dll): two research agents, both right first time |

**The test for "same binary, relocated":** compare two modules word by word; when only words that are
addresses differ (a few hundred of ~25000), every patch address moves by the module's base delta. This turned
three hacks with ~30 addresses into three numbers.

## 2. Debugging techniques that paid off here

- **Exec trace on User::Leave** (`--debug unimp,exec,cpu,nochain --dfilter 0x50058DA8+4`): the leave's r0 (-12) and
  the caller's registers named the failing call (SharedData Assign) in one run (stall 2).
- **Import stubs to names:** a Thumb import stub (`ldr r3,[pc]; ldr r3,[r3]; bx r3`) + the export-name table
  (`port_exports.py`) names the callee: `RSharedDataClient::Assign`, `User::LeaveIfError`.
- **Write watchpoints on the super page:** the start-up type came from the bootstrap with the MMU off; `gva2gpa`
  gave the physical address (0x1056601C) and a watchpoint there caught the store and its caller (stall 3).
- **Compare with the sister phone live:** reading the same kernel words on a running 9500 (flag 1, a security
  blob present) showed which branch the 7710 took wrongly.

## 3. Traps

- **A partition sized from its record:** the factory C: record holds only the FAT header (240 sectors of
  174048). EFile mounted it, but writes past the record failed silently and surfaced far away as a SharedData
  -12 and a jump to 0 in Wserv. Size partitions from the boot sector.
- **QEMU's power-on reset reason:** ARM_SYSST powered on as 0x3A (including "global software reset"). The 9500's
  bootstrap didn't care; the 7710's took a warm restart, the kernel looked for security data kept across the
  reset, found none, and a NULL object crashed EikSrvs 20 s later. Reset-reason registers deserve a look at the
  first strange "restored state" behaviour.
- **Object containers differ between 7.0s kernels:** the 7710's entries are 8 bytes `{DObject*, key}`; the tools
  now take `objstride` per model.
- **`pkill -f` on a pattern also present in the shell's own command line kills the shell**; stop test windows by
  PID.

## 4. Next

The open list is in bring-up.md "Open" and hacks.md "Not modelled / not working": battery state, the remaining
modem requests, RTC, sound, MMC.
