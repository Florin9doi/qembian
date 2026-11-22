#!/usr/bin/env python3
"""n95-cmt probe (tools/cmt/probe.py, -M n95-cmt): image layout and the boot stops fixed so far.

Needs the extracted modem partitions in build/n95-cmt/ (docs/research/modem-firmware-in-flash-files.md) and
the modern QEMU build; skips otherwise. Docs: docs/research/n95-cmt-emulation.md.
"""
import hashlib, os, re, shutil, struct, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'cmt'))
import probe  # noqa: E402

HAVE_FW = os.path.exists(os.path.join(probe.CMT, 'MCUSW@003e0000.hdr'))
HAVE_QEMU = os.path.exists(probe.QEMU)


@unittest.skipUnless(HAVE_FW, 'no extracted MCUSW in build/n95-cmt')
class Image(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.img = open(probe.build_image(self.dir), 'rb').read()

    def test_header_then_code(self):
        self.assertEqual(len(self.img), probe.MCUSW_END - probe.MCUSW_NOR)
        self.assertEqual(struct.unpack_from('<I', self.img, 0x1c)[0], 0x3d0)     # entry offset
        self.assertEqual(struct.unpack_from('<I', self.img, 0x148)[0], 0x3d0)

    def test_entry_is_arm_sub_r4_pc(self):
        # 0xf01003d0: sub r4, pc, #8 (r4 = the image base) -- the entry the machine resets to
        self.assertEqual(struct.unpack_from('<I', self.img, 0x3d0)[0], 0xe24f4008)


@unittest.skipUnless(HAVE_FW and HAVE_QEMU, 'needs build/n95-cmt and build/qemu-modern')
class Boot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp()
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cmt', 'probe.py'), '--boot', 'mcusw', '--secs', '1.5',
                            '--out', cls.out], capture_output=True, text=True, timeout=60)
        cls.report = r.stdout
        cls.access = open(os.path.join(cls.out, 'access.log')).read()

    def test_l2_clean_by_way_poll_no_longer_spins(self):
        # bug: without an L2 controller at 0x50000000 the clean+invalidate-by-way poll (write 0xff to
        # +0x7fc, wait for 0) at 0xf0107756 spun forever; the map's "dev l2x0" completes it
        self.assertNotRegex(self.report, r'R15=f0107756')
        self.assertNotIn('a=500007fc', self.access)

    def test_reaches_bse_pre_init_trace(self):
        # the trace port gets the bse_pre_init format string ("BSE:" = 0x3a455342 little-endian)
        self.assertIn('a=401ef008 s=4 v=3a455342', self.access)

    def test_stays_in_mcusw_no_exception(self):
        self.assertRegex(self.report, r'R15=f01')     # still in MCUSW, no exception vector


@unittest.skipUnless(HAVE_FW and HAVE_QEMU and os.path.exists(probe.os.path.join(ROOT, 'roms', 'RM-320')),
                     'needs the RM-320 flash file, build/n95-cmt and build/qemu-modern')
class NoloBoot(unittest.TestCase):
    """The real chain: NOLO (modem loader) from the NOR image, then MCUSW (tools/cmt/probe.py --boot nolo)."""

    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp()
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cmt', 'probe.py'), '--secs', '3',
                            '--out', cls.out], capture_output=True, text=True, timeout=120)
        cls.report = r.stdout
        cls.access = open(os.path.join(cls.out, 'access.log')).read()
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'cmt'))
        import xti
        cls.trace = '\n'.join(xti.render(d) for _, d in
                              xti.messages(open(os.path.join(cls.out, 'access.log.stream'))))

    def test_nolo_version_line(self):
        self.assertIn('SW : loader_2006_wk41', self.trace)

    def test_asic_type_known(self):
        # bug: with the id register unanswered NOLO printed "Unknown ASIC type" and MCUSW later took the
        # unknown-chip path (a call into the boot ROM's dispatcher + 4); the map's "val 0x20030300"
        self.assertNotIn('Unknown ASIC type', self.trace)
        self.assertIn('HW : RAP3Gv1.10 PA', self.trace)

    def test_pll_lock_bits(self):
        # bug: NOLO spun in FUN_080a11b8 waiting for PLL lock (bit 4 of 0x20032d2c); the map's "orbits"
        self.assertIn('Set MCU and DSP speed', self.trace)
        self.assertIn('hw_conf: MCU speed: 332.8 MHz', self.trace)
        self.assertIn('hw_conf: DSP speed: 235.2 MHz', self.trace)

    def test_sdram_found(self):
        # bug: with the NOR at 0x08000000 (a wrong guess) NOLO's SDRAM probe there failed: "No SDRAM found"
        self.assertNotIn('No SDRAM found', self.trace)
        self.assertIn('hw_conf: SDRAM: 32 MB at 08000000', self.trace)

    def test_nor_flash_detected(self):
        # bug: a plain ROM does not answer the Intel read-id command: "NOR Flash 0 not found", and MCUSW
        # failed 'running_in_sdram || ... HW_CONF_NOR_FLASH_ID' (bse_mmu_init.c#1917); now pflash_cfi01 with
        # device-width 2 (without it word 0 read 0x8988 = id0 << 8 | id1, not 0x89)
        self.assertIn('NOR Flash 0 detected (Mfr: 0089, Dev: 8809', self.trace)
        self.assertNotIn('HW_CONF_NOR_FLASH_ID', self.trace)

    def test_mcusw_authenticated_through_bootrom_stub(self):
        self.assertIn('BOOTROM ffff1209 fn=0x14', self.access)
        self.assertIn('Authenticate MCUSW', self.trace)

    def test_mmu_on_survives_prefetch(self):
        # bug: MCUSW enables its MMU at its physical address and relies on the prefetched "nop; bx lr";
        # QEMU ended the TB at the SCTLR write and faulted at 0x3e6b28 (patch 0011, x-sctlr-prefetch)
        self.assertIn('BSE: MMU switched on, R/O offset 0xEFD20000', self.trace)

    def test_tcm_etm_registers_and_delay_calibration(self):
        # bugs: UNDEF on "mrc p15, 0, r1, c9, c1, 0" (DTCM region) at 0xf0107148, then a spin on ETMSR bit 1
        # at 0xf0107570 (patch 0011, x-arm1136-tcm-etm), then a spin on a stopped counter at 0x20040490
        # (map "counter"): MCUSW now gets past its delay calibration to the chip-id read
        self.assertNotIn('Stack dump from address', self.trace)
        self.assertRegex(self.access, r'W pc=f0[0-9a-f]+ a=20030300 s=4 v=0000000a')   # (PCs approximate: TB chaining)

    def test_secure_call_gate_round_trip(self):
        # bug: MCUSW's first secure (PA) call jumped to the boot ROM's ARM gate 0xffff7be4 (zeros) and fell
        # into the high vectors; the map's "bootrom-pa" stub answers and returns through the resume address
        self.assertIn('PACALL fn=0x18', self.access)
        self.assertIn('MTC_Prod_RatSet', self.trace)          # MCUSW went on to start MTC

    def test_dsp_image_authenticated(self):
        # bug: "Illegal DSP code record" (hal_mdi.c#692): the gate answered 0 to function 0x14
        # (authenticate image) for the DSP0 header at 0x200000; it now answers 1 as NOLO's dispatcher does
        self.assertRegex(self.access, r'PACALL fn=0x14 .*a2=00200000.*-> 1 \(authenticate image: OK\)')

    def test_power_on_reason_and_battery(self):
        # bugs: the power task (FUN_f01793ca) switched the modem off: no start-up reason (Retu 0x16 read 0),
        # then a battery under 3400 mV (Retu ADC unanswered); now Retu 0x16 = 0x2520 (0x0500 + power key released,
        # see test_power_key_released, + SIM card switch, test_sim_no_card), ADC ch 1/8 = 0x200
        self.assertIn('CBUS R dev=1 reg=0x16 v=2520', self.access)
        self.assertNotIn('BSI out of range', self.trace)
        self.assertNotRegex(self.report, r'R15=f010c36c')      # the power-off loop (FUN_f010c354)

    def test_cbus_semaphore_read_to_take(self):
        # bug: a write-stores semaphore at 0x20040204 read back 0xffffffff after a release and the CBUS
        # access spun in FUN_f010dd88; "hwsem" reads 1 (and takes it) when free, any write frees it
        self.assertRegex(self.access, r'CBUS [RW] dev=1 ')

    def test_pmm_one_word_buffered_write(self):
        # bug: QEMU's pflash_cfi01 dropped a one-word buffered write (count 0) as a programming error, the
        # PMM record header write failed and MCUSW asserted (pmm_record_background.c#1768) -> patch 0012
        self.assertNotIn('hw_reset_generate(13)', self.trace)   # 13 = assertion (24, the APE sync timeout, is expected)

    def test_interrupts_and_os_timer(self):
        # missing: no interrupt controller or OS timer, MCUSW idled in WFI forever; now "intc" (0x20400000)
        # and "oneshot" (0x20040100, 32.768 kHz, interrupt 0x2c) let the scheduler run (dozens of IRQs in 3 s)
        irqs = open(os.path.join(self.out, 'int.log')).read().count('Taking exception 5')
        self.assertGreater(irqs, 10)

    def test_ssi_link_comes_up(self):
        # the SSI model + APE peer (map "ssi"): the peer sends the link start 0x00000000, the modem's handler
        # reads it from SSR+0x80 (interrupt 0x3c; bug: the controller acknowledged on a +0x08 read, which the
        # idle code polls, so the interrupt was lost); it used to answer 0x10000ffe once its wake-test timer
        # (hal_timer.c, 0x20040180 at 9.6 MHz, interrupt 0x24; bug: not modelled, the timeout never came)
        # expired; now the peer plays the wake test (see test_wake_test_passes)
        self.assertIn('CMT read ctrl 00000000', self.access)
        self.assertRegex(self.access, r'ctrl <- CMT 10000ff[ef]')

    def test_wake_test_passes(self):
        # bug (2026-11-21): the peer held ACWake high, so the cold-boot wake test failed (0x10000ffe) and the
        # modem's ST_CMT_APE_WAKEUP_TEST (0x3a) reported a failure to the phone. Now ACWake follows CAWake during
        # the test; it took the GenIO pending-pin register (bank 0 +0x3c: learned memory read 0, the dispatcher
        # never saw pin 29) and a fast answer (the modem's timeout is about 1 ms)
        self.assertIn('ACWake low', self.access)
        self.assertIn('ctrl <- CMT 10000fff', self.access)
        self.assertNotIn('ctrl <- CMT 10000ffe', self.access)

    def test_fake_dsp_registers_device_7c(self):
        # bug: MTC's device sync waited for PhoNet device 0x7c (the DSP) and reset with reason 24; the fake DSP
        # (map "dsp") completes the MDI boot handshake and announces 0x7c with a media-0x12 / resource-0xDA message
        self.assertIn('DSP config 00080000 01020304 18010000 00140000', self.access)
        self.assertNotIn('hw_reset_generate(24)', self.trace)

    def test_no_reset_with_power_chip_ids_and_i2c(self):
        # bugs after the sync: hal_light_phy.c#302 (CBUS device 2 type must be 0x0b/0x0d: ids 0x0220/0x0350),
        # hw_audio_tpa6130.c#788 (I2C transfers: the OMAP2-style "i2c" model), a divide by zero in a tick
        # calibration under free-running TCG (probe default -icount shift=2)
        self.assertNotIn('hw_reset_generate', self.trace)

    def test_sos_proxy_registers_itself(self):
        # bugs fixed on the way (2026-11-21): the interrupt controller gated the CPU line on +0x88 (a set/clear
        # pair the OS writes around critical sections, not an IRQ mask), so SSI interrupt 0x3c was never
        # dispatched; the I2C model (16-bit DATA, next byte after the acknowledge, STP clearing) stalled the
        # TPA6130 driver, and with it MTC's start-up. Now MTC broadcasts MTC_STATE_INFO_IND (state 8, action 0x0c)
        # and the SOS proxy registers with the SSI driver by itself
        self.assertNotIn('hw_reset_generate', self.trace)

    def test_modem_answers_mtc_state_query(self):
        # the whole chain (2026-11-21): the APE peer activates the SOS media (router media control 0xD7: 03, then
        # 09 with byte 11 = 3; bug: without it MTC's reply could not leave and MTC asserted mtc_srv.c#3689
        # "!status"), messages carry the SOS media id 0x1b (bug: with 0 they took the proxy's control path and
        # the sender device 0x6c was never learned); then MTC_STATE_QUERY_REQ is answered by the modem itself.
        # Since the self-test loopbacks and the hash accelerator the answer is state 0x0a (RF_INACTIVE, offline):
        # bug (2026-11-21): without the "sham" device the PRNG seeding in the WCDMA core appl 0x3e's init waited
        # forever for the hash's output-ready bit, so MTC stalled creating group 10 and never sent
        # MTC_STATE_INFO_IND, and the SOS proxy never registered
        out = tempfile.mkdtemp()
        m = os.path.join(out, 'q.map')
        with open(m, 'w') as f:
            f.write(open(os.path.join(ROOT, 'tools', 'cmt', 'n95-cmt.map')).read())
            f.write('\nssisendat 2000 00006c1506000020010200 00\n')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cmt', 'probe.py'), '--secs', '8', '--map', m,
                        '--out', out], capture_output=True, text=True, timeout=120)
        with open(os.path.join(out, 'access.log')) as f:
            access = f.read()
        self.assertRegex(access, r'ISI CMT->APE res=15 tid=01 id=65 robj=20 sobj=30 len=12: 1b 6c 00 15 06 00 20 30 '
                                 r'01 65 0a 0a')

    def test_phone_selftest_run_results(self):
        # the phone's DosServer asks the modem's ST server for the start-up test list (35/02 {01 00}) and runs
        # them (35/00 {02 00}); the run response lists the failed tests. Bugs fixed (2026-11-21): power key
        # stuck (0x5a, Retu 0x16 bit 5), sleep clock (0x38: the DSP's E6/0D reply, map "dspreply"), CMT-APE
        # wake-up (0x3a, the peer's wake test), security (0x5d: the PA_ESN entry of the stand-in PA directory,
        # map "pa", nor.pa_stub). Nothing fails any more.
        out = tempfile.mkdtemp()
        m = os.path.join(out, 'st.map')
        with open(m, 'w') as f:
            f.write(open(os.path.join(ROOT, 'tools', 'cmt', 'n95-cmt.map')).read())
            f.write('\nssisendat 3000 00006c350600001800020100\nssisendat 3500 00006c350600001800000200\n')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cmt', 'probe.py'), '--secs', '8', '--map', m,
                        '--out', out], capture_output=True, text=True, timeout=120)
        with open(os.path.join(out, 'access.log')) as f:
            access = f.read()
        run = re.search(r'ISI CMT->APE res=35 tid=00 id=01 .*?: ((?:[0-9a-f]{2} ?)+)', access)
        self.assertIsNotNone(run)
        data = bytes.fromhex(run.group(1).replace(' ', ''))[10:]
        failed = {data[i + 2] for i in range(2, len(data) - 3, 4) if data[i] == 1 and data[i + 1] == 4}
        self.assertEqual(data[:2], b'\x00\x00')     # status OK, no failed-test sub-blocks
        self.assertEqual(failed, set())

    def test_selftest_loopbacks_give_normal_startup(self):
        # bug (2026-11-21): nothing looped the self-test outputs back (ear data, SIM clock/IO, MBUS: map "setclr"
        # and "loop" lines), four start-up self-tests failed, MTC reported startup reason 8 and state 8
        # (SELFTEST_FAIL), and the linked N95's Starter took startup reason 7 and went to its fatal state 0xFA
        self.assertIn('mtc_startup_reason: 1,', self.trace)
        self.assertNotIn('mtc_startup_reason: 8,', self.trace)

    def test_power_key_released(self):
        # bug (2026-11-21): Retu 0x16 bit 5 (power key line, 1 = released) read 0, so ST_PWR_KEY_TEST (0x5a)
        # reported a stuck key and the phone's DosServer declared the modem's self-test failed
        v = int(re.search(r'CBUS R dev=1 reg=0x16 v=([0-9a-f]{4})', self.access).group(1), 16)
        self.assertTrue(v & 0x20)

    def test_pa_stub_directory(self):
        # nor.pa_stub: entry 0 = {offset, name "PA_ESN" at +0x14}, the next entry erased (ends the directory)
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'cmt'))
        import nor
        img = nor.pa_stub(bytearray(b'\xff' * 0x100000), ['PA_ESN'])
        e = nor.PA_DIR
        self.assertEqual(int.from_bytes(img[e:e + 4], 'little'), nor.PA_STUB_OFFSET)
        self.assertEqual(bytes(img[e + 0x14:e + 0x1b]), b'PA_ESN\0')
        self.assertEqual(img[e + 32 + 0x14], 0xff)

    def test_pmm_records_format(self):
        # nor.pmm_records: page headers (active small page 0xe8000 = cc) and a record header
        # {ff, 30, group, index, ffff, len32, total32} at page + 12
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'cmt'))
        import nor
        img = nor.pmm_records(bytearray(b'\xff' * 0x1000000), small=[(0x139, 0, bytes(0x267))])
        p = nor.PMM + 0xe8000
        self.assertEqual(bytes(img[p:p + 12]), b'\xf0\xf0\x03\x00\xff\xff\xff\xcc\0\0\0\0')
        self.assertEqual(bytes(img[p + 12:p + 28]),
                         struct.pack('<BBHHHII', 0xff, 0x30, 0x139, 0, 0xffff, 0x267, (16 + 0x267 + 1) & ~1))

    def test_wlan_permanent_data_read(self):
        # bug (2026-11-21): the PMM area was erased, so WlanServer's PN_PERMANENT_DATA read of group 0x139
        # (615 bytes) got status 11 (no such record), WlanServer exited -5 and IPWServer / the Telephone app
        # panicked; the map's "pmm 0x139 0 0x267" stand-in record answers it with status 00
        out = tempfile.mkdtemp()
        m = os.path.join(out, 'p.map')
        with open(m, 'w') as f:
            f.write(open(os.path.join(ROOT, 'tools', 'cmt', 'n95-cmt.map')).read())
            f.write('\nssisendat 3000 00006c231200001801040139000000000000000000000267\n')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cmt', 'probe.py'), '--secs', '6', '--map', m,
                        '--out', out], capture_output=True, text=True, timeout=120)
        with open(os.path.join(out, 'access.log')) as f:
            access = f.read()
        self.assertRegex(access, r'ISI CMT->APE res=23 tid=01 id=05 \S+ \S+ len=640: 1b 6c 00 23 7a 02 18 2a 01 05 00')

    def test_sim_no_card(self):
        # bug (2026-11-21): with Retu 0x16 bit 13 clear the SIM driver only told the APDU server "no card", the SIM
        # server waited in state 4 for card news and 09/C0 answered 4B (not ready) forever, so the phone's start-up
        # waited for the SIM state. Now the driver powers the card, the Retu SIM timer (map "retuirq") expires with
        # no answer-to-reset and the SIM server concludes "no card": C1 {00 06}
        out = tempfile.mkdtemp()
        m = os.path.join(out, 's.map')
        with open(m, 'w') as f:
            f.write(open(os.path.join(ROOT, 'tools', 'cmt', 'n95-cmt.map')).read())
            f.write('\nssisendat 6000 00006c090600000707c00000\n')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cmt', 'probe.py'), '--secs', '8', '--map', m,
                        '--out', out], capture_output=True, text=True, timeout=120)
        with open(os.path.join(out, 'access.log')) as f:
            access = f.read()
        self.assertIn('SIM timer expired: event 5', access)
        self.assertRegex(access, r'ISI CMT->APE res=09 tid=07 id=c1 .*: 1b 6c 00 09 0a 00 07 36 07 c1 00 06')

    def _probe_with(self, extra, secs=8, cut=None):
        out = tempfile.mkdtemp()
        m = os.path.join(out, 'x.map')
        text = open(os.path.join(ROOT, 'tools', 'cmt', 'n95-cmt.map')).read()
        if cut:
            text = '\n'.join(ln for ln in text.splitlines() if not ln.startswith(cut))
        with open(m, 'w') as f:
            f.write(text + '\n' + extra + '\n')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cmt', 'probe.py'), '--secs', str(secs), '--map', m,
                        '--out', out], capture_output=True, text=True, timeout=120)
        with open(os.path.join(out, 'access.log')) as f:
            return f.read()

    TIME_QUERY = 'ssisendat 9000 00006c1906000018180a0000'
    TIME_ANSWER = r'ISI CMT->APE res=19 tid=18 id=0b .*: 1b 6c 00 19 16 00 18 29 18 0b .* 04 04 (\w\w) 00'

    def test_modem_time_kept(self):
        # bug (2026-11-22): the modem reported its time invalid (19/0B sub-block 04 = 2), so the phone asked for the
        # date and time at every start and its start-up then stalled (state 14). The time appl keeps the stored time
        # only with a charged backup battery (ADC request 0x13 = Retu channel 12 >= 1700 mV; it read 0, "RTC
        # lost"), Retu's RTC marker (reg 5 = 0x007d) and the PMM group 8 records 0, 1 (equal) and 7 (fingerprint)
        import re
        m = re.search(self.TIME_ANSWER, self._probe_with(self.TIME_QUERY, secs=12))
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), '01')

    def test_modem_time_lost_without_backup_battery(self):
        # the same with the backup battery channel at 0: the time counts as lost (as a phone whose RTC battery ran
        # down), sub-block 04 = 2
        import re
        m = re.search(self.TIME_ANSWER, self._probe_with(self.TIME_QUERY, secs=12, cut='retuadc 12 '))
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), '02')

    def test_power_off_request_expires_retu_watchdog(self):
        # bug (2026-11-22): Switch off on the linked N95 never ended: the modem answered MTC_POWER_OFF_REQ (15/03 ->
        # 66) and set Retu's watchdog (reg 0x17) to 0, which on the real phone cuts the power, but nothing modelled
        # that. Now the probe stops on it (and syborg-modem powers the phone off when the link closes)
        access = self._probe_with('ssisendat 9000 00006c150600002002030000', secs=20)
        self.assertRegex(access, r'ISI CMT->APE res=15 tid=02 id=66 ')
        self.assertIn('Retu watchdog expired (reg 0x17 = 0): power off', access)

    def test_bad_loop_term_is_refused(self):
        out = tempfile.mkdtemp()
        m = os.path.join(out, 'bad.map')
        with open(m, 'w') as f:
            f.write('loop 0x20031dc8 0x10000 x:0x19:0x2000\n')
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'cmt', 'probe.py'), '--secs', '0.1', '--map',
                            m, '--out', out], capture_output=True, text=True, timeout=60)
        err = os.path.join(out, 'qemu.err')
        said = r.stdout + r.stderr + (open(err).read() if os.path.exists(err) else '')
        self.assertIn("bad loop term 'x:0x19:0x2000'", said)

    def test_reaches_os_idle(self):
        # ACCPLL latch (0x20032d8c bit 4 follows 0x20032c88/84), I2C buses 1/2 (rev 0x36, reset done), the
        # SSI sub-block resets (0x2002434c, 0x20024034, 0x20024464): MCUSW ends its start-up in the OS idle
        # loop (WFI in FUN_f049be40, called from FUN_f010c372)
        # (with interrupts the OS goes on: the idle loop is one of the places the 3 s run may stop in)
        # (0x20024464, once taken for an SSI reset bit, is the hash accelerator's reset-done: map "sham")
        self.assertIn('MTC_Prod_RatSet', self.trace)


@unittest.skipUnless(shutil.which('gcc'), 'needs gcc')
class ShamModel(unittest.TestCase):
    """The modem's SHA-1/MD5 accelerator model (syborg_cmt_sham.h) driven as the firmware's driver does, against
    hashlib: full blocks, then a close block with the 0..64 byte remainder (padding into one or two blocks)."""

    @classmethod
    def setUpClass(cls):
        cls.exe = os.path.join(tempfile.mkdtemp(), 'sham')
        subprocess.run(['gcc', '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-o', cls.exe,
                        os.path.join(ROOT, 'tests', 'cmt_sham_harness.c')], check=True)

    def digest(self, alg, m):
        return subprocess.run([self.exe, alg, m.hex()], capture_output=True, text=True, check=True).stdout.strip()

    def test_edge_lengths_match_hashlib(self):
        for n in (0, 1, 3, 4, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 300):
            m = bytes((i * 7 + n) & 0xff for i in range(n))
            for alg in ('sha1', 'md5'):
                with self.subTest(alg=alg, n=n):
                    self.assertEqual(self.digest(alg, m), hashlib.new(alg, m).hexdigest())

    def test_known_vector(self):
        self.assertEqual(self.digest('sha1', b'abc'), 'a9993e364706816aba3e25717850c26c9cd0d89d')


if __name__ == '__main__':
    unittest.main()
