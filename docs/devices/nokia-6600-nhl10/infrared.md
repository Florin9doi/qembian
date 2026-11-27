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

The IrDA port carries the plain IrDA (SIR) stream: framing, checksums and the link protocol are done by the phone. A
UDP link (`a`, `b`, `udp:...`) is the board's own socket (`-M ...,irda=udp:HOST:PORT@[LHOST]:LPORT`, set by the
launchers); every other value is a QEMU chardev on the phone's second serial port (`-serial`, the first one is the
kernel's debug output).

| `--irda` | What it does |
|---|---|
| (none) | not linked: what the phone transmits goes to the session's `serial2` file (build/ngage/sess-NAME/) |
| `a` / `b` | the two ends of a local UDP link (127.0.0.1 ports 7470 and 7471); one phone each [V for the UDP link, run with explicit specs; the shorthand itself not yet run] |
| `udp:HOST:PORT@LHOST:LPORT` | any UDP link: send to HOST:PORT, receive on LHOST:LPORT (LHOST may be empty: all addresses; the `@` part is required); e.g. two machines on a LAN [I]. The phone takes datagrams from any sender |
| `tcp:HOST:PORT,server=on,wait=off` and `tcp:HOST:PORT` | a TCP link: one phone listens, the other connects (start the server first) [I] |
| `/dev/ttyUSB0` (a host serial port) | a real serial IrDA adapter; the phone's baud rate (9600 to 115200) is set on the port [I] |

Only one `a`/`b` pair can run on a machine at a time (fixed ports); for a second pair use explicit `udp:` specs with
other ports. A second phone on an end already in use does not start ("cannot bind ... another phone on this end?"). The same values work for a debug session:

```sh
python3 eka1/tools/ngage_sess.py -s ir-1 --model nokia6600 --flash build/ngage/ir-1.flash --irda a start
```

Light is lossy and IrDA retries, so a dropped UDP packet costs a retry, not the transfer. A phone whose
Infrared is off ignores what arrives, as a real one would. A peer that goes away (the other window closed,
`ngage_irsend.py` exited) only loses the phone's bytes: the board's socket never blocks and ignores the "port
unreachable" errors. It replaced QEMU's udp chardev, which froze the whole phone (and its monitor) when the peer
had gone: it read its socket blocking after such an error, holding QEMU's main lock until the next datagram.

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
| the whole phone (and its monitor) freezes once the IR peer, e.g. `ngage_irsend.py`, exits | a phone started before the board's own IR socket (QEMU's udp chardev, `-serial udp:...`): rebuild (`scripts/build-qemu-ngage.sh`) and restart it. Any datagram to its port wakes a frozen one |
| a "computer" peer gets probed mid-push (IAS mRouter, IrCOMM, IrModem, IrLPT) | the phone's PC Suite link (mRouter) reacts to the Computer discovery hint; `ngage_irsend.py` advertises OBEX only (`--hints 8020`) |
| "cannot bind ... another phone on this end?" at start | an earlier QEMU on that end is still running (its window gone): `pgrep -af qemu-system-arm` |

## Limits

Beaming was tried on the 6600 and the 6260 (`--model nokia6260`, the same steps; its receiver listens about one
minute). The 7650, 3650/3660 and 3230 have the port switched on with the same driver, not tried yet; the N-Gage QD
is off (`NgageModel.irda` in `ngage.c`). The 9210 Communicator has its own UART model with the same `--irda` values
(docs/devices/nokia-9210-rae3/bring-up.md "IrDA"); a 9210 to 6600 beam does not complete yet. The model does not
simulate pulse timing, the transceiver's echo or receive errors (details in the research note).
