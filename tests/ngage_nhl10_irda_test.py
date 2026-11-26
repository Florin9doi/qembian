"""Nokia 6600 (NHL-10) IrDA: the facts the SIR UART model (ngage_sir.c) and its board glue rest on, re-read from
the 6600's ROM the way they were found (docs/research/6600-irda.md), plus the regression for the FIQ storm.

Run: python3 tests/ngage_nhl10_irda_test.py   (the core itself: tests/ngage_sir_test.c)
ROM checks are skipped when the (git-ignored) extraction eka1/nhl10/output/rom is missing.
"""
import re
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HW = REPO / 'src/qemu-ngage/hw/arm'
ROMDIR = REPO / 'eka1/nhl10/output/rom'
FILES = ROMDIR / 'files/System'
EKERN_VA = 0x5030ff9c


def src(name):
    return (HW / name).read_text()


def ekern():
    return (FILES / 'Libs/ekern.exe').read_bytes()


def ekern_word(d, va):
    return struct.unpack_from('<I', d, va - EKERN_VA)[0]


def fiq_names(d):
    """ekern's FIQ name table: (name string, kernel word) pairs from FiqTimer0 on; index = interrupt source"""
    def cstr(va):
        o = va - EKERN_VA
        return d[o:d.index(b'\0', o)].decode() if 0 <= o < len(d) else ''
    first = d.index(b'FiqTimer0\0') + EKERN_VA
    o = d.index(struct.pack('<I', first))
    out = []
    while True:
        a, b = struct.unpack_from('<2I', d, o)
        if not (0x50330000 < a < 0x50340000 and 0x80000000 < b < 0x80002000):
            return out
        out.append(cstr(a))
        o += 8


class FiqStatusRegression(unittest.TestCase):
    """fiq_status_kept_as_written: the catch-all kept 0x0C120050 = 7 (the kernel's start-up clear), so the FIQ
    dispatcher saw "special cause pending" on every FIQ, acknowledged it and returned without dispatching bank 3:
    the first FIQ the phone ever took (IrDA on, FiqCTSI_FIQ) re-entered for ever and froze the phone."""

    def test_board_reads_0_and_drops_writes(self):
        t = src('ngage.c')
        self.assertIn('#define NGAGE_FIQ_CAUSES 0x0c120050', t)
        rd = t.split('static uint64_t ngage_unknown_read', 1)[1].split('\n}\n', 1)[0]
        self.assertRegex(rd, r'NGAGE_FIQ_CAUSES\) \{\s*w = 0;')
        wr = t.split('static void ngage_unknown_write', 1)[1].split('\n}\n', 1)[0]
        self.assertRegex(wr, r'NGAGE_FIQ_CAUSES\) \{\s*w = 0;')      # a write never sticks

    @unittest.skipUnless(ROMDIR.exists(), 'no 6600 extraction')
    def test_kernel_writes_7_and_dispatcher_reads_it_first(self):
        d = ekern()
        # start-up: mov r0,#7 ; ldr r1,=offset ; bl write-word(0x50312eb4) with offset 0x120050 (0x58000000 base)
        self.assertEqual(ekern_word(d, 0x503110ac), 0xe3a00007)
        self.assertEqual(ekern_word(d, 0x503110f0), 0x00120050)
        # FIQ dispatcher 0x503112b4: ldr r1,=0x58120050 ; ldr r0,[r1] ; tst r0,#7 ; beq bank-3 dispatch
        self.assertEqual(ekern_word(d, 0x50311334), 0x58120050)
        self.assertEqual(ekern_word(d, 0x503112c0), 0xe3100007)
        self.assertEqual(ekern_word(d, 0x50311330), 0x581201d4)          # then bank 3 +14, the source number


@unittest.skipUnless(ROMDIR.exists(), 'no 6600 extraction')
class SirDriver(unittest.TestCase):
    def test_comm1_is_the_sir_pdd(self):
        esk = (FILES / 'Data/irda.esk').read_text(errors='replace')
        self.assertRegex(esk, r'irPhysicalComPort=\s*1')
        pdd = (FILES / 'Libs/euart1.pdd').read_bytes()
        self.assertIn('SIR.PDD'.encode('utf-16-le'), pdd)
        self.assertIn(b'FiqCTSI_FIQ', pdd)                              # Interrupt::Bind by name

    def test_fiq_source_number(self):
        names = fiq_names(ekern())
        n = int(re.search(r'#define NGAGE_SIR_IRQ (\d+)', src('ngage.c')).group(1))
        self.assertEqual(names[0], 'FiqTimer0')
        self.assertEqual(names.index('FiqCTSI_FIQ'), n)

    def test_register_and_baud_literals(self):
        pdd = (FILES / 'Libs/euart1.pdd').read_bytes()
        words = set(struct.unpack_from('<%dI' % (len(pdd) // 4), pdd))
        h = src('ngage_sir.h')
        for name in ('CTSI_DISABLE', 'CTSI_ENABLE', 'CTSI_STATUS', 'CTRL', 'STAT', 'DIVISOR', 'START', 'STOP',
                     'TXDATA', 'RXDATA', 'TXCOUNT', 'TXPTR', 'RXPTR'):
            addr = int(re.search(r'#define SIR_%s\s+(0x[0-9a-f]+)' % name, h).group(1), 16)
            self.assertIn(addr - 0x0c000000, words, name)               # the kernel accessors take 0x0C000000-relative
        for code, baud in re.findall(r'\{ (0x[0-9a-f]+), (\d+) \}', src('ngage_sir.c')):
            self.assertIn(int(code, 16), words, baud)


class Model(unittest.TestCase):
    def test_only_the_6600_has_it(self):
        t = src('ngage.c')
        with_irda = re.findall(r'static const NgageModel (\w+) = \{(?:(?!\n\};).)*?\.irda = true', t, re.S)
        self.assertEqual(with_irda, ['ngage_nhl10'])


if __name__ == '__main__':
    unittest.main()
