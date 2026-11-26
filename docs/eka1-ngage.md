# N-Gage QD (EKA1) on the `ngage` board: start here

The original Nokia N-Gage QD firmware (RH-29 V 04.10, Symbian 6.1, EKA1 kernel, Series 60
1.x) boots unmodified on our own QEMU board `ngage`, as `-M ngageqd` (`--model ngageqd`). The first N-Gage
(NEM-4 V 4.03, the same kernel generation) is `-M ngage` (`--model ngage`):
[devices/nokia-ngage-nem4/bring-up.md](devices/nokia-ngage-nem4/bring-up.md). Each phone's firmware is extracted into its
own folder (`eka1/rh29/`, `eka1/nem4/`; [../eka1/README.md](../eka1/README.md)). This page covers:
- the state of the work;
- how to run the phone;
- how the pieces fit;
- how to debug;
- what to do next.

The same board also runs the **Nokia 7650** (NHL-2NA, Symbian 6.1) as `-M nokia7650`
([devices/nokia-7650-nhl2/bring-up.md](devices/nokia-7650-nhl2/bring-up.md)) and the **Nokia 6600** (NHL-10, Symbian 7.0s) as `-M nokia6600`: per-phone
values are an `NgageModel` in `ngage.c`; the tools take `--model nokia6600`. Its notes:
[devices/nokia-6600-nhl10/bring-up.md](devices/nokia-6600-nhl10/bring-up.md). Run
`python3 tests/ngage-boot-check.py [--model ngageqd|ngage|nokia6600]` after every board change.
The EKA1 tree also holds boards of their own: the **Nokia 9500 Communicator** (`-M nokia9500`, QEMU's
OMAP1510: [devices/nokia-9500-ra2/bring-up.md](devices/nokia-9500-ra2/bring-up.md)), the **Nokia 7710** (`-M nokia7710`,
the same board, S90 Desk: [devices/nokia-7710-rm12/bring-up.md](devices/nokia-7710-rm12/bring-up.md)), the **Sony Ericsson P800**
(`-M p800`, the P910i's board, UIQ 2.0 home screen: [devices/se-p800/bring-up.md](devices/se-p800/bring-up.md)) and the **Sony Ericsson
P910i** (`-M p910i`, a PNX4000 board modelled from the phone's code, UIQ 2.1 home screen:
[devices/se-p910i/bring-up.md](devices/se-p910i/bring-up.md)). All eight phones (+ the 3230, `-M nokia3230`: [devices/nokia-3230-rm51/bring-up.md](devices/nokia-3230-rm51/bring-up.md), the 7610 and 6670, `-M nokia7610` / `nokia6670`: [devices/nokia-7610-rh51/bring-up.md](devices/nokia-7610-rh51/bring-up.md), the 6260, `-M nokia6260`: [devices/nokia-6260-rm25/bring-up.md](devices/nokia-6260-rm25/bring-up.md), and the 6630, `-M nokia6630`, S60 FP2 on an OMAP1610: [devices/nokia-6630-rm1/bring-up.md](devices/nokia-6630-rm1/bring-up.md), and the N70, `-M nokian70`, S60 FP3 on the 6630's board: [devices/nokia-n70-rm84/bring-up.md](devices/nokia-n70-rm84/bring-up.md), and the 9210 Communicator, `-M nokia9210`, Symbian 6.0 on its own board: [devices/nokia-9210-rae3/bring-up.md](devices/nokia-9210-rae3/bring-up.md)): `docs/device-matrix.md` 2.2b.

Related documents:

| Document | Contents |
|---|---|
| [eka1-ngage-questions.md](eka1-ngage-questions.md) | the question log (Q1-Q56): every stall, its evidence and its answer, marked [V]/[I] |
| [hacks.md](hacks.md), section "N-Gage QD" | every workaround and guess in the board |
| [../eka1/README.md](../eka1/README.md) | the firmware files, extraction and analysis tools |
| [research/eka1-ngage-feasibility.md](research/eka1-ngage-feasibility.md) | the first feasibility study |

## State (2026-11-11)

- **Boot.** Bootstrap → EKA1 kernel → EFile/EStart → window server → Starter's normal
  start-up list (40+ processes) → Startup (self-test, SIM and security checks) → the idle
  screen (N-GAGE QD wallpaper, softkeys "Contacts" and "Game").
- **Boot time.** About 7 s to the home screen once the phone's flash is kept. The first boot
  formats C: and restores factory settings: about 40 s to the logo, 95 s to the UI.
- **Keys.** The keypad (ekeyb's 6x5 matrix) answers the first-boot time and date queries.
- **Cellular side (NOS).** It doesn't run: its code is scrambled. A stand-in stub and a
  reply table answer what Symbian asks of it: product data, the RTC hand-over, ISI over
  PhoNet (self-test, SIM, security, network "searching", audio, SMS).
- **Idle (2026-11-12).** The CPU sleeps at idle (H-ngage-idle): 0.4% of a host core on the idle
  screen, ~18% during a Crash Nitro Kart race (was 100% always, Q56).
- **Display.** The board shows the window server's screen chunk (H-ngage-fb); lcd.ldd's own
  transfers (probably DMA) aren't modelled.

- **Memory card (2026-11-12).** The MMC controller and the DMA engine it uses are modelled
  (`ngage_mmc.c`): a card image (`eka1/tools/ngage_card.py OUT.img GAME_DIR`, MBR + FAT16) is
  identified, mounted and read (multi-block reads, the phone scans the game's files). Run:
  `python3 eka1/tools/ngage_phone.py --mmc build/ngage/games/snakes-card.img` (or
  `ngage_sess.py start --mmc IMG`). Writes are modelled but untested.

- **Bluetooth (2026-11-12).** The BT module (LPRF on CBus device 5) is a minimal HCI controller
  (`ngage_lprf.c`, H-ngage-bt): the phone's stack starts, Bluetooth is "off" by the phone's own
  setting, RHostResolver::GetHostName works. Needs H-ngage-selftest-stop (an ISI message that
  ends SelfTestServer so BTTestServer frees btldd's only channel). docs/research/ngage-bluetooth.md.
- **Games.** Snakes (card) starts and plays (menu, single-player level). Crash Nitro Kart (3D,
  software-rendered) runs unchanged: menus, Arcade > Quick Race, a race on screen at >= 21 frames/s
  (screendump-limited; ~18% of a host core since H-ngage-idle), the kart drives (key 5 accelerates). No sound.

Not working or not modelled yet: see "Next" below.

## First boot is slow (expected): a blank or white screen for minutes

A factory-fresh phone (no flash file yet, or `--fresh`) formats C: before anything is drawn, so the screen stays
blank or white for a long time. **That is not a hang.** It happens once: the phone's flash file
(`build/ngage/phones/NAME.flash`, `--flash`) keeps C:, and later boots are fast.

| Phone | First boot (blank/white screen, then) | Later boots | Why |
|---|---|---|---|
| N-Gage QD | ~40 s to the logo, ~95 s to the UI | ~7 s | EStart formats C: |
| Nokia 6600 | ~60 s format, then the time query | ~20 s | EStart formats C: |
| Nokia 7610 / 6670 | **~3 min** white screen, then the time query | ~30 s to the time query | as the 3230 |
| Nokia 6260 | **~155 s** white screen, then the time query | not timed | as the 3230 |
| Nokia 3230 | **~150 s** white screen, then the NOKIA splash | ~30 s to the time query | C: is formatted **twice**: EStart, then the restore-factory-settings script (`FORMAT C:` in RfsServer), ~0.7 s per 64 KB sector |
| Nokia 6630 | **~3.5 min** logo / white screen, then the home-city list | (a kept NAND: open, stall 26) | mednand formats the NAND's user area (C:) |
| Nokia N70 | **~3.5 min** NOKIA logo (C: formatted), then the city list; ~5 min to the home screen | (a kept NAND: not checked yet) | as the 6630 |
| Nokia 9210 | ~95 s to the Home city query (the DataFlash driver scans all 8192 pages and writes its reserve table) | under a minute, straight to the Desk | |
| Nokia 7710 / 9500 | a few seconds (the C: image is ready-made) | same | |

How to tell it is formatting, without disturbing the phone: `ngage_sess.py -s SESSION status` shows `hit: null` and a
growing uptime. `objects` (it pauses briefly) shows only EKern, EFile, EStart (3 processes) until the format ends. For
proof, a one-shot run with `-- -trace 'pflash_*'` (ngage_run.py) shows the sector erases advancing. Look at it again only
after the times above. The time and date queries then come at every boot (no RTC hand-over yet).

## Running it

One-time set-up:
1. The flash files in `eka1/ngage_rom/` and the extraction (`eka1/README.md`: steps 1-3).
2. `scripts/build-qemu-ngage.sh`: builds the board into its own QEMU 11.1.3 tree
   (`build/qemu-ngage/`). It applies `patches/qemu-ngage/*`; the shared Syborg QEMU is not
   touched.
3. After changing the stub, `src/qemu-ngage/nos-standin/build.sh` (it regenerates
   `ngage_nos_stub.h`; then rebuild QEMU).

The phone in a window (one command; it ends the phone when the window closes):

```sh
python3 eka1/tools/ngage_phone.py            # --phone NAME, --fresh, --scale 3, --keep
```

- **The phone's files.** `build/ngage/phones/NAME.flash` holds the phone's own flash: C: and
  its settings, the 64 KB blocks that differ from the firmware. `--fresh` deletes it.
- **Keys.**

  | Host key | Phone key |
  |---|---|
  | F1 / F2 | left / right softkey |
  | F3 / F4 | call (green) / end (red) |
  | F5 | menu |
  | arrows, Enter | navigation, select |
  | 0-9, `*`, `#` (F10) | keypad |
  | Backspace | C |
  | Left Shift | edit |
  | F12 | power (not modelled) |

  The buttons under the screen do the same.

## How it fits together

| Piece | Where | Notes |
|---|---|---|
| Board | `src/qemu-ngage/hw/arm/ngage.c` | memory map, flash chips (CFI01, read-while-write patch), interrupt controller (4 banks, bank 3 = FIQ), 6 timers, CBUS/UEM, LCD interface and console, kernel debug port → serial, catch-all logging of unmodelled pages, persistent flash (`flash=`) |
| NOS stand-in | `src/qemu-ngage/nos-standin/nos_standin.S` | copied over the NOS entry; registers the variant interfaces as trap objects; each call goes to the board's NOS device (0x0C1F0000); also the RTC hand-over and the ISI delivery DFC |
| ISI / PhoNet | `src/qemu-ngage/hw/arm/ngage_isi.c` | decodes NOSDRIVER sends; answers from `src/qemu-ngage/isi-replies.txt` (`reply RES REQ RESP BYTES`, `len=`, `echo`, `ind UNIT OBJ ...`) |
| Keypad | `src/qemu-ngage/hw/arm/ngage_keys.c` | the matrix ekeyb scans; QEMU input → matrix |
| Machine options | `-M ngage,ppm=DIR,isi=FILE,flash=FILE` | |

Environment knobs (experiments):

| Variable | Effect |
|---|---|
| `NGAGE_VERBOSE=1` | log every interrupt-controller, timer, CBUS and NOS access (slow) |
| `NGAGE_STARTCODE=N` | start-up code (default 1 = normal, -1 = off; Q51) |
| `NGAGE_ITEM12=N` | NOS product data item 12, the SW start-up reason (Q44) |
| `NGAGE_POWERON=N` | UEM power-on reason register (Q51) |

## Debugging

### Use a session, not repeated boots

`eka1/tools/ngage_sess.py` keeps one QEMU per session (`build/ngage/sess-NAME/`) and talks
to it over the gdb protocol, the HMP monitor and QMP:

```sh
S="python3 eka1/tools/ngage_sess.py -s dbg"
$S start [--flash FILE] [--catch 'fatal,pabt,dabt,VA=LABEL?EXPR,VA=~LABEL']
$S wait 200        # until a catch freezes the machine: registers, current thread, named stack
$S x VA N | regs | objects | stack PROC::THREAD | shot out.png | mon 'HMP command'
$S break VA | cont | pause | dump DIR | stop
python3 eka1/tools/ngage_sess.py dumpx build/ngage/sess-dbg/crash-1 VA N   # offline, no QEMU
```

- **Catches.**
  - Default: `fatal` (the kernel fault routine 0x5031e340), `pabt` and `dabt` (vectors
    0x0C/0x10).
  - `VA=LABEL?r1!=0`: freeze only when the condition holds.
  - `VA=~LABEL`: log time, registers, thread and the descriptor at r1 to `trace.log`, then go
    on. Use it for timelines such as process creation (EUser `RProcess::Create`
    0x503B3514). Each hit costs about 1 s, so keep it off hot paths.
  - A crash catch also writes `crash-N/` (SDRAM and state) for offline reads.
- **Useful catch addresses.**

  | Address | What |
  |---|---|
  | 0x503af660 | User::Panic |
  | 0x503b2f90 | RThread::Panic (r2 = reason; `?r2==11` for ViewSrv timeouts) |
  | 0x503b2f38 | RThread::Kill (filter with `?r1!=0`: EFile kills its helpers with 0) |
  | 0x50960e0c | Starter's list selection (r1, r2 = keys) |
  | 0x50bba6f8 | Startup's SetState (r1) |

- **The ISI picture.** Every channel send is logged
  (`grep -a ngage-isi build/ngage/sess-*/qemu.log`). Requests without a reply are marked
  UNANSWERED: check those first when something waits.
- **Logs.** `qemu.log` (board), `serial` (the kernel's debug port: RDebug output) and
  `trace.log` (logging catches). `qemu.log` is buffered, so its tail lags.
- **The phone window** uses session `phone-NAME`; any `ngage_sess` command can join it with
  `-s phone-NAME`.

### Who is blocked on whom

1. Freeze or `pause` the machine. Find the thread in `objects`.
2. Its saved SP is at DThread+0x158 (a run address).
3. In the moving model a process that isn't running has its stack chunk at its home address:
   `home = chunk base + (sp - 0x00400000)`.
4. Print the frames from there up; the deeper words in a stack scan are stale. Example
   (q88): Phone → GSMCUS `ReadSimAlsLine` → ETel `Get` → phonetsy waits for ISI `09 31`.
5. The usual chain on this phone: app → synchronous ETel/custom API call → phonetsy → an ISI
   request with a missing or wrong reply.

### Answering an ISI request

- Find the parser of the response in the client:
  - phonetsy's per-resource switches;
  - Nokia.dsy's receive virtual;
  - SoundDevice.mda.
- Take the accepted message id, the status byte, the sub-block count and the **minimum frame
  length** from it.
- isimsg reads with `Mid(offset, len)`: a reply shorter than the parser reads panics the
  client with USER 21.
- Unsolicited indications to an object that didn't subscribe panic phonet (`ECS panic` -5).
  The DSY takes SIM and security indications without a subscription.
- Mark every reply [V] (the shape from the parser) or [I] (the chosen values) in the table.

### Reading the firmware

```sh
OD=~/gcc-builds/gcc-14.2.0/bin/arm-none-symbianelf-objdump
$OD -D -b binary -m arm [-M force-thumb] --adjust-vma=0x50000000 \
    --start-address=VA --stop-address=VA2 eka1/rh29/output/rom/ROM.bin
```

- **Module layout.** `eka1/rh29/output/rom/manifest.json` gives each module's address and size;
  user exports are named in `eka1/analysis/user-exports.tsv`. Kernel exports are ordinals in
  `eka1/analysis/kernel-api-map.tsv`; names only where `ekern_evidence.tsv` proves them.
- **Imports.** An import stub is `ldr ip,[pc]; ldr pc,[ip]` plus the export table slot; map
  the slot's address back through the kernel-api-map.
- **GCC 2.9 vtables start with two zero words.** Method N is at +8+4·N from the vtable
  pointer: calling "+0x18" once ran the wrong method.
- ROM code (XIP) sits at the same address in every process, so one hardware breakpoint
  covers all of them.
- Thread and process names are 16-bit descriptors. The current thread is at
  `[0x80006740]`; the object containers are at 0x80000398 (processes) and 0x8000039c
  (threads).
- Kernel-side names from the 6.1 SDK may be one ordinal off in "range" rows of
  `ekern-align.tsv`: confirm by code before use.

### Research agents

- For a static question ("what does X need back"), a research agent with a narrow brief
  works well:
  - name the client, the request bytes and the observed state;
  - ask for the parser, the accepted reply bytes, the minimum length and [V]/[I] marks;
  - budget about 30 tool calls and no emulator (CLAUDE.md).
- Check its answer live with a catch before relying on it.

### Performance traps

- **Device accesses in the middle of a translated block are expensive.** QEMU 11 recompiles
  the block and longjmps for each one. Keep hot guest paths in RAM; the NOS idle hook is
  answered by the stub from RAM (Q56).
- **Don't log hot paths.** `ngage_pc()` unwinds the guest state, and logging every access
  cost most of the host time (28 MB of log a minute). The board logs the first 32 accesses
  per register; `NGAGE_VERBOSE` brings back the rest.
- **Measure boot time by screen changes**, not with logging catches: QMP screendump every
  second, hash the image.

## Next

1. **Games.** Snakes runs (2026-11-12). Its abort at 0xFFE503E4 was the game's BTGetName error
   path (null object) after GetHostName returned -44: fixed by the Bluetooth module. Next: more
   games and their own needs. Do not answer SIM `09 23 {25}`: "not available" stopped start-up.
2. **Game cards (MMC).** Done for plain FAT cards (above). A dump of a real game card next. Some N-Gage cards are
   copy-protected.
3. **Time at boot.** The RTC hand-over passes the RTC's own unset time, so every boot asks
   for the time. Pass the host time in request 1 (Q49).
4. **Power key.** UEM (CBUS dev 1 reg 0x1F bit 1, UEM interrupt 6).
5. **First-boot speed.** The C: format takes about 30 s, spent in the kernel and the NOS
   stub (medint's slot 6 calls).
6. **The LCD.** lcd.ldd's own transfers (DMA), so H-ngage-fb can go (Q47).
7. **Audio.** Bluetooth and emon work (H-ngage-bt). BT multiplayer needs a radio link (two
   boards, or the host's Bluetooth).
8. **Device model (P5).** `models/nokia-ngage-rh29`, sydb support, a smoke test.
9. **The real NOS.** Descrambling it is the owner's decision (Q20). It would replace most of
   the stand-in.
