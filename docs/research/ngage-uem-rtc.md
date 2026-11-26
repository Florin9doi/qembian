# EKA1 Nokia phones (ngage board): where the clock comes from (H-uem-rtc)

Read statically in the 7650's ROM (NHL-2 V 5.06; rtc.dll 0x50334370, cbus.dll 0x50367DB0, ekern 0x50308000) and
checked in runs. The 6600 (docs/research/6600-rtc-clock.md) has the same rtc.dll and cbus.dll code, relocated
(+0xAA40). The N-Gage, N-Gage QD, 3650 and 3660 have the same rtc.dll code and the same cbus.dll register table
entries for the RTC, and their kernels keep the time in the same words (0x80006758, 0x800004EC) [V: compared in
the ROMs, `tests/ngage-boot-check.py --clock` passes on each]; the 3230, 7610/6670 and 6260 are assumed to match [I]. [V] = read in the code or seen in a run, [I] = inferred.

## The chain

1. The kernel boots with universal time = TDateTime(2000, January, 0) (7650 ekern 0x5031F11C into base
   0x80006758) + ticks (64-bit count of 1/64 s at 0x800004EC) [V]. Its TTime for 2000-01-01 00:00 is
   0x00E03AB44A676000 [V read in a run]; a plain microsecond count from 0001-01-01 is 378 days off.
2. The UEM (CBus dev 1) keeps only a running counter: reg 0x11 seconds, 0x12 minutes, 0x13 hours, 0x01 days
   (0xFF = not running). rtc.dll reads them (logical 0x20-0x22, 1; read-only type 1 in cbus.dll's table) and never
   writes them; it can only reset them (reg 2 bit 7 pulse, 0x503361B8) when it finds the clock invalid [V].
3. The date the counter counts from is held by the NOS, which hands it over once at start-up: rtc.dll request 1
   (vtable +0x20, 0x50334688) with three TTimes by pointer. rtc.dll takes the first as its base, builds
   now() = base date with the UEM's h:m:s, + the day counter (0x50335C78), and calls the kernel's
   SetUniversalTime (ekern ordinal 461, 0x5030ACA0) with it (0x503347FC) [V].
4. A time the user sets goes kernel -> rtc.dll's hook (+0x28: valid = 1, notification 4); the UEM is not touched [V].
   Keeping it over a restart is the NOS's job (not modelled: each boot follows the host again).

## Board model

- `ngage.c` `ngage_uem_*`: the counter runs from the host's midnight in QEMU's `-rtc` clock (launchers:
  `base=localtime`, `NGAGE_RTC` overrides); reg 2 bit 7 restarts it at 0. NOS device +0x48/+0x4C = the midnight as a
  TTime (0 on models without uem_rtc, whose stand-in then passes rtc.dll's own time as before).
- `nos_standin.S` / `nos_standin_nhl10.S` `rtc_dfc_fn`: request 1 with that date, queued as a DFC.

## Trap: the hand-over must not run at thread level

SetUniversalTime stores the new time (0x5030AD10), waits for the next tick (0x5030960C; the flag is set by the tick
ISR 0x50309530) and only then moves the kernel's second counter (object 0x80006C24, +0x14) to the new time
(0x5032A380). Called from the null thread, the tick DFC ran in between and 0x50329FAC stepped that counter one second
at a time from 2000 to now (one second-queue pass each): 1.2 M steps per host second, several minutes of white screen
for a date in 2026, and the hand-over never seemed to finish [V]. As a DFC (as from the NOS's own kernel context) the
tick DFC runs after it and steps once.

## First boot

Startup.app asks for the time and date when `C:\system\bootdata\FirstBoot.dat` is missing (a factory-fresh phone),
whatever the RTC says [V: the query shows with the clock valid and on the host's time]. Its default is fixed
(7650: 12:00 AM, 6600: 09:00 am) [I], so answering OK sets the clock to it; later boots follow the host again.
