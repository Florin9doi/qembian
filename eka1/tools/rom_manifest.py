#!/usr/bin/env python3
"""An unpacked EKA1 core ROM -> the files the EKA1 tools read (untrusted input: run with python3 -I).

  rom_manifest.py ROOT        ROOT/ROM.bin -> ROOT/files.json, ROOT/output/rom/manifest.json

files.json: [{path, address, size}] (tools/fw/semc.py style, for port_exports.py); manifest.json:
{"files": [{path, rom_virtual_address, size}], "module_lists": {primary_file, secondary_file}} as
eka1/tools/eka1_extract.py writes it (port_addr.py, ngage_run.py read it). The directory walk and the
module-list parser are eka1_extract.py's, unchanged. Written for the Nokia 7710 (RM-12) core, whose
container (eka1/tools/ra2_extract.py) holds the ROM already unpacked.
"""
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import eka1_extract as E  # noqa: E402


def manifest(rom):
    base, size, root = struct.unpack_from('<3I', rom, 0x8C)
    prim, sec = struct.unpack_from('<2I', rom, 0xA0)
    img = E.Image('rom', base, bytearray(rom), [], [])
    ents, _ = E.walk_tree(img, root)
    files = [dict(path=e.path, rom_virtual_address=e.address, size=e.size) for e in ents if not e.is_dir]
    lists = {'primary_file': E.parse_module_list(img, prim, len(rom), 'primary_file'),
             'secondary_file': E.parse_module_list(img, sec, len(rom), 'secondary_file')}
    return dict(rom_base=base, rom_size=size, files=files, module_lists=lists)


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__)
    root = pathlib.Path(argv[1])
    m = manifest((root / 'ROM.bin').read_bytes())
    (root / 'output/rom').mkdir(parents=True, exist_ok=True)
    (root / 'output/rom/manifest.json').write_text(json.dumps(m, indent=1))
    (root / 'files.json').write_text(json.dumps(
        [dict(path=f['path'], address=f['rom_virtual_address'], size=f['size']) for f in m['files']], indent=0))
    print('%d files, %d primary, %d secondary modules' % (
        len(m['files']), len(m['module_lists']['primary_file']), len(m['module_lists']['secondary_file'])))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
