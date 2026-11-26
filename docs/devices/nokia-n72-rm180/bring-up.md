# Nokia N72 (RM-180) bring-up

Firmware V 5.0819.4 (`RM180_50819401.C0` core, `RM180_50819401.V05` variant). S60 2nd Edition **Feature Pack 3**,
Symbian **8.1a**, EKA1, TI **OMAP1710**, BB5 modem: **the N70 relinked**. Board: the 6630's (`-M nokian72`,
`rm1_board_init` with `rm1_phone_n72`, `src/qemu-ngage/hw/arm/nokia9500.c`) with the N70's settings; method and tools
as the N70's ([../nokia-n70-rm84/bring-up.md](../nokia-n70-rm84/bring-up.md)). Workarounds: [../../hacks.md](../../hacks.md)
"Nokia N72".

Marks: **[V]** verified in the firmware or by a run, **[I]** inferred.

## State (2026-11-26)

From the phone's own unmodified ROM, in one session and no stalls past the board entry: NOKIA logo, first boot
(C: formatted; "Select current city" at ~2 min), the Time and Date queries, the **Active Standby home screen**
(clock, date, shortcut bar, "No calendar entries for today", Messag./Gallery softkeys), the **Menu grid** (Messaging,
Contacts, Calendar, Radio, Gallery, Imaging, Clock, Web, My own; it fills in some seconds after the Menu title),
folders open (Imaging: Camera, Movie, RealPlayer, Img. print). **A second boot from the kept NAND** reaches the home
screen (the city kept). Since 2026-11-26 the board answers the RTC time read `19 0A` with QEMU's clock in UTC
(H-rm84-rtc, docs/research/n70-rtc.md): the second boot asks no Time or Date and shows the host's time in the home
city's zone [V run]; a factory-fresh boot still asks (the queries' defaults then win until the next boot). No catches (fatal, pabt, dabt,
undef) in either boot. No sound, network, SIM, DRM, secure environment or memory card. Evidence:
build/evidence/rm180-*.png.

    python3 eka1/tools/ngage_phone.py --model nokian72 --phone NAME [--fresh]   # the window
    python3 eka1/tools/ngage_sess.py -s NAME --model nokian72 --flash build/ngage/NAME.flash --catch fatal,pabt,dabt,undef start

Keys: the N70's (Enter select, F1/F2 softkeys, F5 Menu, arrows = joystick, ...) [V: city, time, date, Menu, a folder].

Prepare the images once (all git-ignored):

    mkdir -p eka1/rm180_rom eka1/rm180/core eka1/rm180/output/rom && cp ../roms/N72/* eka1/rm180_rom/
    python3 -I tools/fiasco.py eka1/rm180_rom/RM180_50819401.C0 --out eka1/rm180/fiasco/core
    python3 -I tools/fiasco.py eka1/rm180_rom/RM180_50819401.V05 --out eka1/rm180/fiasco/v05
    python3 -I eka1/tools/xzip_core.py 'eka1/rm180/fiasco/core/SOS*CORE@00070000.hdr' 'eka1/rm180/fiasco/core/SOS*CORE@00070400.bin' \
        eka1/rm180/core/ROM.bin --rofs eka1/rm180/output/rom/rofs.img
    python3 -I eka1/tools/rom_manifest.py eka1/rm180/core
    python3 -I eka1/tools/rm1_nand.py eka1/rm180/core/nand.img --rofs eka1/rm180/output/rom/rofs.img \
        --rofx eka1/rm180/fiasco/v05/_pre@01ae0000.bin --user-start 0x2600000

## Files (git-ignored) and their SHA-256

| What | Where | SHA-256 |
|---|---|---|
| core flash file (V 5.0819.4) | `eka1/rm180_rom/RM180_50819401.C0` (from `../roms/N72/`) | `5359d7baa5a237a0e5bffa45e36d76a9300e9b29af18bd8c5a4f3fea3a2ed508` |
| variant | `eka1/rm180_rom/RM180_50819401.V05` | `790c9450ec01ec07abbf4de0f49fc7b639e7bdaab750d2e32dcf250ddcf83b63` |
| inflated core ROM (VA 0x50000000, 0xB00000, root 0x50003C60) | `eka1/rm180/core/ROM.bin` (+ `files.json`, `output/rom/manifest.json`) | `7ddf78a47ca547b6690c675edefdd092247980d4fbfd70d041eadf8ddf07ead7` |
| core ROFS (0x12924C0 bytes) | `eka1/rm180/output/rom/rofs.img` | `1e351c645cf739b9bd3b45c6b96927528a489f9ff03da3939b6537ef7fe69c17` |
| variant ROFx (12 MB) | `eka1/rm180/fiasco/v05/_pre@01ae0000.bin` | `99ec93e9aa61c8de3e4d9374b93cd667b3ef9e2c76ff81c5f0cf8dc412d7b4e2` |
| NAND image | `eka1/rm180/core/nand.img` (ROFS at 0x100000, C: at 0x2600000) | built, not hashed |
| screenshots | `build/evidence/rm180-city.png`, `-home-screen.png`, `-menu-grid.png`, `-menu-imaging.png` | |

## Step 1: inventory [V]

The N70's container and tools, unchanged (the same FIASCO layout: SOS*CORE at 0x70400, the ROFx at 0x01AE0000). Core
ROM: 459 files, 384 primary and 348 secondary modules; the N70's set minus camparifpdd.pdd and vtlogger.dll, nothing
new. Thirteen modules changed size (SysAp, NOKIATSY 604 bytes smaller, CommonTSY, PhoneServer, SecUi,
SecurityObserver, bc02 Bluetooth, hci, CamServerCore, PbkView, SelfTestServer, SecurityNotifier); the rest have the
N70's sizes but most are relinked at other addresses.

- **Kernel:** ekern.exe at the N70's address (0x50008F9C) and size; of its words only 272 differ, all in
  0x500343D0..0x50035084 and all pointers into other (moved) modules. So the N70's Kern::Fault (0x5001FA84 ->
  0x5001FA8C), current thread 0x8000295C (21 users in both ROMs) and containers 0x8000048C (17 in both) hold.
- **Drivers:** ekeyp.dll, lcd.ldd, ssi.pdd, SecPdd, IscIsiMultiplexer, xbus, custom.ldd, mednand and asc.dll are
  the N70's code word for word apart from relocated addresses (the keypad and the panel are the N70's).
- **Bootstrap memory table** at ROM +0xD0C (the N70's at +0xD20): 64 MB at 0x10100000, the same.
- **Core patches:** the N70's eleven words (H-rm84-*) each found by a unique exact match of the 32 words around the N70's
  site, inside the same module and at the same offset, old word identical.

## Stalls

| # | Stall | Evidence | Fix |
|---|---|---|---|
| 1 | (board) the N72 machine | the inventory above | `-M nokian72` (128 MB, OMAP1710 IDs, the N70's display and keys, `rm1_patches_n72`), `ngage_run.py` MODELS `nokian72`, `isi-replies-rm180.txt` (the N70's table, unchanged) |

Nothing else stopped the boot. The unanswered modem requests are the N70's (`02 06`, `09 1F`, `09 DE`, `17 2F`,
`19 0A`, `31 30`, `41 08`, `54 0D`, `D7 09`, `DB 01/05`, ...) and so are the three `38 0E` reports (`61 01 01 5A..5C`)
the N90 also shows.

## Next

1. The `38 0E` reports and the unanswered requests, each only when something waits on it.
2. Sound (DSP), network/SIM, DRM/secure environment, the memory card (medmmc.pdd).
