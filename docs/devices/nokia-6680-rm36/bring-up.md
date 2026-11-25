# Nokia 6680 (RM-36) bring-up

S60 2nd Edition FP2, Symbian 8.0a, EKA1, TI OMAP1610, BB5 modem; firmware RM36_5.050440_B5 (`.C0R` core, `.v27`
variant) in `eka1/rm36_rom/`. The board is the 6630's (`-M nokia6680`: `rm1_board_init` with `rm1_phone_6680`,
`src/qemu-ngage/hw/arm/nokia9500.c`); the method and tools are the 6630's
([../nokia-6630-rm1/how-it-was-done.md](../nokia-6630-rm1/how-it-was-done.md)). Workarounds: [../../hacks.md](../../hacks.md)
"Nokia 6680".

## State (2026-11-26)

First boot (no flash file): logo, ~7 min of formatting C: (white screen with the date bar), then the home-city list,
time and date queries, the **S60 home screen** (active standby, Messag./Camera) and the **Menu grid** (Messaging,
Contacts, Calendar, Gallery, Camera, Imaging, Clock, Web, Office). No panic reports (`38 0E`) after the fix below.

## Files

| What | Where | How |
|---|---|---|
| core XIP ROM, ROFS | `eka1/rm36/core/ROM.bin`, `eka1/rm36/output/rom/rofs.img` | `tools/fiasco.py` on the `.C0R` -> `SOS*CORE`, `eka1/tools/xzip_core.py --rofs` |
| variant ROFx | `eka1/rm36/fiasco/v27/_pre@01060000.bin` | `python3 -I tools/fiasco.py --out eka1/rm36/fiasco/v27 eka1/rm36_rom/RM36_5.050440_B5.v27` |
| NAND | `eka1/rm36/core/nand.img` | `python3 -I eka1/tools/rm1_nand.py eka1/rm36/core/nand.img --rofs eka1/rm36/output/rom/rofs.img --rofx eka1/rm36/fiasco/v27/_pre@01060000.bin` (prints "1023 references ... moved") |
| manifest, export names | `eka1/rm36/core/output/rom/manifest.json`, `files.json` | `python3 -I eka1/tools/rom_manifest.py eka1/rm36/core` (the session tools' stack namer needs it; without it a catch's dump crashed the daemon) |

Kernel addresses in `eka1/tools/ngage_run.py` `MODELS['nokia6680']` are the 6630's: ekern differs in 268 of 46065
words, the fault routine's bytes and the literal users of the current-thread and container words agree [V].
EUser RThread::Panic (ordinal 812) is 0x500CEB60 [V catch: FSCLIENT 25 below].

## Stalls

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1-3 | boot logo (earlier sessions) | the 6630's core patches at the 6630's addresses | H-rm36-* core patches at RM-36's (commit "6680 - boot logo") |
| 4 | logo, then `38 0E` panic reports: `EComServer:ecom` USER 22 twenty times, then `EikSrvs:AppArcS` USER 21 | catch on RThread::Panic: first death at 44 s is EFile's loader thread, FSCLIENT 25 (efsrv 0x5010ADB4: TParse::Set of the name failed) on `"<7 garbage chars>Z:\System\Libs\phonetsy.tsy"`, the first line of `Z:\System\Data\Dll Preload list` (EFile InitCompleteThread, TFileText at 0x500F9A68) [V]. The TFileText buffer began 12 bytes before the file: the ROFx's entry for it says 0x324494, the core's 0x3244A0; the variant was built against another core (all core references 4..288 bytes short past 0x17E6D4, from compressed executables of other sizes) [V] | H-rm36-rofx-rebase (`rm1_nand.py` `rebase_rofx`): home city list, home screen, Menu grid |

## Next

As the 6630's "Next": a second boot from a kept NAND (`--flash`), sound (DSP), network/SIM, DRM. Whether a `.v27`
that matches this `.C0R` exists (it would make H-rm36-rofx-rebase unnecessary).
