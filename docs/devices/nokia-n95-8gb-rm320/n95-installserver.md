# Installing development SIS packages

The local `~/Downloads/Symbian Hack.zip` contains a nested `installserver92.zip`
in the N95/N95-2 device group. Its readme identifies SWmail's Symbian 9.2 SIS
installer mod v1.1 (26 May 2008), intended to permit unsigned packages with any
capability, protected UIDs, and invalid certificates.

Install it from the host with the virtual device stopped:

```sh
python3 tools/n95_installserver.py
```

The tool reads that archive, validates the known executable's SHA-256, backs up
`build/n95/drive-c.img`, copies the executable to `C:\sys\bin\installserver.exe`,
and checks the bytes read back from FAT. It requires mtools and host process
visibility so it can reject disks used by running QEMU processes. Optional
`--archive PATH` and `--drive PATH` select another archive location or C: image.

This is a RAM-loaded E32 executable, installed as a C: replacement as instructed
by its author. Do not overwrite the XIP executable's bytes in the ROM with it.
It went into the C: of the N95 that scripts/run-gui.sh used, now the instance `n95-gui`
(`tools/n95_installserver.py` takes `--instance NAME` or `--drive IMG`):

```sh
INSTANCE=n95-gui scripts/drive.sh put e /path/to/app.sisx /
python3 tools/sydb/sydb.py phone n95-gui
```

Open the package from the memory card in the guest file manager. The change
does not install Open4All or RomPatcher, change the RTC, or disable kernel
capability enforcement. The installer mod can be used to install those tools
if needed. Scripted runs using other C: images do not inherit this replacement.

To undo, stop QEMU and restore the printed `drive-c.img.before-installserver-*`
backup, or remove only the replacement with
`scripts/n95-drive.sh rm c /sys/bin/installserver.exe`.

Validation on 2026-11-05: the executable was copied to the persistent C: image
and read back identically (37,413 bytes; SHA-256
`280b68c451c0c9c5c65e84fcaef78a6802f2c59b56fcbe95f00635c1ce807cf0`).
Actual SIS installation and selection of the replacement by the guest loader
have not yet been verified.
