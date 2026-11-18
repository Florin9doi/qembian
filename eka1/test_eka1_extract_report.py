"""Tests for the report helpers of tools/eka1_extract.py.

Regression (bug "report_hardcoded_ngage"): the extraction report and the Ghidra notes quoted the
N-Gage QD's kernel address, header fields, flash bank split and PPM windows for every firmware, so the
Nokia 6600 (NHL-10) report claimed ekern.exe at 0x5031AF9C and a two-bank flash. The helpers now
derive these from the image; these tests pin that on synthetic data.

Run from eka1/: python3 -m unittest -v test_eka1_extract_report
"""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))
import eka1_extract as x  # noqa: E402


def rom_with_kernel(base=0x50000000, addr=0x50001000):
    rom = bytearray(0x2000)
    words = [0x1000007A, 0x1000008B, 0x100058E8, 0, addr + 0x64 + 0x54, addr + 0x64, 0x50001F00,
             0x900, 0x880, 0x40, 0x100, 0x20000, 0x100000, 0x4000, 0x50001F80, 7, 0x50001E00]
    struct.pack_into("<17I", rom, addr - base, *words)
    return bytes(rom)


class KernelHeader(unittest.TestCase):
    def test_fields_from_image(self):
        k = x.kernel_header(rom_with_kernel(), 0x50000000, 0x50001000)
        self.assertEqual(k["code_address"], 0x50001064)
        self.assertEqual(k["entry"], 0x500010B8)
        self.assertEqual(k["header_size"], 0x64)
        self.assertEqual(k["code_size"], 0x900)
        self.assertEqual(k["bss_size"], 0x100)
        self.assertEqual(k["export_count"], 7)          # not the dll ref table (word 14)
        self.assertEqual(k["export_dir"], 0x50001E00)

    def test_out_of_range_is_empty(self):
        self.assertEqual(x.kernel_header(rom_with_kernel(), 0x50000000, 0x40000000), {})
        self.assertEqual(x.kernel_header(rom_with_kernel(), 0x50000000, 0x50001FF0), {})


class BankLines(unittest.TestCase):
    def test_three_banks_nhl10_shape(self):
        mapping = [
            {"flash_address": 0, "flash_length": 0x12C, "rom_offset": 0},
            {"flash_address": 0x164, "flash_length": 0x7FFE9C, "rom_offset": 0x164},
            {"flash_address": 0x04000000, "flash_length": 0x700000, "rom_offset": 0x800000},
            {"flash_address": 0x06000000, "flash_length": 0x6FF000, "rom_offset": 0xF00000},
        ]
        self.assertEqual(x.bank_lines(mapping, 0x50000000), [
            "flash 0x00000000..0x00800000 -> ROM offset 0x0000000 (virtual 0x50000000)",
            "flash 0x04000000..0x04700000 -> ROM offset 0x0800000 (virtual 0x50800000)",
            "flash 0x06000000..0x066FF000 -> ROM offset 0x0F00000 (virtual 0x50F00000)",
        ])

    def test_two_banks_ngage_shape(self):
        mapping = [
            {"flash_address": 0x02000000, "flash_length": 0x265000, "rom_offset": 0xF00000},
            {"flash_address": 0, "flash_length": 0xF00000, "rom_offset": 0},
        ]
        lines = x.bank_lines(mapping, 0x50000000)
        self.assertEqual(len(lines), 2)
        self.assertIn("ROM offset 0x0F00000 (virtual 0x50F00000)", lines[1])

    def test_gap_in_flash_without_gap_in_rom_is_a_new_bank(self):
        mapping = [{"flash_address": 0, "flash_length": 0x100, "rom_offset": 0},
                   {"flash_address": 0x200, "flash_length": 0x100, "rom_offset": 0x100}]
        self.assertEqual(len(x.bank_lines(mapping, 0)), 2)

    def test_empty(self):
        self.assertEqual(x.bank_lines([], 0x50000000), [])


class VariantLines(unittest.TestCase):
    def test_none(self):
        self.assertEqual(x.variant_lines(None), ["- Variant/PPM overlay: not found."])

    def test_without_data_window(self):
        v = {"source": "NHL105530.26", "data_image": None,
             "directory_image": {"flash_address": 0x02000000, "virtual_base": 0x51700000},
             "files": [{"complete": True}, {"complete": False}],
             "validation": {"files_checked": 1, "mismatches": []}}
        text = "\n".join(x.variant_lines(v))
        self.assertIn("flash 0x02000000 mapped at virtual 0x51700000", text)
        self.assertIn("No separate PPM data window", text)
        self.assertIn("extracted: 1;", text)
        self.assertNotIn("0x51230000", text)

    def test_with_data_window(self):
        v = {"source": "RH2941.026",
             "directory_image": {"flash_address": 0x02330000, "virtual_base": 0x51230000},
             "data_image": {"flash_address": 0x00F00000, "virtual_base": 0x51300000, "signature_score": 3},
             "files": [], "validation": {}}
        self.assertIn("virtual 0x51300000", "\n".join(x.variant_lines(v)))


class KeepAddedSections(unittest.TestCase):
    """Regression (bug "report_overwrites_ghidra_notes"): --report rewrote analysis/ghidra-notes.md
    and dropped the hand-written Ghidra-pass sections appended to it (git-ignored, no other copy)."""

    def test_appended_sections_survive(self):
        old = "# Ghidra loading notes (old)\n\n- old fact\n\n## Kernel tables (Ghidra pass)\n\nexport dir 0x50342068\n"
        new = x.keep_added_sections("# Ghidra loading notes (new)\n\n- new fact\n", old)
        self.assertTrue(new.startswith("# Ghidra loading notes (new)"))
        self.assertNotIn("old fact", new)
        self.assertIn("## Kernel tables (Ghidra pass)\n\nexport dir 0x50342068", new)

    def test_nothing_to_keep(self):
        self.assertEqual(x.keep_added_sections("gen\n", None), "gen\n")
        self.assertEqual(x.keep_added_sections("gen\n", "# only generated\n- a\n"), "gen\n")

    def test_rerun_is_stable(self):
        old = "# g\n\n## Added\nx\n"
        once = x.keep_added_sections("# g\n", old)
        self.assertEqual(x.keep_added_sections("# g\n", once), once)


if __name__ == "__main__":
    unittest.main()
