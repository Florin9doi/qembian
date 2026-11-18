#!/usr/bin/env python3
r"""Sony Ericsson (SEMC) firmware files: identify and extract them, without running anything
(untrusted input: run with python3 -I; mtools reads the FAT images).

  semc.py identify PATH... [--json]      what each file is (PATH: files or directories, searched recursively)
  semc.py extract PATH... -o OUT         every layer's files -> OUT/<layer>/..., the merged Z: tree -> OUT/z/
                                         (hard links), modem images flattened, OUT/manifest.json

Seen so far: P1i (UIQ 3.0, Symbian 9.1, EKA2, R6E30 Trade_Russia) and P910i (UIQ 2.1, Symbian 7.0,
EKA1, R5B01 EU-EMEA-APAC). Formats, marked [verified] (read from those files) or [inferred]:

P1i (one file per NAND image, no container around the Symbian images):
  core        EKA2 XIP ROM, TRomHeader at offset 0 (BbyoneSW_CXC162058: romBase 0xF8000000, kernel data
              0x64000000 = moving memory model, ekern.exe inside) [verified]; read with tools/romimg.py
  rofs        ROFS1 (Rofs1Ext_*; iMaxImageSize u32 @0x2c) and the customer ROFS (ACPU_CDA*_ROFS) [verified]
  rofx        ROFS extension (ACPU_SW_*_ROF, magic ROFx): offsets count from ROFS1's iMaxImageSize, entries
              may point at data inside ROFS1 [verified]; tools/rofs.py
  fat         a raw FAT image (EPOC_*: FAT32, OEM "EPOC") [verified]; which drive it is: [inferred] the
              phone's C:/internal disk
  e32image    E32 executables outside the ROM (EPOC2 = USB NAND flash loader "Suzi USB flash loader
              (XSR)", EPOC3 = ISP loader) [verified from their strings]: flashing helpers, not phone files
  babe        Ericsson BABE container (magic BA BE, CCPU modem software and the flash loaders): only the
              header is read, the payload starts at 0x224 after 0xFF fill [verified for the 4 files];
              field meanings [inferred], the payload looks signed/encrypted. The modem is faked, not run.
  emma        GDFtoEMMA scripts ("Created ... by GDFtoEMMA"): `w_free BLOCK UNIT INDEX bytes...;` writes
              to the modem's GDFS parameter store [verified syntax]. Block 2 unit 0x079E holds the
              software versions as tag/length/value [verified values, inferred tag meanings], unit 0x06D4
              the product name ("P1i"), 0x06D3 the product number [inferred]
  gdfs        GDFS service scripts (plain text) [verified]
  other       e.g. Camara_Software (8051 code for the camera module) [inferred from the opcodes]

P910i (each file: a small header, the payload, zero padding so that size + 0x20 is a 4 KB multiple
[verified for all 6 .bin files]):
  semc-eka1   [u32 flash address 0x90000000][u32 length] + EKA1 XIP ROM (TRomHeader of Symbian 6.1/7.0:
              rom_base @0x8C = 0x50000000, root directory list @0x94) [verified]; walked with the N-Gage
              parser (eka1/tools/eka1_extract.py)
  semc-rolf   [u32 flash address 0x91D00000][u32 length] + ROLF image (tools/rofs.py) [verified]: drive A:
              (EStart: EAROLF.fsy on ROLFDRV.PDD, A:\System\Data\productinfo.ini) [verified]
  semc-fat    [u32 0][u8 0xB0][u32 length] + the start of a FAT16 volume (64 MB file system, 9 MB of
              it in the file) [verified]; the 0xB0 byte's meaning [inferred: an image type]; C: with the
              language's documents [inferred]
  srec        "S003" then binary S-records: 'S', type digit, count byte, then count bytes (address 2/3/4
              bytes BE for S1/S2/S3, data, checksum = ~sum(count..data)); 'S8' with count 0 ends the file
              [verified]. The *_avr files: the GSM modem's software (OSE process names) and its GDFS
              backup; flattened to one image each (0xFF fill)
  sis         EKA1 SIS install packages (kept as they are)
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys

FW = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(FW)
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(ROOT, 'eka1', 'tools'))
import rofs as rofsmod      # tools/rofs.py
import romimg               # tools/romimg.py

PAD_TAIL = 0x20             # P910i files: size + 0x20 is a multiple of 4 KB [verified, purpose unknown]
EMMA_NAME = re.compile(r'^Created .* by GDFtoEMMA')


class SemcError(Exception):
    pass


def slurp(path):
    with open(path, 'rb') as f:
        return f.read()


def sha256(b):
    return hashlib.sha256(b).hexdigest()


# --------------------------------------------------------------------------------------------- headers

def prefixed(d):
    """(header length, flash address, payload) of a P910i-style file, or None"""
    if len(d) >= 17 and d[:4] == b'\0\0\0\0' and d[9 + 510:9 + 512] == b'\x55\xaa' and d[9] in (0xE9, 0xEB):
        n = struct.unpack_from('<I', d, 5)[0]                       # [u32 0][u8 tag][u32 length] + FAT
        if 9 + n <= len(d):
            return 9, 0, d[9:9 + n]
    if len(d) >= 16:
        addr, n = struct.unpack_from('<II', d, 0)
        if 0 < n and 8 + n <= len(d) and not any(d[8 + n:8 + n + 64]) and d[8 + n:].count(0) == len(d) - 8 - n:
            return 8, addr, d[8:8 + n]
    return None


def eka1_header(d):
    """EKA1 TRomHeader fields (offsets as in eka1/tools/eka1_extract.py read_eka1_header), or None"""
    if len(d) < 0x100:
        return None
    base, size, root, kdata, klimit, prim, sec, csum = struct.unpack_from('<8I', d, 0x8C)
    if base & 0xFFF or not 0x1000 <= size <= 0x8000000 or not base <= root < base + size:
        return None
    if not base <= prim < base + size:
        return None
    return dict(rom_base=base, rom_size=size, root=root, kern_data=kdata, kern_limit=klimit,
                primary=prim, secondary=sec, checksum=csum)


def eka2_header(d):
    """romimg.Rom of an EKA2 XIP ROM at offset 0, or None"""
    if len(d) < 0x200:
        return None
    try:
        r = romimg.Rom(d)
    except Exception:
        return None
    if r.base & 0xFFF or not r.base <= r.rootlist < r.base + r.size or r.hdrsize not in (0x200, 0x100):
        return None
    return r


def fat_boot(b):
    """FAT boot sector facts or None"""
    if len(b) < 512 or b[510:512] != b'\x55\xaa' or b[0] not in (0xE9, 0xEB):
        return None
    bps, spc, res, nfats, roots, tot16, media, fsz16 = struct.unpack_from('<HBHBHHBH', b, 11)
    tot32, fsz32 = struct.unpack_from('<I', b, 32)[0], struct.unpack_from('<I', b, 36)[0]
    if bps not in (512, 1024, 2048, 4096) or not spc or not nfats:
        return None
    fat32 = fsz16 == 0
    return dict(type='FAT32' if fat32 else ('FAT16' if b[54:59] == b'FAT16' else 'FAT12/16'),
                oem=b[3:11].decode('latin1').strip('\0 '), sector=bps, cluster=bps * spc, reserved=res,
                fats=nfats, roots=roots, fat_sectors=fsz32 if fat32 else fsz16, bytes=(tot16 or tot32) * bps,
                label=(b[71:82] if fat32 else b[43:54]).decode('latin1').strip('\0 '))


def fat_used_end(img, f):
    """byte offset just past the highest cluster the first FAT marks as used (what the file must hold)"""
    fat = img[f['reserved'] * f['sector']:(f['reserved'] + f['fat_sectors']) * f['sector']]
    if f['type'] == 'FAT32':
        ent = struct.unpack_from('<%dI' % (len(fat) // 4), fat)
        ent = [e & 0x0FFFFFFF for e in ent]
        root_bytes = 0
    else:
        ent = struct.unpack_from('<%dH' % (len(fat) // 2), fat)
        root_bytes = f['roots'] * 32
    data0 = (f['reserved'] + f['fats'] * f['fat_sectors']) * f['sector'] + root_bytes
    used = [i for i, e in enumerate(ent) if i >= 2 and e]
    hi = max(used) if used else 1
    return data0 + (hi - 1) * f['cluster'], len(used)


def srecords(d):
    """binary S-records after 'S003' -> (records, problems); a record: (type, address, data)"""
    if d[:4] != b'S003':
        raise SemcError('no S003 header')
    p, out, problems = 4, [], []
    alen = {1: 2, 2: 3, 3: 4, 7: 4, 8: 3, 9: 2}
    while p + 3 <= len(d):
        if d[p] != 0x53:
            if any(d[p:]):
                problems.append('data after the last record at 0x%x' % p)
            break
        t, c = d[p + 1] - 0x30, d[p + 2]
        if t in (7, 8, 9) and c == 0:                             # the end record, no address/checksum
            out.append((t, None, b''))
            p += 3
            continue
        body = d[p + 2:p + 3 + c]
        if len(body) != c + 1 or t not in alen or c < alen[t] + 1:
            problems.append('bad record at 0x%x' % p)
            break
        if (sum(body[:-1]) + body[-1]) & 0xFF != 0xFF:
            problems.append('checksum at 0x%x' % p)
        a = int.from_bytes(body[1:1 + alen[t]], 'big')
        out.append((t, a, bytes(body[1 + alen[t]:-1])))
        p += 3 + c
    if p + 3 > len(d) and any(d[p:]):
        problems.append('data after the last record at 0x%x' % p)
    return out, problems


def flatten(recs):
    """(start address, image) of the data records, 0xFF where nothing is written"""
    data = [(a, b) for t, a, b in recs if t in (1, 2, 3)]
    if not data:
        return 0, b''
    lo = min(a for a, _ in data)
    hi = max(a + len(b) for a, b in data)
    img = bytearray(b'\xff' * (hi - lo))
    for a, b in data:
        img[a - lo:a - lo + len(b)] = b
    return lo, bytes(img)


def babe_header(d):
    """header of an Ericsson BABE file (fields named by position: meanings [inferred])"""
    p = 0x18
    while p < len(d) and d[p] == 0xFF:
        p += 1
    return dict(magic=d[:2].hex(), version=struct.unpack_from('>H', d, 2)[0], header=d[:0x18].hex(),
                payload_offset=p, payload_size=len(d) - p, payload_head=d[p:p + 32].hex())


def emma(text):
    """GDFtoEMMA / GDFS script -> list of writes {block, unit, index, data(hex)} and comment header"""
    writes = []
    for m in re.finditer(r'^\s*w_free\s+([^;]+);', text, re.M):
        v = [int(x, 16) for x in m.group(1).split()]
        writes.append(dict(block=v[0], unit=v[1], index=v[2], data=bytes(v[3:]).hex()))
    return writes


def emma_versions(writes):
    """block 2 unit 0x079E (tag/length/value software versions) and the product name/number units"""
    out = {}
    for w in writes:
        b = bytes.fromhex(w['data'])
        if (w['block'], w['unit']) == (2, 0x079E):
            p, tags = 5, {}          # 01 08 00 00 <n>, then tag, length, ASCII value [inferred layout]
            while p + 2 <= len(b) and b[p]:
                t, n = b[p], b[p + 1]
                v = b[p + 2:p + 2 + n]
                if not v or not all(32 <= c < 127 for c in v):
                    break
                tags['%02x' % t] = v.decode('ascii')
                p += 2 + n
            out['versions'] = tags
        elif (w['block'], w['unit']) == (2, 0x06D4):
            out['product'] = b.split(b'\0')[0].decode('latin1')
        elif (w['block'], w['unit']) == (2, 0x06D3):
            out['product_number'] = b.split(b'\0')[0].decode('latin1')
    return out


# --------------------------------------------------------------------------------------------- identify

def classify(path):
    """(kind, facts) of one file"""
    name = os.path.basename(path)
    with open(path, 'rb') as f:
        d = f.read()
    facts = dict(name=name, size=len(d), sha256=sha256(d))
    if d[:4] in (b'ROFS', b'ROFx'):
        base, fl = rofsmod.files(d)
        facts.update(entries=len(fl), max_image_size=struct.unpack_from('<I', d, 0x2c)[0] if d[:4] == b'ROFS' else None,
                     base=base)
        return ('rofx' if d[:4] == b'ROFx' else 'rofs'), facts
    if d[:2] == b'\xba\xbe':
        facts.update(babe_header(d))
        return 'babe', facts
    if d[:4] == b'S003':
        recs, problems = srecords(d)
        lo, img = flatten(recs)
        facts.update(records=len(recs), start=lo, end=lo + len(img), problems=problems)
        return 'srec', facts
    if d[16:20] == b'EPOC' and struct.unpack_from('<I', d, 0)[0] in (0x1000007A, 0x10000079):
        facts.update(uid3='0x%08x' % struct.unpack_from('<I', d, 8)[0])
        return 'e32image', facts
    if d[:4] == b'\x19\x04\x00\x10' or name.lower().endswith('.sis'):
        return 'sis', facts
    f = fat_boot(d[:512])
    if f:
        facts.update(fat=f)
        return 'fat', facts
    r = eka2_header(d)
    if r and r.size >= len(d) // 2:
        facts.update(rom_base=r.base, rom_size=r.size, kern_data=r.kerndata, kern_limit=r.kernlimit)
        return 'core', facts
    pre = prefixed(d)
    if pre:
        hlen, addr, body = pre
        facts.update(header=hlen, flash_address=addr, payload=len(body),
                     padding_ok=(len(d) + PAD_TAIL) % 0x1000 == 0)
        h = eka1_header(body)
        if h:
            facts.update(h)
            return 'semc-eka1', facts
        if body[:4] == b'ROLF':
            facts.update(entries=len(rofsmod.files(body)[1]))
            return 'semc-rolf', facts
        f = fat_boot(body[:512])
        if f:
            facts.update(fat=f)
            return 'semc-fat', facts
    if EMMA_NAME.match(name) or d.lstrip()[:1] == b';':
        text = d.decode('latin1')
        if 'begin;' in text:
            w = emma(text)
            facts.update(writes=len(w), **emma_versions(w))
            return ('emma' if EMMA_NAME.match(name) else 'gdfs'), facts
    return 'other', facts


def inputs(paths):
    for p in paths:
        if os.path.isdir(p):
            for dp, dn, fn in os.walk(p):
                dn.sort()
                for n in sorted(fn):
                    yield os.path.join(dp, n)
        else:
            yield p


# --------------------------------------------------------------------------------------------- extract

def safe_rel(path):
    parts = [x for x in path.replace('\\', '/').split('/') if x not in ('', '.')]
    if not parts or any(x == '..' or '\0' in x for x in parts):
        raise SemcError('unsafe path in image: %r' % path)
    return parts


def write_file(root, path, data):
    fn = os.path.join(root, *safe_rel(path))
    os.makedirs(os.path.dirname(fn), exist_ok=True)
    with open(fn, 'wb') as f:
        f.write(data)
    return fn


def eka2_files(d):
    r = romimg.Rom(d)
    out = []
    for e in r.walk():
        off = e['addr'] - r.base
        ok = 0 <= off and off + e['size'] <= len(d)
        out.append(dict(path=e['path'], address=e['addr'], size=e['size'], att=e['att'],
                        data=d[off:off + e['size']] if ok else None))
    return out


def eka1_files(body):
    import eka1_extract as E      # the N-Gage parser (directory walk), unchanged
    h = eka1_header(body)
    img = E.Image('rom', h['rom_base'], bytearray(body), [], [])
    ents, roots = E.walk_tree(img, h['root'])
    out = []
    for e in ents:
        if e.is_dir:
            continue
        off = e.address - h['rom_base']
        ok = 0 <= off and off + e.size <= len(body)
        out.append(dict(path=e.path, address=e.address, size=e.size, att=e.attributes,
                        data=bytes(body[off:off + e.size]) if ok else None))
    return out, roots


def rofs_files(d, rofs1=None):
    """files of a ROFS/ROFx/ROLF image; ROFx entries whose data lives in ROFS1 are read from rofs1
    (the ROFS1 image bytes) when given. Address 0xFFFFFFFF with size 0 = a hidden file (it removes the
    lower layer's file) [verified: ACPU_CDA ROFS lists such entries; meaning inferred]."""
    base, fl = rofsmod.files(d)
    out = []
    for fa, fs, path in fl:
        rec = dict(path=path, address=fa, size=fs)
        if fa == 0xFFFFFFFF:
            rec.update(hidden=True, data=None)
        elif fa >= base and fa - base + fs <= len(d):
            rec['data'] = d[fa - base:fa - base + fs]
        elif fa < base and rofs1 is not None and fa + fs <= len(rofs1):
            rec.update(data=rofs1[fa:fa + fs], in_rofs1=True)
        else:
            rec['data'] = None
        out.append(rec)
    return out


def fat_extract(payload, facts, img_path, out_dir):
    """sparse image of the whole file system + the files (mtools)"""
    with open(img_path, 'wb') as f:
        f.truncate(max(facts['bytes'], len(payload)))
        f.write(payload)
    end, used = fat_used_end(payload, facts)
    facts.update(used_clusters=used, used_end=end, complete=end <= len(payload))
    if not shutil.which('mcopy'):
        return ['(mtools not installed: image only)']
    os.makedirs(out_dir, exist_ok=True)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', MTOOLS_NO_VFAT='0')
    subprocess.run(['mcopy', '-s', '-n', '-m', '-p', '-i', img_path, '::/*', out_dir], env=env,
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False)
    files = []
    for dp, dn, fn in os.walk(out_dir):
        for n in fn:
            p = os.path.join(dp, n)
            files.append(dict(path=os.path.relpath(p, out_dir), size=os.path.getsize(p)))
    return files


LAYER_ORDER = {'core': 0, 'semc-eka1': 0, 'rofs1': 1, 'rofx': 2, 'rofs': 3}    # Z: layers, bottom first
# not in Z: a ROLF image is drive A: of its own (P910i EStart mounts EAROLF.fsy on ROLFDRV.PDD and reads
# A:\System\Data\productinfo.ini, which the ROLF holds) [verified]


def merge_z(out, layers):
    """OUT/z: the Z: drive as the phone's file server would see it (later layers win, names compared
    case-insensitively, the first spelling of a directory kept); hard links into the layer trees"""
    z = os.path.join(out, 'z')
    shutil.rmtree(z, ignore_errors=True)
    spell, final = {}, {}
    for lname, recs in layers:
        for r in recs:
            parts = safe_rel(r['path'])
            key = tuple(x.lower() for x in parts)
            for i in range(1, len(parts)):
                spell.setdefault(key[:i], parts[i - 1])
            if r.get('hidden'):
                final.pop(key, None)
            elif r.get('file'):
                final[key] = (parts[-1], r['file'], lname)
    winners = {}
    for key, (leaf, src, lname) in sorted(final.items()):
        dst = os.path.join(z, *[spell[key[:i]] for i in range(1, len(key))], leaf)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        try:
            os.link(src, dst)
        except OSError:
            shutil.copyfile(src, dst)
        winners[lname] = winners.get(lname, 0) + 1
    return len(final), winners


VERSION_RE = re.compile(r'(CXC\d+)\s*(R\w+)')
CDA_RE = re.compile(r'(CDA[\d/]+)\s*(R\w+)')


PRODUCT_RE = re.compile(rb'\b([A-Z]{1,2}\d{3,4}[a-z]?)\b')


def text_of(b):
    if b[:2] in (b'\xff\xfe', b'\xfe\xff') or (len(b) > 3 and b[1] == 0):
        return b.decode('utf-16', errors='replace')
    return b.decode('latin1')


def tree_find(root, rel):
    """case-insensitive lookup of a relative path under root, or None"""
    cur = root
    for part in rel.split('/'):
        if not os.path.isdir(cur):
            return None
        hit = [n for n in os.listdir(cur) if n.lower() == part.lower()]
        if not hit:
            return None
        cur = os.path.join(cur, hit[0])
    return cur


def identity_from(find, inputs):
    """the firmware's identity from its own files [verified on P1i R6E30 and P910i R5B01]:
    PDA software (CXC number + revision) in version.txt (9.1: Z:\\private\\102030EE, 7.0:
    Z:\\System\\Data), customisation (CDA) in cda_version.txt (Z: on 9.1, A: = ROLF on 7.0),
    product from the EMMA scripts or the ROLF's System\\Data\\ProductInfo.rsc ("P910i"; ProductInfo.ini
    has only the EModel "910i"). find(rel) -> the bytes of that file (Z: first, then A:) or None;
    inputs: classify() facts of the package's files."""
    ident = {}
    for rel in ('private/102030EE/version.txt', 'System/Data/version.txt'):
        b = find(rel)
        m = b and VERSION_RE.search(text_of(b))
        if m:
            ident.update(pda=m.group(1), version=m.group(2), version_text=text_of(b).strip())
            break
    for rel in ('private/102030EE/cda_version.txt', 'System/Data/cda_version.txt'):
        b = find(rel)
        m = b and CDA_RE.search(text_of(b))
        if m:
            ident.update(cda=m.group(1), cda_version=m.group(2))
            break
    for x in inputs:
        if x.get('product'):
            ident.setdefault('product', x['product'])
        if x.get('product_number'):
            ident.setdefault('product_number', x['product_number'])
        if x.get('versions'):
            ident.setdefault('component_versions', x['versions'])
    b = find('System/Data/ProductInfo.rsc')
    m = b and PRODUCT_RE.search(b)
    if m:
        ident.setdefault('product', m.group(1).decode('ascii'))
    return ident


def identity(out, manifest):
    """identity_from() over an extraction: z/, then the rolf-* trees (drive A:)"""
    roots = [os.path.join(out, 'z')] + [os.path.join(out, n) for n in sorted(os.listdir(out)) if n.startswith('rolf-')]

    def find(rel):
        for root in roots:
            p = tree_find(root, rel)
            if p and os.path.isfile(p):
                return slurp(p)
        return None
    return identity_from(find, manifest['inputs'])


CORE_KINDS = ('core', 'semc-eka1')
STORE_ROLE = {'core': 'core', 'semc-eka1': 'core', 'rofx': 'core', 'rofs1': 'core', 'rofs': 'rofs3',
              'semc-rolf': 'rofs3', 'fat': 'uda', 'semc-fat': 'uda'}


def quick_core(path):
    """True when the file starts like an SE core image (EKA2 TRomHeader at 0, or the P910i
    [address][length] prefix before an EKA1 TRomHeader): a cheap test before package()"""
    try:
        with open(path, 'rb') as f:
            # the root list must be readable: the P1i's and P990i's is at +0x1A90 (bug 2026-11-17: with
            # 0x400 bytes no SE core was recognised and store.py import stored nothing, silently)
            d = f.read(0x10000)
        size = os.path.getsize(path)
    except OSError:
        return False
    r = eka2_header(d)
    if r and size <= r.size <= 4 * size:
        return True
    return len(d) >= 0x108 and eka1_header(d[8:]) is not None and struct.unpack_from('<I', d, 4)[0] <= size


def classify_safe(path):
    try:
        return classify(path)
    except Exception as e:                          # untrusted bytes: a reader may trip on a lookalike
        return 'unreadable', dict(name=os.path.basename(path), size=os.path.getsize(path), problem=str(e))


def owner_core(path, cores):
    """the core a file belongs to: the only one, else the one sharing the longest directory prefix
    with it (the SE packages keep the ROM, the language/ROLF files and the modem in sibling folders);
    None on a tie"""
    if len(cores) == 1:
        return cores[0]
    d = os.path.dirname(os.path.abspath(path))
    score = sorted(((len(os.path.commonpath([d, os.path.dirname(os.path.abspath(c))])), c) for c in cores), reverse=True)
    return score[0][1] if score[0][0] > score[1][0] else None


def package(paths):
    """Sony Ericsson firmwares among these files, as tools/fw/identify.py records (type code SE-<product>,
    software version = the PDA revision, store roles: core ROM/ROFS1/ROFx -> core, customer ROFS and ROLF
    -> rofs3 with the CDA as id, FAT drives -> uda, the rest -> other). Files belong to the only SE core
    among them, else to the nearest one (owner_core). Nothing is written to disk."""
    found = [(p,) + classify_safe(p) for p in paths]
    cores = [f for f in found if f[1] in CORE_KINDS]
    if not cores:
        return []
    groups, orphans = {c[0]: [] for c in cores}, []
    for f in found:
        if f[1] in ('other', 'unreadable') and not any(os.path.dirname(f[0]) == os.path.dirname(c[0]) for c in cores):
            continue                                # a stranger in another directory (a Nokia .vpl, a readme)
        owner = owner_core(f[0], [c[0] for c in cores])
        if owner:
            groups[owner].append(f)
        else:
            orphans.append(f[2]['name'])
    out = []
    for core_path, fl in groups.items():
        rofs1_size = {f[2].get('max_image_size') for f in fl if f[1] == 'rofs'} & {f[2].get('base') for f in fl if f[1] == 'rofx'}
        zl, al = [], []
        for p, kind, facts in fl:
            d = slurp(p) if kind in ('core', 'semc-eka1', 'rofs', 'rofx', 'semc-rolf') else None
            if kind == 'core':
                zl.append((0, eka2_files(d)))
            elif kind == 'semc-eka1':
                zl.append((0, eka1_files(prefixed(d)[2])[0]))
            elif kind == 'rofs':
                zl.append((1 if facts.get('max_image_size') in rofs1_size else 3, rofs_files(d)))
            elif kind == 'rofx':
                r1 = next((slurp(q) for q, k, x in fl if k == 'rofs' and x.get('max_image_size') == facts['base']), None)
                zl.append((2, rofs_files(d, r1)))
            elif kind == 'semc-rolf':
                al.append(rofs_files(prefixed(d)[2]))
        files = {}
        for _, recs in sorted(zl, key=lambda x: x[0]):
            for r in recs:
                files[r['path'].lower()] = None if r.get('hidden') else r.get('data')
        afiles = {r['path'].lower(): r.get('data') for recs in al for r in recs}

        def find(rel, files=files, afiles=afiles):
            k = rel.lower()
            return files.get(k) if files.get(k) is not None else afiles.get(k)
        ident = identity_from(find, [x for _, _, x in fl])
        prod = re.sub(r'[^\w.-]', '_', ident.get('product', ''))
        rec = dict(type_code=('SE-' + prod) if prod else '', sw_version=ident.get('version', ''),
                   date='', product=ident.get('product', ''), confidence='version.txt' if 'pda' in ident else 'none',
                   products=[], identity=ident, files=[], problems=[])
        for p, kind, facts in fl:
            role = STORE_ROLE.get('rofs1' if kind == 'rofs' and facts.get('max_image_size') in rofs1_size else kind, 'other')
            if kind == 'unreadable':
                role = 'unreadable'
            f = dict(name=facts['name'], role=role, kind=kind, path=p)
            if role == 'rofs3' and ident.get('cda'):
                f['id'] = ident['cda'].replace('/', '-')
            if facts.get('problem'):
                f['problem'] = facts['problem']
            rec['files'].append(f)
        if not rec['sw_version']:
            rec['problems'].append('no version.txt found in the ROM layers')
        if orphans:
            rec['problems'].append('files no single core claims (import each phone on its own): ' + ', '.join(orphans))
        out.append(rec)
    return out


def write_probe_inputs(d, out):
    """core-flat.img + core.json (tools/romimg.py --images --json) for tools/fw/probe.py --device OUT"""
    flat = os.path.join(out, 'core-flat.img')
    with open(flat, 'wb') as f:
        f.write(d)
    subprocess.run([sys.executable, '-I', os.path.join(TOOLS, 'romimg.py'), flat, '--images', '--json',
                    os.path.join(out, 'core.json')], check=True, stdout=subprocess.DEVNULL)


def extract(paths, out):
    os.makedirs(out, exist_ok=True)
    found = [(p,) + classify(p) for p in inputs(paths)]
    rofs1 = {}                                      # iMaxImageSize -> bytes of that ROFS1 image
    for p, kind, facts in found:
        if kind == 'rofs' and facts.get('max_image_size'):
            rofs1[facts['max_image_size']] = slurp(p)
    rofx_bases = {facts['base'] for p, kind, facts in found if kind == 'rofx'}
    manifest = dict(tool='tools/fw/semc.py', inputs=[], layers={}, problems=[])
    zlayers = []
    for p, kind, facts in found:
        d = slurp(p)
        entry = dict(path=os.path.relpath(p), kind=kind, **{k: v for k, v in facts.items()})
        stem = re.sub(r'[^A-Za-z0-9._+-]+', '_', facts['name'])
        recs, layer = None, None
        if kind == 'core':
            recs, layer = eka2_files(d), 'core'
            write_probe_inputs(d, out)
            entry['probe'] = 'core-flat.img, core.json (tools/fw/probe.py --device %s)' % out
        elif kind == 'semc-eka1':
            body = prefixed(d)[2]
            recs, roots = eka1_files(body)
            entry['roots'] = roots
            layer = 'core'
            with open(os.path.join(out, 'core-eka1.img'), 'wb') as f:      # the ROM as at rom_base
                f.write(body)
            entry['rom_image'] = 'core-eka1.img'
        elif kind == 'rofs' and facts.get('max_image_size') in rofx_bases:
            recs, layer = rofs_files(d), 'rofs1'
        elif kind == 'rofs':
            recs, layer = rofs_files(d), 'rofs-' + stem
        elif kind == 'rofx':
            recs, layer = rofs_files(d, rofs1.get(facts['base'])), 'rofx'
        elif kind == 'semc-rolf':
            recs, layer = rofs_files(prefixed(d)[2]), 'rolf-' + stem
        elif kind in ('fat', 'semc-fat'):
            payload = d if kind == 'fat' else prefixed(d)[2]
            img = os.path.join(out, 'drive-%s.img' % stem)
            entry['files'] = fat_extract(payload, entry['fat'], img, os.path.join(out, 'drive-' + stem))
            entry['image'] = os.path.relpath(img, out)
            if not entry['fat'].get('complete', True):
                manifest['problems'].append('%s: the FAT marks clusters past the end of the file' % facts['name'])
        elif kind == 'srec':
            lo, img = flatten(srecords(d)[0])
            fn = os.path.join(out, 'modem', '%s.0x%x.bin' % (stem, lo))
            os.makedirs(os.path.dirname(fn), exist_ok=True)
            with open(fn, 'wb') as f:
                f.write(img)
            entry['image'] = os.path.relpath(fn, out)
        if recs is not None:
            root = os.path.join(out, layer)
            shutil.rmtree(root, ignore_errors=True)
            missing = 0
            for r in recs:
                if r.get('data') is not None:
                    r['file'] = write_file(root, r['path'], r['data'])
                    r['sha256'] = sha256(r['data'])
                elif not r.get('hidden'):
                    missing += 1
            if missing:
                manifest['problems'].append('%s: %d files without data in this image' % (facts['name'], missing))
            entry.update(layer=layer, files_written=sum(1 for r in recs if r.get('file')), missing=missing)
            manifest['layers'][layer] = [{k: v for k, v in r.items() if k not in ('data', 'file')} for r in recs]
            order = LAYER_ORDER.get('rofs1' if layer == 'rofs1' else kind)
            if order is not None:
                zlayers.append((order, layer, recs))
        manifest['inputs'].append(entry)
    zlayers.sort(key=lambda x: x[0])
    if zlayers:
        manifest['z_files'], manifest['z_layers'] = merge_z(out, [(l, r) for _, l, r in zlayers])
    manifest['identity'] = identity(out, manifest)
    with open(os.path.join(out, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1, default=lambda o: o.hex() if isinstance(o, bytes) else str(o))
    return manifest


# --------------------------------------------------------------------------------------------- main

def describe(kind, f):
    if kind in ('core',):
        return 'EKA2 XIP ROM base 0x%08x size 0x%x kernel data 0x%08x' % (f['rom_base'], f['rom_size'], f['kern_data'])
    if kind == 'semc-eka1':
        return 'EKA1 XIP ROM base 0x%08x size 0x%x (flash 0x%08x, payload 0x%x)' % (
            f['rom_base'], f['rom_size'], f['flash_address'], f['payload'])
    if kind in ('rofs', 'rofx', 'semc-rolf'):
        return '%d entries' % f['entries']
    if kind in ('fat', 'semc-fat'):
        x = f['fat']
        return '%s "%s" %d MB file system' % (x['type'], x['oem'], x['bytes'] >> 20)
    if kind == 'srec':
        return '%d records, 0x%x..0x%x%s' % (f['records'], f['start'], f['end'],
                                             (' problems: ' + '; '.join(f['problems'])) if f['problems'] else '')
    if kind == 'babe':
        return 'payload 0x%x bytes at 0x%x' % (f['payload_size'], f['payload_offset'])
    if kind == 'e32image':
        return 'uid3 ' + f['uid3']
    if kind in ('emma', 'gdfs'):
        extra = ' '.join('%s=%s' % kv for kv in f.get('versions', {}).items())
        return '%d writes %s %s' % (f['writes'], f.get('product', ''), extra)
    return ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    i = sub.add_parser('identify'); i.add_argument('paths', nargs='+'); i.add_argument('--json', action='store_true')
    e = sub.add_parser('extract'); e.add_argument('paths', nargs='+'); e.add_argument('-o', '--out', required=True)
    a = ap.parse_args()
    if a.cmd == 'identify':
        res = [dict(path=p, kind=k, **f) for p, k, f in ((p,) + classify(p) for p in inputs(a.paths))]
        if a.json:
            json.dump(res, sys.stdout, indent=1)
            print()
        else:
            for r in res:
                print('%-10s %10d  %-60s %s' % (r['kind'], r['size'], r['name'][:60], describe(r['kind'], r)))
        return
    m = extract(a.paths, a.out)
    for x in m['inputs']:
        extra = ''
        if 'layer' in x:
            extra = '-> %s/ (%d files%s)' % (x['layer'], x['files_written'], ', %d missing' % x['missing'] if x['missing'] else '')
        elif 'image' in x:
            extra = '-> ' + x['image']
        print('%-10s %-55s %s' % (x['kind'], x['name'][:55], extra))
    if 'z_files' in m:
        print('z/: %d files %s' % (m['z_files'], m['z_layers']))
    print('identity:', json.dumps(m['identity']))
    for p in m['problems']:
        print('PROBLEM:', p)


if __name__ == '__main__':
    main()
