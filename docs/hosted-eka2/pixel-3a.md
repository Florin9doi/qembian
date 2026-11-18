# Running a hosted phone on the Pixel 3a (first time)

A step-by-step for the first run of the hosted EKA2 phone (N95 or 808) on a real Pixel 3a (google-sargo). Written
2026-11-13, before any run on the device. Steps marked [untested] have not been done by us yet; facts marked
[verified] come from the cited pages or our own runs, [inferred] are expectations. Device facts:
`cpu-and-device.md`. Design: `src/hosted/README.md`.

## What runs where

```
 PC (Linux)                                   Pixel 3a (postmarketOS = Alpine Linux + mainline kernel)
 ----------                                   ----------------------------------------------------------
 tools/hosted/device-bundle.py  --bundle-->   run.sh (root)
   t/kn_efile  (the hosted kernel + board,      ├─ glserver (64-bit, Mesa/freedreno on the Adreno 615)
                32-bit ARM, static)             └─ kn_efile: the whole Symbian phone as ONE native 32-bit process
   rom/ seed/  (firmware + drives:                  (no emulator: the phone's own CPU runs the ROM code)
                never published)              run.sh view: tools/hosted/phone.py --attach (Qt window in the
 pmbootstrap, fastboot (USB)                    phone's desktop; the touchscreen acts as the mouse)
```

Nothing of Android is used. The phone boots postmarketOS (pmOS), a normal Linux; our phone is a program in it.

## 0. Before you start (PC)

- Tools: `fastboot` and `adb` (Debian/openSUSE package `android-tools`), `pmbootstrap` (pip or distro package),
  `ssh`, a USB-C cable that carries data.
- **Download the Google factory image for sargo now** (developers.google.com/android/images, "sargo", the latest
  Android 12 build) and unpack it. It is the way back to stock Android: `./flash-all.sh` from it restores
  everything (it wipes the phone). Keep it next to you before touching any partition.
- Your bootloader is unlocked [owner]. Check: `fastboot getvar unlocked` in the bootloader says `yes`.
- The phone's data: anything on it may be lost on the "install" path (section 3); netboot (section 2) leaves
  Android's storage alone, except `dtbo` if you erase it (see the caveat there).
- Enter the bootloader: power off, then hold **Volume down + Power**; or `adb reboot bootloader` from Android.

## 1. Build the bundle and the postmarketOS image (PC)

```
# the phone bundle (firmware + drives: keep it private)
python3 tools/hosted/device-bundle.py --rom nokia-808-rm807 --tar /tmp/q808.tar      # or nokia-n95-8gb-rm320
pmbootstrap init         # vendor: google, device: sargo, UI: phosh (or plasma-mobile), user: your name
                         # "extra packages" (Alpine names, 2026-11-14 - build-gl on the device failed without
                         # patch/g++/pkgconf): python3 py3-qt6 gcc g++ musl-dev binutils patch pkgconf
                         #                   glib-dev libepoxy-dev mesa-dev mesa-egl mesa-gles
                         #                   linux-tools util-linux file openssh
pmbootstrap install      # builds the image (on netboot: the rootfs that is served over USB)
```
Size: the 808 bundle carries an 8 GB E: drive image that is mostly holes (`tar --sparse` keeps it small:
`du` of the bundle shows the real use). The image must have room for the unpacked bundle: give pmbootstrap
`--size` or put the bundle on the phone's own storage after an install (section 3).

## 2. Boot without installing: fastboot boot + netboot (keeps Android) [untested on sargo]

pmOS can boot the phone's kernel from RAM (`fastboot boot`, nothing flashed) and serve the root filesystem from
the PC over USB (nbd) [verified: pmOS wiki "Netboot", pmbootstrap netboot docs]:
```
pmbootstrap kconfig check --categories netboot   # the device kernel must have what netboot needs
pmbootstrap initfs hook_add netboot
pmbootstrap install                              # again, with the hook
# phone in the bootloader, cable in:
pmbootstrap flasher boot                         # fastboot boot: kernel+initramfs to RAM, not flashed
# open TCP 9999 on the PC firewall, then:
pmbootstrap netboot serve                        # serves the rootfs to the phone (USB network 172.16.42.x)
```
Caveats [verified: wiki/issues]: Android devices may need `fastboot erase dtbo` first; that **is** a write: Android
then needs `fastboot flash dtbo dtbo.img` (from the factory image) to boot again. The netboot hook assumes 512-byte
sectors (many Android devices use 4096). A Pixel 4a (sunfish) user reported resets while serving. If netboot fails,
use section 3 and restore with the factory image afterwards.

## 3. Install postmarketOS (wipes Android's data; reversible with the factory image)

```
pmbootstrap flasher flash_rootfs     # rootfs to the phone's storage (userdata) [inferred for sargo]
pmbootstrap flasher flash_kernel     # boot partition of the active slot
fastboot reboot
```
Back to Android later: the factory image's `flash-all.sh` (bootloader stays unlocked).

## 4. Put the bundle on the phone and run it

```
# PC -> phone over the USB network (pmOS default: the phone is 172.16.42.1)
scp /tmp/q808.tar user@172.16.42.1:
ssh user@172.16.42.1
tar --sparse -xf q808.tar && cd nokia-808-rm807
sudo ./run.sh check        # 32-bit support, seccomp, kernel options (see the table below)
apk add patch g++ pkgconf # the GL build needs these (bash is required by the build scripts too)
sudo ./run.sh build-gl     # once: the GL server against the phone's Mesa (808: needed; N95: for its GL apps)
sudo ./run.sh              # the phone starts; log: /tmp/qembian/phone.log  (GL=0 sudo ./run.sh: no GL server)
# in the phone's graphical session (a terminal on the phone, or ssh -X / WAYLAND_DISPLAY set):
./run.sh view              # the window: touch = mouse, buttons = the phone's keys, Ctrl+P = diagnosis
```
The 808 needs ~60 s to its home screen in our test VM under emulation [verified]; natively it should be much
faster [inferred: no TCG; the Pixel's Cortex-A75 runs the ROM code itself].

## 5. What can go wrong (check in this order)

| Symptom | Cause | What to do |
|---|---|---|
| `Exec format error` for `t/kn_efile` | kernel without 32-bit user support (`CONFIG_COMPAT`) | a kernel with COMPAT (pmOS mainline kernels have it [inferred]) |
| the program dies at once, `Bad system call` | seccomp unavailable or a syscall refused | `CONFIG_SECCOMP_FILTER`; look at the last lines of phone.log |
| `HOSTED: ... regions not free` / mmap at a fixed address fails | the kernel lacks `MAP_FIXED_NOREPLACE` (Linux < 4.17) or something already maps 0x01000000-0x7f000000 | mainline kernels are fine; nothing else should run in that process |
| SIGILL in ARMv5/v6 code | SWP/CP15 barrier emulation off | not needed for the N95 or the 808 [verified: both boot with CP15 emulation off, the N95's EUSER has no SWP]; `abi.swp` only matters for other firmwares |
| black window, phone.log fine | GL server missing or failed (808) | `/tmp/qembian/gl.log`; `./run.sh build-gl`; Mesa's freedreno must give surfaceless EGL + GLES 3 |
| window size or rotation odd | the window follows the device profile and the frames | the 808 is 360x640 portrait; `--scale` in `run.sh view` (edit) |
| no touch | the window takes mouse events; Qt maps the touchscreen to the mouse | in phosh/plasma it should; else run `phone.py` with `QT_QPA_PLATFORM=wayland` |
| it stops at a dialog after start-up | first-boot steps | the seeds are past the wizard (the 808's since 2026-11-13); taps work as on the PC |

`run.sh` makes three links at the root: `/t`, `/rom`, `/seed` (the phone program reads the test VM's paths).
Remove them with `sudo rm /t /rom /seed` when done.

## 6. Without flashing anything at all: Android + Termux (N95 only, an experiment) [untested]

The phone program is a static Linux ARM binary, so in principle it runs as an Android app process (Termux),
no root, no flashing. Expected blockers [inferred]: the stock Pixel 3a kernel is 4.9 (no `MAP_FIXED_NOREPLACE`:
our user window and chunk reservations would need a fallback); Android's own seccomp filter and SELinux may refuse
some of our system calls; `/t`, `/rom`, `/seed` cannot be created without root (the paths would have to become
configurable); the GL server needs Mesa (glibc), which Android does not have: the 808 cannot compose there. The N95
can boot without GL. Worth a try only after section 2 or 3 works.

## 7. Report back

For the next session: `sudo ./run.sh check` output, `/tmp/qembian/phone.log` (and `gl.log` for the 808), the time
to the home screen, photos of the screen. Device-specific fixes go to `docs/hacks.md` like every other shortcut.

## Sources

- postmarketOS packages, google-sargo: https://pkgs.postmarketos.org/package/main/postmarketos/aarch64/device-google-sargo
- postmarketOS install page (Pixel 3a among community devices): https://postmarketos.org/install/
- pmOS wiki, Netboot: https://wiki.postmarketos.org/wiki/Netboot ; pmbootstrap netboot API:
  https://docs.postmarketos.org/pmbootstrap/main/api/pmb.netboot.html ; live-boot article:
  https://tuxphones.com/postmarketos-linux-live-usb-fastboot-network-boot/
- netboot restart report (sunfish): https://gitlab.com/postmarketOS/pmaports/-/issues/2367
- Google factory images: https://developers.google.com/android/images
