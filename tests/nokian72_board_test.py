"""Checks on the Nokia N72 board (-M nokian72: src/qemu-ngage/hw/arm/nokia9500.c rm1_board_init + rm1_patches_n72,
the 6630's nokia6630_soc.c devices, src/qemu-ngage/isi-replies-rm180.txt) that need no emulator run
(docs/devices/nokia-n72-rm180/bring-up.md). The N72 is the N70 relinked: the N70's machine, keys and replies, its
core patches moved to RM-180's addresses.

Run: python3 tests/nokian72_board_test.py
The ROM checks are skipped when the (git-ignored) inflated core ROM eka1/rm180/core/ROM.bin is not there.
"""
import json
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9500.c'
ROM = REPO / 'eka1/rm180/core/ROM.bin'
FILES = REPO / 'eka1/rm180/core/files.json'
N70_ROM = REPO / 'eka1/rm84/core/ROM.bin'
NEED_ROM = unittest.skipUnless(ROM.exists(), 'N72 core ROM not inflated')
NEED_BOTH = unittest.skipUnless(ROM.exists() and N70_ROM.exists(), 'N72 or N70 core ROM not inflated')


def func(path, name):
    return path.read_text().split(name, 1)[1].split('\n}\n', 1)[0]


def word(rom, va):
    return struct.unpack_from('<I', rom.read_bytes(), va - 0x50000000)[0]


class Machine(unittest.TestCase):
    """-M nokian72 is the 6630 board with the N70's settings (OMAP1710 IDs, 176x208 panel, N70 keys)."""

    def test_registered(self):
        s = BOARD.read_text()
        self.assertIn('MACHINE_TYPE_NAME("nokian72")', s)
        self.assertIn('type_register_static(&nokian72_type);', s)
        # omap1710 = true and no display override: the N70's panel and keypad (rm1_keys_init(mpu, ph->omap1710))
        self.assertRegex(s, r'rm1_phone_n72 = \{ "nokian72", "eka1/rm180/core", rm1_patches_n72, true \}')

    def test_ram_covers_the_bank(self):
        """the bootstrap's memory table: 64 MB at 0x10100000, as the N70's (its stack sits at the top)"""
        body = func(BOARD, 'static void nokian72_class_init(ObjectClass *oc, const void *data)')
        mb = int(re.search(r'default_ram_size = (\d+) \* MiB', body).group(1))
        self.assertGreaterEqual(0x10000000 + mb * 0x100000, 0x10100000 + 0x04000000)

    @NEED_ROM
    def test_rom_bank_table(self):
        self.assertEqual(struct.unpack_from('<2I', ROM.read_bytes(), 0xd0c), (0x10100000, 0x04000000))

    def test_tools_know_the_model(self):
        run = (REPO / 'eka1/tools/ngage_run.py').read_text()
        m = run.split("'nokian72': dict(", 1)[1].split('ttb=', 1)[0]
        self.assertIn('fatal=0x5001fa8c', m)
        self.assertIn('cur_thread=0x8000295c', m)
        self.assertIn('containers=0x8000048c', m)
        self.assertIn('sdram=(0x10000000, 0x08000000)', m)
        self.assertIn('isi-replies-rm180.txt', m)
        self.assertTrue((REPO / 'src/qemu-ngage/isi-replies-rm180.txt').exists())
        phone = (REPO / 'eka1/tools/ngage_phone.py').read_text()
        self.assertIn("'nokian72': 'Nokia N72'", phone)
        self.assertIn("'nokian72': RM84_BUTTONS", phone)

    @NEED_ROM
    def test_rom_fault_routine(self):
        """Kern::Fault 0x5001fa84 adds 0x10000 to the reason and falls into the fault routine 0x5001fa8c (the N70's)"""
        self.assertEqual(word(ROM, 0x5001fa84), 0xe2811801)
        self.assertEqual(word(ROM, 0x5001fa88), 0xeaffffff)

    @NEED_BOTH
    def test_kernel_code_is_the_n70s(self):
        """ekern.exe at the same address and size; only words pointing into other (relinked) modules differ, so
        the N70's kernel data words (current thread, containers) hold"""
        a, b = N70_ROM.read_bytes(), ROM.read_bytes()
        o, n = 0x8f9c, 196648
        diff = [i for i in range(0, n, 4) if a[o + i:o + i + 4] != b[o + i:o + i + 4]]
        for i in diff:
            self.assertEqual(b[o + i + 3], 0x50, hex(0x50008f9c + i))   # a ROM address, not code
        for lit in (0x8000295c, 0x8000048c):
            p = struct.pack('<I', lit)
            self.assertEqual(a.count(p), b.count(p))


class Rm180CorePatches(unittest.TestCase):
    """The N70's DSP/secure-environment patches at RM-180's addresses, old words checked."""

    PATCHES = re.compile(r'rm1_patch\(core, len, "(H-rm180-[a-z-]+)", (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\);')

    def patches(self):
        return [(h, int(v, 16), int(o, 16), int(n, 16))
                for h, v, o, n in self.PATCHES.findall(func(BOARD, 'static void rm1_patches_n72('))]

    def test_all_six_hacks(self):
        self.assertEqual({p[0] for p in self.patches()},
                         {'H-rm180-dsp-boot', 'H-rm180-dsp-timeout', 'H-rm180-bal-run', 'H-rm180-eapgwc',
                          'H-rm180-secenvinit', 'H-rm180-drmhelper'})
        self.assertEqual(len(self.patches()), 11)

    def test_same_new_words_as_the_n70(self):
        n70 = re.findall(r'rm1_patch\(core, len, "H-rm84-[a-z-]+", 0x[0-9a-f]+, (0x[0-9a-f]+), (0x[0-9a-f]+)\);',
                         func(BOARD, 'static void rm1_patches_n70('))
        self.assertEqual([(int(o, 16), int(n, 16)) for o, n in n70], [(o, n) for _, _, o, n in self.patches()])

    @NEED_ROM
    def test_old_words_in_rom(self):
        for hack, va, old, new in self.patches():
            self.assertEqual(word(ROM, va), old, '%s %08x' % (hack, va))

    @NEED_BOTH
    def test_same_module_offset_as_the_n70(self):
        """each site is at the N70's offset inside the same module, and the 32 words around it are the N70's"""
        n70_files = json.loads((REPO / 'eka1/rm84/core/files.json').read_text())
        n72_files = {f['path'].lower(): f for f in json.loads(FILES.read_text())}
        n70_va = [int(v, 16) for v in re.findall(r'rm1_patch\(core, len, "H-rm84-[a-z-]+", (0x[0-9a-f]+),',
                                                 func(BOARD, 'static void rm1_patches_n70('))]
        a, b = N70_ROM.read_bytes(), ROM.read_bytes()
        for va70, (hack, va72, _, _) in zip(n70_va, self.patches()):
            m70 = next(f for f in n70_files if f['address'] <= va70 < f['address'] + f['size'])
            m72 = n72_files[m70['path'].lower()]
            self.assertEqual(va70 - m70['address'], va72 - m72['address'], hack)
            self.assertEqual(a[va70 - 0x50000000 - 64:va70 - 0x50000000 + 64],
                             b[va72 - 0x50000000 - 64:va72 - 0x50000000 + 64], hack)


if __name__ == '__main__':
    unittest.main()
