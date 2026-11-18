#!/usr/bin/env bash
# Builds build/n95apps/glprobe1.exe: the smallest GLES 1.1 client of an S60 3rd FP1 phone's libgles_cm.dll
# (glprobe1.cpp). Toolchain as eglprobe/devagent (GCC 14.2 symbianelf, elf2e32_next, S60 3rd FP1 SDK).
# Run on a booted phone: sydb rerun build/n95apps/glprobe1.exe --wait 10; sydb pull C:\data\glprobe1.txt -
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
OUT="$ROOT/build/n95apps"; OBJ="$OUT/obj-glprobe1"
mkdir -p "$OUT" "$OBJ"
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
"$GCC-as" -march=armv5t -mfloat-abi=soft -o "$OBJ/startup.o" "$HERE/../devagent/startup.s"
"$GCC-g++" -c -march=armv5t -marm -mthumb-interwork -mfloat-abi=soft -O2 -std=gnu++98 \
  -nostdinc -fno-exceptions -fno-rtti -fno-threadsafe-statics -fshort-wchar -fno-strict-aliasing -fno-builtin \
  -Wno-attributes -Wno-narrowing -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__ \
  -D__EXE__ -D_UNICODE -D__SUPPORT_CPP_EXCEPTIONS__ -DNDEBUG -D_UREL \
  -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os_v9.2.hrh\"" -include "$OBJ/gcce_fixed.h" \
  -I"$SDK/include" -I"$SDK/include/variant" -I"$SDK/include/gcce" -o "$OBJ/glprobe1.o" "$HERE/glprobe1.cpp"
LIBS="$OBJ/lib"; rm -rf "$LIBS"; mkdir -p "$LIBS"
ln -s "$SDK/release/armv5/lib"/* "$LIBS/"
LIBGCC="$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
LINK="glprobe1{000a0000}[e0001dc1].exe"
L="$SDK/release/armv5/lib"
"$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 --default-symver \
  --entry _E32Startup -u _E32Startup -soname "$LINK" "$OBJ/startup.o" "$OBJ/glprobe1.o" \
  "$L/euser.dso" "$L/efsrv.dso" "$L/ws32.dso" "$L/scppnwdl.dso" "$L/drtaeabi.dso" "$L/dfpaeabi.dso" \
  "$L/libgles_cm.dso" "$LIBGCC" -o "$OBJ/glprobe1.elf"
"$E2E" --elfinput="$OBJ/glprobe1.elf" --output="$OUT/glprobe1.exe" --targettype=EXE --linkas="$LINK" \
  --uid1=0x1000007a --uid2=0x00000000 --uid3=0xE0001DC1 --sid=0xE0001DC1 --vid=0x0 --capability=None \
  --fpu=softvfp --heap=0x1000,0x400000 --stack=0x8000 --libpath="$LIBS" --compressionmethod=none | grep -i error && exit 1 || true
ls -la "$OUT/glprobe1.exe"
