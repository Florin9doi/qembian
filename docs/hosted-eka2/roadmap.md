# Hosted EKA2: phased roadmap, exit and kill criteria

Effort is one experienced person with agents, full time, in calendar weeks; ranges are honest guesses
[inferred]. Every phase ends with a written result in this folder; a kill criterion met sends the
work back to (d) (today's QEMU, incl. Android) or, for a non-Qualcomm phone with EL2, to (b).

| Phase | Content | Exit criteria | Effort | Kill criteria |
|---|---|---|---|---|
| **P0 feasibility on real hardware** | On the owner's Pixel 3a (stock + adb, or rooted): `getprop ro.product.cpu.abilist`; run a static armhf binary; `cat /proc/cpuinfo`; boot log / `dmesg` for "started at EL1/EL2"; on a pmOS or Mobian boot (`fastboot boot`, no flash): `/proc/sys/abi/swp`, `cp15_barrier`, a 32-bit test that `mmap`s `0x80000000`, `0x90000000`, `0xC8000000` `MAP_FIXED`, executes `swp`, a CP15 `dmb` and a `svc #0x800001` under a seccomp filter keyed on `instruction_pointer`; `getconf PAGE_SIZE`; display/touch/evdev/DRM present | 32-bit EL0 works; the three mappings succeed; `swp` and `svc` are trapped as designed; `/dev/kvm` absent (documents option b as dead); pmOS/Mobian boots with display and touch | 1-2 days | 32-bit processes refused by the mainline kernel on this SoC, or a fixed mapping impossible, or no Linux image boots with a display: stop, keep (d) |
| **P1 hosted nanokernel** | `nkern/linux` from `nkern/win32` + the generic `nkern/*.cpp`, built with `arm-linux-gnueabihf-g++`; scheduler lock, futex parks, timer thread (1 ms), DFC threads, `NKern::Sleep`, fast mutexes, nanokernel timers; a 64-bit nucleus only as launcher; runs on the `virt` machine (`qemu-setup.md`) | a test exe with two nanothreads and a semaphore, timers firing at 1 ms, round-robin at one priority, a DFC queue, all under the EKA2 scheduler's decisions (trace shows the same reschedule sequence as the Win32/ARM kernel for the same program) | 2-3 weeks | the Win32 scheduling model cannot be reproduced with futexes/signals without touching `nkern/*.cpp` beyond `#ifdef` glue |
| **P2 first unmodified ARM32 Symbian exe** | `kernel/linux` (exec dispatch, personality tables from `exec.map`, exceptions via signals), minimal `memmodel/emul/linux` (ROM mapping, kernel data memfd, chunks, one process), trampoline patching of EUSER stubs by the composer, seccomp safety net; the ROM's EUSER + a tiny ROM-resident test exe (our own, built with GCCE as today, placed in the composed ROM) | the test exe runs from the ROM at `0x80000000+`, allocates on `User::Heap()` through fast exec F01, creates a thread, waits on a semaphore, `User::After`, `RDebug::Print` arrives on the trace socket; exec count per second measured with and without stub patching; `swp` sites counted | 3-5 weeks | patched-stub exec cost > 1 us or SIGSYS path needed for > 5 % of execs after patching; or EUSER needs a kernel service that only the MMU can give (none expected: the Win32 emulator proves EUSER's needs are logical) |
| **P3 processes and client/server IPC** | one Linux process per Symbian process; the agent thread; shared kernel data across processes; global chunks; IPC descriptor copies (memfd alias); `RServer2`/`RSessionBase`, `RMessage2` read/write, request completion across processes, thread/process death and cleanup | two ROM exes (our own) as client and server exchange descriptors both ways 100,000 times; a client death mid-message is handled (KERN-EXEC panics as in the reference); the exec/IPC traces match the syborg reference for the same binaries | 4-6 weeks | cross-process kernel state cannot be kept coherent without a lock per exec (performance), or thread creation/kill across processes is unreliable under signals |
| **P4 F32 and system start-up** | RAM-loaded code segments (memfd at the fixed code address, the loader path), local media PDD over drive images, ROFS media stand-in, `ESTART`, the phone's `efile.exe` loader, then the real boot: `domainSrv`, Starter/SSM, the fake modem (compiled in), `ECOM`, `FBSERV`; no display yet | the N95 ROM (sym92 personality) boots to `100058f4/1 = 3` (start-up done) headless with every Nokia server of the reference boot started; exec trace of `efile` start matches the reference call-for-call; a sum of unanswered requests equal to the reference | 6-10 weeks | the loader/codeseg model needs per-process code addresses that the multiple model does not have (then the moving-model path, Win32-style data copying, would be required: an extra 4-6 weeks, decide then) |
| **P5 WSERV with a framebuffer** | LCD stand-in with a DRM/KMS (virt: `virtio-gpu`) back end, keys/touch from evdev, sound to ALSA (optional), `sydb` `hosted` session type: log, screenshot, keys, strace, catch, gdbserver | the N95 home screen, Carousel and Switch off (`tests/device-smoke.sh` steps) on the `virt` machine; screenshots match the reference within tolerance; the N8 (flexible model, ARMv6K EUSER with TPIDRURW) boots to its home screen without the GPU path | 4-8 weeks | WSERV/alf depend on a display driver behaviour only an MMU-backed shared chunk gives (unexpected) |
| **P6 the Pixel 3a** | pmOS/mainline (sdm670-mainline tree) on the phone, the same user-space tree, DRM panel + touch, ALSA; measure: boot time, app start, exec cost; then the N8/808 host-GPU path through Mesa freedreno | N95 home screen on the phone's display with touch/keys at native speed; boot time measured against the Android TCG build (expect >= 5x faster [inferred]); N8 home screen; a written comparison | 3-6 weeks (+4-8 for the GPU path) | panel/touch on mainline unreliable on the owner's unit (aftermarket displays break touch [verified: ferdiOS note]) with no Halium fallback; or native speed < 2x the TCG build (then the signal/trap overheads dominate and the design is wrong) |

Total to a phone home screen: 24-40 weeks; with the GPU path and the 808: 30-50 weeks.

## Main risks (ordered)

1. **The semantics tail (P3-P4).** Everything the ARM kernel does in `.cia` (exception frames, user
   callbacks `NKern::QueueUserModeCallback`, `User::Leave` frames, `TRAP` fast execs, thread kill
   from another process, trap handler on exception) must be re-done with signals and `ucontext`.
   The Win32 variant shows it is finite (1,425 + 2,176 lines) but each piece hides a week.
2. **Exec cost.** Patched stubs make it a call; anything left on the SIGSYS path costs microseconds.
   `User::Heap()`, `TrapHandler`, `PushTrapFrame` are per-call fast execs. Count them in P2.
3. **`swp` and CP15 barriers.** Every `swp` is an undef trap under Linux emulation. If the count per
   second is high (request completion is one per completed request), patch those sites too.
4. **Phone kernel-side drivers.** Any phone LDD/PDD we keep today (kabi-checked) would need the hosted
   kernel to export the 9.2 vtable ABI from a Linux-built ELF. Decide per driver in P4; the
   fallback is a stand-in.
5. **Fidelity.** No kernel/user protection, no ASID/domain effects, Linux signal timing. Bugs found on
   the hosted machine must be reproduced on the syborg reference before they count as firmware facts.
6. **Hardware window.** AArch32 user mode exists on the 3a and on 2022-23 SoCs' little/mid cores only
   (Linux asymmetric 32-bit); Pixel 7+/X3-A715 class cannot run it. (d) stays the portable path.

## What the owner gets at each exit

- P0: a one-page fact sheet about the phone (cheap, useful for (b)/(c) decisions too).
- P2: a measured answer to "is a hosted EKA2 fast and sane on this hardware", with the trampoline
  composer feature usable on its own for an exec-cost study.
- P4: the first headless native Symbian boot; the exec/IPC trace tooling works on both machines.
- P5/P6: a native-speed N95/N8 on the Pixel 3a with sydb; the QEMU machine stays the reference and the
  only option for 64-bit-only hosts.

## First real phone environment (owner's decision, 2026-11-12, reprioritised the same day)

The hosted kernel targets a real phone user side (unmodified ROM; drivers replaced by stand-ins, the ROM
recomposed as today), in QEMU first.
1. **Nokia N95 / N95 8GB first** (S60 3rd FP1, Symbian 9.2, ARM1136 = ARMv6, framebuffer UI without
   compositing). Why it fits best: the **multiple memory model** gives every process the same fixed
   addresses (ROM 0x80000000, process data 0x00400000+, global 0x90000000), which maps 1:1 onto one Linux
   process per Symbian process with the kernel data shared (htest t_map, t_shared); it is the best-tested
   phone here (sym92 personality, seeds, smoke test, sound, SIM, SMS); ARMv6 semantics are closer to an
   ARMv8 AArch32 core than ARMv5. Its user code is still largely ARMv5-built: SWP sites must be counted
   (trap cost) and patched if hot.
2. **Nokia N73** (S60 3rd, Symbian 9.1, ARM926 = ARMv5TE, **moving** memory model, `syborgmv`): later, as
   a compatibility check. The moving model relocates the running process's data (run vs home address),
   which is the Win32-emulator style of data copying; plus pre-ARMv6 unaligned-load rotation semantics.
3. **Nokia 5800** (S60 5th, touch, flexible model, no GPU compositing): only if the N95 path fails; later
   phones (N8, 701, 808) need the GPU path and compositing.
Before P4: htest cases for the ARMv5/v6 behaviours the N95 ROM relies on (unaligned accesses, SWP in hot
paths, LDREX/STREX, VFP), run on the virt machine and on the phone.

## N95 boot status (2026-11-12) [verified, kn_efile]

The unmodified N95 user side runs on the hosted kernel up to the system servers: efile.exe (drives Z:,
C:, E:), EStart.exe, domainSrv, SecEnvInit, IntegrityCheckServer, EInfoServer, centralrepositorysrv,
LanguageSelection (~54k exec calls in 8 s under TCG). What made it possible:
- the hosted board (src/hosted/variant/hboard.cpp): I/O bus with device models (NVMemory), interrupts,
  driver physical RAM, physical windows (I/O, ROFS flash, RAM);
- built-in kernel modules from their own sources (htest KMODULES; a module can name its .mmp): ELOCD,
  MEDINT, medrofs, MEDNVMEMORY, mednvmem2, the ISC stand-ins, custom, sosadlserver, d_display, secldd,
  ehwa(omap), dvfs_interface, mobilecrashdriver, light ldd/pdd, PIPELIB. Each module is one partial link
  with only what other objects use left global (modules from one source keep their own code and data);
- per-process EXE data (memmodel/linux mprocess.cpp, mutils.cpp): every EXE's .data/.bss in its own
  memory file, mapped at the run address on process switch, reachable through an alias for IPC.

**The fake modem (done 2026-11-12):** the QEMU device was split into a modem core shared by QEMU and the
hosted board (src/qemu-syborg/hw/arm/syborg_modem_core.c/.h, dual licensed GPL-2.0-or-later OR EPL-1.0 by
the owner) and two hosts: syborg_modem.c (QEMU: properties, MMIO, timers, NIC, migration) and
src/hosted/variant/hmodem.cpp (I/O bus windows, interrupts 15/16, a timer thread; no network or sound
yet). iscpeer.inl reaches the registers through ModemRead/ModemWrite (volatile accesses on ARM, the I/O
bus hosted). With replies the boot goes on to the System Agent and the window server (kn_efile).

**Display, variant HAL, exceptions (2026-11-12):** LCD.DLL (baseport fb) built in on a syborg-fb register
model (hboard.cpp: size from the profile, pixels in the driver's video RAM: HostedFbPixels); the test
variant answers the variant HAL as our Syborg variant (assp.cpp); hardware exceptions on nanothreads go
to the kernel (ncthrd.cpp: signal -> exception record on the thread's stack -> trampoline ->
Exc::Dispatch), so kernel-side faults on bad client descriptors return errors as on ARM. FbServ, the
window server and EComServer run (~143k exec calls at 15 s).

**RAM-loaded code and the start-up chain (2026-11-12):** RAM code segments (mcodeseg.cpp DoCreateRam: RAM
code region 0x70000000, DLL data region 0x78000000, the loader writes code and the .data template, EXE
data copied into the process area when loaded, DLL data per process as ROM DLL data; export directory,
exception descriptor); built-ins ECOMM (fixed copy: pointer/zero comparison), EUART1, EKEYB, EPOINTER.
Fixes found on the way, each pinned by kn_efile: zero-length IPC copies from address 0 (loader, -38);
TExcTrap::Exception did not leave the kernel level the exception path entered (completion callbacks never
ran: FbServ/ECom/window server deadlock); modules whose .mmp names baseport specific/ got the real
syborg.h. Start-up runs EStart, the window server, FbServ, ECom, mobile crash, C32/Etel, FeatMgr, mpm (~170k
exec calls by 60 s).

**The Nokia splash screen (2026-11-12):** with the device's own LCD.DLL build (lcd_n95 from the profile:
native-landscape panel; htest takes [kernel.lcd] and .mmp MACROs), built-in modules reloading with fresh
.data/.bss, and ROM DLL static data mapped at its fixed address, start-up runs Starter, SplashScreen,
AknIconSrv, ApsExe, EikSrvs, AknSkinSrv and the DRM servers (~810k exec calls by 60 s under TCG), and the
framebuffer shows the Nokia splash (kn_efile writes it to build/hosted/htest/vmout/<run>/kn_efile-fb.ppm).

**Next:** run on to the idle screen (Phone, SysAp, Startup; DcfRepSrv panics CDCFREPSRV 1000), a live
display window instead of a dump, the keyboard device model (keys), speed (several times slower than
the ARM QEMU board under TCG).

## Speed (2026-11-12, owner's priority: speed, then a live display, then the home screen)

Measured by the time each process starts (kn_efile notes `started: NAME at N ms`), QEMU TCG cortex-a55:

| Change | SplashScreen starts | EikSrvs starts | exec calls in 60 s |
|---|---|---|---|
| SIGSYS trap per exec (baseline) | 16.0 s | 29 s | 0.81 M |
| direct exec calls (patched stubs) | 6.0 s | 8.7 s | 1.08 M |
| + tick suppression (IdleTime/Advance) | 5.2 s | 7.0 s | 1.07 M |
| + lone `svc` stubs patched too | 4.9 s | 5.9 s | 1.07 M |
| + EXE data copied on switch, not remapped | 4.6 s | 5.6 s | 1.07 M |
| ARM board (QEMU syborg, IPC trace build) | 3.7 s | 4.2 s | - |

Start-up is 5.2x faster than this morning (EikSrvs 29 s -> 5.6 s) and within ~1.4x of the ARM board
(intentengine 16 s vs 11.7 s). Tools: kn_efile `started: X at N ms`, `htest --env PROFILE=1` (sampling
profiler, nkern/linux/hprof.cpp: region split and hottest pcs, symbolise with llvm-addr2line on dev/<device>/t/kn_efile),
counters in the probe (thread switches, interrupts/suppressed ticks, idle). Where the rest goes (20 s at
1.07 M exec calls): the process is on the CPU ~25% of the time; ~109k thread switches, each a futex hand-over
between Linux threads (TLxEvent Set/Wait = ~27% of the CPU samples, plus wake-up latency); the interrupt lock
~16%; ROM code ~24%. A speed advisory (2026-11-12) ranks the next steps: measure wake-up latency first,
then one CPU for the whole machine, private futexes, longer tick suppression, interrupts on the calling thread;
all nanothreads on one host thread only if hand-overs still dominate (future.md sections 1-3).

Fixed on the way: the direct path must realign the stack (EUSER calls stubs with sp 4 mod 8; clang code
assumes 8 and computed `d + 6` as `d | 6`, the modem's IMEI reply came out shifted and the DRM server waited
for ever); ARM11 CLZ encodings emulated (AknIconServer). Next measures: process switches (EXE data remap,
two mmap per switch), thread hand-over cost, then the display.

## The home screen (2026-11-12)

The unmodified N95 user side reaches its idle (home) screen on the hosted kernel within ~20 s under TCG:
active standby with clock and date, shortcut bar, softkeys. The last blocker was the start-up tone: Startup
waits for it to finish, and the N95 plays it through the ehwa stand-in into syborg-snd, which the hosted
board now models as a sink without output (variant/hsnd.cpp: the QEMU device's registers, rings consumed at
the playback rate, watermark/EOS interrupts); sybsnd.h's register accessors go through the I/O bus when
hosted (they dereferenced the registers directly).

## Live display (2026-11-12)

`python3 tools/hosted/phone.py` boots the N95 on the hosted kernel in the test VM and shows its screen in a
window (about 25 s to the home screen under TCG). The VM gets an ivshmem device whose memory is a host file
(build/hosted/htest/phone.shm); variant/hdisplay.cpp copies the changed framebuffer rows into it and bumps a
frame counter, the viewer maps the same file. Closing the window (or Ctrl-C) stops the VM. Keys go the other
way: the window writes scan codes (the device keymap, the sydb phone window's host keys: arrows, Enter = OK,
F5/F7 = soft keys, digits, Backspace = C) into a ring in the same memory, the VM feeds them to a syborg-keyboard
model that the unmodified EKEYB reads (hkeyb.cpp); on the home screen "1" opens the dialer. A start-up stall at the start-up tone in about one boot of three was a deadlock:
the sound ISR reading syborg-snd while the interrupted nanothread held the device lock (fixed, kn_devlock).

**After the home screen (2026-11-12, phone window):** the first home screen is drawn at ~20 s, then the phone
stays busy until ~63 s: efile and its local-drive thread with sisregistryserv (the boot-time drive and SIS
registry scans), no timeout. In a 60 s boot only ~14 s are CPU time of the process and ~4 s idle: the rest is
thread hand-overs (~475k, 22 us each under TCG in nk_basic's ping-pong, plus the waits between them) and
interrupt round trips. One vCPU (`--smp 1`) changed nothing, so it is the switch cost itself: the
single-host-thread design (future.md section 3) is the fix for this phase, and native hardware cuts the
per-switch cost by an order of magnitude [inferred]. The null thread spun at 100% after boot (fixed: idle =
WFI in NKIdle; now ~14% at rest, mostly the Supervisor thread, the display copy and lxtop).

**Explicit minimal guest kernel (2026-11-12):** the test VM's Linux went from arm64 `defconfig` (4,693 options,
78 MB Image) to `allnoconfig` + 51 listed options (558 after dependencies, 4.8 MB Image;
src/hosted/linux/hosted.config). Under TCG the guest kernel's own work is emulated too, so the boot got faster:
EikSrvs starts at 3.95 s (5.65 s), the phone settles after its boot-time scans at ~34 s (~63 s). One trap on the
way: with `CONFIG_EXPERT`, `COMPAT_32BIT_TIME` must be listed, or every 32-bit sleep and clock call fails.

## Belle FP2 (Nokia 808) on the hosted kernel (2026-11-12, owner's goal: the 808 on the Pixel 3a)

Headless (`htest --rom nokia-808-rm807 kn_efile`), no network or sound needed. Fixed so far, each with a test:
the user-side thread-data block (ARM1176 kernel flavour per device), inline exec svcs outside EUSER's stub block
(trapped and patched on first use), drive images larger than 4 GB (pread + overlay), the flexible model's ROM DLL
data area (our RAM DLL data region moved below it), the profile's stand-ins built in (ISA modem driver, surface
manager, touch, accelerometer, boot properties, audio), a modem reset as a failure. The boot now runs EStart, the
system servers and system state management (~75k exec calls) until the window server: it fails to initialise
(EWsPanicFailedToInitialise) because its render stages (alf, compositor: GCE, EGL/OpenVG) need the syborg-gl
device, which the hosted board does not have yet. Next: the graphics path (syborg-gl as a QEMU-free core).

**808 graphics on the host (2026-11-12):** syborg-gl runs in a host GL server (src/hosted/glserver: QEMU's
syborg_gl.c unchanged, against a small QEMU shim) on the host GPU; the hosted board forwards its registers and
serves guest memory over a shared-memory channel (`htest --gl`, tests: tests/glserver_test.py, kn_gl). With user
chunks below 0x80000000 (FbServ hands out heap addresses as TInts) the 808's window server initialises, GCE
composition and EGL run on the host GPU (Mesa radeonsi here). Open: the SQL server reports its databases corrupt
(KSqlErrCorrupt: tzserver, wmdrmserver), start-up then resets the phone.

**808 status, late 2026-11-12:** start-up runs to the system-state sequence; the window server, GCE and EGL work on
the host GPU. Blocker: the SQL server reports databases corrupt (KSqlErrCorrupt) on the hosted kernel and not on
QEMU, even a database it creates fresh: tzserver dies, start-up resets the phone. Measured so far: the bytes the
server receives match the files (IPCSUM CRCs vs host copies); SQLite's corruption point is pager.c:4970
(sqlite3PagerAcquire with page 0, PROBE_CODE on sqlite3CorruptError in sqlsrv.exe at +0x11074); not the CPU model,
the heap clamp, the RAM size, IPC copy chunking or user stack contents. Tools added: SYBCODE lines and PROBE_CODE
probes for RAM-loaded code, IPCSUM, NVDELTA (+ tools/hosted/nvdelta.py), clean seeds (tools/hosted/cleanseed.py).

**Fixed (2026-11-12): user stack alignment.** On ARMv6K phones (user-side thread data) every user thread started
with sp 4 mod 8; the phone's kernel gives 0 there. Varargs read 64-bit arguments from the wrong slot: SQLite's
`printf("%lld", 3)` wrote "12884901891", schemas had root page 0, every database read as corrupt. Found by
reading the stack at the corruption probe. Method, ABI checklist and tools: `debugging.md`. The 808 now passes the
SQL stage, loads its locale and runs system states 101-103; it then goes to state 117 (start-up failure) and
resets at ~37 s. Next: why 103 -> 117 (compare the state manager's commands with QEMU's IPC trace).

**Fixed (2026-11-12): second instances of an EXE; the composed screen.** The diff of sysstatemgr's IPC after state
103 (hosted `--trace 80000800` vs `build/sydb-h808ipc`) showed the loader answering -5 to the start-up list's
`cntsrv.exe -nontransient`: a second process of a loaded EXE, which the hosted memory model refused as the Win32
emulator does. With `AttachExistingCodeSeg` the 808 reaches state 104 (start-up phase "critical OK") and stays up.
The GL server now publishes the composed frame and the board shows it: the 808's start-up animation (the progress
dots) on the host GPU. Open: state 110 not reached by 90 s (QEMU: 51 s); weatherpublisher dies with KERN-EXEC 3
(not on QEMU). Fixed the same day: `Lookup(0)` returned the word before the export directory (libdl's dlopen of a Qt
plugin took it for a named symbol table); `GetMemoryInfo` answered. Open: the user window (1.75 GB shared by every
process's chunks, heaps clamped to 16 MB) fills up at ~100 processes (welcome2 STDLIBS-INIT -4, timing-dependent);
design reviews: `chunk-address-space.md` (plan: per-process local window, as the multiple model).
At 150 s the 808 shows its start-up animation (the Nokia "hands", host GPU). The N95 board gained the
framebuffer interrupt (VSYNC): its screensaver band now shows the clock instead of black.

**Local window (2026-11-12):** process-local chunks get a window every process has whole, as the multiple model
(`chunk-address-space.md`, `src/hosted/memmodel/linux/mlocal.cpp`). The 808 runs ~130 processes with 34 MB
heaps without running out and reaches system state 110 (start-up complete, as QEMU); the screen still shows the
start-up animation at 150 s.

**808 past the animation (2026-11-12):** physical commits for driver chunks (the audio DSP window): the audio engine
lives and start-up shows the first-boot language dialog, as QEMU. The phone window takes the device's screen size,
takes the mouse as the finger (hosted syborg-pointer) and shows the device's keys as buttons. With a settable RTC the first-boot wizard
(language, region, date, time) ends on the **home screen**; Menu opens the application grid, the lock key locks,
taps open apps (Calendar), all composed on the host GPU.



## Speed, 2026-11-13 (the 808's UI)

Tool: `tools/hosted/uibench.py` (headless boot with the post-wizard seed; start-up, swipe frames per second,
Menu and app-launch latency from the screen's frame counter; `--env GLSTAT=1` prints per-second counters:
GL requests and their time, thread switches, interrupts, exec calls by number). Findings and steps:
- GL channel: ~1500 register requests and ~3000 memory callbacks a second while drawing, 100-150 us each (up to
  23% of the time). Channel v3 (deferred writes, prefetch, write log): ~400 requests and ~250 callbacks.
  The host server is then busy ~100 ms/s with GL work itself.
- `BTraceOut`: ~500,000 calls a second from the font and bitmap client/server, each a full slow exec. Now
  answered in the trampoline when the category is filtered out.
- Display thread: 19% of a CPU composing unchanged frames; now copies changed rows directly, ~7%.
- Thread switches: 10,000-20,000 a second while drawing (~17 us each under TCG). Private futexes and waking
  only sleeping waiters made no measurable difference; spinning before sleeping caught 18% and was turned off.
  The real cure is section 3 of future.md (all nanothreads on one host thread).
- One CPU for the VM (`--smp 1`) is a loss: the busy-polling devices (GL requests) need a second CPU.

## 808 Settings -> Phone hang (2026-11-13), and the device bundle

- The hang (QEMU and hosted): Settings -> Phone starts NfcServer and waits for its rendezvous; nfcserver never
  finished starting because the TSY's UICC power-control request 8C 21 {F7} (NFC secure element on the SIM)
  was never answered. Reply found in the 701's nokiatsy (msg 22, sub-block 002A flag 0 = no SE):
  platform/isi/replies-bb5.txt. QEMU: Settings -> Phone opens. Found with the IPC trace (gsapp's last message:
  a process launch; nfcserver's ETel requests 36000/36003 never completed).
- Hosted: with that reply, the state manager's command thread panics USER 11 right after (dcmostartupcustcmd.dll),
  only on the hosted kernel; the reply is held back there for now (docs/hacks.md). Open.
- Hosted, intermittent: ~3 boots in 15 stop at system state 104 with the home screen up but its widgets
  missing (empty animated slots), the window server's compositor busy. Open.
- Phone-mode diagnosis: Ctrl+P in the window (shared word 0x80) prints PHONEDIAG: servers with unanswered
  messages, user threads and their waits.
- `tools/hosted/device-bundle.py` + `docs/hosted-eka2/pixel-3a.md`: the phone for a real arm64 Linux device
  (Pixel 3a under postmarketOS), and `phone.py --attach`. Both phones boot with the kernel's CP15 barrier
  emulation off [V]; the N95's EUSER needs no SWP.

## QEMU vs hosted, measured (2026-11-13, tools/ui_compare.py)

Same actions on both, frames counted at the GL device (syborg-gl frame-file), the 808 settled 90 s after state 110
and unlocked, medians of 2 runs:

| | QEMU | hosted |
|---|---|---|
| Menu open: first frame / settled | 99 ms / 1.09 s | 140 ms / 1.17 s |
| Menu close: settled | 0.46 s | 0.52 s |
| Swipe: frames composed in 1.5 s | 15-19 | 15-17 |
| Calendar: first frame / settled | 155 ms / 0.83 s | 179 ms / 0.94 s |

So the phone itself is 7-15% slower hosted, +25-40 ms to the first frame. The large difference seen in the windows
was the hosted display path: the GL server published on a 16 ms timer, the VM's display thread copied every 30 ms
and the window polled every 20 ms; during a swipe 7 of 15 composed frames reached the window, 25-60 ms late.
Fixed: the server publishes each composed frame as it completes, the window reads it from the channel file directly
(one hop, as QEMU's window) every 8 ms; 11-15 of 15-20 composed frames now reach it (the rest are identical frames).
Earlier per-switch estimates (future.md 3) were not measured in the UI: thread hand-overs are not shown to be the
cause of anything. Measuring traps found on the way: the 808 locks its keypad after ~1 min idle (touch ignored);
End returns to the application grid, not home.

## Home page 2 (widget placeholders), profiled (2026-11-13)

Page 2 of the 808's home screen shows widget slots with spinning placeholders that never load, on QEMU too (not a
hosted bug). They animate at 8.5 fps on QEMU and 5-6 fps hosted. Profile of 30 s of that animation, 90 s after state
110 (`htest --phone --env PROFILE=1 --env GLSTAT=1`; Ctrl+P opens and closes a profile window;
`tools/hosted/profsym.py LOG --device ID` names the samples and the callers of the hot functions):

| share of the process's CPU | what | why |
|---|---|---|
| ~23% | `MapChunk` + `TBitMapAllocator::NotAllocated` + the mmap calls | homescreen.exe, AknIconSrv.exe and sqlsrv.exe use the same window addresses (the window is full) and displace each other on every switch: ~350 switch-in remaps a second each, and `MapChunk` tests every page of the chunk's *maximum* size (13,000 pages for the home screen's heap): 8.2 M page tests a second (`LOCALSTAT` lines, HSTAT `scanned`) |
| ~13% | `memmove` from `MM::DoProcessSwitch` | DLL writable static data swapped in and out on process switches (the emulator-kernel model) |
| ~11% | `TLxEvent::Set/Wait` | thread hand-overs (11-12 k a second) |
| ~4% | `HostedExecBTraceB` | the filtered-trace fast path, called from fbscli |
| ~22% | ROM code | the phone's own work (fbscli, scdv bitmaps, euser) |

So about a third of the CPU while home page 2 animates is the hosted memory model's process-switch work, which the
phone's own kernel (flexible model: a page-table switch) does not do.

Fixed the same day, measured the same way (one run each, page 2 frames over 30 s):

| | page 2 | local remaps/s | process-switch share of the CPU |
|---|---|---|---|
| before | 6.0 fps | ~750 | MapChunk + page tests + mmap ~23%, DLL data memmove ~13% |
| switch-in walks only displaced pages; placement by a hot map; DLL data >= 16 KB mapped with MAP_POPULATE | 6.9 fps | 0 | hmmap 18.7% (worse than the copy it replaced) |
| the same, not populated | **8.3 fps** (QEMU: 8.5) | 0 | hmmap 2.9%, memmove 3.6% |

Left: thread hand-overs (TLxEvent Set/Wait, LxSelf, Park, Resume ~17%); smaller DLL data still copied (sgresource.dll,
2 KB, ~11,000 swaps a second: nearly every thread switch; ten more at ~1,650/s; ~90 MB/s in all, `WSDSTAT` lines on
Ctrl+P); `memcmp` ~5% with no caller in the samples.

## Home page 2 widgets never load hosted (2026-11-13): starvation, not a slow server

On QEMU the page-2 widgets load in under a second; hosted they stay spinners. Found with the phone-mode tools
(Ctrl+P with `--env PROFILE=1 --env IPCSTAT=1 --env GLSTAT=1`):
- Page 2's widgets load only when page 2 is shown: homescreen -> cpserver -> sqlsrv (both priority 12).
- The spinners start at once. Hosted, alfredserver (15), fbserv (19) and homescreen (19) then take the whole CPU:
  `IDLE 0 ms of 19998 ms`, alfredserver 62%, fbserv 12%. cpserver and sqlsrv are `READY` and get 0% (LXTOP). The
  content never arrives, so the spinners never stop.
- On page 1 the CPU is idle 68% (41 of 60 s), drive I/O is ~8 ms/s (`NVSTAT`), and no IPC is slow (`IPCSTAT`: only
  requests that never complete, notifications). So no server, storage or IPC path is 10x slower.
- Proof: page 2, back to page 1 for 20 s (no animation: the loaders run), page 2 again: the widgets are there.

What makes the animation cost the whole CPU hosted (alfredserver's samples: 60% our kernel, 39% ROM code): the GL
request path (`hsys6` from `Request`, SelfCopy, PrefetchKick, ApplyWlog ~8%), the trace trampoline (4.5%), thread
hand-overs with fbserv (fbserv's samples are all hand-overs; ~13,500 switches a second, ~1,500 per frame), `LxSelf`
(2.5%), 80,000 slow `ChunkBase` exec calls a second (fbscli `CFbsBitmap::DataAddress`). On a native CPU (the Pixel)
all of this is far cheaper than under TCG [I].

Tools added for this: per-thread CPU samples (`PROF thread/tpc`, `profsym.py` "by Symbian thread"), `IDLE` and
`READY <thread>: priority` on Ctrl+P, `IPCSTAT` (a 5 ms sampler of the kernel's message pool: per server and function,
messages, ms queued and served, max, still open), `NVSTAT` (drive transfers per second and their time).

Follow-up the same day:
- Nanothreads pinned to one CPU, helpers to the others (`HPIN`, now the phone-mode default): page 2 11.2 -> 18.1
  fps. Still `IDLE 0`: the spinner animation is not paced, it draws as fast as the CPU allows, so speed alone does
  not free the CPU. With pinning the loaders sometimes win the race (one of two pinned runs showed the widgets on
  page 2 without leaving it; no unpinned run did).
- What alfredserver does per frame (`IPCFN`, synchronous sends per thread and function, 20 s): ~50,000 sends
  (2,540/s in pairs, ~140 per frame), homescreen ~20,000; `ChunkBase` exec 114,000/s. ROM behaviour: hosted only
  sets the price of each call.
- Next: what paces alfredserver's frames on the phone (refresh timer, surface-update completion, vsync), and whether
  our display path completes too early; then the remaining per-call costs (GL request wait with interrupts off,
  `ChunkBase` and the trace trampoline).

**Fixed (2026-11-13, evening): pacing.** The device answered EGL window swaps at once (swap interval 0), so nothing
paced alfredserver's frames; and a GL request spun with interrupts disabled, so nothing else ran during the host's GL
work. Now a thread's GL request waits blocked (hgl.cpp) and the hosted GL server answers a swap at the next 60 Hz
vsync (glserver `--vsync-hz`), as on the phone. Page 2 (two runs, settled 90 s and 20 s): the widgets load, the
CPU is idle 93-97%. ui_compare (medians of 3): Menu open 109 ms / settled 0.93 s (was 140 ms / 1.17 s), close
0.43 s (0.52), Calendar 162 ms / 0.76 s (179 ms / 0.94 s).

**The notification drawer and later home pages (2026-11-13, evening): no hidden stall; the frame pipeline itself.**
Traced with new tools: `--env SCHEDTRACE=1` (every thread switch on Ctrl+P), `--env GLTRACE=1` (every GL round trip and
every composed frame, same clock), `IPCFN` lines now name the target server, `SYBORG_GL_STATS=1` on the GL server
prints GPU memory per process (what GOOM reads) and every VG call by function, `GLSERVER_ARGS` passes options to
htest's GL server. Findings for one drawer pull (4-9 frames in ~1.3 s):
- The CPU is busy for the whole animation (no idle gap between frames): ~150 ms of CPU per frame, spread over
  akncapserver, fbserv, Wserv and alfredserver. Not a wait, not the vsync pacing (`--vsync-hz 0`: the same frames).
- GPU memory in use stays at 2.6 of 128 MB (GOOM has no reason to act).
- Per frame: one `vgImageSubData` of ~200,000 pixels (a near-full-screen image drawn in software and uploaded),
  ~20 `vgGetMatrix` round trips (a client-side query on the phone's driver, a host round trip here), ~125
  `EFbsMessBitmapDuplicate` + `EFbsMessClose` pairs from alfredserver to fbserv (akncapserver ~40).
- QEMU's GL device shows the same per-frame pattern (`vgImageSubData` ~160,000 pixels per frame, the same call mix):
  the ROM's normal path on our GL device, not a hosted-only fault. Hosted pays more per step.
Leads: answer `vgGetMatrix` (and other pure state queries) in the guest without a round trip; the guest's
`eglClientWaitSyncKHR` and endpoint poller sleep 20/16 ms (31.25 ms with the 1/64 s tick) between asks
(GOOM polls a sync every 31 ms).
- Done (same evening): `vgGetMatrix` answered in the guest from a bit-exact copy of the matrix state
  (src/khr/vgshadow.cpp; KEEP IN SYNC with the host RI, tests/vgshadow_test.py): its host calls went from ~150 a second
  during the UI tour to 0. Page 2 animated 50 frames in 5 s (was 36-40); the drawer is unchanged (4-5 frames): its
  time is the software drawing and the fbserv round trips.
- Measured before building the event-driven sync/endpoint waits: during a UI tour the guest's EGL calls are only
  MakeCurrent and SwapBuffers (one each a frame) and GOOM's eglClientWaitSyncKHR poll (32 a second, constant); no
  endpoint calls. Neither poller is in the UI path: an event-driven sync wait would only save GOOM's idle wake-ups
  (worth it on the Pixel for battery, not for frames). A thread hand-over costs ~10 us (t_thread, TCG, with or without
  HPIN); the drawer makes ~10,000 a second: ~10% of its CPU, the one-host-thread scheduler's (future.md 3) share.
- Is the per-frame software drawing + full-screen upload ours? Since boot alfredserver creates one window surface
  and one context, never a pbuffer or pixmap surface, although huirenderpluginvg.dll has offscreen classes
  (CHuiVg10CanvasRenderBuffer, CHuiFxVg10OffscreenRenderbuffer). Tried: advertising the real phone's full EGL
  extension list (the N8 eglprobe's 14 + image_endpoint, build/evidence/eglprobe) through the GL server's
  --identity: no change (same EGL calls, same upload per frame, same drawer frames). So the extension string is not
  the gate. huirenderpluginvg's only extension name, EGL_NOK_window_type_eglwindowbase, picks the window surface's
  native type (eglwindow.dll vs RWindow), not offscreen rendering. Open: what makes it render Avkon windows in
  software (a config/central-repository setting, a capability query, or the ROM's normal path).
