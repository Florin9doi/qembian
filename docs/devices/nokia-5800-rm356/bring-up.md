# 5800 XpressMusic (RM-356) bring-up notes (2026-11-07)

Firmware V 31.0.101 (prd.core.C00, prd.rofs2.V05, C01_prd.rofs3). Symbian 9.4 / S60 5th Edition on
the BB5 RAPIDO platform (same flash layout as the 5730/N79). Reached the home screen, menu and touch
input (L1); seeded boot to idle 9 s.

- **Personality sym94** (new): 9.4 inserts one slow exec (S45 TimeNowSecure) and appends five; process
  creation info 8 bytes longer than 9.2's. `personalities/sym94/README.md`.
- **Start-up**: the 9.3 modem answers worked unchanged (SIM_IND on channel 35, SIM lock status, lock
  info). Startup then waited for the start-up tone: RAPIDO audio is not emulated. F3 0x34 data requests
  now get "all taken" (iscpeer), the MdaifDriver stand-in buffer is 1 MB, and the tone is off
  (H-startuptone-none, a `[[rom.rofs_patches]]` edit of CenRep 101F8763 in our ROFS2 copy).
- **Display**: 360x640 UI on a natively landscape panel (wsini rotation 270, as the N95); N95 LCD HAL build.
- **Touch**: Nokia's exyin.dll replaced by the `exyin` stand-in (kernel DDigitiser on the Syborg pointer
  device, own DFC thread, 10 ms sampling while pressed); the Syborg mouse driver EPOINTER is left out
  (`kernel.omit_baseport`). Coordinates need `rotate = 270`: the digitiser reports the panel's 640x360
  and the window server turns them by the screen mode. Found with the on-screen keypad of the first-boot
  date query (a tap on "5" must type 5; a list tap selects whatever row it hits, so it proves nothing).
  `sydb phone` sends mouse press/drag/release as absolute QMP events; QEMU's own window shows the cursor.
- **Open**: touch latency in some apps (dialler: seconds per tap); home-screen shortcut row; audio;
  sydb agent on 9.4 start-up; proximity sensor; scripted taps need a hold of at least ~40 ms.
- **Open (2026-11-07, owner)**: parts of the screen flicker in the `sydb phone` window. The 5800 runs
  better since the QEMU TLB work and the touch changes, which may have exposed it. Not known whether
  the real phone does it. Candidates to tell apart, cheapest first:
  1. the window's read races the guest's drawing: `sydb phone` copies the frame buffer from shared guest
     RAM 60 times a second with no handshake, so it can catch a half-drawn region (compare with
     `sydb screenshot` series: if screendumps never show the broken state, it is the window side;
     a fix is to read only after the guest's update or vsync, e.g. a frame counter from syborg-fb);
  2. the d_display stand-in's update path: direct screen bitmap (DSB) double buffering, partial updates
     and the 60 Hz vsync pacing (src/standins/d_display); a region drawn, cleared and redrawn between
     two vsyncs would show the clear;
  3. the GLES path (sygl blob, host GL read-backs) for apps that use it (Carousel);
  4. faithful behaviour of the app (some 5th edition apps redraw whole areas on every change).
  Note which screens and apps flicker, and whether it follows touch input or is periodic.

## Open issue reported by the owner (2026-11-08)

2026-11-09: **Settings-entry pause isolated** to a mismatched SIM service in the
fake modem's response to `GetAlsBlocked`; original/corrected/restored reply tests
give 9.843 s / 0.001 s / 10.187 s. See [settings-delay.md](settings-delay.md) for
evidence, static firmware analysis and the implemented reply correction. Rebuilt
5800 test: 1 ms completion; smoke passed. 2026-11-10: N95 smoke passes, N73 reply unchanged, N8 Delight sub-views 1.2-1.4 s (were 3.7-8.4 s).

Random UI actions take seconds (e.g. Settings, then any item); suspected: a request to a driver or
server that completes only on a timeout. The same happens on the N8. First steps: docs/roadmap.md
"Open issues reported by the owner".

## Fixed 2026-11-13: the menu died on an unaligned halfword load (KERN-EXEC 3, back to the home screen)

Owner's report: "the applications menu does not open, it crashes and kicks us back to the home screen".

Reproduced on the owner's 5800 instance (V31.0.101), first by accident in a normal run and then with a
catch: `Menu` key, then tapping an icon. `menu3.exe`, `alfredserver.exe`, `alfappservercore.dll` and
`hitchcock.dll` load, the grid draws, and then — when the ALF redraw timer blits something (a folder
icon, an entry animation) — thread `ALF::ALF` takes a data abort:

```
Exc 1 Cpsr=80000010 FAR=44211dbd FSR=00000001        (data abort, FSR 1 = alignment)
 R2=44211dbc  R6=44211dbd  R8=00000001  R12=0001fffe  R15=789e5930 (hitchcock.dll+45930)
Thread ALF::ALF Panic KERN-EXEC 3
Thread Menu::Menu Panic USER 0                        (its server died)
```

`hitchcock.dll+45930` is `ldrh r4, [r2, r8]` with `r8 = ip>>16 = 1`: [V] disassembly of the shipped
DLL (`tools/rofs.py --extract sys/bin/HITCHCOCK.dll`, `tools/e32load.py --base 0x789a0000`). The
function (starts `hitchcock.dll+45a8c8`) fills a span with a gradient whose endpoints it reads *two
8-bit pixels at a time* out of a byte row (`lsr r7, r6, #8` / `and r6, r6, #0xff` = the two samples of
one halfword); with an odd x offset the load is deliberately unaligned. The caller
(`hitchcock.dll+3c684`, the 12-argument blit helper) reaches it from a `switch` on mode `0x100`; the
chain above is `ALF`'s redraw timer (`CPeriodic::RunL`) -> `alfappservercore.dll` ->
`hitchcock.dll` -> `CFbsBitGc::BitBlt` (frame scan of `<session>/catches/*.txt`). [V]

Cause: our ARM1136 kernel programmed SCTLR with `MMUCR_A` (alignment check) and without `MMUCR_U`;
the EPL default for `CFG_CPU_ARM1136` (`kernelhwsrv/kernel/eka/include/kernel/arm/bootcpu.inc`). The
phones' own kernels do not check alignment, and the ROM is written accordingly (Hitchcock's canvas
command-buffer reader also casts `Ptr()+offset` to `TUint16*`). QEMU enforces the alignment of normal
memory only while SCTLR.A is set, so the guest's A bit was the whole difference. Fixed in the
`syborg` baseport (`InitialMMUCR` = the ARM1136 list without `A`, with `U`; patch
patches/qemu-baseport/0026, `docs/hacks.md` H-no-align-check). Regression test:
`tests/device_py_test.py DeviceTest.test_arm1136_kernel_does_not_check_alignment`.

Not the menu's only oddity, and not a 5800-only bug: every ARM1136 device of ours had it. The menu's
grid also ignores scripted taps (a tap on an icon works once the grid has settled, and the home
screen's bottom shortcut row ignores taps — see the open list above); the dialler/Clock opened from
taps, so the digitiser mapping is right (`sydb tap x y` = UI pixels).

### Seen on the way (2026-11-13, not investigated): a game's null dereference leaves the screen black

With the alignment fix in, tapping the Applications folder -> `RT GR` (Raging Thunder GR) launches the
game, which opens the display driver (`ctl 10/11/31`, then `ctl 44` = `SYBSTUB: ... ignored 0`, the
one control our `d_display` stand-in does not implement) and immediately takes a *translation* abort:
`Exc 1 Cpsr=20000010 FAR=0000000c FSR=00000005` (a load from 0xc) in its own thread — a null pointer,
not our alignment bug. After that the phone keeps running (`status: idle`) but the screen stays black
and nothing redraws, also not after keys: WSERV seems to hold the display for a client that died.
Worth its own investigation (does a dead DSA/DSB claimant release the screen? what is ctl 44?).

## Known non-fatal panic: `SosPmmHandler -5` (pre-existing, not modem-related)

Every boot logs, about 1-2 s in:
```
1.942 Thread sospmmhandler.exe::Main Panic SosPmmHandler: N -5
```
The full category is "No PMM partition found from NAND." (-5 = KErrNotSupported): sospmmhandler.exe looks
for the product-data (PMM) partition on the NAND, which our flash image does not have. The process dies and
the boot goes on; the home screen, menu and touch are unaffected. [V] It predates the 2026-11-18 ISI reply
realignment (docs/research/n95-isi-encoders.md): the same line is in the 5800-v20 instance log of 2026-11-14
(`1.690 ... Panic SosPmmHandler: N -5`), the 5730/E52/6720 logs of 2026-11-17 and the N79/E72 logs of
2026-11-14; the full text and call stack are in the N86 catch of 2026-11-10
(`build/evidence/bringup-2026-11-10/n86-rm484/catches/001-sospmmhandler.exe_Main.txt`, caller
sospmmhandler.exe+d6). The N8 shows the same panic (docs/devices/nokia-n8-rm596/bring-up.md). The cause is the
flash layout, not the modem [I] (the message names the NAND partition).
Not fixed: a PMM partition stand-in would only be needed if a product-data reader turns out to matter.
