# Running the N95 8GB modem firmware in QEMU (NOLO -> MCUSW): state and log

Goal: run the modem CPU's own software (RAP3G, ARM1136) instead of ISI reply tables, so each phone's
own modem firmware implements its ISI services. Background, flash layout and the decision to try it:
docs/research/modem-firmware-in-flash-files.md. [V] = seen in the code or a run, [I] = inferred.

## State (2026-11-22; section 10 is the newest)

NOLO -> MCUSW boot to the modem OS; **interrupts and timers** (intc 0x20400000, OS timer 0x20040100 at
32.768 kHz, hal_timer.c microsecond timer 0x20040180 at 9.6 MHz) let the scheduler and its servers run; the
**SSI link comes up** with the built-in APE peer (wake test -> 0x10000ffe; protocol:
docs/research/n95-cmt-ssi-protocol.md), and ISI messages from the Symbian side reach the modem through the
modem's own GDMA (tools/cmt/probe.py --link SOCKET + `syborg-modem.link`, the N95 booting in an isolated sydb
session; first message 23 04 delivered 2026-11-21).

The modem does not answer yet. Two linked blockers:
1. **SOS proxy not registered**: the modem's SOS proxy task (sos_proxy_t.c, FUN_f01e6054) registers with the SSI
   driver (FUN_f012cfba, proxy pointer 0xd80104c4) only when it sees the modem's own MTC_STATE_INFO_IND (15 C0)
   with action 0x0c or 0x04; until then received messages are dropped ("Sos proxy not registered").
2. **MTC device sync**: MTC (mtc_srv.c, FUN_f0176736) waits for PhoNet device 0x7c ('|', list at 0xf04b1959) to
   be registered in the router's device table (0xd800d282, one byte per dev >> 2: the serving media) and resets
   with reason 24 after its DevSyncTimer (FUN_f017866e -> 0xa1 -> reset 0x18). At that point the table is
   empty: no media has registered any device (also not the APE's 0x6c). Who registers 0x7c is open: a media
   layer of the modem (DSP / MDI?), a registration message from the APE (pn_mctrl.c, router-internal resource
   0xd7), or PMM configuration (the device list may come from the erased per-phone data).

   Narrowed down (2026-11-21): the router learns a device from the first message that reaches it from that
   device (FUN_f011c75e: sender device [2], media [0] -> FUN_f011ccf4 writes the table). The device list is a
   constant ("|", 0xf04b1959), the sync mode (+0x11 = 2) and pending mask (+0x12) are initialised data.
   MTC dispatches event 0xdc01 to FUN_f0177f10, which re-checks the table and goes on. Experiment: marking
   device 0x7c by hand (gdb, `set {char}0xd800d2a1 = 1` at FUN_f0176736) passes the sync; the next stop is
   mtc_product_conf.c#601: a message to **device 0x7c, resource 0xE6** (template 0xf04e45a2) cannot be routed
   (my fake media 1 does not exist). So 0x7c is a remote device with resource 0xE6 behind a real media:
   most likely the **modem DSP over the MCU-DSP interface** (reset 24 = 0x18 is also hal_mdi.c's DSP-failure
   reset).

   Done (2026-11-21): a fake DSP (map `dsp`, docs/research/n95-cmt-mdi-protocol.md) completes the MDI boot
   handshake and registers device 0x7c; MTC passes its sync and talks to it (requests f2/0c, e6/0d, e6/48 with
   payload 00 18, 15/7a with sub-blocks, e6/f0 and 19/f0 subscriptions). Then three more stops, all fixed:
   hal_light_phy.c#302 (CBUS chip ids: Retu 0x0220, device 2 0x0350), hw_audio_tpa6130.c#788 (I2C transfers: an
   OMAP2-style I2C model, devices 0x60 and 0x48 on bus 2), and a divide by zero in a tick calibration that
   free-running TCG makes see 0 elapsed ticks (fixed with -icount shift=2, now the probe's default: virtual time
   is deterministic). The modem now runs without resets.

3. **Where it stands**: messages from the Symbian side are still dropped, because the SOS proxy only registers
   on MTC_STATE_INFO_IND (action 0x0c/0x04), and MTC does not send one in 3 s. A gdb list of everything through
   the router's send (FUN_f011b902) shows no 15/C0. MTC's requests to the DSP get no reply; generic "id + 1,
   status 0" replies are rejected by the MCU (f0 01 <id> = not identified), so the DSP's real replies for
   resources 0xF2/0xE6/0x15 would have to be read out of the DSP image or the MCU's response parsers. Next:
   find the condition MTC waits for before MTC_STATE_INFO_IND (mtc_srv.c state machine).

   Chain found (2026-11-21): the SOS proxy registers on MTC_STATE_INFO_IND 15/C0 with action 0x0c, built by
   FUN_f0178bda and sent from FUN_f017735a, which MTC's message switch (FUN_f0177b1a, table 0xf0177bb3) runs on
   **MTC message 0x18** from an internal sender. mtc_ctrl_t.c sends 15/18 to MTC itself (FUN_f01786e0, objects
   0x30 -> 0x30) once its state transition is complete (state block 0xd8016ed4: +1 new state, +2 current,
   +3 target, +0x20 = 2 while a transition runs; FUN_f0178ab0). At 5 s: new = target = 8, current 0 - the
   transition 0 -> 8 has not finished. Its steps send COMMGR (resource 0x10) messages 0x80/0x82/0x84 to the local
   servers, objects 0x11..0x16 (router log via gdb on FUN_f011b902); objects 0x11, 0x12, 0x13 and 0x16 get all
   three, **object 0x15 only 0x80**: most likely the server behind object 0x15 has not answered its first step.
   Next: which task owns object 0x15 and what it waits for.

   Resolved (2026-11-21, later): object 0x15 = task 0x15 = WRLC_UL_TASK (task names: table 0xf04a0fe8, entry
   points: table 0xf04a0560, 16 bytes per task); it was not the cause. The real stalls were in my models: the
   interrupt controller gated the CPU's IRQ line on +0x88/+0x8c (set/clear pairs the OS writes around critical
   sections), so SSI interrupt 0x3c was never dispatched; and the I2C model (DATA is 16 bits wide, the next
   XRDY/RRDY follows the driver's acknowledge, STP must clear) left the TPA6130 headphone-amp driver
   busy-waiting, which blocked MTC's start-up. With both fixed, MTC sends 15/18 and broadcasts
   MTC_STATE_INFO_IND (state 8, action 0x0c), and **the SOS proxy registers with the SSI driver by itself**
   (proxy pointer 0xd80104c4 = 0xf01e5f99). The DSP gets an MDI keep-alive ping (inline 0xDA message, about
   every 31 s); the fake DSP answers it, otherwise the MDI receive task resets with reason 9.

5. **The modem answers the phone (2026-11-21, night)**. Two more things the APE side must do, found by reading the
   SOS media code: (a) messages to the modem carry the **SOS media id 0x1b** (router media table 0xd8001714:
   0x12 MDI, 0x1e, 0x10, 0x1b SOS, 0x25, 0x00); with 0 the proxy treats them as control traffic and the
   sender device 0x6c is never learned; (b) the SOS media must be **activated** with router media control
   (resource 0xD7) message 0x03 and then 0x09 with byte 11 = 3 (sos_usb_t.c, task 0x2d, FUN_f01e5e7a /
   FUN_f01e5d10 case 3 -> state 0xd801a568 = 2); until then every answer fails to leave and MTC asserts
   (mtc_srv.c#3689 "!status"). The APE peer does both (retrying until the modem answers 0xD7). Also: the modem
   traces on several channels (0x401ef000 stride 8, 0x40172000 stride 4, ...): `stream 0x40100000 0x100000`
   in the map now captures them all, which is how "*** assertion failed: mtc_srv.c, 3689 !status" was seen.

   Linked run with the N95 (isolated sydb session): 58 messages from the phone, 52 answers from the modem's own
   firmware: SIM 09/1F, 09/19, 09/E4, 09/1D, MTC 15/02, 15/16, resources 0x1B, 0x17, 0x31, 0x0B, 0xDB, 0x1C.
   Unanswered: 23/04 (probably needs no answer), 0A/0B (resource 0x0A, likely network). The phone was still on
   its white start-up screen after 120 s. Next: what the phone waits for (its serial log / the requests it
   repeats), and the unanswered 0A/0B.

6. **Self-tests pass; the phone's fatal start-up explained (2026-11-21, evening)**. The linked N95 stayed on its
   white screen because of the modem's **start-up self-tests**, not the audio replies: a sydb catch on the
   audio server's KERN-EXEC 0 showed SessionSendSync (exec 0x4c) on an `!AccServer` session from
   AudioRoutingController; AccServer never started because Starter set the startup reason (101f8766/0x42) to 7
   and its global state (0x41) to 0xFA (fatal) right away. The reason came from the modem: MTC answered the
   state query with `15 65 {08 08}` = MTC_SELFTEST_FAIL (tables: 01 NORMAL). Measured with gdb after each test
   body (st_*_test, FUN_f0164afc..): st_uem_cbus_if_test passed (chip ids only), st_ear_data_loop_test,
   st_sim_clk_loop_test, st_sim_io_ctrl_loop_test and st_mbus_rx_tx_loop_test failed with 2 (no loopback).
   They drive board test lines and read them back (pin table 0xf04b0df4, hal_st.c; outputs set/clear at
   0x20031cd4/cd8, inputs 0x20031dc8; Retu routing bits; MBUS 0x20032004 -> 0x2003200c inverted): the map's new
   `setclr` and `loop` lines declare that wiring (each line commented with its evidence; SIM tests:
   docs/research/n95-cmt-sim-interface.md section 0). All five now pass and MTC traces
   `mtc_startup_reason: 1` (was 8).

   Next stop: MTC now heads for state 0x0a (MTC_RF_INACTIVE, offline: state block 0xd8016ed4 new = target =
   0x0a, current 0 after 25 s). Every local COMMGR step (resource 0x10, 80/82/84 to objects 0x0d..0x19) goes
   out, but the transition does not finish, so MTC_STATE_INFO_IND is not sent and the SOS proxy does not
   register ("Sos proxy not registered"; the phone's messages are dropped again). MTC's requests to the DSP
   are unanswered: F2/0C, E6/0D, E6/48, 15/7A (148 bytes, from MTC), E6/F0, F3/64, F3/1D (many). Their reply
   formats: docs/research/n95-cmt-dsp-replies.md (in progress). The SIM interface itself is mapped in
   docs/research/n95-cmt-sim-interface.md (with no card the driver reports "no card" within milliseconds).
   tests/cmt_probe_test.py test_modem_answers_mtc_state_query now runs without the loop lines (the state 8
   path) so it still covers the link chain.

7. **Offline state reached; the phone's start-up self-test run (2026-11-21, night)**.
   - MTC's 0 -> 0x0a transition stalled creating step group 10 (docs/research/n95-cmt-dsp-replies.md): the
     WCDMA core appl 0x3e's init (FUN_f032461c -> FUN_f0331940) seeds the PRNG (FUN_f012ea20), which hashes with
     the **SHA-1/MD5 accelerator at 0x20024400** (driver FUN_f012eeaa..; the OMAP2 SHA1/MD5 layout) and waited
     for its output-ready bit. Found with `probe.py --gdb-after SECS` (gdb attached once the guest is past the
     early boot, so breakpoints do not slow it on the way) and call-return breakpoints. The map's `sham` line
     models it (src/qemu-syborg/hw/arm/syborg_cmt_sham.h, checked against hashlib: tests ShamModel). Its +0x64
     reset-done bit was the "SSI reset" orbits line before. MTC then reaches **state 0x0a (RF_INACTIVE =
     offline)**: the state query is answered `0a 0a`.
   - Linked run: the phone's Starter reaches 202 and the UI comes up, but the modem crashed at ~12 s (prefetch
     abort at 0xf0c20000 from the IRQ path). Cause: the interrupt controller model. +0x88 is a real IRQ lock
     (the IRQ entry code 0xf049c25c and critical sections write it); +0x8c (end of critical section) **and
     +0x90 (end of interrupt, both IRQ exits)** unlock. The earlier model ignored +0x90, so gating left the lock
     set after the first interrupt; not gating let interrupts into critical sections. Now: gated, +0x90 unlocks.
   - The phone showed "Phone start-up failed. Contact the retailer.": DosServer runs the modem's self-tests over
     resource 0x35 (35/02 {01 00} list -> 03; 35/00 {02 00} run -> 01 with one sub-block `01 04 id result`
     per failed test; test table 0xf04b0c14: fn, name, id, kind) and then sets MTC state 08. Failed: 0x5a
     ST_PWR_KEY_TEST (Retu 0x16 bit 5 = key released: 0x0520 now), 0x38 ST_SLEEPCLK_FREQ_TEST (DSP E6/0D ->
     8A {00 00, u32 BE} in a window around 38.4e6: map `dspreply`), 0x3a ST_CMT_APE_WAKEUP_TEST (the cold-boot
     wake test: the peer now plays it with CAWake 0x20030800 and ACWake = GenIO bank 0 pin 29, interrupt 0xf6;
     the GenIO dispatcher reads the pending pin from bank +0x3c), and **0x5d ST_SECURITY_TEST**, still open: SEC
     1B {00 09} (FUN_f01a3f6c) needs FUN_f01a2a92 (per-phone security data checks) and FUN_f01a32d2 ->
     FUN_f01e4506, which looks up the protected application "PA_ESN" in flash and calls it through the secure
     gate (result 1 and output 1 wanted). Next: what the PA lookup finds in our NOR image and what the gate must
     answer.

8. **The owner's real N95 and the security self-test (2026-11-21, late night)**. isiprobe.exe
   (src/n95apps/isiprobe, docs/tools.md) asked the owner's real N95 for the modem servers' versions: all nine
   (MTC, INFO, Perm, Security, SIM, NET, EM, Self-test, Monitor) answer byte for byte as our 35.0.001 firmware
   does, so the emulated modem runs the phone's own firmware. On the real phone every server sits one PhoNet
   object lower (MTC 0x2f, ours 0x30) [open: one task less there]. The IMEI is read with 1B/00 {41} (the
   owner's IMEI is kept out of the repo). A stock modem gives out no memory or flash by address and keeps the
   PA/security records inside (docs/research/n95-cmt-memory-read.md), so ST_SECURITY_TEST cannot be fed real
   data: the map's `pa PA_ESN` line (tools/cmt/nor.py pa_stub) adds a directory entry without configuration,
   which the firmware treats as "not provisioned" (FUN_f01e4506 -3) and the test passes. The phone's self-test
   run is now all passed (35/01 {00 00}) and DosServer no longer sets SELFTEST_FAIL.
   Next stop (linked run): the phone stays on the Nokia start-up screen (Starter 202). WlanServer reads
   PN_PERMANENT_DATA 23/04 {01 39 .. 02 67} (WLAN per-phone record), the modem answers {11 00} (not found:
   the PMM area is erased), WlanServer exits -5, IPWServer panics "IPW Server 100", the Telephone app follows
   (KERN-EXEC 3). Research on the perm server and the PMM record format: docs/research/n95-cmt-perm-data.md.
   Fixed: the map's `pmm 0x139 0 0x267` line has tools/cmt/nor.py format the erased PMM area with that record
   (615 zero bytes); the modem answers status 00 with the data. That reply is 640 bytes, and our ISC stand-in
   (src/standins/_shared/iscpeer.inl) cut every message to the client at 256 bytes ("IsiMessage: OFFS -9" in
   WlanServer): messages to clients may now be 2048 bytes (sends stay 256). WlanServer still exits -5, as in
   table mode (no WLAN driver).
   **Next stop**: the phone stays on the Nokia screen. With the modem offline (RF_INACTIVE) the Telephone app
   starts the internet-telephony path (SIP client resolver, IPWServer), which table mode never starts and the
   owner's real offline phone did not start either (docs/real-phone-tracing.md); IPWServer panics "IPW Server
   100" (its active scheduler, after WlanServer's exit) and the Telephone app follows (KERN-EXEC 3), three
   times. To find: what makes the phone take that path here (an MTC/NET/SIM answer that differs from the
   real phone's) or what IPWServer needs from the WLAN side.
   Found (catches on User::Leave in IPWServer and on its TRAP at 0x825862c2): IPWServer's ConstructL
   (FUN_82585f12 -> FUN_82586e38) opens the internet-telephony settings through RCSE.dll, whose secure DBMS
   databases (policy 10202869: RCSE_SIPSetting, RCSE_AudioCodec, ...) do not exist on a fresh phone ->
   RDbNamedDatabase::Open leaves -1 -> main panics "IPW Server" 100. IPWServer is started by EVoIPEngine.dll
   (IPWatchdog.dll), which the Phone app loads; during the run DBS_10202869_RCSE_VoIPProfiles gets created.
   Table mode never takes this path, nor did the owner's real phone, whose board (a replacement, per the
   owner) may have no WLAN. Next: the condition under which Phone.exe starts the VoIP engine (WLAN / VoIP
   feature flags, or the offline state) and what the real phone answers there.
   Resolved (catches after each ConstructL call; the stack scan had pointed at RCSE wrongly): the leave is
   IPWServer's WLAN management client (FeatureSupported(0x6d) -> ECom 0x101f8eff), which needs WlanServer;
   WlanServer has no driver and exits. The N95 profile now switches WLAN off (H-n95-wlan-off). Also fixed: the
   interrupt controller lock nests (a critical section inside a handler reopened interrupts; the OS then
   crashed in its IRQ dispatch, STR to 0x32 at 0xf0110134, after ~10 s of linked runs).
   **Where it stands**: with a normal profile the phone asks MTC for NORMAL (15/01 {01}); the modem accepts
   and stalls on DSP requests (E6/F0 {15 00}, F0/55 {03 05}: radio side). With the Offline profile
   (seed build/seeds/nokia-n95-8gb-rm320-offline, made on a table-mode phone) the phone shows "Continue
   using phone in offline mode?", then asks 15/01 {0a 00} and the modem answers 15/64 {06 00} (already in
   0x0a: it started RF-inactive because its saved RF state, PMM group 0x2b index 4, is missing); NokiaDSY
   waits for an MTC_STATE_INFO_IND that never comes. A real modem starts NORMAL (saved byte 1), so the way
   on is the radio side: DSP replies for the NORMAL transition (docs/research/n95-cmt-normal-state.md).

9. **SIM, clock, and the start-up tone (2026-11-21, late night)**.
   - Indication subscriptions now reach the modem (syborg-modem link mode sends PN_COMMGR 10/10 with the union
     of the channels' resources): MTC_STATE_INFO_IND after a state change reaches the phone.
   - SIM "no card": with Retu 0x16 bit 13 clear the SIM driver only told the APDU server "no card" and the SIM
     server (SIMSON, state 4) waited for card news forever (docs/research/n95-cmt-sim-nocard.md). Now the bit
     is set, the driver powers the card and arms the Retu SIM timer; the map's `retuirq 0xfa 32768` models
     Retu's interrupt status (reg 1, write-1-to-clear), mask (reg 2) and the SIM timer -> event 5 (no
     answer-to-reset), and SIMSON concludes no card: 09/C0 -> C1 {00 06}. The phone's SIM check passes.
   - Clock: `retortc` runs Retu's RTC (reg 0x03 days|seconds, 0x04 hours|minutes [I layout]); frozen, the
     modem polled reg 0x03 tens of thousands of times. With no saved time the modem reports the time as not
     set and the phone asks for date and time, as a real phone whose clock lost power does.
   - **Where it stands**: Offline seed, linked: after the date/time queries Startup waits in state 11/12 for
     the operator tone's MapcPlayComplete (docs/devices/nokia-n95-8gb-rm320/n95-startup-animation.md). The
     tone's audio route gets 77/07 -> 08 {00 55} about 5.6 s after 77/01 (table mode never sends 77/07). Also
     open: with the Offline profile the phone asks 15/01 {0a} and the modem answers 06 (already active).

10. **Offline idle on the real modem firmware (2026-11-22)**. The linked N95 (offline seed, and factory-fresh
   without a SIM) boots to the Offline idle screen with no date/time query, opens Menu, Clock, Settings,
   Messaging, Contacts, Log and Calculator, the power-key profile list works, and choosing General gives
   "Insert SIM card" (a real phone without a SIM does the same). Two fixes:
   - **Modem time kept.** The time appl (object 0x29; init FUN_f0150ed4 / FUN_f0150c9a, hal_rtc_if.c) keeps
     the stored time only when all of these hold, else it resets it to {0, 0, 2} and 19/0B sub-block 04 says
     "invalid": the backup battery reads >= 1700 mV (0x6a4; ADC request 0x13 = logical channel 14 = Retu
     physical channel 12 via the table at 0xf049f3e4, 5 samples, linear gain 20000; raw 0x3a0 = 2447 mV
     [V gdb]; at 0 the RTC counts as lost and flag 0xd8003dcc is cleared), Retu reg 5's minutes field is 0x3d
     (the "RTC kept" marker, read by FUN_f0152858; the fingerprint FUN_f01526fa is then 0x1a5e0), and PMM
     group 8 has index 0 and 1 equal (12 bytes {base, elapsed, type 3}) and index 7 = {0x1a5e0, Retu reg 6
     low byte} (8 bytes). Map lines `retuadc 12 0x3a0`, `cbusreg 1 0x05 0x007d`, three `pmm 0x8` lines. The
     phone's own clock is syborg-rtc (H-rtc), so the date shown is the session's RTC, not the modem's base
     (the base value's epoch is [I]).
   - **Replies to the object that asked.** Several clients open ISC channel 36 (the audio server's object
     serves 0x0b/0x1c/0x80, Aamm's resource-0x77 client another one). A modem message names only the
     channel number, and link mode gave it to the object opened last, so 77/09 ("route done") reached an
     object with no 0x77 handler: the key tone never finished (AudioServer kept request 22 waiting for bit
     0x10), the key sound server blocked in DevSound, and Idle, calling the key sound server from
     HandleForegroundEventL, missed a view event and was killed (ViewSrv 11) when Menu closed. Found with
     `kwalk.py --user` (new: user-side stack of any thread, through its page tables) and catches on Aamm's
     dispatcher (FUN_8088053e) and handler registration (FUN_80880422). syborg-modem now delivers a reply
     to the object that last sent a request to that resource on that channel number (sybmodem_link_route),
     indications to every subscribed object; table mode was never affected (it answers the sender).
   - **Switch off.** The modem answers MTC_POWER_OFF_REQ (15/03 -> 66), broadcasts states 03 and 04, and sets
     Retu's watchdog (reg 0x17, normally kicked with 0x20) to 0, which on the phone cuts the power. The probe
     now stops on that write, and syborg-modem powers the phone off when its link closes after having been
     open; the session ends about 4 s after "Switch off!".
   - **WLAN (2026-11-22).** With WLAN feature 109 on, the N95's own-modem boot fails every time: WlanServer
     exits -5 (no WLAN hardware), IPWServer (internet calls, started by the Phone app in Offline) panics
     "IPW Server 100" and the Telephone app crashes (KERN-EXEC 3); start-up stays at 211. Owner instances made
     before the WLAN-off drive file existed had it on; `sydb start` now adds missing drive files. Other phones:
     the N8, 701 and 808 have 109 = 0x25 (supported, modifiable, persisted) in `features.dat` in both ROFS1 and
     ROFS2 (the rofs2 copy is the one Z: shows). An A/B on the N8 (111.040 table mode, both copies patched to
     0x24 vs not) changed nothing: WlanServer starts and exits -5 at ~14.8 s either way (the -5 comes from its
     ECom/plugin start-up, not a leave in its own code), the WLAN connection-method plugin loads, and no
     IPWServer exists on Symbian^3, so the N95's crash chain is not there. The patch was not kept. If the N8 with
     its own modem firmware breaks on WLAN, the next options are a stand-in WLAN driver (no networks, radio off)
     or finding what starts WlanServer there.

4. **Earlier state**: the modem runs for minutes without resets and every message from the N95 (23/04,
   SIM 09/1F) reaches it over the SSI and the modem's own GDMA, but no answer comes back. Traced with gdb: SSI
   rx done (FUN_f012d3b2) -> proxy callback (FUN_f01e5f98) -> FUN_f01e5f76 marks it media 1 and queues it for
   task 0x2e (SOS_PROXY_TASK, entry 0xf01e6055; the OS send returns 1 = queued), but the proxy task's receive
   loop (FUN_f01e6054) never takes it off the queue: it only ever receives the MTC indication. Next: the
   modem OS's queue selection in FUN_f0111550 (which queue a message goes to, and which one the proxy task
   waits on), or whether the proxy task is blocked elsewhere. tools/cmt/sos-proxy-workaround.map (hand
   registration) is no longer needed and is kept only as an example of `pokev`.

NOLO's own log (decoded from the trace port, `tools/cmt/xti.py`):

```
SW : loader_2006_wk41 (Oct  1 2009 13:32:30)
HW : RAP3Gv1.10 PA                      (chip id: our choice, see below)
CBUS initialised / BSI out of range / SMPS not found
Set MCU and DSP speed
SDR 32 MB (16bit) configured (110.4 MHz)
NOR Flash 0 detected (Mfr: 0089, Dev: 8809-0000-0000, Rev: 0000)
Set NOR flash 0 to synchronous mode / Sub-TOC 1 OK / Move DSP window to correct location
hw_conf: SDRAM: 32 MB at 08000000
hw_conf: NOR Flash 0: 16 MB / NOR Flash 0 PMM: 1024 kB at 00F00000
hw_conf: MCU speed: 332.8 MHz / DSP speed: 235.2 MHz
hw_conf: Startup Reason: 03000000 / TOC location: 00080000 / VCore: No calibration done
HW configuration written to SDRAM (address: 0x09FFFF80)
Authenticate MCUSW / Image found at address : 0x003E0000
BSE: bse_pre_init(0x50000000, 0x00000000, 0x09FFFF80, 0x00000000)
*** WARNING: check ARM1136 errata item 399234 ***
BSE: MMU switched on, R/O offset 0xEFD20000
```

The modem CPU clock: **332.8 MHz** as NOLO programs it (DSP 235.2 MHz, SDRAM 110.4 MHz). These are the
values NOLO sets from its tables, not measurements; our PLLs only report "locked" [V for NOLO's choice].

## Tools

- Machine `-M n95-cmt` (`src/qemu-syborg/hw/arm/syborg_cmtprobe.c`): ARM1136 (`arm1136-r2`, MIDR 0x4107b366 =
  r0p6, `x-sctlr-prefetch` and `x-arm1136-tcm-etm` on: patch 0011). Every address not declared is a
  "learning" memory: it keeps writes, reads unwritten bytes as 0, and logs each distinct (pc, address, size,
  direction) once. Declared hardware comes from a map file: `ram`/`mirror`, `rom`, `pflash`, `val`, `orbits`,
  `latch`, `counter`, `hwsem`, `cbus`/`cbusreg`/`retuadc`, `stream`, `quiet`, `bootrom`, `bootrom-pa`, `dev`
  (format in the file's header comment).
- `python3 tools/cmt/probe.py [--boot nolo|mcusw] [--secs N] [--exact] [--dump A:L:F] [--gdb PORT
  [--gdb-from-start] --gdb-cmds FILE]`: builds the NOR image (`tools/cmt/nor.py`, all CMT partitions of the
  RM-320 C0R at their locations), runs the machine, samples the PC over QMP, prints the final registers,
  hot PCs, exceptions and the learned accesses; `--gdb-from-start` runs a gdb script from reset (hardware
  breakpoints work; `x/s`, registers incl. TTBR0_EL1 / SCTLR_EL1 / DACR). `--exact` (one instruction per TB)
  gives exact PCs in the access log but defeats the SCTLR prefetch, so MCUSW faults at its MMU enable.
- `python3 tools/cmt/xti.py [access.log.stream]`: the trace port's messages as text (format string +
  arguments; doubles in FPA word order).
- Map of what was found: `tools/cmt/n95-cmt.map` (every line with its evidence).
- Ghidra (owner's project, folder `/n95cmt`): `mcusw_f0100000.bin` (base 0xf0100000, Thumb seeded),
  `nolo_080a0400.bin` (NOLO; it is position-independent, analysed at 0x080a0400, runs at 0x000a0400),
  `sram_30000400.bin` (NOLO's code copied to SRAM: the NOR id routine at 0x3000072a). Decompiled text:
  `build/n95-cmt/ghidra/nolo.c`, `hwconf.c`.
- Tests: `tests/cmt_probe_test.py` (image layout; the MCUSW-alone probe; the NOLO -> MCUSW chain with one
  regression test per stop below).

## Memory map and hardware found [V unless marked]

| Address | What | Evidence | Model |
|---|---|---|---|
| 0x00000000 | NOR flash, 16 MB: TOC at 0x1000 ("RAP3"/"TOC"), SWBL TOC at 0x80000 (NOLO 0xa0000, DSP0 0x200000, MCUSW 0x3e0000, VECTORS 0x880000; per-phone NPC/VARIANT/HWC/R&D/CCC at 0x60000-0x7ffff, not in the flash file), PMM at 0xf00000 | NOLO sets its TOC pointer to 0x1000; the TOC entries | `pflash` (QEMU pflash_cfi01, Intel ids 0x89/0x8809, device width 2). The N95's real part is not known [I]; NOLO supports Intel, ST, Spansion and Samsung parts and tries Intel's read-id first |
| 0x08000000 | SDRAM (NOLO: 32 MB, 16 bit, 110.4 MHz) | NOLO's size probe writes 0x08000000 + (1 << bit) | 16 MB RAM mirrored over 128 MB; NOLO then reports 32 MB [I: the real size] |
| 0x18000000 | SDRAM controller | NOLO FUN_080a1062 | learning |
| 0x20030300 | chip id: write a selector, read `[3:0]` type (0 "3G", 1 GSM, 2 CDMA, 3 IDO, 4 U, 5 IDO2G, 6 TOR), `[19:8]` version | NOLO FUN_080a194c, MCUSW FUN_f049c8c8 | `val 0x1100` = "RAP3Gv1.10 PA"; MCUSW knows 0x1000, 0x1100, 0x1200, 0x2000, 0x11100, 0x21100, 0x31100; the N95's own value is not known [I] |
| 0x20032d24/44/84/a4 (+8) | PLLs 0..3: config, control (lock = bit 4) | NOLO FUN_080a11b8 | `orbits` (lock bit set) |
| 0x20032d14 | reset control (`hw_reset_generate` writes it) | MCUSW FUN_f010523c | learning (a reset does nothing yet) |
| 0x20040480 (+0x10) | free-running counter, read after a write of 0 to +0x14 | MCUSW delay calibration FUN_f010aeac, NOLO | `counter 32768` [I: rate] |
| 0x20031d40/80/c0 | timer channels (enabled by MCUSW after the calibration) | access log | learning |
| 0x30000000 | internal SRAM: NOLO's data and code copies, boot stacks | NOLO's scatter table, sp | learning (quiet) |
| 0x401ef000 | trace port (XTI class): byte 0xc7 opens a message; format string and arguments follow | access log | `stream` -> `tools/cmt/xti.py` |
| 0x50000000 (virtual 0xff600000) | ARM L2 cache controller, L210/L220 class | 0x100, 0x730, 0x7fc, 0xf40 | QEMU `l2x0` |
| 0xffff0000.. | on-chip boot ROM (not in any flash file) | NOLO calls 0xffff1209; MCUSW 0xffff7be4 etc. | `bootrom` / `bootrom-pa` stubs (below) |
| 0x20030700 | CBUS controller (serial bus to the power chips; device 1 = Retu) | hw_csm.c FUN_f010ce44/FUN_f010cdde, NOLO FUN_080a07ec | `cbus`; Retu 0x16 = 0x0500 (0x100 battery in range, 0x400 power-on reason [I: power key]); Retu ADC (reg 0x08) channels 1 and 8 = 0x200 -> 3954 mV |
| 0x20040204.. | hardware semaphores (0 guards the CBUS) | FUN_f010dd88 / dda2 / ddb0 | `hwsem`: a read takes a free one (returns 1), any write frees it [I: semantics] |
| 0x20032d8c bit 4 | ACCPLL lock, follows a request latch: 0x8000 to 0x20032c88 on, to 0x20032c84 off | FUN_f010cb2a / FUN_f010caf8 | `latch` |
| 0x20032200, 0x20032300 | I2C buses 1 and 2, TI OMAP I2C layout (REV +0, SYSS +0x10, SYSC +0x20, CON +0x24) | hal_i2c_rap.c FUN_f01b9f28 | REV 0x36 [I], reset done; transfers not modelled |
| 0x20024000.. | SSI, the link to the APE: SSR (receive) 0x20024600, SST (transmit) 0x20024700, sub-block resets at +0x30/34, +0x348/34c, +0x460/464 | ssi_hw.c ("CMT_SSI: ..."), FUN_f012eb80 | reset done bits only |

CPU-side (patch `patches/qemu-modern/0011-arm1136-sctlr-prefetch-tcm-etm.patch`, opt-in per CPU):
- `x-sctlr-prefetch`: an SCTLR write does not end the TB. MCUSW turns its MMU on while running at its
  physical address (NOLO jumps to 0x3e03d0, header +0x148) and relies on the already-fetched `nop; bx lr`
  (0xf0106b24..2c) to reach its virtual address; its tables map only the first MB flat.
- `x-arm1136-tcm-etm`: the TCM region registers (c9, c1, 0/1: 0 = no TCM) and the ETM's cp14 interface
  (op1 = 1: RAZ/WI, except ETMCR kept and ETMSR bit 1 = ETMCR bit 10). MCUSW reads DTCM at 0xf0107148 and
  programs the ETM at 0xf0107564.

## The boot ROM [V from both callers]

1. **Security service dispatcher, 0xffff1209 (Thumb)**: `r0` = 16-byte service id (12 bytes
   `ab0fbd96af2ac271d26264af`, u16 0, u16 function), `r1` = 7, `r2`/`r3` arguments. NOLO uses function 0x14
   (authenticate the BB5 image whose header is at r2; 1 = OK) and has wrappers for 0x18 and 0x1d. Stand-in:
   answers 0x14 with 1, logs the rest.
2. **Secure (PA) call gate, per chip version** (stand-in `bootrom-pa 0xffff7be4`, answers function 0x14 with 1
   and every other function with 0; calls seen at boot: 0x18 x3, 0x1d x2, 0x0a, 0x13, 0x14 (DSP0 header at
   0x200000), 0x17): MCUSW FUN_f049c8c8 picks a ROM pair by chip id (0x1100 ->
   0xffff7c15 / 0xffff7be4; 0x31100 -> 0xffff8ad5 / 0xffff8a94; ...), FUN_f049c540 saves its state, cleans the
   caches, translates its stack and request to physical, pushes a physical resume address (0xf049c65d's),
   turns the MMU off (SCTLR & ~0x9805 | 0x2002: high vectors, alignment), remaps the peripheral port
   (c15, c2, 4 = 0x20000013) and jumps (ARM) to the even address with r4 = the request, r5 = its physical
   address, r9/r10 = the virtual-to-physical delta. The ROM returns through the resume code with a status in
   r4 (2 = special case: return straight to lr). About 50 MCUSW functions (0xf01e3fb2..0xf01e5b16) go
   through this gate: the modem's security API.

## Where it stops now

The OS idles (WFI) with nothing to wake it. Next layer: the interrupt controller (find it from the IRQ
vector at 0xffff0018: the high vectors are on, so the boot ROM region needs the vector page MCUSW expects),
the OS tick timer (the timer channels at 0x20031d40/80/c0), then the SSI with a peer: the Symbian side's
`nokiaiscdriver`/`sisadriver` link instead of syborg-modem's reply tables.

## Stops passed (each has a regression test in tests/cmt_probe_test.py)

1. MCUSW alone: L2 clean-by-way poll (0x500007fc) -> QEMU `l2x0`.
2. MCUSW alone: no hw_conf records (bse_pre_init walks address 0) -> run NOLO instead.
3. NOLO: PLL lock (bit 4) -> `orbits`.
4. NOLO: "No SDRAM found" (SDRAM is at 0x08000000, the NOR at 0) -> RAM there, mirrored.
5. NOLO: boot ROM call 0xffff1209 (authenticate MCUSW) -> `bootrom` stub.
6. NOLO: "NOR Flash 0 not found" -> pflash_cfi01, device width 2; MCUSW's `bse_mmu_init.c#1917` assertion gone.
7. MCUSW: prefetch abort at 0x3e6b28 after the MMU enable -> patch 0011 `x-sctlr-prefetch`.
8. MCUSW: UNDEF on the DTCM region read -> patch 0011 `x-arm1136-tcm-etm`; then the ETMSR poll -> ETMCR/ETMSR.
9. MCUSW: delay calibration on a stopped counter -> `counter`.
10. MCUSW/NOLO: "Unknown ASIC type" -> chip id 0x1100.
11. MCUSW: first secure call into 0xffff7be4 (zeros) -> `bootrom-pa` gate stand-in.
12. MCUSW: CBUS spin on a semaphore that read back -1 -> `hwsem` (read takes, write frees).
13. MCUSW: power-off by the power task: no start-up reason -> Retu 0x16 = 0x0500; then battery < 3400 mV ->
    Retu ADC channels 1/8.
14. MCUSW: ACCPLL off-wait spun on the fixed lock bit -> `latch`.
15. MCUSW: I2C revision/reset waits on buses 1 and 2 -> REV 0x36, SYSS reset done.
16. MCUSW: "Illegal DSP code record" (hal_mdi.c#692) -> the gate answers function 0x14 with 1.
17. MCUSW: assertion pmm_record_background.c#1768 (PMM record write failed) -> QEMU patch 0012: pflash_cfi01
    dropped one-word buffered writes (count 0).
18. MCUSW: SSI sub-block soft-reset waits -> reset-done bits. Reaches the OS idle loop.
19. MCUSW: start-up self-tests (ear data, SIM clock/IO, MBUS loopbacks) failed -> MTC state 8, the phone's
    Starter fatal -> `setclr` + `loop` board wiring; startup reason 1.
20. MCUSW: MTC stalled in group 10 (PRNG seeding waits for the hash accelerator) -> `sham`; state 0x0a.
21. Linked: crash from the IRQ path at ~12 s -> intc +0x90 unlocks, IRQ line gated on the lock.
22. Phone self-test run: power key, sleep clock (`dspreply`), wake test (CAWake/ACWake in the peer) fixed;
    security test open.
23. Security test: stand-in PA directory entry PA_ESN (`pa`); all phone self-tests pass.
24. WLAN record: stand-in PMM record (`pmm`); ISC stand-in messages to clients up to 2048 bytes.
25. Modem time invalid (date/time query, start-up state 14): backup battery ADC (Retu channel 12), RTC marker
    (Retu reg 5), PMM group 8 records 0/1/7.
26. Linked: Idle killed (ViewSrv 11) after Menu: replies on a shared ISC channel went to the wrong object
    (tests/modem_unit_test.py ModemCoreLink).
27. Linked: Switch off never ended: Retu watchdog = 0 (power cut) not modelled; link close = phone power off.

## Open questions [I]

- The N95 8GB's real chip version, NOR part and SDRAM size (all chosen from what the code accepts).
- The counter's rate (32.768 kHz assumed).
- The Retu bits (0x400 = power key, 0x1000 = charger) and which ADC channel is the battery voltage.
- The secure-call functions other than 0x14 (answered 0; nothing has failed on that yet).
- The per-phone areas (NPC, VARIANT, HWC, R&D, CCC at NOR 0x60000-0x7ffff; PMM at 0xf00000) are erased:
  MCUSW will look for IMEI, RF tuning and security data there (the `.pm` files of the flash package hold
  part of it).
- DSP0 is in flash but nothing runs it; the modem DSP (layer 1) will need at least a boot/mailbox stand-in.
