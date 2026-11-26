"""Camera checks for the DCT4 S60 2nd FP1 boards (3230, 7610/6670, 6260: src/qemu-ngage/hw/arm/ngage.c) that need no
emulator run. docs/devices/nokia-7650-nhl2/camera.md, docs/hacks.md H-dct4-selftest-stop, H-rh51-camera,
H-rm25-camera, H-rm51-camera.

Run: python3 tests/dct4_camera_test.py
ROM checks are skipped when the (git-ignored) firmware is not extracted.
"""
import json
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
ISI = REPO / 'src/qemu-ngage'

# phone: (model struct, ISI table, extraction folder)
PHONES = {'nokia3230': ('ngage_rm51', 'isi-replies-rm51.txt', 'rm51'),
          'nokia7610': ('ngage_rh51', 'isi-replies-rh51.txt', 'rh51'),
          'nokia6260': ('ngage_rm25', 'isi-replies-rm25.txt', 'rm25')}


def model(struct_name):
    t = (HW / 'ngage.c').read_text()
    return t.split('static const NgageModel %s = {' % struct_name, 1)[1].split('\n};\n', 1)[0]


def isa_channel1_object(folder):
    """isa_if_driver.ldd's object switch (sub r0, #LO; cmp r0, #N; ldrls pc, [pc, r0, lsl #2]): the object whose case
    returns channel 1 (mov r0, #1), or None without the extraction"""
    man = REPO / 'eka1' / folder / 'output/rom/manifest.json'
    if not man.exists():
        return None
    ent = [e for e in json.loads(man.read_text())['files'] if e['name'] == 'isa_if_driver.ldd'][0]
    base = ent['rom_virtual_address']
    b = (REPO / 'eka1' / folder / 'output/rom/files' / ent['path']).read_bytes()
    for i in range(0, len(b) - 12, 4):
        w = struct.unpack_from('<3I', b, i)
        if (w[0] & 0xfffff000) == 0xe2400000 and (w[1] & 0xfff0f000) == 0xe3500000 and w[2] == 0x979ff100:
            lo, n = w[0] & 0xff, (w[1] & 0xff) + 1
            for k in range(n):
                t = struct.unpack_from('<I', b, i + 16 + 4 * k)[0]
                if struct.unpack_from('<I', b, t - base)[0] == 0xe3a00001:
                    return lo + k
    raise AssertionError('no object switch in isa_if_driver.ldd')


class SelfTestStop(unittest.TestCase):
    def test_selftest_session_ends_so_camera_channel_is_free(self):
        """Regression "bttestserver_holds_camera" (3230/7610/6670/6260): Camera stayed blank because BTTestServer
        kept the camera channel it opens for SelfTestServer's boot session. The table sends SelfTestServer
        PN_EPOC_TEST msg 1 sub-id 8 after DosServer's 59 01, to isa_if channel 1's object (0x37 / 0x36)."""
        want = {'nokia3230': 0x37, 'nokia7610': 0x36, 'nokia6260': 0x37}
        for phone, (_, table, folder) in PHONES.items():
            lines = (ISI / table).read_text().splitlines()
            i = [n for n, l in enumerate(lines) if l.startswith('reply 59 01 02')][0]
            m = re.match(r'reply\+ 59 01 01 08 00 media=00 robj=([0-9A-Fa-f]{2})\b', lines[i + 1])
            self.assertTrue(m, phone)
            self.assertEqual(int(m.group(1), 16), want[phone], phone)
            obj = isa_channel1_object(folder)
            if obj is not None:
                self.assertEqual(obj, want[phone], phone)


class Models(unittest.TestCase):
    def test_camera_flags(self):
        self.assertIn('.camera = NGAGE_CAM_RM25', model('ngage_rm25'))
        self.assertIn('.camera = NGAGE_CAM_CAMX', model('ngage_rh51'))
        self.assertIn('.camera = NGAGE_CAM_RM51', model('ngage_rm51'))
        self.assertIn('ngage_rh67 = ngage_rh51;', (HW / 'ngage.c').read_text())     # the 6670 copies the 7610

    def test_rm51_camera_pins_mapped(self):
        """Regression "rm51_camera_bus_dead": the 3230's module never saw a command; its I2C pins (0x0C15020C /
        0x0C150210) were not mapped on that board, so writes went to the catch-all."""
        t = (HW / 'ngage_keys.c').read_text()
        rm51 = t.split('if (kpd.rm51) {', 1)[1].split('}', 1)[0]
        self.assertIn('0x0c150200', rm51)
        self.assertIn('joy_ops', rm51)

    def test_camx_still_buffer_fits(self):
        """Regression "camx_still_not_armed": Select on the 7610 armed nothing; the JPEG still's 0xED82C-byte
        transfer was over the model's buffer limit (0x97000) and the board refused the DMA."""
        h = (HW / 'ngage_cam.h').read_text()
        size = int(re.search(r'#define CAM_BUF_SIZE\s+(0x[0-9a-f]+)', h).group(1), 16)
        still = int(re.search(r'#define CAM_CAMX_STILL\s+(0x[0-9a-f]+)', h).group(1), 16)
        self.assertEqual(still, 0xed82c)
        self.assertGreaterEqual(size, still)

    def test_camx_mode_table_in_rom(self):
        """camxpdd's mode 5 is the variable-length still: expected size 0, DMA 0xED82C (table 0x503e7c64)."""
        man = REPO / 'eka1/rh51/output/rom/manifest.json'
        if not man.exists():
            self.skipTest('7610 firmware not extracted')
        ent = [e for e in json.loads(man.read_text())['files'] if e['name'] == 'camxpdd.pdd'][0]
        b = (REPO / 'eka1/rh51/output/rom/files' / ent['path']).read_bytes()
        o = 0x503e7c64 - ent['rom_virtual_address'] + 5 * 40
        self.assertEqual(struct.unpack_from('<2I', b, o + 0x20), (0, 0xed82c))
        o = 0x503e7c64 - ent['rom_virtual_address'] + 1 * 40
        self.assertEqual(struct.unpack_from('<2I', b, o + 0x20), (0x962c, 0x962c))     # 44 + 160x120x2


if __name__ == '__main__':
    unittest.main()
