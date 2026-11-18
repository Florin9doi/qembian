"""Tests for the Nokia 9500 TrueFFS stand-in (H-ra2-doc): src/qemu-ngage/nos-standin/doc_standin_ra2.S,
its generated header and where the board puts it.

Run from eka1/: python3 -m unittest -v test_ra2_doc_standin
The ROM checks are skipped when the (git-ignored) core ROM is not extracted.
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / 'src/qemu-ngage/nos-standin/doc_standin_ra2.S'
HDR = REPO / 'src/qemu-ngage/hw/arm/ngage_ra2_doc_stub.h'
BOARD = REPO / 'src/qemu-ngage/hw/arm/nokia9500.c'
ROM = REPO / 'eka1/ra2/core/ROM.bin'

DOC_DLL_HEADER = 0x50062370          # DiskOnChip.dll's TRomImageHeader [V]
DOC_ENTRY = 0x500623D4               # its entry point: one branch, called by the kernel on load [V]


def header():
    text = HDR.read_text()
    va = int(re.search(r'NGAGE_RA2_DOC_NOS_STUB_VA (0x[0-9a-f]+)', text).group(1), 16)
    size = int(re.search(r'ngage_ra2_doc_nos_stub\[(\d+)\]', text).group(1))
    syms = {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r'NGAGE_RA2_DOC_NOS_SYM_(doc_\w+) (0x[0-9a-f]+)', text)}
    return va, size, syms


def board_exports():
    """(export address, stub symbol) pairs from ra2_doc_standin() in the board."""
    body = BOARD.read_text().split('static void ra2_doc_standin', 1)[1].split('\n}\n', 1)[0]
    return [(int(a, 16), s) for a, s in re.findall(r'\{ (0x[0-9a-f]+), NGAGE_RA2_DOC_NOS_SYM_(doc_\w+) \}', body)]


class DocStandin(unittest.TestCase):
    def test_stub_not_on_dll_entry_point(self):
        """Regression doc_stub_on_entry_point: the stub was copied over DiskOnChip.dll's entry point;
        the kernel calls the entry on load (r0=-1, r1=0), the stub read the IOreq at 0 -> kernel abort."""
        va, size, _ = header()
        self.assertFalse(va <= DOC_ENTRY < va + size, 'stub covers the entry point %08x' % DOC_ENTRY)

    def test_stub_does_not_cover_the_exports(self):
        va, size, _ = header()
        for addr, _ in board_exports():
            self.assertFalse(va <= addr < va + size, 'stub covers export %08x' % addr)

    def test_board_branches_to_existing_symbols(self):
        _, _, syms = header()
        exports = board_exports()
        self.assertEqual(len(exports), 5)
        for _, sym in exports:
            self.assertIn(sym, syms)

    def test_setenv_does_not_store_through_third_argument(self):
        """Regression doc_setenv_store: flSetEnvAll is called with two arguments (r2 = 0x4014, left over);
        storing through r2 aborted the kernel."""
        text = SRC.read_text()
        body = text.split('doc_setenv:', 1)[1].split('.global', 1)[0]
        self.assertNotRegex(body, r'\bstr')

    def test_block_device_in_kernel_peripheral_mapping(self):
        """The stub's BLK (virtual) and the board's RA2_BLK_BASE (physical) are the same page."""
        blk_va = int(re.search(r'\.equ\s+BLK,\s+(0x[0-9a-f]+)', SRC.read_text()).group(1), 16)
        blk_pa = int(re.search(r'#define RA2_BLK_BASE (0x[0-9a-f]+)', BOARD.read_text()).group(1), 16)
        self.assertEqual(blk_va - 0x58000000, blk_pa - 0xfff00000)

    @unittest.skipUnless(ROM.exists(), 'core ROM not extracted')
    def test_rom_entry_point_and_export_directory(self):
        """Regression doc_ord2_patched_target: the board patched 0x50068154, the target of export 2's
        thunk (shared code), instead of the export itself at 0x50065F28."""
        rom = ROM.read_bytes()
        w = struct.unpack_from('<17I', rom, DOC_DLL_HEADER - 0x50000000)
        self.assertEqual(w[4], DOC_ENTRY)
        n, exp_dir = w[15], w[16]
        self.assertEqual(n, 5)
        exports = struct.unpack_from('<%dI' % n, rom, exp_dir - 0x50000000)
        self.assertEqual(sorted(exports), sorted(a for a, _ in board_exports()))


if __name__ == '__main__':
    unittest.main()
