"""Checks on the Nokia N70 board (-M nokian70: src/qemu-ngage/hw/arm/nokia9500.c rm1_board_init + rm1_patches_n70,
the 6630's nokia6630_soc.c devices, src/qemu-ngage/isi-replies-rm84.txt) that need no emulator run. One regression
test per stall fixed (docs/devices/nokia-n70-rm84/bring-up.md), named after it.

Run: python3 tests/nokian70_board_test.py
The ROM checks are skipped when the (git-ignored) inflated core ROM eka1/rm84/core/ROM.bin is not there.
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9500.c'
SOC = REPO / 'src/qemu-ngage/hw/arm/nokia6630_soc.c'
ISI = REPO / 'src/qemu-ngage/hw/arm/ngage_isi.c'
REPLIES = REPO / 'src/qemu-ngage/isi-replies-rm84.txt'
ROM = REPO / 'eka1/rm84/core/ROM.bin'
NEED_ROM = unittest.skipUnless(ROM.exists(), 'N70 core ROM not inflated')


def func(path, name):
    return path.read_text().split(name, 1)[1].split('\n}\n', 1)[0]


def rom_word(va):
    return struct.unpack_from('<I', ROM.read_bytes(), va - 0x50000000)[0]


def reply(res, req):
    """The reply-table line for (res, req), without its comment, as tokens."""
    for line in REPLIES.read_text().splitlines():
        t = line.split('#', 1)[0].split()
        if len(t) > 3 and t[0] == 'reply' and int(t[1], 16) == res and int(t[2], 16) == req:
            return t[3:]
    return None


class Machine(unittest.TestCase):
    """Stall 1: -M nokian70 is the 6630 board with the N70's phone description."""

    def test_registered(self):
        s = BOARD.read_text()
        self.assertIn('MACHINE_TYPE_NAME("nokian70")', s)
        self.assertIn('type_register_static(&nokian70_type);', s)
        self.assertRegex(s, r'rm1_phone_n70 = \{ "nokian70", "eka1/rm84/core", rm1_patches_n70, true \}')

    def test_6630_unchanged_phone(self):
        self.assertRegex(BOARD.read_text(),
                         r'rm1_phone_6630 = \{ "nokia6630", "eka1/rm1/core", rm1_patches_6630, false \}')

    def test_tools_know_the_model(self):
        run = (REPO / 'eka1/tools/ngage_run.py').read_text()
        m = run.split("'nokian70': dict(", 1)[1].split('ttb=', 1)[0]
        self.assertIn('fatal=0x5001fa8c', m)
        self.assertIn('cur_thread=0x8000295c', m)
        self.assertIn('objstride=2', m)
        self.assertIn('sdram=(0x10000000, 0x08000000)', m)


class Rm84BootstrapRam(unittest.TestCase):
    """Stall 2 (rm84_bootstrap_restarts): the bootstrap's bank is 64 MB at 0x10100000; with 64 MB from 0x10000000
    its stack fell off the RAM and it restarted for ever."""

    def test_ram_covers_the_bank(self):
        body = func(BOARD, 'static void nokian70_class_init(ObjectClass *oc, const void *data)')
        mb = int(re.search(r'default_ram_size = (\d+) \* MiB', body).group(1))
        self.assertGreaterEqual(0x10000000 + mb * 0x100000, 0x10100000 + 0x04000000)

    @NEED_ROM
    def test_rom_bank_table(self):
        b = ROM.read_bytes()
        self.assertEqual(struct.unpack_from('<2I', b, 0xd20), (0x10100000, 0x04000000))


class Rm84ApWakeLatch(unittest.TestCase):
    """Stall 4 (rm84_no_hello): the N70's ssi.pdd sets AP wake (GPIO4 bit 3) without making it an output; the
    modem's hook must see the output latch, not latch & ~direction."""

    def test_hook_gets_the_latch(self):
        body = func(SOC, 'static void gpio_write(')
        self.assertIn('rm1_gpio_hook[g->index](rm1_gpio_hook_opaque[g->index], g->out);', body)
        self.assertNotIn('g->out & ~g->dir', body)

    def test_ap_wake_is_bit3(self):
        self.assertIn('(out & 8) && !s->ctrl_sent', func(SOC, 'static void ssi_ap_wake('))


class Rm84IscConfig(unittest.TestCase):
    """Stall 5 (rm84_isc_config): the multiplexer's 00 05 must be answered with 06 {max size, peer object, byte},
    both u16 non-zero."""

    def test_reply(self):
        t = reply(0x00, 0x05)
        self.assertEqual(t[0].lower(), '06')
        d = [int(x, 16) for x in t[1:6]]
        self.assertNotEqual(d[0] << 8 | d[1], 0)          # max message size
        self.assertNotEqual(d[2] << 8 | d[3], 0)          # the ISC peer object
        self.assertEqual(d[2] << 8 | d[3], 0x01)


class Rm84IscChannelOpen(unittest.TestCase):
    """Stall 6 (rm84_estart_waits_channel_open): the channel open 00 01 {channel, 00} must be answered as an ISC
    control message (media 0, from the peer object) 02 {channel, object}; the 6630's CheckCrc {00 1D} named
    channel 0 and EStart waited for ever."""

    def test_reply(self):
        t = reply(0x00, 0x01)
        self.assertEqual(t[:3], ['02', 'rq10', 'rq10'])
        self.assertIn('media=00', t)
        self.assertIn('sobj=01', t)            # = the peer object of the 06 reply

    def test_reply_table_tokens(self):
        s = ISI.read_text()
        for tok in ('"rq10"', '"media=', '"sobj='):
            self.assertIn(tok, s)
        enq = func(ISI, 'const NgageIsiReply *r)\n{')
        self.assertIn('m->b[10 + r->rq10[i]] = reqlen > 10 ? req[10] : 0;', enq)
        self.assertIn('m->b[0] = r->media;', enq)
        self.assertIn('m->b[7] = r->sobj;', enq)

    def test_defaults_mirror(self):
        """Every other reply and indication keeps mirroring the request's media and objects (-1 = mirror)."""
        s = ISI.read_text()
        self.assertEqual(len(re.findall(r'\.media = -1, \.sobj = -1', s)), 3)


class Rm84SecMonOmap1710(unittest.TestCase):
    """Stall 7 (rm84_secmon_b_dot): SecPdd's monitor calls the secure ROM only when 0xFFFED400 reads 0x0333xx00;
    QEMU's OMAP310 value sent it into `b .`. The N70 board reports the OMAP1710's IDs."""

    def test_ids(self):
        body = func(BOARD, 'static uint64_t rm1_id_read(')
        self.assertIn('case 0xfffe2004: v = 0x0000b5f7;', body)
        self.assertIn('case 0xfffed400: v = 0x03330000;', body)
        self.assertIn('if (ph->omap1710) {', func(BOARD, 'static void rm1_board_init('))

    @NEED_ROM
    def test_rom_monitor_accepts_the_value(self):
        self.assertEqual(rom_word(0x500a997c), 0xfffed400)
        self.assertIn(0x03330000, [rom_word(0x500a9980), rom_word(0x500a9984), rom_word(0x500a9988)])
        self.assertEqual(rom_word(0x500a9994), 0x3c00)      # then the secure ROM entry (H-rm1-secrom)

    @NEED_ROM
    def test_rom_kernel_types_the_1710(self):
        self.assertEqual(rom_word(0x5000cc14), 0x580e2004)
        self.assertEqual(rom_word(0x5000cc18), 0xb5f7)


class Rm84CorePatches(unittest.TestCase):
    """Stall 8 (rm84_dsp_boot_spin and the 6630's DSP/secure stalls): each N70 patch with its old word, and the
    old words are the ROM's."""

    PATCHES = re.compile(r'rm1_patch\(core, len, "(H-rm84-[a-z-]+)", (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\);')

    def patches(self):
        return [(h, int(v, 16), int(o, 16), int(n, 16))
                for h, v, o, n in self.PATCHES.findall(func(BOARD, 'static void rm1_patches_n70('))]

    def test_all_six_hacks(self):
        self.assertEqual({p[0] for p in self.patches()},
                         {'H-rm84-dsp-boot', 'H-rm84-dsp-timeout', 'H-rm84-bal-run', 'H-rm84-eapgwc',
                          'H-rm84-secenvinit', 'H-rm84-drmhelper'})

    @NEED_ROM
    def test_old_words_in_rom(self):
        for hack, va, old, new in self.patches():
            self.assertEqual(rom_word(va), old, '%s %08x' % (hack, va))

    @NEED_ROM
    def test_dsp_boot_loop(self):
        """the N70 spun at 0x504fe188..0x504fe194; the patched word is the loop's backward branch"""
        self.assertEqual(rom_word(0x504fe194), 0x1afffffb)       # bne 0x504fe188


class Rm84AscSubBlock(unittest.TestCase):
    """Stall 9 (rm84_dms_user21_asc): the 1C 2E reply needs sub-block 01 (6 bytes) at frame offset 16, its count at 15."""

    def test_reply_frame(self):
        t = reply(0x1c, 0x2d)
        d = [int(x, 16) for x in t[1:]]
        frame = [0] * 10 + d                 # data starts at frame byte 10
        self.assertIn(frame[10], (1, 2, 7))
        self.assertEqual(frame[15], 1)       # one sub-block
        self.assertEqual(frame[16], 0x01)    # id 01
        self.assertEqual(frame[17], 6)       # total length: id, length, two u16
        self.assertGreaterEqual(len(frame), 16 + 6)


class Rm84SimReadField76(unittest.TestCase):
    """Stall 10 (rm84_ctelserver_user22_sim): 09 31 {3B ..} needs status 01 and a 76-byte frame (Mid(16,60))."""

    def test_reply(self):
        t = reply(0x09, 0x31)
        self.assertEqual(t[0], '32')
        n = int(next(x for x in t if x.startswith('len='))[4:])
        self.assertGreaterEqual(10 + n, 76)
        data = [x for x in t[1:] if not x.startswith('len=')]
        self.assertEqual(data[:2], ['3B', '01'])


class Rm84Keypad(unittest.TestCase):
    """The N70's key list (RM84_KEYS): each (row, column) gives its scan code in the N70's ekeyp table, and the
    board picks the N70 list (the 6630's table has no keys at row 4/5 column 2)."""

    def entries(self):
        t = (REPO / 'src/qemu-ngage/hw/arm/nokia6630_keys.h').read_text().split('#define RM84_KEYS', 1)[1]
        return [(k, int(r), int(c), int(sc, 16)) for k, r, c, sc in
                re.findall(r'\{ Q\((\w+)\), (\d+), (\d+), (0x[0-9a-f]+) \}', t)]

    def test_wired(self):
        self.assertIn('rm1_keys = n70 ? rm1_keys_n70 : rm1_keys_6630;', SOC.read_text())
        self.assertIn('rm1_keys_init(mpu, ph->omap1710);', func(BOARD, 'static void rm1_board_init('))

    def test_menu_and_extras(self):
        e = self.entries()
        self.assertIn(('F5', 6, 3, 0xb4), e)
        self.assertIn(('F10', 4, 2, 0xe3), e)
        self.assertIn(('F11', 5, 2, 0xba), e)
        w = (REPO / 'eka1/tools/ngage_phone.py').read_text()
        self.assertIn("'nokian70': RM84_BUTTONS", w)
        self.assertIn("'nokian70': 'Nokia N70'", w)

    @NEED_ROM
    def test_scan_codes_match_the_rom(self):
        rom = ROM.read_bytes()
        table = rom_word(0x50085f0c) - 0x50000000            # literal: the scan-code table 0x50086F3A
        for k, r, c, sc in self.entries():
            self.assertEqual(rom[table + c + r * 4], sc, 'KEY %s (row %d, col %d)' % (k, r, c))

    @NEED_ROM
    def test_column_words(self):
        cols = rom_word(0x50085de8) - 0x50000000
        self.assertEqual(struct.unpack_from('<4H', ROM.read_bytes(), cols), (0x7e, 0x7d, 0x7b, 0x77))


if __name__ == '__main__':
    unittest.main()
