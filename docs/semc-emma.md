# The Emma III archive: what is in it, and how the firmware comes out

State of 2026-11-13. Covers `se_emma/Emma_III-OFFLINE.tar.zst` (a 2009 Sony Ericsson service tool
installation, Indonesian reseller "kelik", user id 1039214, 3.3 GB of downloaded firmware). The
tool is `tools/fw/emma.py`, the tests are `tests/emma_test.py`. What the firmware *format* is, per
phone, is in `docs/semc-firmware.md`; this file only gets the bytes out of the service tool.

Two firmware families for the QEMU work come out of it, and nothing else here is needed for the
emulator: the P990i/W950i/W960i/P1i/M600i family and the G700/G900 family (all 2006-2008 DB2001-era
UIQ 3 phones). Everything else in the database is A2-platform feature-phone material (K/W/Z series
"FS_*" bundles, W980, the X1 PocketPC NBH) — read, not used.

## The two layers

An offline Emma keeps a copy of the server's catalogue locally, in `db/<user id>/`:

| | what | how it is stored |
|---|---|---|
| `local/ma3localdb.script` | HSQLDB 1.8 database, 22 tables, 78 895 rows: products, scripts (the flashing flows), files, statistics | every VARCHAR value is base64 of **SAFER+**, 16-byte blocks, ECB, 247 464 values [V] |
| `blob_fs/` | the payloads, 754 files: 617 firmware files + 81 icons | `FILE_*` are **RC4** around a gzip stream, `ICON_*` are plain GIF/PNG [V] |

The server is gone and the tool is Windows-only (bundled JRE 6, `Emma.exe`), but neither layer needs
it: the local DB is self-contained and both ciphers are reversible with what is on disk.

`[V]` facts, all read from this archive:

- **blob names** are `FILE_<serverversion_id>_<serverfile_lastupdated>_<serverversion_lastupdated>_
  <servercontentinfo_id>_<contentlength>_<securityname>` and `ICON_<server id>_<lastupdated>`; the six
  FILE components are exactly the columns TBLFILE is looked up by, so a blob can be matched to its
  row without decrypting anything.
- **`contentlength` is the plain length**, while the blob holds the gzip stream, so the blob is
  smaller (`..._52_INFILE` is 38 bytes and holds 52).
- **the gzip payload is the file itself** — plain Sony Ericsson images (EKA2 core, ROFS1, ROFx, the
  "FS" FAT drive, BABE modem software, GDFS scripts), i.e. exactly what `tools/fw/semc.py` reads.
  There is no container around them.
- **RC4 key** is the base64 `DoL6FBfnYcNJBjH31Vnz6lKATTaDGe4y` of
  `com.sonyericsson.cs.ma3.common.security.b` (24 bytes, `0e82fa14...819ee32`). Checked twice: the
  Python decrypt is byte-identical to the same decrypt done with the archive's own class for all 133
  files extracted here, and `tests/semc_test.py`'s rules apply to the cipher itself (RC4 has published
  vectors).
- **the SAFER+ key** comes from the client's own handler
  (`com.sonyericsson.cs.ma3.serviceclient.core.h.a.h()`, 24 bytes, a constant in the client, not per
  user); the *value* codec is `com.sonyericsson.cs.ma3.core.localdb.a.d`: `String -> ISO-8859-1 ->
  zero pad to 16 -> SAFER+ ECB -> base64`, and decryption trims the padding. Reimplementing that
  cipher is not worth it, so `emma.py localdb` compiles a 55-line helper and runs *the archive's own
  class* over *the archive's own script* — one command, no key anywhere in the repo.
- **the login** (`password emma3.txt`: `kelik` / `Y+c4@e&8`) is only needed to *run* Emma. The DB's
  `settings/serviceclient_local_settings.properties` keeps `SHA1(password)` per user, and the DB
  directory is named after the user id.

## What comes out

`emma.py extract` groups the files into firmware sets the way the tool's scripting refers to them
(a set is a `(Model, CDA)` pair of the flashing scripts; the shared images — core, ROFS1, ROFS1_EXT —
go to every customisation of that model; a script without a CoreOS/EROM image, such as "Set product
to P1", is not a set). 13 sets, 133 distinct files, 1.18 GB with the duplicates hard-linked away,
in `build/se_emma/roms/`:

| set | PDA software | CDA | size | what |
|---|---|---|---|---|
| `SE-P1c_P1i/R6G04-CDA162024-52` | CXC162108 R6G04 | CDA162024/52 Trade Indonesia | 99 MiB | core 0x500000 @0xf8000000, ROFS1, ROFx, ROFS2, C: |
| `SE-P1c_P1i/R6G04-CDA162022-2` | CXC162108 R6G04 | CDA162022/2 Generic World2 | 96 MiB | same images, other customisation |
| `SE-P990c_P990i/R6E28-CDA162007-2` | CXC162036 R6E28 | CDA162007/2 | 111 MiB | + Hermione EROM CXC162060 |
| `SE-M600i/R6A16-CDA162011-2` | CXC162071 R6A16 | CDA162011/2 | 99 MiB | + Lily EROM CXC162096 |
| `SE-W950i/R6E28-CDA162014-2` | CXC162088 R6E28 | CDA162014/2 | 89 MiB | + Nina EROM CXC162097 |
| `SE-W960i/R6G04-CDA162032-2` | CXC162146 R6G04 | CDA162032/2 Generic World2 | 115 MiB | |
| `SE-W960i/R6G04-CDA162034-54` | CXC162146 R6G04 | CDA162034/54 Trade Indonesia | 115 MiB | |
| `SE-G700/R6J825-CDA1204-3126` | 1202-7347 R6J825 | 1204-3126 Generic World2 | 87 MiB | |
| `SE-G700/R6J825-CDA1207-5730` | 1202-7347 R6J825 | 1207-5730 Customized ID | 91 MiB | |
| `SE-G700_G700B/R6J825-CDA1215-3912` | 1212-3350 R6J825 | 1215-3912 Customized SG | 86 MiB | |
| `SE-G900/R6J825-CDA1204-3149` | 1203-8784 R6J825 | 1204-3149 Generic World2 | 92 MiB | |
| `SE-G900/R6J825-CDA1207-5816` | 1203-8784 R6J825 | 1207-5816 Customized Indonesia | 96 MiB | |

Per set, 20-34 files: the core (`ARMSOFTWARE ... CoreOS`), two ROFS layers, the customer ROFS2 and
FAT "FS" image, the CCPU (modem, Ericsson DB2001) BABE software, ACPU loaders (E32 images),
the EROM, and the GDFS product/SIM-lock scripts. `tools/fw/semc.py extract` then unpacks each set into
layers and a merged Z: under `build/firmware/<set>/` (e.g. P1i R6G04: 223 core files, ROFS1 1419,
ROFx 2861, ROFS2 867, Z: 3935).

## Kernel side: all of them fit the N73's personality

`tools/fw/probe.py --device build/firmware/<set> stubs --vs nokia-n73-rm132` on every UIQ 3 set:
258 SWI sites (27 fast, 231 slow), the same shape (`mov ip, lr; swi`), the same ordered sequence —
"the exec map of nokia-n73-rm132 fits" for all 13. So `personalities/sym91` applies to the whole
family, not just to the P1i R6E30 we had. This is the cheapest route to a second bootable SE phone,
and it also gives a *newer* P1i (R6G04 vs R6E30) with the same ROM layout (romBase 0xF8000000,
size 0x500000, kernel data 0x64000000..0x66200000).

The G700/G900 (their ROMs are the newest here) also report the 9.1 shape, which is worth remembering: their `version.txt`
carries a part number (`PDA: 1202-7347 R6J825`) instead of a `CXC…`, which `semc.py` now accepts.

## Reading a new archive

```sh
# 0. (once) unpack the archive somewhere git-ignored
mkdir -p build/se_emma/raw && tar -C build/se_emma/raw -xf se_emma/Emma_III-OFFLINE.tar.zst --zstd

# 1. what is inside (needs the DB; the helper compiles against the archive's own plugins/*.jar)
python3 -I tools/fw/emma.py localdb build/se_emma/raw/Emma\ III-OFFLINE-kelik -o build/se_emma/localdb.script
python3 -I tools/fw/emma.py inventory build/se_emma/raw/Emma\ III-OFFLINE-kelik \
        --localdb build/se_emma/localdb.script

# 2. the firmware files, one directory per set (3.3 GB of blobs -> 1.2 GB of files, ~2 min)
python3 -I tools/fw/emma.py extract build/se_emma/raw/Emma\ III-OFFLINE-kelik \
        -o build/se_emma/roms --localdb build/se_emma/localdb.script [--models P1i,P990i] [--icons]

# 3. unpack one set into layers + Z: (tools/fw/semc.py, unchanged)
python3 -I tools/fw/semc.py extract build/se_emma/roms/SE-P1c_P1i/R6G04-CDA162024-52 \
        -o build/firmware/SE-P1c_P1i/R6G04-CDA162024-52

# 4. is it the kernel we think it is?
python3 -I tools/fw/probe.py --device build/firmware/SE-P1c_P1i/R6G04-CDA162024-52 \
        stubs --vs nokia-n73-rm132
```

Into the ROM store: `semc.package()` reads each set's own identity (PDA `CXC162108 R6G04`, CDA
`CDA162024/52 R8A`) but no product, so `store.py import` would file it under an empty type code. The
model is known — from the DB's `Model` identifiers (which is how `emma.py extract` names the
directory) and, inside the files, only as a byte string in the ROFx layer's `Resource/Apps/ProductInfo.rsc`
(`03 'P1i'`, not the EKA1 `System\Data\ProductInfo.rsc` that `identify.py` looks for). Either teach
`identify.py` that file, or pass the model from the manifest; that is the one step between this and
a `roms/SE-P1i/R6G04/` entry `tools/device.py build` can use.

`emma.py blobs` is the escape hatch: it decrypts every blob 1:1 without the DB (no names, no
grouping). `emma.py inventory --json` writes the whole catalogue (products, versions, models per
file) for scripts.

## Kept out of the repo, and what is still unknown

All firmware bytes stay in `build/` (git-ignored): `build/se_emma/raw/` (the archive), `roms/` (the
extracted files), `build/firmware/SE-*/`. Nothing proprietary is committed; `tools/fw/emma.py` and
the tests run on synthetic data only, plus the two lines of key material above, which are the
client's own constants.

Unknown or not needed:

- **the EROM images** (`Hermione`/`Lily`/`Nina EROM CXC1620xx`, 0.9 MB) are the ACPU boot ROM: an ARM
  branch table at 0, then ~0.9 MB with no readable string table and no Symbian header — masked or
  signed, not decoded [V: 1282..1363 accidental strings, none a name]. `semc.py` now calls them
  `semc-armimg` and keeps them byte for byte; whether booting needs them at all is the next question
  once a P1i runs from NAND images.
- **the BABE payloads** (CCPU/modem, also not decoded) — the fake modem wants the *protocol*, not
  this code; `docs/semc-firmware.md` has the plan.
- **the A2 material** (2.5 GB of `SSW` bundles, the W980, the X1 PocketPC NBH) is left in the archive
  on purpose: no Symbian there.
- **P910i (UIQ 2.1, EKA1) is not in this database**: it has DB2000-era *loaders* and GDFS scripts,
  but no UIQ 2.x application software. The P910i images we do have came another way, see
  `docs/semc-firmware.md`.
