"""Nokia 6630 (-M nokia6630) camera checks that need no emulator run: the board wiring (nokia6630_cam.c), the modem
reply CamServerCore waits on, and (when the firmware is extracted) the driver facts the model relies on.
docs/devices/nokia-7650-nhl2/camera.md "Nokia 6630", docs/hacks.md H-rm1-camera, H-rm1-imei.

Run: python3 tests/rm1_camera_test.py
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
ROM = REPO / 'eka1/rm1/core/ROM.bin'
CAMPDD = (0x5009f3a0, 26768)            # System/Libs/campdd.pdd in the inflated core (files.json)


def rom():
    return ROM.read_bytes() if ROM.exists() else None


class Imei(unittest.TestCase):
    def test_imei_reply(self):
        """Regression "rm1_camera_waits_for_imei": Camera stayed blank; CamServerCore waits for
        RMobilePhone::GetPhoneId, which the TSY asks the modem as INFO_SERIAL_NUMBER_READ {41} (1B 00): unanswered.
        The reply has the N95 modem's layout (syborg_modem_core.c H-imei); the string one byte earlier made the
        TSY fault and the kernel stop."""
        lines = (REPO / 'src/qemu-ngage/isi-replies-rm1.txt').read_text().splitlines()
        rows = [l.split('#')[0].split() for l in lines if l.startswith('reply 1B 00 ')]
        self.assertEqual(len(rows), 1)
        b = [int(x, 16) for x in rows[0][3:]]       # after "reply 1B 00"
        self.assertEqual(b[0], 0x01)                        # INFO_SERIAL_NUMBER_READ_RESP
        status, nsb, sb = b[1], b[2], b[3:]
        self.assertEqual((status, nsb), (0, 1))
        self.assertEqual(sb[0], 0x41)                       # INFO_SB_SN_IMEI_PLAIN
        self.assertEqual(sb[1], len(sb))                    # sub-block length
        self.assertEqual(sb[2], 0)
        n = sb[3]
        imei = bytes(sb[4:4 + n]).decode()
        self.assertEqual(n, 15)
        self.assertTrue(imei.isdigit())
        self.assertEqual(sb[4 + n], 0)                      # NUL
        digits = [int(c) for c in imei]                     # a valid (Luhn) IMEI
        s = sum(d if i % 2 == 0 else (d * 2 - 9 if d * 2 > 9 else d * 2) for i, d in enumerate(digits))
        self.assertEqual(s % 10, 0)


class Board(unittest.TestCase):
    def test_wiring(self):
        c = (HW / 'nokia6630_cam.c').read_text()
        self.assertIn('#define RM1_CAM_CCP_BASE 0x2007d000', c)
        self.assertIn('#define RM1_CAM_IRQ      50', c)
        self.assertIn('#define RM1_CAM_DRQ      3', c)
        self.assertIn('rm1_gpio_set_dir_hook(0, rm1_cam_scl_dir', c)
        self.assertIn('rm1_gpio_set_dir_hook(3, rm1_cam_sda_dir', c)
        self.assertIn('ngage_cam_rm1_setup', c)
        b = (HW / 'nokia9500.c').read_text()
        for phone, variant in (('6630', 'RM1_CAM_6630'), ('6680', 'RM1_CAM_6630'), ('n90', 'RM1_CAM_N90')):
            m = re.search(r'rm1_phone_%s = \{([^}]*)\}' % phone, b)
            self.assertTrue(m and m.group(1).strip().endswith(variant), 'the %s has camera %s' % (phone, variant))
        self.assertIn('if (ph->camera) {', b)

    def test_gpio_direction_hook(self):
        """The sensor's lines are open drain driven through DIRECTION (0x34): the hook must fire on it."""
        s = (HW / 'nokia6630_soc.c').read_text()
        self.assertRegex(s, r'rm1_gpio_dir_hook\[g->index\] && \(addr & ~3\) == 0x34')


class Firmware(unittest.TestCase):
    def setUp(self):
        self.rom = rom()
        if self.rom is None:
            self.skipTest('eka1/rm1/core/ROM.bin not extracted')

    def words(self):
        b, n = CAMPDD
        return [struct.unpack_from('<I', self.rom, b - 0x50000000 + i)[0] for i in range(0, n - 3, 4)]

    def test_ccp_registers(self):
        """campdd's receiver set-up table (0x500a5064): the 7650's sync codes at +10..+40 of VA 0x58105000."""
        w = self.words()
        i = w.index(0x58105010)
        self.assertEqual(w[i:i + 8], [0x58105010, 0xff000002, 0x58105020, 0xff000003,
                                      0x58105030, 0xff000000, 0x58105040, 0xff000001])

    def test_i2c_pins(self):
        """the bit-banged bus: GPIO4 (VA 0x580BBC00) and GPIO1 (0x580BE400) DATAIN / DIRECTION"""
        w = set(self.words())
        for va in (0x580bbc2c, 0x580bbc34, 0x580be42c, 0x580be434):
            self.assertIn(va, w)

    def test_interrupt_name(self):
        """Lv2_CCPStatus is the kernel name-table entry after Lv2_SPI (line 49): level-2 line 50"""
        self.assertIn(b'Lv2_Gpio4\x00\x00\x00Lv2_SPI\x00Lv2_CCPStatus\x00', self.rom)
        b, n = CAMPDD
        self.assertIn(b'Lv2_CCPStatus', self.rom[b - 0x50000000:b - 0x50000000 + n])


ROM_6680 = REPO / 'eka1/rm36/core/ROM.bin'
CAMPDD_6680 = (0x500a00e0, 26232)       # System/Libs/campdd.pdd in the 6680's core (files.json)


class Firmware6680(unittest.TestCase):
    """The 6680 (RM-36) takes the 6630's model unchanged (docs/devices/nokia-7650-nhl2/camera.md "Nokia 6680"):
    its campdd is another build of the 6630's driver with the same receiver, pins and 44-byte frame layout."""
    def setUp(self):
        if not ROM_6680.exists():
            self.skipTest('eka1/rm36/core/ROM.bin not extracted')
        b, n = CAMPDD_6680
        r = ROM_6680.read_bytes()[b - 0x50000000:b - 0x50000000 + n]
        self.w = [struct.unpack_from('<I', r, i)[0] for i in range(0, n - 3, 4)]

    def test_same_receiver_and_pins(self):
        i = self.w.index(0x58105010)
        self.assertEqual(self.w[i:i + 8], [0x58105010, 0xff000002, 0x58105020, 0xff000003,
                                           0x58105030, 0xff000000, 0x58105040, 0xff000001])
        for va in (0x580bbc2c, 0x580bbc34, 0x580be42c, 0x580be434):
            self.assertIn(va, self.w)

    def test_44_byte_header_frames(self):
        """the 6630's sizes (header 44 + w x h x 2), not the N90's 296-byte header"""
        for w, h in ((160, 120), (640, 480), (176, 144)):
            self.assertIn(44 + w * h * 2, self.w)
            self.assertNotIn(296 + w * h * 2, self.w)


if __name__ == '__main__':
    unittest.main()
