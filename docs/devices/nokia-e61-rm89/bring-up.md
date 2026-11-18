# Nokia E61 (RM-89, S60 3rd, Symbian 9.1): bring-up log

Profile `models/nokia-e61-rm89` (family `bb5-s60`, personality `sym91`, firmware 3.0633.09.04), made
2026-11-17 from the N73's profile: the same platform generation (S60 3rd edition, 9.1, TI OMAP1710,
moving memory model, `syborgmv` kernel, ISC/SSI modem link). [V] = seen on a run or in the firmware's code,
[I] = inferred.

```sh
tools/device.py build nokia-e61-rm89 --kernel
python3 tools/sydb/sydb.py phone e61 --fresh        # instance e61
```

The E61 is kept apart on purpose: everything it needs that the N73 does not is in its own files, so the
two can be compared.

| What | Where | Why the E61 differs from the N73 |
|---|---|---|
| display 320x240 landscape | `model.toml` [display] | the E61's screen |
| keys | `keymap.toml` (E72 navigation/softkeys + E90 QWERTY) | QWERTY phone; its own key table not read yet [I] |
| WLAN | `model.toml` hardware.wlan = true | the N73 has none: the WLAN server starts only on the E61 |
| res 0x54 msg 0x0D: status 00 | `isi-replies.txt` | see 2 below |
| PN_PHONE_INFO 0x23 -> 0x24, WLAN MAC | `isi-replies.txt` | see 4 below |

## Boot log

1. **First boot: Nokia logo for ever** (2026-11-17). Owner's report; the screensaver still came on after a while
   (the window server was alive).
2. **State 252, critical start-up failure** [V catch on Starter 0xf916820c with KErrDied]:
   `npaprivlistener.exe` asked res 0x54 msg 0x0D six times, got the shared table's reply (status 01, recorded on
   a real N8), closed its channel and exited; Starter monitors it as a critical process. The E61's table answers
   status 00; start-up goes on to state 211.
3. **ETel died USER 21** (CTelServer, in ISIMSG called from NOKIATSY+0x155ea) on the NET_MODEM_REG_STATUS_GET
   reply (`0A 00`) that had gone into the BB5 family table on 2026-11-17 for the Messaging hang. It broke the
   N73 the same way [V]. The 2006 TSY reads that reply in another layout: a common-only reply panics it too; a
   reply with no sub-blocks does not panic but does not complete the request either [V]. Nothing on the
   9.1 phones waits for it, so the reply moved out of the family table into each newer member's own table
   (family rule: it was never true of every BB5 phone). The N73 reaches state 213 again [V]. Test:
   `tests/isi_replies_test.py EtelPanicsOnRegStatus91`.
4. **Stuck at state 211** [V IPC trace, `device.py build --trace 0x80000800`]: the System Agent Extension
   (`sae.exe`) completed all its DosServer calls and started `ConnMonExe`; the Connection Monitor's connect to
   **WlanServer** never completed. `WlmServerExe` had sent PN_PHONE_INFO (0x1B) msg 0x23 on ISC channel 32 at
   start-up and waited synchronously: the request reads the WLAN MAC address. The reply format, from
   `wlmplatforminit.dll` (core 0xf82b8c8c..0xf82b8d44, request built at 0xf82b923c) [V]: resource 0x1B,
   message 0x24, byte 10 status (must be 0, else Leave -2), bytes 11..16 the MAC in reverse order (0xf82b9282).
   Ours answers MAC 02:00:00:E6:10:01 (locally administered). Then: 213 Telephone (22 s), the city/date
   queries, 212, SysAp 203 and the active standby (Offline) [V screenshot]. The first-boot queries come late:
   the screensaver shows over them until a key is pressed.
5. Deaths after idle, as on the N73 [V]: FaxModem KERN-EXEC 3, UsbWatcher/usbsvr/sdpserver -1, SYSTEMAMS -3,
   CalenSvr -11 (first boot), !MsvServer -11.

Unanswered at idle [V `tools/isi.py log`]: NET_MODEM_REG_STATUS_GET (9.1 layout, see 3), ch 24 msgs 0x11 and
0x2B, RES_94 msg 00 (as the N73).

## Open

- The E61's own key table (EKData / keypad driver) is not read: keys are borrowed from the E72 and E90.
- Seeded drives for the smoke test (first-boot queries).
- WLAN past start-up (no scan results expected; the WLAN driver is not modelled).
