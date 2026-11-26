"""Checks on the N-Gage (NEM-4) board and the N-Gage / N-Gage QD split that need no emulator run.

-M ngage is the first N-Gage (NEM-4, ngage.c ngage_nem4, nos_standin.S built with its ekern addresses);
-M ngageqd is the N-Gage QD (RH-29, ngage_rh29). Each value of ngage_nem4 is re-read here from the NEM-4 ROM
the way it was found (docs/devices/nokia-ngage-nem4/bring-up.md), and the QD's known values are read the same
way from its ROM, so the method itself is checked too.

Run: python3 tests/ngage_nem4_board_test.py
ROM checks are skipped when the (git-ignored) extractions are missing (eka1/README.md).
"""
import importlib.util
import json
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
NEM4 = REPO / 'eka1/nem4/output/rom'
RH29 = REPO / 'eka1/rh29/output/rom'
B = 0x50000000
ORDINALS = ('KERN_VARIANT', 469), ('SET_POWER_MODEL', 394), ('POWER_MODEL_CTOR', 493), ('KERN_ADD_DFC', 7), \
           ('THREAD_WRITE', 479), ('CHANNEL_COMPLETE', 64)


def model(name):
    t = (HW / 'ngage.c').read_text()
    return t.split('static const NgageModel %s = {' % name, 1)[1].split('\n};\n', 1)[0]


def build_line(prefix):
    """the build.sh call for a stub: (NOS entry, {defsym: value})"""
    t = (REPO / 'src/qemu-ngage/nos-standin/build.sh').read_text().replace('\\\n', ' ')
    m = re.search(r'^build nos_standin\.S\s+(0x[0-9a-f]+)\s+%s\s+(.*)$' % prefix, t, re.M)
    return int(m.group(1), 16), {k: int(v, 16) for k, v in re.findall(r'--defsym (\w+)=(0x[0-9a-f]+)', m.group(2))}


class Rom:
    def __init__(self, d):
        self.rom = (d / 'ROM.bin').read_bytes()
        self.files = {f['path'].split('/')[-1].lower(): f for f in json.loads((d / 'manifest.json').read_text())['files']}
        self.blocks = json.loads((d / 'manifest.json').read_text())['mapping']

    def va(self, name):
        return self.files[name]['rom_virtual_address']

    def word(self, va):
        return struct.unpack_from('<I', self.rom, va - B)[0]

    def export(self, module, ordinal):
        h = struct.unpack_from('<26I', self.rom, self.va(module) - B)   # TRomImageHeader: +15 count, +16 export dir
        self.n_exports = h[15]
        return self.word(h[16] + 4 * (ordinal - 1))

    def nosstarter(self):
        """(NOS entry, NOS bss): the literal pool after nosstarter's code (header 26 words, then 16 code words)"""
        lit = self.va('nosstarter.ext') + 26 * 4 + 16 * 4
        return self.word(lit), self.word(lit + 16)

    def nos_imports(self):
        """nosdriver.ldd's import slots just before its image that hold NOS (0x514xxxxx) addresses: open, ctor"""
        nd = self.va('nosdriver.ldd')
        return [self.word(a) for a in range(nd - 0x100, nd, 4) if 0x51400000 <= self.word(a) < 0x51800000]


@unittest.skipUnless((RH29 / 'ROM.bin').exists(), 'QD ROM not extracted in eka1/rh29')
class MethodOnQd(unittest.TestCase):
    """The QD's values in ngage.c / build.sh come out of the same reads: the method holds."""

    def test_qd_values_from_rom(self):
        r = Rom(RH29)
        self.assertEqual(r.export('ekern.exe', 469), 0x5032e7ac)
        self.assertEqual(r.export('ekern.exe', 64), 0x5032f0c4)
        self.assertEqual(r.nosstarter(), (0x5149e4c4, 0x8000c378))
        self.assertEqual(r.nos_imports(), [0x514a1c90, 0x514a260c])
        self.assertEqual(build_line('ngage')[0], 0x5149e4c4)


@unittest.skipUnless((NEM4 / 'ROM.bin').exists(), 'NEM-4 ROM not extracted in eka1/nem4')
class Nem4Values(unittest.TestCase):
    def setUp(self):
        self.r = Rom(NEM4)
        self.m = model('ngage_nem4')

    def test_stub_ekern_addresses(self):
        """the stub calls these kernel functions: a wrong one is a jump into the wrong code at NOS start"""
        entry, sym = build_line('ngage_nem4')
        for name, ordinal in ORDINALS:
            self.assertEqual(sym[name], self.r.export('ekern.exe', ordinal), name)
        self.assertEqual(self.r.n_exports, 506)                 # the QD's numbering

    def test_nos_entry_and_bss(self):
        entry, sym = build_line('ngage_nem4')
        self.assertEqual((entry, sym['RAM']), self.r.nosstarter())
        ram = sym['RAM']
        self.assertIn('.nos_rx_dfc = 0x%08x + 0x200' % ram, self.m)
        self.assertIn('.nos_rx_flag = 0x%08x + 0x1f8' % ram, self.m)
        hdr = (HW / 'ngage_nem4_nos_stub.h').read_text()
        self.assertIn('#define NGAGE_NEM4_NOS_STUB_VA 0x%x' % entry, hdr)

    def test_nosdriver_imports(self):
        opn, ctor = self.r.nos_imports()
        t = (HW / 'ngage.c').read_text().split('ngage_nem4_nos_exports[][2] = {', 1)[1].split('};', 1)[0]
        self.assertIn('{ 0x%08x, NGAGE_NEM4_NOS_SYM_nos_channel_ctor }' % ctor, t)
        self.assertIn('{ 0x%08x, NGAGE_NEM4_NOS_SYM_nos_channel_open }' % opn, t)

    def test_rom_map_matches_flash_blocks(self):
        """ROM.bin over three chips as the MCU file's blocks place it (chip n at n << 25)"""
        got = re.search(r'\.rom_map = \{ (.*?) \},\n', self.m).group(1)
        got = [tuple(int(v, 0) for v in g.split(',')) for g in re.findall(r'\{ ([^}]*) \}', got)]
        flash = {}
        for b in self.r.blocks:
            if b['flash_length'] > 0x1000:
                chip = b['flash_address'] >> 25
                start = b['rom_offset'] & ~0xfff
                flash.setdefault(chip, start)
        self.assertEqual([(o, c) for o, _, c in got], sorted((o, c) for c, o in flash.items()))
        last = got[-1]
        self.assertEqual(last[0] + last[1], len(self.r.rom))
        self.assertIn('.nchips = 3', self.m)

    def test_md5_entries_at_qd_offsets(self):
        """the bootstrap digests: the QD's offsets (md5_entries unset = 0x8F00 / 0x8F18)"""
        start, end = struct.unpack_from('<2I', self.r.rom, 0x8f18)
        self.assertEqual(start, 0x50009000)
        self.assertLess(end - B, len(self.r.rom))
        self.assertNotIn('md5_entries', self.m)

    def test_kernel_data_layout_as_qd(self):
        """screen chunk word, current thread and containers: the QD's addresses, referenced as often"""
        q = Rom(RH29) if (RH29 / 'ROM.bin').exists() else None
        for a in (0x800066ec, 0x80006740, 0x80000398):
            n = self.r.rom.count(struct.pack('<I', a))
            self.assertGreater(n, 0)
            if q:
                self.assertEqual(n, q.rom.count(struct.pack('<I', a)), hex(a))
        self.assertIn('.screen_chunk_ptr = 0x800066ec', self.m)


SCAN = {'left soft': 0xa4, 'menu': 0xb4, 'clear': 0x01, '0xB6': 0xb6, 'right soft': 0xa5, '*': 0x2a, '#': 0x7f,
        'edit': 0x12, 'call': 0xc4, 'end': 0xc5, **{str(d): 0x30 + d for d in range(10)}}


def nem4_keys():
    t = (HW / 'ngage_keys.c').read_text().split('static const NgageKey nem4_keys[] = {', 1)[1].split('};', 1)[0]
    return [(n, int(r), int(c)) for n, r, c in re.findall(r'\{ KEY_\w+, "([^"]+)", (-?\d+), (-?\d+) \}', t)]


class Nem4Keypad(unittest.TestCase):
    def test_board_uses_nem4_layout(self):
        """-M ngage reads the joystick port (0x0C15020A); unmapped it read 0 = every line held: the 6600's plumbing"""
        t = (HW / 'ngage.c').read_text()
        self.assertIn('ngm == &ngage_nem4 ? NGAGE_KPD_NEM4', t)
        k = (HW / 'ngage_keys.c').read_text()
        # the whole kpd.nhl10 assignment (it may span lines and name other layouts): NEM-4 is in it
        m = re.search(r'kpd\.nhl10\s*=([^;]*);', k)
        self.assertIsNotNone(m)
        self.assertIn('layout == NGAGE_KPD_NEM4', re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S))

    @unittest.skipUnless((NEM4 / 'ROM.bin').exists(), 'NEM-4 ROM not extracted in eka1/nem4')
    def test_matrix_positions_from_ekeyb(self):
        """each matrix key where ekeyb's sub-matrix lookup (0x50419D14) finds its scan code"""
        r = Rom(NEM4)
        lists = {1: r.word(0x50419c90), 2: r.word(0x50419c94)}        # the scan routine's literal pool
        self.assertEqual(lists[1], r.va('ekeyb.dll') + 0x233c)            # the key list's start
        keys = nem4_keys()
        self.assertEqual(len([k for k in keys if k[1] >= 0]), 20)
        for name, row, col in keys:
            if row < 0:
                continue
            g = 1 if row < 2 else 2
            bit, line = (row, col) if g == 1 else (row - 2, col - 2)
            self.assertTrue(0 <= line < (1 << g), name)
            scan = r.rom[lists[g] + 4 * (line + (bit << g)) - B]
            self.assertEqual(scan, SCAN[name], name)


class MachineLinks(unittest.TestCase):
    """ngage = N-Gage (NEM-4), ngageqd = N-Gage QD (RH-29): machine, model and files stay paired."""

    def test_machines(self):
        t = (HW / 'ngage.c').read_text()
        self.assertIn('.name = "ngage",', model('ngage_nem4'))
        self.assertIn('.name = "ngageqd",', model('ngage_rh29'))
        self.assertIn('mc->init = ngage_init_nem4;', t.split('static void ngage_machine_class_init', 1)[1]
                      .split('\n}\n', 1)[0])
        self.assertIn('MACHINE_TYPE_NAME("ngageqd")', t)
        self.assertIn('mc->init = ngage_init_rh29;', t.split('static void ngage_rh29_class_init', 1)[1]
                      .split('\n}\n', 1)[0])

    def test_tools(self):
        spec = importlib.util.spec_from_file_location('nr', REPO / 'eka1/tools/ngage_run.py')
        nr = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(nr)
        ng, qd = nr.MODELS['ngage'], nr.MODELS['ngageqd']
        self.assertEqual((ng['machine'], qd['machine']), ('ngage', 'ngageqd'))
        self.assertEqual(ng['root'], REPO / 'eka1/nem4')
        self.assertEqual(qd['root'], REPO / 'eka1/rh29')
        self.assertIn('/eka1/nem4/', str(ng['rom']) + str(ng['ppm']))
        self.assertTrue(str(qd['rom']).startswith(str(REPO / 'eka1/rh29/')))
        self.assertTrue(str(qd['ppm']).startswith(str(REPO / 'eka1/rh29/')))


def known_set(addr):
    """the 'set' bits ngage.c's ngage_known table forces on for this register word (0 = not listed)"""
    t = (HW / 'ngage.c').read_text().split('} ngage_known[] = {', 1)[1].split('\n};\n', 1)[0]
    bits = 0
    for a, s in re.findall(r'\{\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),', t):
        if int(a, 16) == addr:
            bits |= int(s, 16)
    return bits


class Nem4UartTxEmptyFreeze(unittest.TestCase):
    """Bug (2026-11-26): the NEM-4 froze after start-up (kept flash: before SysAp's UI). euart1.pdd spun in
    `while (*0x800004A8 && !(*0x0C150514 & 4))` with no timeout, because the catch-all register read back 0.
    Fix H-nem4-uart-txempty: the board reports bit 2 (transmitter empty) set."""

    def test_board_forces_tx_empty(self):
        self.assertEqual(known_set(0x0c150514) & 4, 4)

    @unittest.skipUnless((NEM4 / 'ROM.bin').exists(), 'NEM-4 ROM not extracted in eka1/nem4')
    def test_nem4_euart1_waits_on_bit2(self):
        r = Rom(NEM4)
        f = r.files['euart1.pdd']
        va, size = f['rom_virtual_address'], f['size']
        self.assertEqual(va, 0x510e8370)
        lits = [a for a in range(va, va + size, 4) if r.word(a) == 0x00150514]
        self.assertEqual(len(lits), 4)
        # the wait loop at 0x510E8A0C: ldr r0, =0x00150514; bl read16; tst r0, #4; beq retry
        self.assertEqual(r.word(0x510e8a0c), 0xe59f0050)        # ldr r0, [pc, #80] -> 0x510E8A64
        self.assertEqual(r.word(0x510e8a64), 0x00150514)
        self.assertEqual(r.word(0x510e8a14), 0xe3100004)        # tst r0, #4
        self.assertEqual(r.word(0x510e8a18), 0x0afffff8)        # beq: no timeout but *0x800004A8

    @unittest.skipUnless((RH29 / 'ROM.bin').exists(), 'QD ROM not extracted in eka1/rh29')
    def test_qd_euart1_bounds_the_same_wait(self):
        """why only the NEM-4 froze: the QD's newer euart1.pdd polls the same bit but gives up after 1000 tries"""
        r = Rom(RH29)
        self.assertEqual(r.files['euart1.pdd']['rom_virtual_address'], 0x503f30c0)
        self.assertEqual(r.word(0x503f345c), 0xe3a04ffa)        # mov r4, #1000
        self.assertEqual(r.word(0x503f3464), 0xe2544001)        # subs r4, r4, #1
        self.assertEqual(r.word(0x503f3468), 0x0a000007)        # beq out
        self.assertEqual(r.word(0x503f353c), 0x00150514)
        self.assertEqual(r.word(0x503f3478), 0xe3100004)        # tst r0, #4


class FlashFiqNotHostTimerBound(unittest.TestCase):
    """Bug (2026-11-26): medint.pdd programs one halfword per timer-3 FIQ (period 0x186-0x1FE ticks, 11-15 us), but each
    period waited for a QEMU host timer (~200 us late), so flash writes ran ~20x too slow (NEM-4 fresh boot 252 s,
    ViewSrv 11 restarts suspected). H-ngage-flash-fiq: short timer-3 periods elapse at the start (+10) and at each
    acknowledgement (+08)."""

    src = (HW / 'ngage.c').read_text()

    def fast_fn(self):
        return self.src.split('static bool ngage_timer_fast(NgageTimer *s)', 1)[1].split('\n}\n', 1)[0]

    def test_threshold_covers_medint_not_the_tick(self):
        ns = int(re.search(r'#define NGAGE_TIMER_FAST_NS (\d+)', self.src).group(1))
        hz = 34666667
        self.assertLess(0x1fe * 1e9 / hz, ns)          # medint's longest flash period: 14.7 us
        self.assertGreater(0x843e2 * 1e9 / hz, ns)      # the kernel tick (timer 0): 15.6 ms
        f = self.fast_fn()
        self.assertIn('s->index == 3', f)               # only medint's timer
        self.assertIn('== 2', f)                        # only periodic runs

    def test_ack_and_start_raise_at_once(self):
        w = self.src.split('static void ngage_timer_write', 1)[1].split('\n}\n', 1)[0]
        ack = w.split('case 0x08:', 1)[1].split('break;', 1)[0]
        self.assertIn('ngage_timer_fast(s)', ack)
        self.assertIn('qemu_set_irq(s->irq, 1)', ack)
        start = w.split('case 0x10:', 1)[1].split('break;', 1)[0]
        self.assertIn('ngage_timer_fast(s)', start)
        self.assertIn('qemu_set_irq(s->irq, 1)', start)
        e = self.src.split('static void ngage_timer_expire', 1)[1].split('\n}\n', 1)[0]
        self.assertLess(e.index('ngage_timer_fast(s)'), e.index('ngage_timer_arm('))   # no host timer re-armed


class TimerZeroCountExpires(unittest.TestCase):
    """one_shot_zero_count_never_fired (2026-11-26, 6260 infrared): a count of 0 written to a one-shot timer stopped it.
    The kernel's one-shot queue loads timer 1 with the head's delta unclamped when the queue was empty (ekern
    0x503414d4 -> 0x50341428 on the 6260); ecomm.ldd's IrDA turnaround timer has a delta of 0, so it never expired
    and the write it gates never reached the SIR UART. H-ngage-timer-zero: mode 1 + count 0 expires at once."""

    src = (HW / 'ngage.c').read_text()

    def test_zero_count_in_one_shot_mode_arms(self):
        w = self.src.split('static void ngage_timer_write', 1)[1].split('\n}\n', 1)[0]
        start = w.split('case 0x10:', 1)[1].split('break;', 1)[0]
        self.assertRegex(start, r'(?s)else if \(s->regs\[0x04 >> 2\] == 1\) \{.*?ngage_timer_arm\(s, 0\);')
        # stopped (mode 0) or periodic timers loaded with 0 still stop
        self.assertLess(start.index('ngage_timer_arm(s, 0)'), start.rindex('timer_del(s->t)'))


if __name__ == '__main__':
    unittest.main()
