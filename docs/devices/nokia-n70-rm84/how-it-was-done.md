# Nokia N70: how it got to the Menu, and what we learned

The Nokia N70 (RM-84, S60 2nd Edition FP3, Symbian 8.1a, EKA1, TI OMAP1710, BB5 modem) went from flash files to the
S60 home screen and the Menu grid in one session (2026-11-20), 11 stalls, on the 6630's board. The stall record is
[bring-up.md](bring-up.md); every workaround is in [../../hacks.md](../../hacks.md) "Nokia N70". The method is the
6630's ([../nokia-6630-rm1/how-it-was-done.md](../nokia-6630-rm1/how-it-was-done.md)); this page lists what was new.

## 1. What carried over unchanged

- The tools: `tools/fiasco.py`, `eka1/tools/xzip_core.py` (the same compressed core), `rom_manifest.py`,
  `port_exports.py`, `rm1_nand.py` (one new option).
- The board: every 1610 device of `nokia6630_soc.c` (GP timer tick, GPIO, SoSSI panel, NAND, SSI link, watchdog) and
  QEMU patches 0021-0027. The bootstrap's and the kernel's register literals are the 6630's, page for page [V scan].
- Six core patches (DSP boot, DSP timeout, Bal ords 19/22, EapGwc/MuMdrcGwc, SecEnvInit, DRMHelper), found at the N70's
  addresses by code match with the old words identical; and most ISI replies.

## 2. What was new

| Layer | N70 difference | Fix |
|---|---|---|
| RAM | the bootstrap's bank is 64 MB at 0x10100000 (6630: 32 MB); its stack fell off a 64 MB board and it restarted for ever | 128 MB |
| NAND | 32 MB of ROFS + ROFx: C: moves to 0x2600000 | `rm1_nand.py --user-start` |
| Kernel | another build: Kern::Fault export 278, current thread 0x8000295C, container entries {object, key} | `ngage_run.py` MODELS `nokian70` |
| Modem link | ssi.pdd sets AP wake without making the pin an output | the modem watches the output latch |
| Modem stack | **ISC** (IscIsiMultiplexer) instead of isa_if_driver: a configuration request (00 05 -> 06 {max size, peer object}), then channel opens answered as control messages from that peer object | reply table tokens `rq10`, `media=`, `sobj=` |
| SoC ID | SecPdd's monitor only calls the secure ROM when 0xFFFED400 reads 0x0333xx00 (an OMAP1710); else `b .` with the MMU off | the N70 board reports the 1710's IDs |
| ISI parsers | asc.dll's `1C 2E` and the TSY's `09 32` read more than the 6630's | replies read from the parsers |
| Keys | two extra matrix keys | `RM84_KEYS` |

## 3. Techniques that paid off

- **Port before you debug**: kernel words (current thread, containers) by matching every code user of the 6630's
  literal in the N70's kernel and reading the literal there (34 of 35 agreed); the 6630's patches by masked code match
  (branch and literal offsets masked), keeping the old-word check.
- **A stuck CPU in a `b .` with the MMU off**: map the physical address back to a module by searching the ROM for the
  instruction words around it (SecPdd's monitor copy), then read the branch that leads there.
- **A new modem stack**: decompile the multiplexer and find (1) what it accepts while unconfigured, (2) how its router
  tells control messages from data (sender object + media), (3) what completes a channel open; then answer only that.
- **isimsg message classes**: sub-block count and first sub-block offsets per message id come from the class's vtable
  (+0x10/+0x14), field offsets from its field-id switch: that gives the minimum frame length without guessing.
- Ghidra MCP for everything (localhost:8089): cut module at its VA, import, rebase (analysis follows), Thumb modules
  re-disassembled with TMode set, a whole-module decompilation into a C file, import stubs named from their slots.

## 4. Traps

- The FIASCO header says "OMAP1610": that is the loader's algorithm name; the SoC is an OMAP1710 (the kernel's chip check).
- `objects` failed with "Cannot access memory" twice for different reasons: kernel data not mapped yet (still in the
  bootstrap), and later the CPU in SecPdd's physical monitor with the MMU off. Check the mode/PC before blaming tools.
- A reply that worked on the 6630 can be too short on the N70 (asc `1C 2E`, the TSY's `09 32`): the panic (USER 21/22)
  comes from isimsg in the client's thread; catch User::Panic and read the client's parser.
