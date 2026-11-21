#!/usr/bin/env bash
# Build the SE PVR2DIF.dll replacement (H-p1i-pvr2dif-stub): no imports, 15 exports (the P1i's and the P990i's
# PVR2DIF.dll both export 15).
#   src/semc/pvr2dif/build.sh [DEVICE]   (default se-p1i)
#   output: build/roms/<DEVICE>/<fw>/pvr2dif/PVR2DIF.dll (model.toml rofs_replace)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
DEV="${1:-se-p1i}"
OUT="$(SYBORG_DEVICE="$DEV" python3 -I "$ROOT/tools/device.py" paths dir)/pvr2dif"; OBJ="$OUT/obj"; mkdir -p "$OBJ"
"$GCC-gcc" -c -march=armv5te -marm -O2 -fno-common -ffreestanding -Wall -o "$OBJ/pvr2dif.o" "$HERE/pvr2dif.c"
"$GCC-as" -march=armv5te -o "$OBJ/startup.o" "$HERE/startup.s"
{ echo "PVR2DIF { global:"; grep -o 'pvr2dif_[a-z_0-9]*' "$HERE/pvr2dif.def" | sed 's/$/;/'; echo "local: *; };"; } > "$OBJ/pvr2dif.vers"
link="PVR2DIF{000a0000}.dll"
"$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 \
  --version-script "$OBJ/pvr2dif.vers" --entry _E32Dll -u _E32Dll -soname "$link" \
  "$OBJ/startup.o" "$OBJ/pvr2dif.o" -o "$OBJ/pvr2dif.elf"
# the real PVR2DIF's header: uid1 DLL, uid2/uid3 0, vid 0x7000000f, All -TCB (docs/research/p1i-pvr2d.md)
"$E2E" --elfinput="$OBJ/pvr2dif.elf" --output="$OUT/PVR2DIF.dll" --targettype=DLL --linkas="$link" \
  --uid1=0x10000079 --uid2=0 --uid3=0 --sid=0 --vid=0x7000000f --capability="All-TCB" \
  --definput="$HERE/pvr2dif.def" --defoutput="e2edefoutput" --dso="$OBJ/PVR2DIF.dso" --compressionmethod=none >"$OBJ/e2e.log" 2>&1 \
  || { cat "$OBJ/e2e.log"; exit 1; }
echo "pvr2dif: $OUT/PVR2DIF.dll $(stat -c %s "$OUT/PVR2DIF.dll") bytes"
