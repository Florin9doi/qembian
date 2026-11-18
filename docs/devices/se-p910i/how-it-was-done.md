# P910i: how it got to the home screen, and what we learned

The Sony Ericsson P910i (UIQ 2.1, Symbian 7.0, EKA1, Philips PNX4000) went from flash files to its UIQ home
screen in one session (2026-11-18): 17 stalls, a new QEMU board for an SoC with no datasheet, and a fake GSM side
for a protocol nobody here had seen. The stall-by-stall record is [bring-up.md](bring-up.md); the workarounds are
in [../../hacks.md](../../hacks.md) "Sony Ericsson P910i". This page is the method and the knowledge, written for
the next EKA1 phone on an unknown SoC and the next Ericsson (OSE baseband) phone.

## 1. The path, in phases

| Phase | Stalls | What it took |
|---|---|---|
| Inventory | - | `tools/fw/semc.py` already read every file; the ROM header's extension chain (each image header's word 23) and the bootstrap's memory table gave the kernel-side picture in an hour |
| Board to first tick | 1-3 | NOR at the flash file's address, a logging catch-all for everything else, the bootstrap's polls answered one by one; one QEMU bug (SCTLR bit 30) |
| Storage | 4-6 | a DiskOnChip API stand-in at TrueFFS's single internal dispatcher (the 9500's idea, one layer up) |
| GSM link and start-up | 7-10 | the OSE link: sync, the StartSignal, then each request the phone sent; I2C for the codec |
| UI and input | 11-13 | display = video RAM; touch and keypad panels; the wizard and the Power menu; C: kept across boots |
| Telephony to the home screen | 14-17 | the TSY's requests (SIM emergency numbers, SIM presence and SIM reads), a busy loop and a starved UI |

Research agents (static reading, ~30-40 tool calls each, [V]/[I] marks) did 14 focused reads: SoC map,
DiskOnChip, GSM link, StartSignal, display, sound, input, TeleApp, TSY link, splash, battery. Each one came back
with addresses to patch or a reply table line; most were right first time, and every one was checked by a run.

## 2. Building a board for an SoC with no datasheet

- **Start from the bootstrap's memory table.** EKA1 bootstraps carry a table of `[phys, size, attr, virt]` entries
  (here at 0x50000314, type in the low attr bits: 1 ROM, 2 RAM, 3 I/O). I/O pages with virt 0 get consecutive
  pages from the kernel's peripheral window (0x58000000 here) in table order: that one rule translates every
  register address in the kernel and the drivers.
- **Catch-all first, models later.** One low-priority region over the whole address space that keeps written
  values, logs the first accesses with the PC, and ORs in "ready" bits from a small table. The first runs then
  read like a to-do list: each poll is a log line with a PC.
- **The kernel names its interrupts.** The IRQ name table (64 TLitC pointers) gave every line's device; the
  dispatcher's code gave the controller layout. Device registers come from the drivers' accessor helpers.
- **Model the semantics the code depends on, not the chip.** The interrupt controller needed edge-select and
  polarity because the drivers acknowledge only edge lines and flip pin polarities; the LCD needed none of its
  chip's memory because the video RAM is a fixed RAM page; DMA only had to complete.
- **Shared QEMU code needs care.** QEMU applied SCTLR.TE (bit 30) on every core; the P910i's bootstrap sets bit 30
  on an ARM926 (an ARM920T clock bit). One-line patch, then the N-Gage, 7650 and 6600 boot checks.

## 3. Ericsson OSE baseband: the protocol, as far as we know it

- **Link** (UART unit 2, elh4drv.ldd): the phone sends "L1" until the peer answers "LA". Then frames
  `[dst pid][len LE16][src pid][signo LE16][len-2 data]`; frames to the phone carry one trailer byte.
- **Indications vs requests.** Signals 0xC350..0xC50E are indications, delivered to every subscriber (dst ignored);
  everything else is a request from a Symbian session pid (0xE0..0xE3, 0x8D, 0xDE, 0xDF) to a GSM process
  (0x30, 0x3A, 0x49, 0x61, 0x62, 0x69, 0x74 ...) and the answer goes back to the request's src.
- **EStart's StartSignal** is indication 0xC384, sent by the GSM side unasked about a second after sync.
- **Every request has a stub in swbp.dll** that allocates the signal (`bl 0x508a6ddc` with r1 = the number),
  waits for a list of signals (the literal loaded before `bl 0x508a6db4`, format `{count, signo...}`) and decodes
  the reply in the function called right after. The reply number is not request + 1 (4398 -> 4F4E, 413F -> 22C9,
  4339 -> 173E). A stub without a receive is send-only.
- **0x0533 is generic:** to GSM process 0x49, with a sub-command in its first data byte (0x18, 0x73, 0x5E, 0x0C,
  0x52, 0x9A, 0x1E seen: SIM access [I]); each sub-command has its own stub and reply signal, hence the table's
  `reply SIGNO/SUB` form. Decoders share a shape: `sig[3] != 0` = OK, then a block.
- **Indications the baseband sends by itself** (no request asks): the StartSignal 0xC384, the battery level
  0xC39C {percent, charger} (batpwr.lsu -> event 0x10202 -> status bar). Look for the subscriber's signal list
  (ereq subscribe calls) when a value is missing but nothing is unanswered.
- **The TSY speaks OSE, not AT.** erigsm.tsy goes through ereq.dll -> swbp.dll on the same link; ecmux.csy
  (MUXCOMM, eesp/edldrv) is only the data-call port.
- **Zero is not a neutral answer.** A zero count of SIM emergency numbers made Gsmadv leave KErrNotFound and the
  phone app panic (stall 14); a zero SIM-presence byte showed "No SIM card" (stall 16). Read what the caller does
  with a field before choosing a value.

## 4. Debugging techniques that worked (EKA1, `eka1/tools/ngage_sess.py`)

- **QEMU CPU-state trace on one block:** `--debug unimp,exec,cpu,nochain --dfilter 0xVA+4` logs r0-r15 at every
  entry to that block at full speed. On `User::Leave` (EUser 0x5004B184) it found stall 14's leaving call (lr)
  where breakpoints had failed.
- **Conditional breakpoints armed at run time:** `break 'VA=LABEL?r0==0xffffffff and 0x400000<=sp<0x500000'`
  (added to the tool here). `sp` in 0x004xxxxx = a thread of the process currently mapped at its run address.
- **CPU sampling:** pause/summary/cont a few times. Twice the answer was "always in one thread" (DOCBGTH's
  defragment loop, then SocketServer polling a UART): both looked like "the UI ignores input".
- **Reading a non-current process:** its chunk's home address (`DChunk +0x28`) + (VA - run base); `x` on the
  run address fails while another process is mapped.
- **Kernel words by literal voting:** port each 0x80xxxxxx literal from a known ROM's functions to the new ROM by
  matching the surrounding code; the most common answer wins (current thread 242 votes, containers 73). Kern::Fault
  matched word for word. EUser ordinals are frozen: `eka1/tools/port_exports.py` names a new ROM's exports from
  the N-Gage's list.
- **The page directory** was found by searching RAM for a 16 KB-aligned table whose entry for 0x50000000 maps
  the flash.

## 5. Traps (each cost a round)

- **Too many breakpoints fail silently:** with more than about six gdb breakpoints, none fired at all. Keep catch
  sets small, or use the CPU-state trace.
- **A logging catch ignores its condition** (`VA=~LABEL?EXPR` logs every hit, about a second each).
- **Breakpoints change more than timing:** with a hot conditional breakpoint armed, pen taps stopped registering
  (the pen's samples are timed in virtual time). The "no panic" runs under breakpoints were runs where the tap
  never happened: not a race.
- **Peripheral mappings differ by mode:** the media driver runs in EFile's thread in user mode, so its stand-in
  must use the user-accessible alias (0x5E000000), not the kernel's mapping of the same window (0x5C000000).
- **A peer log that buffers hides traffic:** the phone's request after the StartSignal sat in the log buffer
  until the log was flushed by a timer.
- **Stale stack frames mislead:** MediaClient frames on the dying thread's stack were from an earlier call; the
  leaving call was in Gsmadv.
- **"Nothing to do" has a value:** the defragment call had to answer TrueFFS status 63, or the background thread
  never slept.

## 6. Next

The open list is in hacks.md ("Not modelled / not working"): network registration and
SIM contents (the TSY's next requests), sound (codec + PCM), Memory Stick, RTC, Bluetooth/IrDA/USB. Each OSE
request is now a few minutes' work with the swbp recipe; hardware blocks need their driver read first.
