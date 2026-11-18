# P1i: Bluetooth stack never starts (no controller)

Static reading only (no runs). Tags: **[V]** read in the firmware or code, **[I]** inferred, not checked on a run.
Addresses: core ROM (`build/roms/se-p1i/SE-P1i_R6E30_CDA162024-70/core-flat.img`, base 0xf8000000);
`erihci.dll` XIP at 0xf8301980 (Thumb), `btdrv.ldd` XIP at 0xf83b5590 (ARM).

## Summary

- ESock's `z:\private\101F7989\Esock\bt.bt.esk` loads `bt.prt` (btlinkmanager, l2cap, rfcomm, sdp, avctp, avdtp)
  and names the HCI: `[hci] hcidllfilename= erihci.dll`, `port= 6` [V].
- `erihci.dll` (SE "HCTLUART") does **not** use C32. It loads LDD `BTDRV` and opens a channel on driver
  `BtDrvDRV` (`btdrv.ldd`), port/unit 6 from the .esk [V strings; unit = DoCreate unit I].
- `btdrv.ldd` is SE's own UART driver (a cut-down ECOMM: "Checked by EUART.PDD"). It has no register
  addresses of its own: 26 imports from `ecust.dll` (the PNX ASSP/variant) and 6 from `GPIODRIVER.LDD`, 48 from
  ekern [V]. Our hybrid ROM has no `btdrv.ldd` (and no SE `ecust`) [V: hybrid manifest has `erihci.dll` but
  not `btdrv.ldd`], so `LoadLogicalDevice("BTDRV")` or the channel open fails, `bt.prt` never comes up, and
  every BT client (RHCIDirectAccess, RSdp, HID) gets -1 [I: matches the reported -1s].
- So the phone's `btdrv.ldd` cannot be kept; a **stand-in `btdrv.ldd`** with a built-in fake controller is the
  workable design (below), like the N-Gage `ngage_lprf.c` (H-ngage-bt) but in the guest kernel or on QEMU.
- Cheaper alternative for the port handler panic only: `z:\private\101F7989\BtBearer.ph` holds `1`
  (same for `IrBearer.ph`, `UsbBearer.ph`) [V]; the port handler names these files [V], so `0` or a missing
  file most likely turns its BT bearer off [I]. This does not bring BT up.

## 1. Path to the chip

```
bt.prt --(hcidllfilename)--> erihci.dll --RBusLogicalChannel "BtDrvDRV", unit 6--> btdrv.ldd --ecust.dll/GPIO--> UART + BGB203/204
```

`btdrv.ldd` channel interface (DLogicalChannel, HandleMsg at 0xf83b6978) [V unless marked]:

| kind | no. | args | what it does |
|---|---|---|---|
| DoRequest | 0 | status, a1 = ?, a2 = TInt16* (len) | read, mode 0 (stores status +0x10c, max length from *a2) |
| DoRequest | 1 | status, a1 = TDesC8* | write one frame: ThreadDesRead into a 2048-byte buffer; > 2048 -> completes -9; busy -> -46 |
| DoRequest | 2 | status, a1 = TUint* signals | notify signal change (erihci: DoRequest(2, &iStatus, &iSignals)) |
| DoRequest | 3 | status | break / flush (completes after 0xf83b577c) [I] |
| DoRequest | 4 | status, a1, a2 = TInt16* (len) | read, mode 1 = frame read ("RecFrame") [I] |
| DoRequest | 5 | - | completes -5 (not supported); > 5 -> -6 |
| DoControl | 0 | a1 = TDes8 (8 bytes) | set config (8-byte struct: rate etc.) |
| DoControl | 1 | a1 = TDes8 | caps/info: 20-byte struct with 2 bytes from ecust [I] |
| DoControl | 2 | - | wait for TX empty, reset counters (reset buffers) |
| DoControl | 3 | - | returns current signals (erihci calls it) |
| DoControl | 4 | *a1 = set, *a2 = clear | SetSignals |
| DoControl | 5, 6 | *a1 | SetBreak / EnableFifoHwFlowCtrl or Start [I] |
| DoControl | 7, 8, 9 | - | mode 0/1/2 (power/sleep/wake of the chip) [I]; erihci calls 9 |
| DoControl | 10 | - | returns rx buffer length (halfword) |

Requests are rejected with -11 if the same request is already pending. Kernel code reads `*a1`, `*a2` with plain
loads (the 9.1 moving model allows it); a stand-in should use `kumemget32`.

Framing: H4-style [I]. erihci builds command frames as opcode (LE16) + length + parameters
(0xf8304c84) and puts a packet-type byte through 0xf8304c68 (called with 3 for SCO) [V code, meaning I].
The driver reads whole frames ("RecFrame"); whether the type byte travels in the buffer must be checked on
the first write (log the first DoRequest 1 buffer).

## 2. HCI conversation at start-up

Opcodes found as constants in `erihci.dll` [V]: `0x0C03` Reset, `0x1003` Read Local Supported Features,
`0x1009` Read BD_ADDR, and vendor `0xFC0F`, `0xFC22`, `0xFC2B` (OGF 0x3F; Philips BGB203/204 "BlueberryOne").
Files it reads [V]: `z:\private\101F7989\HCI\BTVersionMapping.txt` (firmware strings "BbyoneSW_2LLP 1.x" ->
R-state), `FactorySettingsMapping.txt` (HW type BGB203/BGB204_S01/BGB204_S02 -> `.ofs` factory settings, 264 B),
`BGBPatchFiles.ini` / `BGBPatchFiles_S02.ini` (-> `.fdw` patch, 1.9-2.8 KB). The HW type comes from the
modem side: erihci imports `ccpustorage.dll` and `OpaClient.dll` (3 each) [V]; fallback names `UNKNOWN`,
`UNKNOWN_S02` [V]. Likely order [I]: Reset -> Read Local Version (chip firmware) -> vendor patch download
(FC2B/FC22 per .fdw chunk) -> factory settings (.ofs, FC0F?) -> BD_ADDR (from CCPU) -> Read BD_ADDR.

`bt.prt` (Symbian 9.1 link manager) then needs the standard set [I, from the 9.1 stack]: Read Buffer Size
(0x1005), Read Local Version (0x1001), Read Local Features (0x1003), Read BD_ADDR (0x1009), Read/Write Class of
Device (0x0C23/0x0C24), Write Local Name (0x0C13), Write Scan Enable (0x0C1A), Write Page/Conn-Accept timeouts
(0x0C18/0x0C16), Set Event Filter (0x0C05), Host Buffer Size / flow control (0x0C33/0x0C31), Write Auth/Encrypt
(0x0C20/0x0C22), Set Event Mask (0x0C01).

Minimal answers: Command Complete `04 0E len 01 <opcode LE> 00 <params>` for every command (Num_HCI_Packets 1),
with these return parameters:

| opcode | return params after status 0 |
|---|---|
| 0x1001 | HCI ver 2, rev 0, LMP ver 2, manufacturer 0x0025 (Philips) [I], LMP subver 0 |
| 0x1003 | 8 bytes features, e.g. `FF FF 8F FE 9B F9 00 80` |
| 0x1005 | ACL len 0x0153 (339), SCO len 0x3C, ACL num 8, SCO num 0 |
| 0x1009 | BD_ADDR 6 bytes (fixed) |
| 0x0C14 / 0x0C23 | name (248 bytes) / CoD (3 bytes) |
| vendor 0xFCxx | none (status 0 only) [I: erihci may check a byte; read the handler if it stalls] |
| others | none |

Inquiry (0x0401): Command Status, then Inquiry Complete status 0. Create Connection: Command Status, then
Connection Complete status 0x04 (page timeout). Same as `ngage_lprf.c`.

## 3. Cost and design

Cheapest workable: **stand-in `btdrv.ldd`** (`src/standins/btdrv/`, `[[standins]] name = "btdrv"`, kind ldd,
rom_name `btdrv`, factory name `BtDrvDRV`) with the channel above, keeping the **controller on the QEMU side**
(guest = forwarding only): a small Syborg device "bthci" (TX FIFO of frames from DoRequest 1, RX FIFO of
event frames for DoRequest 0/4, IRQ on RX) whose C model answers every command with Command Complete
status 0 + the table above (reuse the reply logic of `src/qemu-ngage/.../ngage_lprf.c`). Controls 0-10 just
return 0 (3 returns DSR|CTS|DCD set, 10 returns 0). Request 2 stays pending until cancel. Estimate: stand-in
~300 lines, QEMU device ~250 lines (half from ngage_lprf), tests in `tests/` for the reply table: 1-2 days.
Risks [I]: the CCPU (OPA) query for the HW type/BD_ADDR may need a reply in `syborg_ccpu.c`; the vendor
patch steps may expect specific vendor event bytes (look at the 0xFC2B/0xFC22 handlers 0xf83079bc,
0xf8307fb6 if start-up stalls after the first vendor command).

Fallback if only the port handler must survive: `BtBearer.ph` = `0` (ROFS override) [I], no BT.
