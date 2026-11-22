#!/usr/bin/env bash
# Builds build/n95apps/isiprobe.exe (ISI messages to the modem over IscApi, see isiprobe.cpp) against the S60 3rd FP1 (Symbian 9.2) SDK, so import
# ordinals match the N95's 9.2 DLLs. Host tools only: GCC 14.2 symbianelf + elf2e32_next.
# Usage: src/n95apps/isiprobe/build.sh [--dump]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
LIBDIR="$SDK/release/armv5/lib"
OUT="$ROOT/build/n95apps"
OBJ="$OUT/obj-isiprobe"
mkdir -p "$OUT" "$OBJ"

UID3=0xE0001DBC
# the SDK preinclude uses a DOS path (#include "..\symcpp.h"); use a fixed copy
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
CXXFLAGS=(
  -c -march=armv5t -marm -mthumb-interwork -mfloat-abi=soft -O2 -std=gnu++98
  -nostdinc -fno-exceptions -fno-rtti -fno-threadsafe-statics -fshort-wchar
  -fno-strict-aliasing -fno-builtin -Wno-attributes -Wno-narrowing -Wno-return-local-addr
  -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__
  -D__EXE__ -D_UNICODE -D__SUPPORT_CPP_EXCEPTIONS__ -DNDEBUG -D_UREL
  -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os_v9.2.hrh\""
  -include "$OBJ/gcce_fixed.h"
  -I"$SDK/include" -I"$SDK/include/variant" -I"$SDK/include/gcce"
)

echo ">> compile"
"$GCC-g++" "${CXXFLAGS[@]}" -o "$OBJ/isiprobe.o" "$HERE/isiprobe.cpp"
"$GCC-as" -march=armv5t -mfloat-abi=soft -o "$OBJ/startup.o" "$HERE/startup.s"

LIBGCC="$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
# IscApi.dll has no import library in the SDK: make one from iscapi.def (ordinals, docs/devices/nokia-n95-8gb-rm320/n95-isc-isi-protocol.md)
"$E2E" --definput="$HERE/iscapi.def" --dso="$OBJ/iscapi.dso" --linkas="iscapi{000a0000}[1020209f].dll" >/dev/null
ln -sf "$OBJ/iscapi.dso" "$LIBDIR/iscapi{000a0000}.dso"; ln -sf "$OBJ/iscapi.dso" "$LIBDIR/iscapi.dso"   # elf2e32 finds imports only on --libpath (as devagent does)
DSOS=(euser efsrv scppnwdl drtaeabi dfpaeabi)
DSOPATHS=("$OBJ/iscapi.dso"); for d in "${DSOS[@]}"; do DSOPATHS+=("$LIBDIR/$d.dso"); done

echo ">> link"
"$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 \
  --default-symver --entry _E32Startup -u _E32Startup \
  -soname "isiprobe{000a0000}[e0001dbc].exe" \
  "$OBJ/startup.o" "$OBJ/isiprobe.o" "${DSOPATHS[@]}" "$LIBGCC" \
  -Map "$OBJ/isiprobe.map" -o "$OBJ/isiprobe.elf"

echo ">> elf2e32"
"$E2E" --elfinput="$OBJ/isiprobe.elf" --output="$OUT/isiprobe.exe" \
  --targettype=EXE --linkas="isiprobe{000a0000}[e0001dbc].exe" \
  --uid1=0x1000007a --uid2=0x00000000 --uid3=$UID3 --sid=$UID3 --vid=0x0 \
  --capability=All-TCB --fpu=softvfp --heap=0x1000,0x100000 --stack=0x5000 \
  --libpath="$LIBDIR" --compressionmethod=none

ls -la "$OUT/isiprobe.exe"
if [ "${1:-}" = "--dump" ]; then
  "$E2E" --dump=hsi --e32input="$OUT/isiprobe.exe"
fi
