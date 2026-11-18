#!/usr/bin/env python3
"""Unit tests for tools/fw/semc.py and the ROLF reader in tools/rofs.py, on synthetic images (no firmware
bytes): python3 tests/semc_test.py"""
import json, os, struct, sys, tempfile, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fw'))
import rofs
import semc


# ------------------------------------------------------------------------------------------ builders

def rofs_entry(rolf, name, att, fs, fa):
    """TRofsEntry: 9.x (UID block, iStructSize includes the padding) or ROLF (no UIDs, size without padding)"""
    eoff = 2 if rolf else 18
    body = (b'' if rolf else bytes(16)) + struct.pack('<BBIIBB', eoff + 12, att, fs, fa, 0, len(name)) \
        + name.encode('utf-16le')
    size = 2 + len(body)
    raw = struct.pack('<H', size if rolf else (size + 3) & ~3) + body
    return raw + b'\xff' * (-len(raw) % 4)


def build_rofs(tree, magic=b'ROFS'):
    """tree: {name: bytes | dict}; returns the image (header 0x30, dirs, file blocks, data)"""
    rolf = magic == b'ROLF'
    dirs = []

    def collect(node):
        dirs.append(node)
        for v in node.values():
            if isinstance(v, dict):
                collect(v)
    collect(tree)
    pos, at = 0x30, {}
    for node in dirs:                                   # sizes depend on names only
        subs = sum(len(rofs_entry(rolf, n, 0x10, 0, 0)) for n, v in node.items() if isinstance(v, dict))
        files = sum(len(rofs_entry(rolf, n, 0, 0, 0)) for n, v in node.items() if not isinstance(v, dict))
        at[id(node)] = dict(dir=pos, dsize=12 + subs, fblock=pos + 12 + subs if files else 0, fsize=files)
        pos += 12 + subs + files
    data_at = {}
    for node in dirs:
        for n, v in node.items():
            if not isinstance(v, dict):
                data_at[(id(node), n)] = pos
                pos += (len(v) + 3) & ~3
    img = bytearray(pos)
    img[0:4] = magic
    img[4] = 0x30
    struct.pack_into('<I', img, 8, 0x30)
    for node in dirs:
        a = at[id(node)]
        struct.pack_into('<HBBII', img, a['dir'], a['dsize'], 0xff, 12, a['fblock'], a['fsize'])
        p = a['dir'] + 12
        for n, v in node.items():
            if isinstance(v, dict):
                e = rofs_entry(rolf, n, 0x10, at[id(v)]['dsize'], at[id(v)]['dir'])
                img[p:p + len(e)] = e
                p += len(e)
        for n, v in node.items():
            if not isinstance(v, dict):
                da = data_at[(id(node), n)]
                e = rofs_entry(rolf, n, 0, len(v), da)
                img[p:p + len(e)] = e
                p += len(e)
                img[da:da + len(v)] = v
    return bytes(img)


def build_eka1(tree, base=0x50000000):
    """an EKA1 XIP ROM: TRomHeader fields at 0x8C, one root directory list; tree: {name: bytes | dict}
    (a flat {name: bytes} goes under System/)"""
    if all(not isinstance(v, dict) for v in tree.values()):
        tree = {'System': tree}
    img = bytearray(0x1000)
    pos = [0x240]

    def ent(name, addr, size, att):
        e = struct.pack('<IIBB', size, addr, att, len(name)) + name.encode('utf-16le')
        return e + b'\0' * (-len(e) % 4)

    def emit(node):
        """write the directory (and everything under it); return its address"""
        here = pos[0]
        body = b''.join(ent(n, 0, 0, 0) for n in node)            # sizes do not depend on addresses
        pos[0] += 4 + len(body)
        body = b''
        for n, v in node.items():
            if isinstance(v, dict):
                body += ent(n, base + emit(v), 0, 0x10)
            else:
                at = pos[0]
                pos[0] += (len(v) + 3) & ~3
                img[at:at + len(v)] = v
                body += ent(n, base + at, len(v), 0)
        img[here:here + 4 + len(body)] = struct.pack('<I', len(body)) + body
        return here
    root = emit(tree)
    struct.pack_into('<III', img, 0x200, 1, 0x05050505, base + root)
    struct.pack_into('<8I', img, 0x8C, base, 0x100000, base + 0x200, 0x80000000, 0x80500000,
                     base + 0x300, base + 0x310, 0)
    return bytes(img[:max(pos[0], 0x400)])


def build_eka2_lookalike(branch=b'\x7e\x00\x00\xea', base=0xF8000000, size=0x200000,
                         rootlist=None, root_count=None, dir_size=None):
    """an image whose TRomHeader fields read like an EKA2 XIP ROM but which is not one: Sony
    Ericsson's ACPU boot ROM (the "EROM CXC1620xx" files) is an ARM branch table at 0 followed by
    masked code, and the bytes at 0x7c happen to look like base/root list/header size. root_count
    writes a root list count, dir_size a directory that claims more bytes than the file holds."""
    img = bytearray(0x400)
    img[0:4] = branch
    struct.pack_into('<I', img, 140, base)
    struct.pack_into('<I', img, 144, size)
    struct.pack_into('<I', img, 148, base + 0x100 if rootlist is None else rootlist)
    struct.pack_into('<I', img, 188, 0x200)                      # iHeaderSize
    if root_count is not None:
        struct.pack_into('<I', img, 0x100, root_count)
    if dir_size is not None:
        struct.pack_into('<II', img, 0x100, 1, 0x05050505)       # one root
        struct.pack_into('<I', img, 0x108, base + 0x200)         # -> directory at 0x200
        struct.pack_into('<I', img, 0x200, dir_size)
        e = struct.pack('<IIBB', 4, base + 0x300, 0, 2) + 'ab'.encode('utf-16le')
        img[0x204:0x204 + len(e)] = e
        img[0x300:0x304] = b'DATA'
    return bytes(img)


def prefix(addr, body):
    d = struct.pack('<II', addr, len(body)) + body
    return d + bytes(-(len(d) + semc.PAD_TAIL) % 0x1000)


def fat16_boot(total=100, fat_sectors=1, roots=16):
    b = bytearray(512)
    b[0:3] = b'\xe9\x00\x90'
    b[3:11] = b'EPOC\0\0\0\0'
    struct.pack_into('<HBHBHHBH', b, 11, 512, 1, 1, 1, roots, total, 0xF8, fat_sectors)
    b[43:54] = b'\0' * 11                    # a NUL label, as on the phones
    b[54:62] = b'FAT16   '
    b[510:512] = b'\x55\xaa'
    return bytes(b)


def srec(t, addr, data, alen):
    body = bytes([alen + len(data) + 1]) + addr.to_bytes(alen, 'big') + data
    return b'S' + bytes([0x30 + t]) + body + bytes([~sum(body) & 0xFF])


# ------------------------------------------------------------------------------------------ tests

class RofsTest(unittest.TestCase):
    TREE = {'sys': {'bin': {'a.dll': b'AAAA1', 'bb.exe': b'B' * 9}}, 'readme.txt': b'hello'}

    def test_rofs_9x_walk(self):
        base, fl = rofs.files(build_rofs(self.TREE))
        self.assertEqual(base, 0)
        self.assertEqual(sorted(p for _, _, p in fl), ['readme.txt', 'sys/bin/a.dll', 'sys/bin/bb.exe'])

    def test_rolf_entries_are_4_byte_aligned_without_padding_in_size(self):
        """2026-11-13: the first ROLF walk stepped by iStructSize alone; a ROLF entry's size excludes the
        padding to the next 4-byte boundary, so the walk read padding as an entry (garbage directory at
        0x2b40000). Names of odd length make every entry padded."""
        tree = {'custom': {'system': {'apps': {'x.url': b'u' * 7}}, 'abc': b'12345'}, 'Sysdata': {'s.mp4': b'm'}}
        img = build_rofs(tree, b'ROLF')
        base, fl = rofs.files(img)
        got = {p: img[fa:fa + fs] for fa, fs, p in fl}
        self.assertEqual(got, {'custom/system/apps/x.url': b'u' * 7, 'custom/abc': b'12345', 'Sysdata/s.mp4': b'm'})

    def test_rofs_still_rejects_other_magic(self):
        with self.assertRaises(SystemExit):
            rofs.files(b'XXXX' + bytes(60))


class ContainerTest(unittest.TestCase):
    def test_prefixed_8_byte_header_and_padding(self):
        body = b'ROLF' + bytes(28)
        d = struct.pack('<II', 0x91D00000, len(body)) + body
        d += bytes(-(len(d) + semc.PAD_TAIL) % 0x1000)
        self.assertEqual(semc.prefixed(d), (8, 0x91D00000, body))

    def test_prefixed_rejects_data_after_payload(self):
        d = struct.pack('<II', 0x90000000, 4) + b'abcd' + b'\0' * 100 + b'x'
        self.assertIsNone(semc.prefixed(d))

    def test_prefixed_fat_9_byte_header(self):
        boot = fat16_boot()
        d = b'\0\0\0\0\xb0' + struct.pack('<I', 1024) + boot + bytes(512) + bytes(64)
        hlen, addr, body = semc.prefixed(d)
        self.assertEqual((hlen, addr, body[:512]), (9, 0, boot))

    def test_fat_label_nul_padded(self):
        """2026-11-13: the label of the phones' FAT volumes is 11 NULs; it was shown as NUL characters"""
        self.assertEqual(semc.fat_boot(fat16_boot())['label'], '')

    def test_fat_used_end(self):
        boot = fat16_boot()
        fat = bytearray(512)
        struct.pack_into('<5H', fat, 0, 0xFFF8, 0xFFFF, 0, 0, 0xFFFF)       # cluster 4 in use
        img = boot + bytes(fat)
        f = semc.fat_boot(boot)
        end, used = semc.fat_used_end(img, f)
        self.assertEqual((end, used), ((1 + 1) * 512 + 16 * 32 + 3 * 512, 1))

    def test_fat_used_end_empty(self):
        f = semc.fat_boot(fat16_boot())
        end, used = semc.fat_used_end(fat16_boot() + bytes(512), f)
        self.assertEqual(used, 0)
        self.assertLessEqual(end, 2048)

    def test_babe_header(self):
        d = b'\xba\xbe\x00\x04' + bytes(20) + b'\xff' * 0x20C + b'\x01\xe0payload'
        h = semc.babe_header(d)
        self.assertEqual((h['version'], h['payload_offset'], h['payload_size']), (4, 0x224, 9))


class SrecTest(unittest.TestCase):
    def test_srec_s8_terminator_without_checksum(self):
        """2026-11-13: the phones end the file with 'S8' and a count of 0 (no address, no checksum); it
        was counted as one bad checksum per file"""
        d = b'S003' + srec(1, 0x10, b'\x01\x02', 2) + srec(2, 0x300000, b'\x03', 3) + b'S8\x00' + bytes(40)
        recs, problems = semc.srecords(d)
        self.assertEqual(problems, [])
        self.assertEqual(recs, [(1, 0x10, b'\x01\x02'), (2, 0x300000, b'\x03'), (8, None, b'')])

    def test_srec_bad_checksum_reported(self):
        r = bytearray(srec(1, 0, b'\x55', 2))
        r[-1] ^= 1
        recs, problems = semc.srecords(b'S003' + bytes(r))
        self.assertEqual(len(recs), 1)
        self.assertTrue(problems and problems[0].startswith('checksum'))

    def test_srec_trailing_garbage(self):
        recs, problems = semc.srecords(b'S003' + srec(1, 0, b'\x00', 2) + b'\x00\x07')
        self.assertTrue(any('after the last record' in p for p in problems))

    def test_srec_truncated_record(self):
        recs, problems = semc.srecords(b'S003' + srec(1, 0, b'\x00' * 8, 2)[:-3])
        self.assertTrue(any(p.startswith('bad record') for p in problems))

    def test_flatten_fills_gaps(self):
        lo, img = semc.flatten([(1, 4, b'ab'), (1, 8, b'c'), (8, None, b'')])
        self.assertEqual((lo, img), (4, b'ab\xff\xffc'))

    def test_flatten_empty(self):
        self.assertEqual(semc.flatten([(8, None, b'')]), (0, b''))


class Eka1Test(unittest.TestCase):
    def test_eka1_tree(self):
        rom = build_eka1({'EKern.exe': b'K' * 10, 'EUser.dll': b'U' * 3})
        h = semc.eka1_header(rom)
        self.assertEqual(h['rom_base'], 0x50000000)
        files, roots = semc.eka1_files(rom)
        self.assertEqual({f['path']: f['data'] for f in files}, {'System/EKern.exe': b'K' * 10, 'System/EUser.dll': b'U' * 3})

    def test_eka1_header_rejects_noise(self):
        self.assertIsNone(semc.eka1_header(bytes(0x100)))
        self.assertIsNone(semc.eka1_header(b'\xff' * 0x100))


P1I_CORE = os.path.join(ROOT, 'roms', 'SE-P1i', 'R6E30', 'BbyoneSW_CXC162058')


@unittest.skipUnless(os.path.exists(P1I_CORE), 'needs the P1i core in the ROM store')
class QuickCoreTest(unittest.TestCase):
    def test_root_list_beyond_the_first_kilobyte(self):
        """bug 2026-11-17 (P990i import): quick_core read 0x400 bytes, the SE cores' root list is at +0x1A90, so
        no SE core was recognised and store.py import stored nothing without a word"""
        head = open(P1I_CORE, 'rb').read(0x10000)
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(head)
            f.truncate(os.path.getsize(P1I_CORE))
        try:
            self.assertTrue(semc.quick_core(f.name))
        finally:
            os.unlink(f.name)


class Eka2LookalikeTest(unittest.TestCase):
    """2026-11-13: a real EROM ("Lily EROM CXC162096", 907861 bytes) was called core and then killed the
    ROM walker: its root list count reads as 1600114342 and the entry parse ran off the end of the file
    (struct.error). The first byte pattern must not be enough to call an image a ROM."""

    def test_erom_lookalike_is_not_core(self):
        d = build_eka2_lookalike(root_count=0xBEAD)
        self.assertIsNone(semc.eka2_header(d))
        tmp = tempfile.NamedTemporaryFile(suffix='EROM CXC162096.bin', delete=False)
        try:
            tmp.write(d)
            tmp.close()
            self.assertEqual(semc.classify(tmp.name)[0], 'semc-armimg')
        finally:
            os.unlink(tmp.name)

    def test_root_list_count_rejected_with_reason(self):
        with self.assertRaises(semc.romimg.RomError) as cm:
            list(semc.romimg.Rom(build_eka2_lookalike(root_count=0xBEAD)).walk())
        self.assertIn('root list count', str(cm.exception))

    def test_root_list_outside_the_file(self):
        d = build_eka2_lookalike(rootlist=0xF8000000 + 0x4000)
        with self.assertRaises(semc.romimg.RomError) as cm:
            list(semc.romimg.Rom(d).walk())
        self.assertIn('outside the image', str(cm.exception))

    def test_truncated_directory_stops_instead_of_crashing(self):
        d = build_eka2_lookalike(dir_size=0x10000)
        files = list(semc.romimg.Rom(d).walk())
        self.assertEqual([(f['path'], f['size'], f['addr']) for f in files], [('ab', 4, 0xF8000300)])

    def test_branch_table_alone_is_not_enough(self):
        self.assertFalse(semc.arm_image(struct.pack('<I', 0xEA00007E) + bytes(0x100)))
        self.assertTrue(semc.arm_image(struct.pack('<I', 0xEA00007E) + bytes(0x400)))


class EmmaTest(unittest.TestCase):
    SCRIPT = ('begin;\nencryption on;\n'
              'w_free 0x0002 0x079E 0x0000 0x01 0x08 0x00 0x00 0x4A 0x01 0x03 0x43 0x58 0x43 0x0A 0x02 0x52 0x31 0x00 0x0D;\n'
              'w_free 0x0002 0x06D4 0x0000 0x50 0x31 0x69 0x00;\nend;\n')

    def test_writes_and_versions(self):
        w = semc.emma(self.SCRIPT)
        self.assertEqual([(x['block'], x['unit']) for x in w], [(2, 0x079E), (2, 0x06D4)])
        v = semc.emma_versions(w)
        self.assertEqual(v, {'versions': {'01': 'CXC', '0a': 'R1'}, 'product': 'P1i'})


class ExtractTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, name, data):
        p = os.path.join(self.d, 'in', name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def test_safe_rel(self):
        self.assertEqual(semc.safe_rel('a\\b/c'), ['a', 'b', 'c'])
        for bad in ('../x', 'a/../../b', '', 'a/\0b'):
            with self.assertRaises(semc.SemcError):
                semc.safe_rel(bad)

    def test_merge_z_upper_wins_hidden_removes_first_spelling(self):
        out = os.path.join(self.d, 'out')
        lay = {}
        for lname, files in (('core', {'Sys/Bin/a.dll': b'core', 'Sys/Bin/gone.dll': b'g'}),
                             ('rofs1', {'sys/bin/A.DLL': b'rofs', 'sys/bin/new.exe': b'n'})):
            recs = []
            for p, v in files.items():
                recs.append(dict(path=p, file=semc.write_file(os.path.join(out, lname), p, v)))
            lay[lname] = recs
        lay['rofs1'].append(dict(path='sys/bin/gone.dll', hidden=True))
        n, winners = semc.merge_z(out, [('core', lay['core']), ('rofs1', lay['rofs1'])])
        self.assertEqual(n, 2)
        self.assertEqual(sorted(os.listdir(os.path.join(out, 'z'))), ['Sys'])
        self.assertEqual(sorted(os.listdir(os.path.join(out, 'z', 'Sys', 'Bin'))), ['A.DLL', 'new.exe'])
        self.assertEqual(open(os.path.join(out, 'z', 'Sys', 'Bin', 'A.DLL'), 'rb').read(), b'rofs')

    def test_rolf_not_merged_into_z(self):
        """2026-11-13: the ROLF (drive A: on the P910i) was merged into z/ with the ROM"""
        self.put('rom.bin', prefix(0x90000000, build_eka1({'EStart.exe': b'E' * 4})))
        rolf = build_rofs({'system': {'data': {'cda_version.txt': b'CDA162005/4 R5A02'}}}, b'ROLF')
        self.put('rolf.bin', prefix(0x91D00000, rolf))
        m = semc.extract([os.path.join(self.d, 'in')], os.path.join(self.d, 'out'))
        self.assertEqual(sorted(x['kind'] for x in m['inputs']), ['semc-eka1', 'semc-rolf'])
        self.assertEqual(m['z_layers'], {'core': 1})
        self.assertEqual(m['identity'].get('cda'), 'CDA162005/4')
        self.assertTrue(os.path.isfile(os.path.join(self.d, 'out', 'rolf-rolf.bin', 'system', 'data', 'cda_version.txt')))
        with open(os.path.join(self.d, 'out', 'manifest.json')) as f:
            json.load(f)

    def test_rofx_reads_data_from_rofs1(self):
        """a ROFx entry below its base points into ROFS1 (the image with that iMaxImageSize)"""
        r1 = bytearray(build_rofs({'x.dll': b'R1DATA'}))
        struct.pack_into('<I', r1, 0x2c, 0x1000)
        base, fl = rofs.files(bytes(r1))
        fa, fs, _ = fl[0]
        rx = bytearray(0x60)
        rx[0:4], rx[4] = b'ROFx', 0x30
        struct.pack_into('<I', rx, 8, 0x1000 + 0x30)                   # tree right after the header
        e = rofs_entry(False, 'x.dll', 0, fs, fa)
        struct.pack_into('<HBBII', rx, 0x30, 12, 0xff, 12, 0x1000 + 0x3c, len(e))
        rx[0x3c:0x3c + len(e)] = e
        recs = semc.rofs_files(bytes(rx), bytes(r1))
        self.assertEqual((recs[0]['data'], recs[0].get('in_rofs1')), (b'R1DATA', True))
        self.assertIsNone(semc.rofs_files(bytes(rx))[0]['data'])

    def test_hidden_rofs_entry(self):
        img = bytearray(build_rofs({'gone.rsc': b''}))
        base, fl = rofs.files(bytes(img))
        p = img.find('gone.rsc'.encode('utf-16le')) - 30 + 18 + 6     # the address field
        struct.pack_into('<I', img, p, 0xFFFFFFFF)
        self.assertTrue(semc.rofs_files(bytes(img))[0]['hidden'])

    def test_identity_utf16_version_and_productinfo(self):
        out = os.path.join(self.d, 'out')
        semc.write_file(os.path.join(out, 'z'), 'private/102030EE/version.txt',
                        '﻿PDA: CXC162108 R6E30'.encode('utf-16-le'))
        semc.write_file(os.path.join(out, 'rolf-x'), 'system/data/ProductInfo.ini',
                        b'//EModel\r\n4=959524969\r\n')                     # only "910i": not the name
        semc.write_file(os.path.join(out, 'rolf-x'), 'system/data/ProductInfo.rsc', b'\x00\x02P910i\x00')
        ident = semc.identity(out, dict(inputs=[]))
        self.assertEqual((ident['pda'], ident['version'], ident['product']), ('CXC162108', 'R6E30', 'P910i'))

    def test_identity_accepts_a_part_number_instead_of_a_cxc(self):
        """2026-11-13: the UIQ 3.3 phones (G700/G900 in the Emma archive) write a part number, not a
        CXC, as the software id (\"PDA: 1203-6566\"): the identity of the whole firmware came out empty."""
        out = os.path.join(self.d, 'out2')
        semc.write_file(os.path.join(out, 'z'), 'private/102030EE/version.txt', b'PDA: 1203-8787 R6J825\r\n')
        ident = semc.identity(out, dict(inputs=[]))
        self.assertEqual((ident['pda'], ident['version']), ('1203-8787', 'R6J825'))

    def test_classify_kinds(self):
        cases = {
            'rofs': build_rofs({'a': b'1'}),
            'babe': b'\xba\xbe\x00\x04' + bytes(60),
            'srec': b'S003' + srec(1, 0, b'\x01', 2) + b'S8\x00',
            'e32image': struct.pack('<4I', 0x1000007A, 0, 0x10208e68, 0) + b'EPOC' + bytes(60),
            'other': b'\x00\x80\x6e\x02' + bytes(100),
        }
        for kind, data in cases.items():
            self.assertEqual(semc.classify(self.put(kind, data))[0], kind, kind)
        self.assertEqual(semc.classify(self.put('Created Fri by GDFtoEMMA 1.31', EmmaTest.SCRIPT.encode()))[0], 'emma')


class PackageTest(unittest.TestCase):
    """identify.py / store.py records for Sony Ericsson packages (semc.package)"""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, rel, data):
        p = os.path.join(self.d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def p910(self, root):
        rom = build_eka1({'System': {'Data': {'version.txt': b'Baseline: x\rPDA:\rCXC162041 R5B01 050509'},
                                     'Libs': {'EKern.exe': b'K' * 8}}})
        rolf = build_rofs({'system': {'data': {'cda_version.txt': b'CDA162005/4 R5A02',
                                               'ProductInfo.rsc': b'\x00\x01P910i\x00'}}}, b'ROLF')
        lang = b'\0\0\0\0\xb0' + struct.pack('<I', 1024) + fat16_boot() + bytes(512)
        return [self.put(root + '/Symbian Flash/rom.bin', prefix(0x90000000, rom)),
                self.put(root + '/Language/rolf.bin', prefix(0x91D00000, rolf)),
                self.put(root + '/Language/lang.bin', lang + bytes(-(len(lang) + 0x20) % 0x1000)),
                self.put(root + '/AVR/modem.bin', b'S003' + srec(1, 0, b'\x01', 2) + b'S8\x00')]

    def test_quick_core(self):
        files = self.p910('P910')
        self.assertEqual([semc.quick_core(p) for p in files], [True, False, False, False])

    def test_owner_core_nearest_and_tie(self):
        cores = ['/r/P1/core', '/r/P910/Symbian Flash/rom.bin']
        self.assertEqual(semc.owner_core('/r/P910/Language/rolf.bin', cores), cores[1])
        self.assertEqual(semc.owner_core('/r/P1/rofs', cores), cores[0])
        self.assertIsNone(semc.owner_core('/r/other/x', cores))
        self.assertEqual(semc.owner_core('/anywhere/x', cores[:1]), cores[0])

    def test_package_p910_layout(self):
        recs = semc.package(self.p910('P910'))
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual((r['type_code'], r['sw_version'], r['problems']), ('SE-P910i', 'R5B01', []))
        roles = {os.path.basename(f['path']): (f['role'], f.get('id')) for f in r['files']}
        self.assertEqual(roles, {'rom.bin': ('core', None), 'rolf.bin': ('rofs3', 'CDA162005-4'),
                                 'lang.bin': ('uda', None), 'modem.bin': ('other', None)})

    def test_package_two_phones_side_by_side(self):
        """each phone's sibling folders go to its own core"""
        files = self.p910('A') + self.p910('B')
        recs = semc.package(files)
        self.assertEqual([len(r['files']) for r in recs], [4, 4])
        self.assertTrue(all(not r['problems'] for r in recs))

    def test_package_without_core_is_empty(self):
        self.assertEqual(semc.package([self.put('x/rolf.bin', prefix(0x91D00000, build_rofs({'a': b'1'}, b'ROLF')))]), [])

    def test_unreadable_lookalike_does_not_raise(self):
        d = bytearray(0x20)                    # a directory whose entries run past the end of the file
        d[0:4] = b'ROFS'
        struct.pack_into('<I', d, 8, 0x10)
        struct.pack_into('<HBBII', d, 0x10, 0xffff, 0xff, 12, 0, 0)
        struct.pack_into('<H', d, 0x1c, 0x40)
        bad = self.put('x/ROFS.bin', bytes(d))
        self.assertEqual(semc.classify_safe(bad)[0], 'unreadable')


class ProbeDirTest(unittest.TestCase):
    def test_probe_device_may_be_a_directory(self):
        """tools/fw/probe.py --device DIR: a firmware without a profile (semc.py extract output)"""
        import probe
        with tempfile.TemporaryDirectory() as d:
            for n in ('core-flat.img', 'core.json', 'rofs1.img', 'notrofs.img'):
                open(os.path.join(d, n), 'wb').close()
            p = probe.paths(d)
            self.assertEqual((p['core_flat'], p['rofs']), (os.path.join(d, 'core-flat.img'), [os.path.join(d, 'rofs1.img')]))


class RomcomposeRofsTest(unittest.TestCase):
    """tools/romcompose.py rofs_partitions (ROFS flash header v2)"""
    def setUp(self):
        import romcompose
        self.rp = romcompose.rofs_partitions

    def rofs(self, magic, size, max_size=0):
        d = bytearray(size)
        d[0:4] = magic
        struct.pack_into('<I', d, 44, max_size)
        return bytes(d)

    def test_rofx_joins_previous_partition_at_max_image_size(self):
        """2026-11-13: v2 (one drive per image) gave the P1i's ROFx a drive of its own; EROFS finds a ROFx only
        at ROFS1's iMaxImageSize in the same partition, so ROFS1 + ROFx must be one partition"""
        r1, rx, r3 = self.rofs(b'ROFS', 0x100, 0x1000), self.rofs(b'ROFx', 0x80), self.rofs(b'ROFS', 0x40, 0x40)
        parts = self.rp([('r1', r1), ('rx', rx), ('r3', r3)])
        self.assertEqual(len(parts), 2)
        self.assertEqual(bytes(parts[0][:0x100]), r1)
        self.assertEqual(bytes(parts[0][0x100:0x1000]), b'\xff' * 0xf00)
        self.assertEqual(bytes(parts[0][0x1000:]), rx)
        self.assertEqual(bytes(parts[1]), r3)

    def test_rofx_after_oversized_image_rejected(self):
        with self.assertRaises(SystemExit):
            self.rp([('r1', self.rofs(b'ROFS', 0x200, 0x100)), ('rx', self.rofs(b'ROFx', 0x40))])

    def test_not_rofs_rejected(self):
        with self.assertRaises(SystemExit):
            self.rp([('x', b'XXXX' + bytes(60))])

    def test_leading_rofx_is_its_own_partition(self):
        self.assertEqual(len(self.rp([('rx', self.rofs(b'ROFx', 0x40))])), 1)


class IdentitySeTest(unittest.TestCase):
    def test_se_version_txt_is_sw(self):
        """tools/fw/identity.py rofs_versions: the UIQ 3 version file private/102030EE/version.txt"""
        import identity
        img = build_rofs({'private': {'102030EE': {'version.txt': b'PDA: CXC162108 R6E30'}}})
        self.assertEqual(identity.rofs_versions(img), {'sw': {'version': 'R6E30', 'pda': 'CXC162108'}})


class PersonalitySym91rTest(unittest.TestCase):
    MAPS = os.path.join(ROOT, 'personalities')

    def lines(self, name):
        out = {}
        for l in open(os.path.join(self.MAPS, name, 'exec.map')):
            l = l.split('#', 1)[0].split()
            if l:
                out[l[0]] = l[1]
        return out

    def test_sym91r_differs_only_in_tls(self):
        """2026-11-13: the P1i's RVCT EUSER passes the 9.2 TLS arguments; the sym91 shims set DRTAEABI's
        exception globals to NULL and the first leave faulted (__cxa_allocate_exception, address 0x8)"""
        a, b = self.lines('sym91'), self.lines('sym91r')
        self.assertEqual(sorted(a), sorted(b))
        diff = {k: (a[k], b[k]) for k in a if a[k] != b[k]}
        self.assertEqual(diff, {'S4d': ('@DllTls91:DllTls', 'DllTls'), 'S75': ('@DllSetTls91:DllSetTls', 'DllSetTls')})

    def test_sym91r_registered(self):
        import tomllib
        with open(os.path.join(self.MAPS, 'registry.toml'), 'rb') as f:
            ids = tomllib.load(f)['ids']
        self.assertEqual(ids['sym91r'], 9)
        self.assertEqual(len(set(ids.values())), len(ids))
        self.assertTrue(all(0 <= v < 16 for v in ids.values()))       # 4-bit ROM header field

    def test_table_tags_unique(self):
        """sym91r's tables must not collide with sym91's (the generator tags by the name's digits)"""
        import subprocess
        execs = os.path.join(ROOT, 'build', 'epocroot', 'sf', 'os', 'kernelhwsrv', 'kernel', 'eka', 'kernel', 'execs.txt')
        if not os.path.exists(execs):
            self.skipTest('no kernel tree')
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, 'h.h')
            subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'personality', 'gen_exec_map.py'), execs, out,
                            'sym91=' + os.path.join(self.MAPS, 'sym91', 'exec.map'),
                            'sym91r=' + os.path.join(self.MAPS, 'sym91r', 'exec.map'),
                            'symbian3=' + os.path.join(self.MAPS, 'symbian3', 'exec.map')], check=True,
                           stdout=subprocess.DEVNULL)
            h = open(out).read()
        for tag in ('91', '91R', '3'):
            self.assertEqual(h.count('static const TInt16 KExec%sFast[' % tag), 1, tag)


if __name__ == '__main__':
    unittest.main()
