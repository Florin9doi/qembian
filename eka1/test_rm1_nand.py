"""Tests of eka1/tools/rm1_nand.py (the Nokia 6630's NAND image, H-rm1-nand).
Run: python3 eka1/test_rm1_nand.py
"""
import pathlib, struct, sys, unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'tools'))
import rm1_nand as r  # noqa: E402


class Ecc(unittest.TestCase):
    def test_vector_shared_with_the_c_model(self):
        """the same vector as tests/nokia6630_nand_test.c: tool and board compute one ECC"""
        v = (b'Nokia 6630 NAND' * 40)[:512]
        self.assertEqual(r.ecc(v), 0x3350335)
        self.assertEqual(r.ecc_pack(0x3350335), bytes((0x35, 0x35, 0x33)))

    def test_vectorised_equals_reference(self):
        import random
        rnd = random.Random(7)
        data = bytes(rnd.randrange(256) for _ in range(512 * 4))
        got = r.ecc_pages(data)
        for i in range(4):
            self.assertEqual(int(got[i]), r.ecc(data[i * 512:(i + 1) * 512]))

    def test_blank_page_is_zero(self):
        self.assertEqual(r.ecc(b'\xff' * 512), 0)


class Image(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rofs = bytearray(b'ROFS' + bytes(range(256)) * 100)
        struct.pack_into('<I', rofs, 0x2c, 0x8000)          # iMaxImageSize
        cls.rofs = bytes(rofs)
        cls.rofx = b'ROFx' + b'\x55' * 5000
        cls.img, cls.recs = r.build(cls.rofs, cls.rofx)

    def page(self, pg):
        return self.img[pg * r.RAW:pg * r.RAW + r.PAGE], self.img[pg * r.RAW + r.PAGE:(pg + 1) * r.RAW]

    def test_size_is_the_chip(self):
        self.assertEqual(len(self.img), 4096 * 32 * 528)

    def test_toc_records(self):
        """pages 0x100-0x101 = what the board copies to SRAM 0x20000100: mednand's lookup by name"""
        toc = self.page(0x100)[0] + self.page(0x101)[0]
        recs = {}
        for i in range(32):
            rec = toc[i * 32:i * 32 + 32]
            name = rec[20:32].rstrip(b'\0').rstrip(b'\xff').decode('latin1')
            if name.startswith('SOS-'):
                recs[name] = struct.unpack_from('<II', rec)
        self.assertEqual(recs['SOS-ROFS'][0], 0x00100000)
        self.assertEqual(recs['SOS-USER'], (0x01000000, (4096 - 0x4c) * 0x4000 - 0x01000000))

    def test_rofs_and_rofx_in_place_with_ecc(self):
        pg = 0x00100000 // 512
        main, spare = self.page(pg)
        self.assertEqual(main[:4], b'ROFS')
        self.assertEqual(spare[6:9], r.ecc_pack(r.ecc(main)))
        self.assertEqual(spare[5], 0xff)                     # not a bad block
        start = 0x00100000 + 0x8000          # regression rm1_rofx_place: at the ROFS's iMaxImageSize
        self.assertEqual(self.page(start // 512)[0][:4], b'ROFx')
        self.assertEqual(self.recs[0][2], start + len(self.rofx) - 0x00100000)

    def test_user_area_blank(self):
        main, spare = self.page(0x01000000 // 512)
        self.assertEqual(main + spare, b'\xff' * 528)

    def test_rofs_bigger_than_its_max_refused(self):
        rofs = bytearray(b'ROFS' + bytes(0x100))
        struct.pack_into('<I', rofs, 0x2c, 0x10)
        with self.assertRaises(ValueError):
            r.layout(bytes(rofs), b'ROFx')

    def test_too_big_rofs_refused(self):
        with self.assertRaises(ValueError):
            r.layout(b'\0' * (0x01000000 - 0x00100000 + 1), b'')


class Rm84UserStart(unittest.TestCase):
    """Regression rm84_nand_layout: the N70's ROFS (iMaxImageSize 0x13F0000) and 12 MB ROFx end past the 6630
    image's C: start 0x1000000; --user-start moves SOS-USER (C:) behind them."""

    def rofs(self, max_size, size):
        b = bytearray(size)
        b[:4] = b'ROFS'
        struct.pack_into('<I', b, 0x2c, max_size)
        return bytes(b)

    def test_n70_layout(self):
        parts, recs = r.layout(self.rofs(0x13f0000, 0x1346ed4), b'x' * 0xba1800, 0x2600000)
        self.assertEqual(recs[0], ('SOS-ROFS', 0x100000, 0x13f0000 + 0xba1800))
        self.assertEqual(recs[1], ('SOS-USER', 0x2600000, r.USER_END - 0x2600000))
        self.assertIn((0x100000 + 0x13f0000, b'x' * 0xba1800), parts)

    def test_default_start_refuses_the_n70(self):
        with self.assertRaises(ValueError):
            r.layout(self.rofs(0x13f0000, 0x1346ed4), b'x' * 0xba1800)

    def test_unaligned_or_outside_refused(self):
        for start in (0x2600001, r.USER_END, 0x100000):
            with self.assertRaises(ValueError):
                r.layout(self.rofs(0x10000, 0x100), b'', start)


if __name__ == '__main__':
    unittest.main()
