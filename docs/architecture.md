# qembian architecture

The phone's own ROM always runs unmodified. What qembian supplies depends on the kernel generation:

| Path | Phones | Kernel | Machine | Started with |
|---|---|---|---|---|
| **EKA2 on QEMU** (main) | Symbian 9.1 and later: S60 3rd/5th, Symbian^3, Anna, Belle, UIQ 3 | ours, from the open-source EKA2 + a personality | QEMU `-M syborg` | `tools/sydb/sydb.py phone NAME` |
| **EKA1 on QEMU** | Symbian 6.0 to 8.1: S60 1st/2nd, Series 80/90, UIQ 2, N-Gage | the phone's own | a QEMU board per chip (`-M ngage`, `nokia6630`, `p910i`, ...) | `eka1/tools/ngage_phone.py --model M` |
| **Hosted EKA2** (experimental) | N95 8GB, 808 | ours, as a 32-bit ARM Linux process | none: the phone's code runs natively | `tools/hosted/phone.py` |

```
            EKA2 on QEMU                              EKA1 on QEMU                 Hosted EKA2
 ┌──────────────────────────────────┐   ┌─────────────────────────────────┐   ┌──────────────────────┐
 │ phone ROM user side (unmodified) │   │ phone ROM (unmodified, kernel   │   │ phone ROM user side  │
 │ our kernel side: EKA2 kernel,    │   │ included)                       │   │ (native AArch32)     │
 │   personality, Syborg baseport,  │   │                                 │   │ our EKA2 kernel as a │
 │   stand-in drivers               │   │                                 │   │ Linux process        │
 ├──────────────────────────────────┤   ├─────────────────────────────────┤   ├──────────────────────┤
 │ QEMU 11.1.3, Syborg board        │   │ QEMU 11.1.3, board per chip     │   │ Linux (arm64/AArch32)│
 │ (src/qemu-syborg)                │   │ (src/qemu-ngage)                │   │ (src/hosted)         │
 └──────────────────────────────────┘   └─────────────────────────────────┘   └──────────────────────┘
        sydb (gdb, agent, catches, snapshots, UI as data, recorder) drives the EKA2-on-QEMU path
```

## EKA2 on QEMU

### Host: the Syborg board

One QEMU binary (`build/qemu-modern/`, QEMU 11.1.3 with `src/qemu-syborg/` copied in by
`scripts/build-qemu-modern.sh`) serves every EKA2 phone. The CPU is the profile's `board.cpu` (`arm1136-r2` for
the ARM11 phones). Everything phone-specific is a QEMU argument generated from the device profile
(`tools/device.py qemu-args`, docs/devices.md section 3), never a code change.

| Base | Device | IRQ | Source |
|---|---|---|---|
| `0x00000000` | RAM (profile `board.ram_mb`; 168 MB for the N95) | | |
| `0x60000000` | read-only flash window: the ROFS partitions (the phone's NAND), read by our `MEDROFS.PDD` | | `syborg.c` |
| `0xc0000000` | interrupt controller | → CPU | `syborg_intc.c` |
| `0xc0001000` | RTC (host time + an offset kept per session) | | `syborg_timer.c` |
| `0xc0002000` | timer (also the 32 kHz fast counter) | 1 | `syborg_timer.c` |
| `0xc0003000` / `0xc0004000` | keyboard (profile keymap) / pointer, touch | 2 / 3 | `syborg_input.c` |
| `0xc0005000` | framebuffer (profile screen size, panel model) | 4 | `syborg_fb.c` |
| `0xc0006000`–`0xc0009000` | serial 0–3 (serial 0 = the kernel debug port) | 5–8 | `syborg_serial.c` |
| `0xc000c000` | Ethernet NIC (on `-nic`) | 9 | `syborg_net.c` |
| `0xc000d000` / `0xc0010000` | NVMemory: E: / C: drive images | 14 / 12 | `syborg_nvmem.c` |
| `0xc000e000` | sound (`syborg-snd`: DSP-bridge streams, RAPIDO rings, MP3/AAC decoded on the host) | 13 | `syborg_audio.c` |
| `0xc000f000` | GL: OpenGL ES 1.1/2.0, OpenVG, EGL and composition on the host GPU | | `syborg_gl.c` |
| `0xc0011000` | platform: power off/reset, RAM size, device id, board config string, device tree | | `syborg.c` |
| `0xc0019000` / `0xc001b000` | cellular modem ports 0 / 1 (see below) | 15 / 16 | `syborg_modem.c`, `syborg_modem_core.c` |
| `0xc001d000` | accelerometer (gravity set by the host) | | `syborg_sensor.c` |

Also on the board: the Sony Ericsson communications processor stand-in (`syborg_ccpu.c`, UIQ 3 phones) on the
modem port, and the screen and sound recorder (`src/qemu-syborg/ui/avrec.c`, `sydb record`).

### Guest: the hybrid ROM

A phone ROM holds a kernel side (kernel, memory model, hardware drivers) and a user side (file server, window
server, every DLL and application). `tools/romcompose.py` builds one image from both:

- **Our kernel side**: the EKA2 kernel from the Symbian Foundation's `kernelhwsrv` (EPL), the Syborg board
  support package (bootstrap, variant, our drivers for the board above) and the stand-ins, built per device by
  `tools/device.py build`. Multiple memory model for most phones; the moving model (`syborgmv`) for the 9.1
  phones that need it.
- **The phone's user side**, byte for byte at its original addresses, so every import and DLL reference stays
  valid. Its ROFS partitions go into the flash window; a profile can make same-length edits there
  (`[[rom.rofs_patches]]`) or replace whole files with ours (`[[rom.rofs_replace]]`).
- QEMU loads the image with `-kernel` at physical `0x10000`; the Syborg bootstrap builds the page tables, maps
  the ROM at its linear base (`0x80000000`, or `0xF8000000` on the moving model) and starts the kernel.

**Personalities** (`personalities/`, one per Symbian version: `sym91`, `sym92`, `sym94`, `symbian3`, ...) make
our kernel look like the one the phone shipped with: the exec-call numbering (`exec.map`), structure and vtable
differences (`kabi.toml`) and kernel patches. The ROM header selects one at boot;
`tools/personality/check.py --device ID` compares a firmware's EUSER against it.

**Stand-in drivers** (`src/standins/`) replace the phone's kernel-side drivers for hardware the board does not
have (OMAP display, DSP bridge, ISC modem link, lights, security, ...). Each answers the interface the phone's
user-side clients use, at the level they need. The profile's `[[standins]]` picks them (bld.inf and ROM order
are generated); `config` keys reach a stand-in as the board config string.

**Graphics**: on the N95 family the phone's GLES library is replaced by ours (`src/sygl/`); on Symbian^3 and
Belle the phone's own libraries talk to our EGL/OpenVG (`src/khr/`) and composition backend (`src/gcebe/`).
All of them forward to the GL device on the host.

**Device model**: everything specific to a phone is declared in `models/<model>/model.toml` +
`firmware/<fw>/firmware.toml` (families in `platform/families/`); instances (`tools/instance.py`) are your
phones with their own drives. Details: `docs/devices.md`; adding a phone: `docs/adding-a-device.md`.

### Cellular modem: fake or real

The phone's ISC/ISA drivers are stand-ins (`src/standins/_shared/iscpeer.inl`) that pass every ISI message to
QEMU's `syborg-modem` device. That device answers in one of two ways:

- **Fake modem** (default, every BB5 phone): reply tables (`platform/isi/replies-bb5.txt` + the device's
  `isi-replies.txt`) and the stateful code in `src/qemu-syborg/hw/arm/syborg_modem_core.c` (MTC states, SIM,
  network, SMS, packet data). Shared with hosted EKA2.
- **Real modem** (`--modem real`, devices with `[modem_cpu]`: the N95 8GB): `syborg-modem` forwards the messages
  over a socket to a second QEMU, the `n95-cmt` machine (`src/qemu-syborg/hw/arm/syborg_cmtprobe.c`), which
  runs the modem firmware from the phone's own flash file (NOLO -> MCUSW) with its board described in
  `tools/cmt/n95-cmt.map`. Replies go back to the channel object that asked; the modem's power-off (Retu
  watchdog) ends the phone. `tools/cmt/isirecord.py` turns recordings of this into fake-modem corrections.
  Docs: `docs/research/n95-cmt-emulation.md`.

### Tools and debugging

- **sydb** (`tools/sydb/`, `docs/sydb.md`): a daemon per session runs QEMU and keeps a gdb attached; an agent on
  the phone (`devagent.exe`, installed on C:) does file, process, window-group and key operations. CLI and MCP
  server. Crash catches, snapshots and forks, IPC strace, `sydb ui` (the screen as data, docs/ui-automation.md),
  recording. `tools/sydb/kwalk.py` shows where every thread waits.
- **Symbols**: `tools/syb_sym.py` (ROM and kernel symbols, Ghidra decompiles per module), `SYBCODE:` lines for
  RAM-loaded code.
- **Serial 0**: `Kern::Printf`/`RDebug` and the kernel trace; `tools/isi.py log` decodes the modem traffic in it.
- Debugging method: `.claude/skills/symbian-debugging/SKILL.md`; tool list: `docs/tools.md`.

## EKA1 on QEMU

No open-source EKA1 kernel exists, so these phones run their whole ROM, kernel included, on a QEMU board modelled
after their chip, separate from the Syborg board (`src/qemu-ngage/`, built into `build/qemu-ngage/` by
`scripts/build-qemu-ngage.sh`): `ngage` (N-Gage QD; DCT4 variants for the 7650, 6600, 3230), QEMU's OMAP1510 for
the 9500 and 7710 (made an OMAP1610 for the 6630 and N70), `nokia9210`, and a Philips PNX4000 board for the
P800 and P910i, the last ones modelled from the phone's own code. The phone's modem side (NOS, OSE) is a stand-in
on the board. Start with `docs/eka1-ngage.md` and each phone's `docs/devices/<id>/bring-up.md`; the method for a
new chip: `docs/devices/se-p910i/how-it-was-done.md`.

## Hosted EKA2

Our EKA2 kernel runs as a 32-bit ARM Linux process; the phone's ROM code runs inside it natively, with exec
calls, interrupts and devices rerouted (no instruction emulation of Symbian code). It runs in a QEMU aarch64 test
VM today; the target is a device whose CPU still runs AArch32 code (a Pixel 3a). It shares the fake modem core
with the QEMU board. Docs: `src/hosted/README.md`, `docs/hosted-eka2/`.

## Build pipeline

Two containers, built on first use (`container/`):

- **Debian 6 i386** (`Containerfile`): the Symbian build tree (`scripts/setup-epocroot.sh`, which applies the
  kernel and board patches), Raptor (`sbs`) with GCCE 4.4.1 for the kernel, the board support package and the
  stand-ins (`sbs -c arm.v5.<udeb|urel>.gcce4_4_1.surge`), the host tools `elf2e32` and `rombuild`.
- **Debian 13** (`Containerfile.modern`): QEMU, GCC 14 for `arm-none-symbianelf` and `elf2e32_next` for the
  guest code we add to a phone (sygl, the sydb agent, uiprobe, isiprobe; these also need the S60 SDK, mounted at
  build time).

```
scripts/fetch.sh ─► pinned sources and toolchains (sources.lock)
scripts/setup-epocroot.sh, build-hosttools.sh, build-kernel.sh     once
scripts/build-qemu-modern.sh                                       once
tools/fw/store.py import ─► tools/fw/inventory.py ─► tools/device.py build ID [--kernel]   per phone
        flash files          extracted images,       stand-ins, kernel side, composed ROM,
                             symbols, driver list    GL blob, device tree, reply table
```

Frozen DEF files fix export ordinals; `.cia` files are C++ with inline assembler converted to GCC syntax.
Build details and their history: `docs/build-log.md`.

## Patch sets

Everything we change in other projects is kept as patch files and applied at build time, never as edited copies:

| Patch set | Applies to | Why |
|---|---|---|
| `patches/kernelhwsrv/platform/` | the open-source EKA2 kernel | build with modern toolchains, tracing, kernel personality support |
| `patches/kernelhwsrv/policy/` | the kernel | optional run-time policies (e.g. survive a critical user thread that exits) |
| `personalities/*/patches/` | the kernel | each Symbian version's call numbering and ABI differences |
| `patches/qemu-baseport/` | the Syborg board support package | phone-like CPU, RAM, timers, RTC, display modes, keyboard |
| `patches/qemu-modern/` | QEMU | CPU model details the phone firmware relies on |
| `patches/qemu-ngage/` | QEMU (EKA1 boards) | OMAP1510/1610 details the EKA1 phones rely on |
| `patches/elf2e32_next/` | elf2e32_next | post-linking GCC 14 output |
| `models/<id>/code-patches.txt` | the composed phone ROM | small byte patches where a stand-in cannot help (each one documented, original bytes verified before patching) |

Every workaround is listed in `docs/hacks.md` and `models/<id>/hacks.md`.
