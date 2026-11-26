# Nokia 6600 infrared: how to use it

The emulated 6600 has a working IrDA port. Two emulated phones can beam items to each other the way two real
6600s held face to face do. The phone's own IrDA software runs unchanged; the board models the infrared UART and
connects it to a QEMU serial port, which can be linked to another phone, a socket or a host serial device.
How it was built and what is modelled: [../../research/6600-irda.md](../../research/6600-irda.md) (hacks
H-nhl10-sir, H-upp-fiq-status in docs/hacks.md).

**[V]** = tried and seen working, **[I]** = expected to work, not tried.

## Quick start: beam a Calendar entry between two phones [V]

1. Start two phones, each with its own flash (`--phone`) and opposite ends of the link (`--irda a` / `--irda b`):

   ```sh
   python3 eka1/tools/ngage_phone.py --model nokia6600 --phone ir-1 --irda a
   python3 eka1/tools/ngage_phone.py --model nokia6600 --phone ir-2 --irda b
   ```

   A new phone shows a white screen for about a minute while it formats C:, then asks for the time and date
   (OK twice). Later boots are fast.

2. **Receiver** (either phone): Menu (F5), scroll down to **Connect.**, Select, then **Infrared**, Select. The phone
   now listens for about two minutes (it goes quiet again after that: open Infrared again if needed).

3. **Sender**: Menu, **Calendar**, Options, New entry, Memo; type a subject; Done. Select the day to open it,
   Options, **Send**, **Via infrared**. The sender shows "Connecting via infrared", then "Sending data".

4. On the receiver, the new-message envelope appears at the top right. Menu, **Messaging**, **Inbox**: the item is
   there as "Infrared device: Calendar entry". Open it to see the memo.

Host keys in the window: F1/F2 = softkeys, F5 = Menu, arrows and Enter = joystick, F4 = End
(docs/eka1-ngage.md "Running it").

## What can be sent

| Item | How | State |
|---|---|---|
| Calendar entry (memo, meeting, anniversary) | Calendar, open the day, Options, Send, Via infrared | [V] |
| Images, sound clips, videos | Gallery, Options, Send, Via infrared | [I]: a fresh phone's Gallery is empty |
| Business card | Contacts, Options, Send, Via infrared | blocked: Contacts does not open yet (bring-up page) |
| Notes, Java apps, other apps' items | the app's Send menu | [I] |

## Linking options (`--irda`)

The IrDA port is the phone's second serial port (QEMU `-serial`, the first one is the kernel's debug output). The
bytes on it are the plain IrDA (SIR) stream: framing, checksums and the link protocol are done by the phone.

| `--irda` | What it does |
|---|---|
| (none) | not linked: what the phone transmits goes to the session's `serial2` file (build/ngage/sess-NAME/) |
| `a` / `b` | the two ends of a local UDP link (127.0.0.1 ports 7470 and 7471); one phone each [V for the UDP link, run with explicit specs; the shorthand itself not yet run] |
| `udp:HOST:PORT@LHOST:LPORT` | any UDP link: send to HOST:PORT, receive on LHOST:LPORT; e.g. two machines on a LAN [I] |
| `tcp:HOST:PORT,server=on,wait=off` and `tcp:HOST:PORT` | a TCP link: one phone listens, the other connects (start the server first) [I] |
| `/dev/ttyUSB0` (a host serial port) | a real serial IrDA adapter; the phone's baud rate (9600 to 115200) is set on the port [I] |

Only one `a`/`b` pair can run on a machine at a time (fixed ports); for a second pair use explicit `udp:` specs with
other ports. The same values work for a debug session:

```sh
python3 eka1/tools/ngage_sess.py -s ir-1 --model nokia6600 --flash build/ngage/ir-1.flash --irda a start
```

Light is lossy and IrDA retries, so a dropped UDP packet costs a retry, not the transfer. A phone whose
Infrared is off ignores what arrives, as a real one would.

## Seeing what happens

- The session's `qemu.log` (build/ngage/sess-NAME/) has `ngage-sir:` lines: the port opening, the first register
  accesses and every baud change. A transfer shows `115200 baud` on both phones, then `9600 baud` again on the
  receiver when the link closes.
- Without `--irda`, `serial2` holds the raw transmitted bytes: IrDA discovery frames start with `c0 ff 3f` and end
  with `c1`; the last one of each round carries the name "Nokia 6600".

## When it does not work

| Symptom | Cause |
|---|---|
| "Connecting via infrared" and no progress | the other phone's Infrared is not on (or switched itself off after two minutes): open Infrared on it and send again |
| nothing arrives, both phones look fine | both phones were started with the same end (`a` and `a`), or another pair already uses ports 7470/7471 |
| the window does not start, "session is running" | that phone (`--phone NAME`) is already running in another window or session |
| a long white screen at start | the first boot of a new phone formats C: (about a minute); not a hang |

## Limits

Only the 6600 has the infrared port switched on. The 7650, 3650/3660, 3230, 6260 and N-Gage QD carry the same
kind of driver; each needs its own check before it is enabled (`NgageModel.irda` in `ngage.c`). The model does not
simulate pulse timing, the transceiver's echo or receive errors (details in the research note).
