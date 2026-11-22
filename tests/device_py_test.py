#!/usr/bin/env python3
"""Unit tests for tools/device.py (plain unittest): tests/device_py_test.py"""
import json, os, re, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as D

MAP = os.path.join(ROOT, 'build', 'epocroot', 'sf', 'adapt', 'qemu', 'baseport', 'syborg', 'keyboard', 'syborg_keyboard_map.h')
BASEPORT_CFG = os.path.join(ROOT, 'build', 'epocroot', 'sf', 'adapt', 'qemu', 'baseport', 'syborg', 'config.inc')


class DeviceTest(unittest.TestCase):
    def test_p1i_memory_stick_starts_formatted(self):
        """2026-11-17: the SE P1i showed "Memory corrupt D:" on every fresh boot: its D: (Memory Stick) image was a blank
        file and SE's start-up does not format it. `format = "fat"` drives are made as an empty FAT volume."""
        import shutil, tempfile
        if not shutil.which('mformat'):
            self.skipTest('mtools not installed')
        prof = D.load('se-p1i')
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'nvmemory2.img')
            msg = D.drive_image(prof, 'nvmem2', out, uda=False)
            self.assertIn('empty FAT32', msg)
            b = open(out, 'rb').read(512)
            self.assertEqual(b[510:512], b'\x55\xaa')
            self.assertEqual(b[82:87], b'FAT32')

    def test_rofs_patch_hex_bytes(self):
        """2026-11-17: ROFS code patches (SE P1i porthandlersvr.dll, H-p1i-no-bt-hci) give old/new as hex bytes"""
        self.assertEqual(D.rofs_patch_bytes('00f0 e6ec', 'hex'), bytes([0, 0xf0, 0xe6, 0xec]))
        self.assertEqual(D.rofs_patch_bytes('ab', 'utf-16le'), b'a\x00b\x00')

    def test_store_takes_ra_type_codes(self):
        """2026-11-17: the E90 Communicator's type code is RA-6; the ROM store and device.py took only RM-/SE-: the
        imported firmware never reached the index and firmware-add said: not in the ROM store"""
        import re
        for tc in ('RM-596', 'RA-6', 'RH-17', 'SE-P1i'):
            self.assertTrue(re.fullmatch(D.TYPE_CODE_RE, tc), tc)
        self.assertFalse(re.fullmatch(D.TYPE_CODE_RE, 'firmware'))
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
        import identify
        self.assertEqual(identify.from_name('RA-006 v.300.3 E-90.rar')[0], 'RA-006')   # file-name fallback only
        self.assertEqual(identify.from_name('RM-596_025.007.zip'), ('RM-596', '025.007'))

    def test_profiles_check(self):
        for dev in D.device_ids():
            errs, _ = D.check(D.load(dev))
            self.assertEqual(errs, [], dev)

    def test_gl_msaa_at_most_the_phones(self):
        """2026-11-13: OpenVG surfaces were 8x multisampled; the phones' VideoCore IV offers EGL at most 4x
        (models/nokia-n8-rm596/egl-configs.txt), and 8x costs more on a phone GPU (Pixel 3a). No profile asks for more."""
        for dev in D.device_ids():
            gl = D.GL_HOST_DEFAULTS_OF(D.load(dev))
            if 'msaa' in gl:
                self.assertLessEqual(gl['msaa'], 4, dev)

    def test_panel_shows_only_posted_updates(self):
        """2026-11-13: the 5800 and 5230 scanned out WSERV's video RAM directly, so every redraw showed half done
        (regions blinking black). Their display stand-in now keeps a panel buffer filled only by ctl 9/8, as the real
        driver (d_display.panel): the profiles must ask for it."""
        import subprocess
        for dev in ('nokia-5800-rm356', 'nokia-5230-rm588', 'nokia-n95-rm159', 'nokia-n95-8gb-rm320', 'nokia-e5-00-rm632',
                    'nokia-n97-rm505', 'nokia-e52-rm469', 'nokia-5730-rm465', 'nokia-n79-rm348', 'nokia-e72-rm530',
                    'nokia-n86-rm484'):
            out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools/device.py'), 'qemu-args'],
                                 env=dict(os.environ, SYBORG_DEVICE=dev), capture_output=True, text=True).stdout
            self.assertIn('d_display.panel=1', out, dev)

    def test_registry_cli_mixed_case_code(self):
        """2026-11-13: `device.py registry CODE` upper-cased CODE and looked it up as is, so the Sony Ericsson
        rows (SE-P1i, lower-case letters in the code) raised KeyError"""
        import subprocess
        for code in ('SE-P1i', 'se-p910i', 'rm-132'):
            r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'device.py'), 'registry', code],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(r.stdout.upper().startswith(code.upper()), r.stdout)
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'device.py'), 'registry', 'XX-1'],
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('Traceback', r.stderr)

    def test_qemu_args_without_isi_modem(self):
        """2026-11-13: qemu_args built the ISI reply table for every phone; the P1i (no ISI modem, no
        isi-replies.txt) failed to start. Only phones whose modem is the ISI fake peer get the table."""
        for dev in D.device_ids():
            prof = D.load(dev)
            isi = D.get(prof, 'modem.kind') == 'isi-fake-peer'
            if not isi or os.path.exists(D.paths(prof)['dir']):
                args = ' '.join(D.qemu_args(prof, None)) if not isi else None
                if args is not None:
                    self.assertNotIn('syborg-modem.replies', args, dev)

    def test_store_accepts_se_type_codes(self):
        """2026-11-13: the ROM store scan took RM-nnn only; roms/SE-P1i/R6E30 was invisible (no-firmware)"""
        import tempfile
        old = D.STORE
        with tempfile.TemporaryDirectory() as d:
            for tc in ('SE-P1i', 'RM-132', 'junk'):
                os.makedirs(os.path.join(d, tc, 'V1'))
                open(os.path.join(d, tc, 'V1', 'rom.toml'), 'w').write('type_code = "%s"\nsw_version = "V1"\n' % tc)
            try:
                D.STORE = d
                got = sorted(D._store_entries())
            finally:
                D.STORE = old
        self.assertEqual(got, ['RM-132/V1', 'SE-P1i/V1'])

    def test_moving_model_rom_base_in_symbol_tools(self):
        """2026-11-13: syb_sym.py and xipdis.py assumed ROM base 0x80000000; on moving-model ROMs (0xF8000000:
        N73, P1i) the index had 0 modules and xipdis crashed. They read the base from core.json now."""
        import subprocess
        prof = D.load('se-p1i')
        if not os.path.exists(D.paths(prof)['core_json']):
            self.skipTest('se-p1i not inventoried')
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'syb_sym.py'), 'addr', '0xf8041812'],
                           capture_output=True, text=True, env=dict(os.environ, SYBORG_DEVICE='se-p1i'))
        self.assertIn('DRTAEABI.dll', r.stdout, r.stderr)

    def test_extends(self):
        child = D.load('nokia-n95-8gb-rm321')
        self.assertEqual(child['_extends'], 'nokia-n95-8gb-rm320')
        self.assertFalse(D.get(child, 'hardware.wlan'))
        self.assertEqual(D.get(child, 'board.ram_mb'), 168)          # inherited
        self.assertEqual(len(child['standins']), len(D.load('nokia-n95-8gb-rm320')['standins']))

    def test_merge_arrays_by_name(self):
        m = D._merge({'s': [{'name': 'a', 'x': 1}, {'name': 'b'}]},
                     {'s': [{'name': 'a', 'x': 2}, {'name': 'c'}], 'remove': ['b']})
        self.assertEqual(sorted(x['name'] for x in m['s']), ['a', 'c'])
        self.assertEqual([x for x in m['s'] if x['name'] == 'a'][0]['x'], 2)

    def test_every_model_and_firmware_loads(self):
        for m in sorted({v[0] for v in D._index().values()}):
            for fw in D.firmware_names(m):
                prof = D.load('%s@%s' % (m, fw))
                self.assertEqual(prof['_model'], m)
                self.assertEqual(prof['_firmware'], fw)

    def test_registry_matches_profiles(self):
        reg = D.registry()
        ids = set(D.device_ids())
        for r in reg.values():
            if r.get('model'):
                self.assertIn(r['model'], ids, r['code'])
        for i in ids:
            tc = D.get(D.load(i), 'device.type_code')
            self.assertIn(tc, reg, i)
            self.assertTrue(i.startswith(reg[tc]['model']), (i, tc))   # firmware variants: <model>-<suffix>

    def test_merge_remove_table_key(self):
        m = D._merge({'files': [{'role': 'core', 'p': {'core': 'C', 'rofs-after-core': 'C'}}]},
                     {'files': [{'role': 'core', 'p': {'rofs': 'R', 'remove': ['rofs-after-core']}}]})
        self.assertEqual(m['files'][0]['p'], {'core': 'C', 'rofs': 'R'})

    def test_aliases_and_default(self):
        self.assertEqual(D.resolve('n95-8gb'), 'nokia-n95-8gb-rm320')
        self.assertEqual(D.resolve('default'), 'nokia-n95-8gb-rm320')

    def test_qemu_args(self):
        a = D.qemu_args(D.load('nokia-n95-8gb-rm320'))
        for want in ('syborg-fb.width=240', 'syborg-fb.height=320', '168M', 'arm1136-r2'):
            self.assertIn(want, a)

    @unittest.skipUnless(os.path.exists(MAP), 'no generated key map yet (scripts/build-syborg.sh)')
    def test_built_keymap_is_current(self):
        """the EKEYB table in the build tree (patch qemu-baseport 0005 includes it) = the profile's"""
        gen = '\n'.join(D.keymap_header(D.load('nokia-n95-8gb-rm320')))
        self.assertEqual(open(MAP).read().strip(), gen.strip())

    def test_keymap_header_is_device_independent(self):
        """with QEMU-side translation every device gets the same guest table (shared baseport builds)"""
        heads = {'\n'.join(D.keymap_header(D.load(d))) for d in D.device_ids()
                 if D.get(D.load(d), 'board.input.translation') == 'qemu'}
        self.assertEqual(len(heads), 1)

    @unittest.skipUnless(os.path.exists(BASEPORT_CFG), 'no baseport build tree yet (scripts/setup-epocroot.sh)')
    def test_arm1136_kernel_does_not_check_alignment(self):
        """2026-11-13: the 5800's menu died with KERN-EXEC 3 the moment Hitchcock's software renderer read a pair of
        8-bit pixels out of a byte row with an unaligned `ldrh` (hitchcock.dll+45930): the EPL kernel's ARM1136 default
        programmes SCTLR with MMUCR_A and without MMUCR_U, while Nokia's ROM code does unaligned accesses on purpose.
        The syborg baseport must ask for no alignment check, like the syborgfx one."""
        cfg = open(BASEPORT_CFG, errors='replace').read()
        lines = [l for l in cfg.splitlines() if re.search(r'INIT_NUMERIC_CONSTANT\s+InitialMMUCR', l)]
        self.assertTrue(lines, 'syborg/baseport config.inc sets no InitialMMUCR (patch qemu-baseport 0026)')
        for l in lines:
            self.assertNotIn('MMUCR_A', l.split(',', 1)[1], l)
            self.assertIn('MMUCR_U', l.split(',', 1)[1], l)

    @unittest.skipUnless(os.path.exists(os.path.join(D.paths(D.load('nokia-n95-8gb-rm320'))['dir'], 'rofs1.img')), 'no extracted firmware')
    def test_identify(self):
        facts, matches = D.identify(D.paths(D.load('nokia-n95-8gb-rm320'))['dir'])
        self.assertEqual(facts['sw']['type_code'], 'RM-320')
        self.assertEqual((matches[0][0], matches[0][1]), (0, 'nokia-n95-8gb-rm320'))
        self.assertTrue(all(n for n, d, _ in matches[1:]))        # the RM-321 profile does not match


    def test_user_data_layer(self):
        import tempfile
        self.assertEqual(D.size_bytes('64M'), 64 << 20)
        self.assertEqual(D.size_bytes('1G'), 1 << 30)
        p = D.load('nokia-n8-rm596-v111')
        self.assertTrue([f for f in D.uda_entries(p) if f.get('drive') == 'C'])   # inherited from the N8 base
        self.assertIsNone(D.uda_image(p, 'E'))
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, 'c.img')
            self.assertIn('blank', D.drive_image(p, 'nvmem', out, uda=False))
            self.assertEqual(os.path.getsize(out), D.size_bytes('64M'))
            src = D.uda_image(p, 'C')
            if os.path.exists(src):                     # extracted on this machine (firmware not in git)
                os.remove(out)
                self.assertIn('user data layer', D.drive_image(p, 'nvmem', out, uda=True))
                self.assertEqual(os.path.getsize(out), os.path.getsize(src))
                self.assertEqual(open(out, 'rb').read(512), open(src, 'rb').read(512))


class ExecMapTest(unittest.TestCase):
    """Bug 2026-11-13 (N81 stall at the splash): the firmware's RCriticalSection keeps its counter with
    User::LockedInc/LockedDec, which the N81's EUSER calls as *fast* execs 0x80000a/0x80000b. The sym92
    map had both slots as `!invalid`, our handlers returned 0 without touching the counter, the counter
    never moved and the critical section could be taken and never signalled - FbServ's two server
    threads deadlocked and the phone never drew its logo. tools/personality/check.py also counted an
    `!invalid` slot as implemented, which hid it."""

    @staticmethod
    def _map(personality):
        import subprocess
        out = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'personality', 'check.py'),
                              '--help'], capture_output=True, text=True)
        rows = {}
        path = os.path.join(ROOT, 'personalities', personality, 'exec.map')
        if not os.path.exists(path):
            return rows
        for line in open(path):
            m = re.match(r'^([FS])([0-9a-fA-F]+)\s+(\S+)', line)
            if m:
                rows[(m.group(1), int(m.group(2), 16))] = m.group(3)
        return rows

    def test_invalid_slots_are_not_implemented(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('pcheck', os.path.join(ROOT, 'tools/personality/check.py'))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        keys = mod.supported_execs(['F0a !invalid', 'F0b @LockedDec91', 'S10 @Foo'])
        self.assertEqual(keys, {('F', 0x0b), ('S', 0x10)})

    def test_locked_inc_dec_are_implemented(self):
        rows = self._map('sym92')
        for slot in (('F', 0x0a), ('F', 0x0b)):
            self.assertIn(slot, rows, 'F%02x unmapped: RCriticalSection counters stop moving' % slot[1])
            self.assertNotEqual(rows[slot], '!invalid', 'F%02x is a stub: RCriticalSection breaks' % slot[1])

    @unittest.skipUnless(os.path.exists(os.path.join(ROOT, 'build/roms')), 'no build tree')
    def test_every_swi_used_by_a_critical_section_export_is_mapped(self):
        """the general form of the bug: no export the firmware needs may fall in an unmapped slot"""
        for dev in D.device_ids():
            prof = D.load(dev)
            abi = os.path.join(D.paths(prof)['dir'], 'abi', 'n95-euser-swi.json')
            if not os.path.exists(abi):
                continue
            pers = D.get(prof, 'platform.personality')
            if not pers or not os.path.isdir(os.path.join(ROOT, 'personalities', pers)):
                continue
            rows = self._map(pers)
            if not rows:
                continue
            byname = json.load(open(abi))['byname']
            for name in ('_ZN16RCriticalSection4WaitEv', '_ZN16RCriticalSection6SignalEv',
                         '_ZN4User9LockedIncERi', '_ZN4User9LockedDecERi'):
                for swi in byname.get(name, []):
                    key = ('F', swi & 0x7fffff) if swi & 0x800000 else ('S', swi)
                    self.assertIn(key, rows, '%s uses %s, unmapped in personalities/%s' % (name, key, pers))
                    self.assertNotEqual(rows[key], '!invalid', '%s uses %s, which is a stub' % (name, key))


class RapidoSilentWithoutStreams(unittest.TestCase):
    """Bug 2026-11-17: every S60 3.2 / 5th edition phone (5230, 5800, 5730, N79, N86, E52, E72, E5-00, N97, ...)
    played nothing: aamdaifdriver binds a RAPIDO stream's ring to a syborg-snd slot only when the profile
    declares the stream's PCM format ([board.audio] streams), and only the N8/701/808 declared it."""

    def test_every_rapido_phone_declares_stream_formats(self):
        for dev in D.device_ids():
            prof = D.load(dev)
            if D.get(prof, 'board.audio.dsp', '') != 'rapido':
                continue
            with self.subTest(dev=dev):
                streams = D.get(prof, 'board.audio.streams', '')
                self.assertTrue(streams, 'no [board.audio] streams: the phone is silent')
                self.assertRegex(streams.split(',')[0], r'^\d+/[12]/(16|24|32)(/\d+)?$')
                self.assertIn('audio.streams=', ' '.join(D.qemu_args(prof)) if hasattr(D, 'qemu_args') else 'audio.streams=')


class Accelerometer5800(unittest.TestCase):
    """Bug 2026-11-17: Raging Thunder GR died (KERN-EXEC 3) on the 5800: no accelerometer channel, because the
    5800's accssy loads accelerometerldd.ldd / accelerometerpdd.pdd (absent) and speaks another protocol."""

    def test_rom_names_and_protocol(self):
        prof = D.load('nokia-5800-rm356')
        oby = D.standins_oby(prof)
        self.assertIn('device accelerometer ldd accelerometerldd', oby)
        self.assertIn('device accelerometer pdd accelerometerpdd', oby)
        acc = [s for s in prof['standins'] if s['name'] == 'accelerometer'][0]
        self.assertEqual(acc.get('config', {}).get('protocol'), 'pns')
        self.assertTrue(D.get(prof, 'board.accelerometer.info'))
        self.assertNotIn('accelerometerldd.ldd', D.get(prof, 'firmware.missing_drivers', []) or [])

    def test_n8_keeps_default_names(self):
        oby = D.standins_oby(D.load('nokia-n8-rm596'))
        self.assertIn('device accelerometer ldd', oby)


class InstanceTest(unittest.TestCase):
    """tools/instance.py: instances in a throw-away QEMBIAN_HOME, [overrides] through tools/device.py"""

    def setUp(self):
        import tempfile
        self.tmp = tempfile.mkdtemp()
        self.env = dict(os.environ)
        os.environ['QEMBIAN_HOME'] = self.tmp
        os.environ.pop('SYBORG_INSTANCE', None)
        os.environ.pop('SYBORG_DEVICE', None)
        import instance as I
        self.I = I

    def tearDown(self):
        import shutil
        os.environ.clear(); os.environ.update(self.env)
        shutil.rmtree(self.tmp)

    def run_i(self, *args):
        return self.I.main(list(args))

    def test_create_primary_and_overrides(self):
        I = self.I
        self.assertEqual(self.run_i('create', 'a', 'nokia-n8-rm596-delight'), 0)
        self.assertEqual(self.run_i('create', 'b', 'nokia-n8-rm596@111.040.1511~delight-6.7'), 0)
        self.assertEqual(I.device_id(I.read('b')), 'nokia-n8-rm596-delight')       # model@fw = the firmware's own id
        self.assertTrue(I.read('a')['primary']); self.assertFalse(I.read('b')['primary'])
        self.assertEqual(I.primary_for('nokia-n8-rm596-delight')['name'], 'a')
        self.assertEqual(self.run_i('set', 'b', 'primary', 'true'), 0)
        self.assertFalse(I.read('a')['primary'])                                   # one primary per device
        d = I.idir('b')
        with open(os.path.join(d, 'instance.toml'), 'a') as f:
            f.write('\n[overrides.board]\nram_mb = 200\n')
        os.environ['SYBORG_INSTANCE'] = d
        self.assertEqual(D.load()['_id'], 'nokia-n8-rm596-delight')                 # the instance names the device
        self.assertEqual(D.get(D.load(), 'board.ram_mb'), 200)
        self.assertNotEqual(D.get(D.load('nokia-n95-8gb-rm320'), 'board.ram_mb'), 200)   # another device: no overrides
        with open(os.path.join(d, 'instance.toml'), 'a') as f:
            f.write('\n[overrides.rom]\nx = 1\n')
        with self.assertRaises(D.DeviceError):
            D.load()                                                                # composed tables are refused

    def test_firmware_change_needs_a_drive_decision(self):
        self.assertEqual(self.run_i('create', 'n8', 'nokia-n8-rm596'), 0)
        self.assertEqual(self.run_i('set', 'n8', 'firmware', '013.016'), 2)
        self.assertEqual(self.run_i('set', 'n8', 'firmware', '013.016', '--wipe-drives'), 0)
        self.assertEqual(self.I.device_id(self.I.read('n8')), 'nokia-n8-rm596-v13')
        self.assertEqual(open(os.path.join(self.I.idir('n8'), 'device')).read().strip(), 'nokia-n8-rm596-v13')

    def test_clone_rename_delete(self):
        self.assertEqual(self.run_i('create', 'x', 'nokia-n95-8gb-rm320', '--variant', 'audio-exp'), 0)
        os.makedirs(os.path.join(self.I.idir('x'), 'drives'))
        open(os.path.join(self.I.idir('x'), 'drives', 'nvmemory.img'), 'wb').write(b'C' * 4096)
        self.assertEqual(self.run_i('clone', 'x', 'y'), 0)
        self.assertEqual(open(os.path.join(self.I.idir('y'), 'drives', 'nvmemory.img'), 'rb').read(), b'C' * 4096)
        self.assertTrue(self.I.rom(self.I.read('y')).endswith('hybrid-exp.img'))
        self.assertEqual(self.run_i('rename', 'y', 'z'), 0)
        self.assertEqual(self.I.read('z')['name'], 'z')
        self.assertEqual(self.run_i('delete', 'z'), 2)                               # needs --yes
        self.assertEqual(self.run_i('delete', 'z', '--yes'), 0)
        self.assertEqual(self.run_i('create', 'x', 'nokia-n95-8gb-rm320', '--replace'), 2)   # not ephemeral



@unittest.skipUnless(__import__('shutil').which('mformat') and __import__('shutil').which('mtype'), 'needs mtools')
class DriveFiles(unittest.TestCase):
    """[[board.drive_files]]: files a new drive gets on top of the firmware's user data image. The N95's
    C:\\Private\\10205054\\runtimefeatures.txt turns WLAN (feature 109) off: with it on and no WLAN hardware,
    WlanServer exited and IPWServer panicked (bug 2026-11-21, linked runs with the modem firmware)."""

    def test_n95_runtime_features_on_a_new_drive(self):
        import subprocess, tempfile
        prof = D.load('nokia-n95-8gb-rm320')
        entries = [f for f in D.get(prof, 'board.drive_files', []) if f['path'].endswith('runtimefeatures.txt')]
        self.assertEqual(len(entries), 1)
        img = os.path.join(tempfile.mkdtemp(), 'c.img')
        with open(img, 'wb') as f:
            f.truncate(8 << 20)
        subprocess.run(['mformat', '-i', img, '::'], check=True, capture_output=True)
        self.assertEqual(D.add_drive_files(prof, 'C', img), ['Private/10205054/runtimefeatures.txt'])
        data = subprocess.run(['mtype', '-i', img, '::/Private/10205054/runtimefeatures.txt'],
                              capture_output=True, check=True).stdout
        self.assertEqual(data.decode('utf-16le').split(), ['109', '0'])
        self.assertEqual(D.add_drive_files(prof, 'E', img), [])      # nothing declared for E:

    def test_drive_made_before_the_file_was_declared_gets_it(self):
        # bug (2026-11-22): drive files were only written when a drive image was created, so the owner's N95 from
        # 2026-11-10 kept WLAN on; with its own modem firmware IPWServer panicked and the Telephone app crashed at
        # every boot. sydb start now adds missing ones (missing_only), and leaves an existing file as it is
        import subprocess, tempfile
        prof = D.load('nokia-n95-8gb-rm320')
        img = os.path.join(tempfile.mkdtemp(), 'c.img')
        with open(img, 'wb') as f:
            f.truncate(8 << 20)
        subprocess.run(['mformat', '-i', img, '::'], check=True, capture_output=True)
        self.assertEqual(D.add_drive_files(prof, 'C', img, missing_only=True),
                         ['Private/10205054/runtimefeatures.txt'])
        self.assertEqual(D.add_drive_files(prof, 'C', img, missing_only=True), [])     # there now: left alone
        mine = os.path.join(os.path.dirname(img), 'mine.txt')
        open(mine, 'wb').write('109 1\r\n'.encode('utf-16le'))
        subprocess.run(['mcopy', '-o', '-i', img, mine, '::/Private/10205054/runtimefeatures.txt'], check=True)
        self.assertEqual(D.add_drive_files(prof, 'C', img, missing_only=True), [])     # a user's own file kept
        data = subprocess.run(['mtype', '-i', img, '::/Private/10205054/runtimefeatures.txt'],
                              capture_output=True, check=True).stdout
        self.assertEqual(data.decode('utf-16le').split(), ['109', '1'])



class ModemFirmwareFile(unittest.TestCase):
    """--modem real: the modem firmware comes from the device's own core flash file (bug 2026-11-22: tools/cmt/nor.py
    read a hard-wired roms/RM-320/35.0.001 C0R, so any other firmware's phone side would have run with that modem)"""

    def test_core_file_of_the_device_firmware(self):
        prof = D.load('nokia-n95-8gb-rm320')
        store = D.paths_v2(prof)['store']
        if not os.path.isdir(store):
            self.skipTest('firmware not in the ROM store')
        f = D.firmware_file(prof, 'core')
        self.assertEqual(os.path.dirname(f), store)
        self.assertTrue(f.endswith('.C0R'))

    def test_unknown_role_is_an_error(self):
        with self.assertRaises(D.DeviceError):
            D.firmware_file(D.load('nokia-n95-8gb-rm320'), 'no-such-role')

    def test_modem_needs_modem_cpu_in_the_profile(self):
        import sys
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'cmt'))
        import nor
        with self.assertRaises(SystemExit) as e:
            nor.device_c0r('nokia-n8-rm596')
        self.assertIn('[modem_cpu]', str(e.exception))

if __name__ == '__main__':
    unittest.main()

