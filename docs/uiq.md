# UIQ phones (Sony Ericsson, Motorola, BenQ): what qembian could run

Assessment of 2026-11-07, every claim [I] (inferred from the platform history and from how qembian
works). **2026-11-13:** two firmwares are now inventoried: P1i (UIQ 3.0) and P910i (UIQ 2.1); formats,
contents and the next steps are in [semc-firmware.md](semc-firmware.md), which supersedes this page
where they differ. Twelve more UIQ 3 sets (P990i, M600i, W950i, W960i, P1i R6G04, G700, G900) were
extracted from the Emma archive the same day: [semc-emma.md](semc-emma.md). All of them report the
Symbian 9.1 kernel shape the `sym91` personality already covers.

## UIQ 2.x (P800, P900, P910, Motorola A920/A1000, BenQ P30): the N-Gage route

2026-11-18: done for the **P910i** (R5B01): the UIQ 2.1 home screen on its own board `-M p910i`
([devices/se-p910i/bring-up.md](devices/se-p910i/bring-up.md)). The P800/P900 share its platform family [I].

2026-11-13: since the N-Gage QD (EKA1, Symbian 6.1) boots unmodified on its own QEMU board
([eka1-ngage.md](eka1-ngage.md)), UIQ 2 is in scope the same way: the phone's own EKA1 kernel on a
board for its SoC (P910i: Philips PNX4000), not a personality. The P910i ROM reads with the N-Gage
parser ([semc-firmware.md](semc-firmware.md)). The text below (2026-11-07) explains why a personality
cannot do it.

These run Symbian OS 7.0 or 8.0a on the **EKA1** kernel [I]. Their user side is built against EKA1's
EUSER, and EKA1 differs from EKA2 in much more than call numbering:
- the executive interface: a different set of kernel calls with different semantics underneath;
- the process, thread and server model, and DLL loading;
- executables: the pre-platform-security E32Image format, without capabilities;
- the ROM layout.

A personality (`personalities/`) maps one EKA2 numbering onto ours and adapts a few structure sizes and
calling conventions. For EKA1 it would have to emulate a whole other kernel on top of ours: a separate
project, not a personality.

## UIQ 3.x (P990, P1i, M600, W950, W960, Motorola Z8, Z10): doable with what we have

These run UIQ 3.0/3.1/3.3 on Symbian 9.1/9.2/9.3, the **EKA2** generation of S60 3rd Edition [I]:
- **Kernel side**: our kernel with the existing `sym91` / `sym92` personalities should mostly work;
  confirm with `tools/fw/probe.py --device ID stubs --vs <the S60 phone of that version>` and
  `exec-sizes` (docs/adding-a-device.md step 3).
- **Tools**: the ROM composer, the device profiles, sydb and the phone window apply unchanged.
- **New work, as for any phone**: the stand-ins. Sony Ericsson's modem does not speak Nokia's BB5 ISI
  protocol, so the fake modem needs a new front end and message set (not a `bb5-s60` family member:
  a new family). Display controller, keys, the jog dial and the resistive touch screens differ; touch
  can reuse the `exyin` approach of the 5800 (kernel DDigitiser on the Syborg pointer device).
- **Firmware container**: Sony Ericsson's, not Nokia's FIASCO. 2026-11-13: there is no container
  around the Symbian images on the P1i: `tools/fw/semc.py` reads them ([semc-firmware.md](semc-firmware.md));
  `tools/fw/inventory.py` still expects FIASCO.

First candidate when this starts: a Symbian 9.1 UIQ 3.0 phone (M600 or P990), to reuse `sym91` as
the N73 did.
