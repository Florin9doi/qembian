# Nokia E6-00 (RM-609, Belle Refresh 111.140.0058): bring-up log

Profile `models/nokia-e6-00-rm609` (family `bb5-s60`, personality `sym101`), made 2026-11-17 from the N8
Belle Refresh profile: same RAPIDO/BCM2727 platform, GCE with host composition, host EGL/OpenVG/GLES.
[V] = seen on a run or in the firmware, [I] = inferred.

```sh
tools/fw/store.py import RM-609_*.zip            # the archive holds two firmwares (fixed, below)
src/khr/build.sh --device nokia-e6-00-rm609 && tools/device.py build nokia-e6-00-rm609 --kernel
python3 tools/sydb/sydb.py phone e6 --fresh      # instance e6
```

Differences from the N8 [V unless marked]:
- Display 640x480, landscape native (`line_pitch` 2560, 480 panel rows), touch screen + QWERTY.
- Keys: the N8's side keys, the E72's navigation/call/end/clear and the E90's QWERTY (its own table not read [I]).
- Firmware layers: core, rofs2 `35.01_India_QW`, rofs3 `C00.01`, user area `U05.01` as drive C.
- The N8's FM radio code patches apply unchanged to the E6's (original bytes matched).

## Boot log

1. **Tool fix**: `tools/fw/store.py import` died FileNotFoundError on the RM-609 archive. It holds two firmware
   versions sharing one package; the first record moved the shared files away. Only the last record moves now
   (`tests/fw_store_test.py TwoFirmwaresOneArchive`).
2. **Widgets spinning for ever** on the home screen (owner's report): start-up stayed at state 105 (emergency
   calls only), so the non-critical start-up list never ran. Missing: the N8 Belle overlay's modem indication
   res 0x09 msg 0xEF `05 06` on channel 35 (and the `aamdaifdriver` control map), not carried into the draft.
   Added: 103 -> 104 -> 110, the first-boot wizard, then the home screen with the clock widget.
3. Popup menus drew a black backdrop instead of the dimmed screen (all Belle phones): the host's DARKEN
   blend was wrong over a transparent buffer. Fixed in syborg_gl.c (docs/devices/nokia-n8-rm596/vg-host.md,
   "Fill limits"), checked on the 808.
4. GPU: the E6 uses the host GL/VG path like the N8 (no fallback to software composition) [V gcebe commits].

## Open

- The E6's own key table (application keys, the hardware-state key).
