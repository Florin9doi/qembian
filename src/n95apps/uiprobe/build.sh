#!/usr/bin/env bash
# Builds uiprobe (docs/ui-probe.md) against the S60 3rd FP1 (9.2) SDK, GCC 14.2 symbianelf + elf2e32_next:
#   build/n95apps/uiprobe.dll   ECom plugin DLL (uid2 0x10009D8D, uid3 0xE0001DD0), capabilities All-TCB
#                               (as CONE: a DLL needs every capability of each process it is loaded
#                               into, and may link only to DLLs that have all of its own; All killed
#                               akncapserver at boot, -46: CONE.DLL lacks TCB)
#   build/n95apps/uiprobe.rsc   its ECom registration (C:\resource\plugins\uiprobe.rsc)
# `sydb install` / `sydb update-agent` put both on the phone.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SDK="${S60SDK:-$HOME/work/S60_3rd_FP1/Epoc32}"
GCC="${GCC14:-$HOME/gcc-builds/gcc-14.2.0}/bin/arm-none-symbianelf"
E2E="${ELF2E32:-$HOME/work/elf2e32_next/elf2e32_next_release}"
GNUPOC="${GNUPOC:-$HOME/work/gnupoc-package/tools}"
LIBDIR="$SDK/release/armv5/lib"
OUT="$ROOT/build/n95apps"
OBJ="$OUT/obj-uiprobe"
mkdir -p "$OUT" "$OBJ"
sed 's#"\.\.\\symcpp\.h"#<symcpp.h>#' "$SDK/include/gcce/gcce.h" > "$OBJ/gcce_fixed.h"
# the Windows SDK's headers include each other in the wrong case (<LineBreak.h> for linebreak.h):
# each missing one is linked, under the name asked for, into a shim directory searched first
SHIM="$OBJ/caseshim"; mkdir -p "$SHIM"
compile() {
  "$GCC-g++" -c -march=armv5t -marm -mthumb-interwork -mfloat-abi=soft -O2 -std=gnu++98 \
    -nostdinc -fno-exceptions -fno-rtti -fno-threadsafe-statics -fshort-wchar \
    -fno-strict-aliasing -fno-builtin -Wno-attributes -Wno-narrowing \
    -D__SYMBIAN32__ -D__EPOC32__ -D__MARM__ -D__MARM_ARMV5__ -D__EABI__ -D__GCCE__ \
    -D__DLL__ -D_UNICODE -DNDEBUG -D_UREL \
    -D__PRODUCT_INCLUDE__="\"$SDK/include/variant/symbian_os_v9.2.hrh\"" -include "$OBJ/gcce_fixed.h" \
    -I"$SDK/include" -I"$SHIM" -I"$SDK/include/variant" -I"$SDK/include/gcce" -o "$OBJ/uiprobe.o" "$HERE/uiprobe.cpp"
}
for _ in $(seq 60); do
  if compile 2>"$OBJ/cc.log"; then break; fi
  miss=$(grep -o "fatal error: [^:]*: No such file" "$OBJ/cc.log" | sed 's/fatal error: //; s/: No such file//' | head -1)
  [ -n "$miss" ] || { cat "$OBJ/cc.log"; exit 1; }
  hit=$(find "$SDK/include" -maxdepth 2 -ipath "$SDK/include/$miss" | head -1)
  [ -n "$hit" ] || { cat "$OBJ/cc.log"; exit 1; }
  mkdir -p "$SHIM/$(dirname "$miss")"; ln -sfn "$hit" "$SHIM/$miss"
done
"$GCC-as" -march=armv5t -mfloat-abi=soft -o "$OBJ/startup.o" "$HERE/startup.s"
LIBGCC="$("$GCC-g++" -march=armv5t -mfloat-abi=soft -print-libgcc-file-name)"
LIBS="$OBJ/lib"; rm -rf "$LIBS"; mkdir -p "$LIBS"; ln -s "$LIBDIR"/* "$LIBS/"
LINK="uiprobe{000a0000}[e0001dd0].dll"
DSOS=(); for d in euser ws32 cone eikcore eikcoctl avkon ecom fbscli egul eikctl scppnwdl drtaeabi dfpaeabi; do DSOS+=("$LIBDIR/$d.dso"); done
echo "UIPROBE { global: _Z24ImplementationGroupProxyRi; local: *; };" > "$OBJ/uiprobe.vers"
"$GCC-ld" --target1-abs --no-undefined -nostdlib -shared -Ttext 0x8000 -Tdata 0x400000 \
  --version-script "$OBJ/uiprobe.vers" --entry _E32Dll -u _E32Dll -soname "$LINK" \
  "$OBJ/startup.o" "$OBJ/uiprobe.o" "${DSOS[@]}" "$LIBGCC" -Map "$OBJ/uiprobe.map" -o "$OBJ/uiprobe.elf"
"$E2E" --elfinput="$OBJ/uiprobe.elf" --output="$OUT/uiprobe.dll" --targettype=PLUGIN --linkas="$LINK" \
  --uid1=0x10000079 --uid2=0x10009D8D --uid3=0xE0001DD0 --sid=0xE0001DD0 --vid=0x0 --capability=All-TCB \
  --fpu=softvfp --definput="$HERE/uiprobe.def" --dso="$OBJ/uiprobe.dso" \
  --libpath="$LIBS" --compressionmethod=none >"$OBJ/e2e.log" 2>&1 || { cat "$OBJ/e2e.log"; exit 1; }
grep -i "error\|warning" "$OBJ/e2e.log" || true
cpp -P -I"$SDK/include" "$HERE/uiprobe.rss" -o "$OBJ/uiprobe.rpp"
PATH="$PATH:$GNUPOC/petran-1.1.0/e32uid" "$GNUPOC/rcomp-7.0.1/src/rcomp" -o"$OUT/uiprobe.rsc" \
  -s"$OBJ/uiprobe.rpp" -u < /dev/null
ls -la "$OUT/uiprobe.dll" "$OUT/uiprobe.rsc"
