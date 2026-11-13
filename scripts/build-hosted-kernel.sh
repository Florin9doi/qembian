#!/usr/bin/env bash
# Build the arm64 Linux guest kernel for the hosted-EKA2 virt machine (docs/hosted-eka2/qemu-setup.md).
#   scripts/build-hosted-kernel.sh [LINUX_SRC]
# Source: downloads/linux-6.18.63.tar.xz (longterm, pinned in sources.lock) unpacked into build/hosted/,
# or a full kernel tree given as LINUX_SRC; built out of tree into build/hosted/linux;
# config = allnoconfig + src/hosted/linux/hosted.config (the whole requested config; every line checked).
# Needs an aarch64 cross gcc (CROSS_COMPILE, default aarch64-suse-linux- or aarch64-linux-gnu-).
# Output: build/hosted/linux/arch/arm64/boot/Image
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
KVER=6.18.63
SRC=${1:-$ROOT/build/hosted/linux-$KVER}
if [ -z "${1:-}" ] && [ ! -d "$SRC" ]; then
  [ -f downloads/linux-$KVER.tar.xz ] || scripts/fetch.sh
  mkdir -p build/hosted && tar -C build/hosted -xJf downloads/linux-$KVER.tar.xz
fi
[ -f "$SRC/arch/arm64/kernel/entry.S" ] || { echo "no full arm64 kernel source in $SRC"; exit 1; }
if [ -z "${CROSS_COMPILE:-}" ]; then
  for c in aarch64-suse-linux- aarch64-linux-gnu-; do command -v ${c}gcc >/dev/null && CROSS_COMPILE=$c && break; done
fi
O=$ROOT/build/hosted/linux
mkdir -p "$O"
K="make -C $SRC O=$O ARCH=arm64 CROSS_COMPILE=$CROSS_COMPILE"
CFG=src/hosted/linux/hosted.config
if [ ! -f "$O/.config" ] || [ $CFG -nt "$O/.config" ] || [ "$0" -nt "$O/.config" ]; then
  $K allnoconfig KCONFIG_ALLCONFIG="$ROOT/$CFG" >/dev/null
  # every requested line must hold: CONFIG_X=y/value as written, CONFIG_X=n as "is not set"
  bad=0
  while read -r line; do
    opt=${line%%=*}; val=${line#*=}
    if [ "$val" = n ]; then
      grep -q "^$opt=" "$O/.config" && { echo "config: $opt=n requested, but $(grep "^$opt=" "$O/.config")"; bad=1; }
    else
      grep -qx "$opt=$val" "$O/.config" || { echo "config: $opt=$val requested, got: $(grep "^$opt=\|^# $opt is" "$O/.config" || echo absent)"; bad=1; }
    fi
  done < <(sed -e 's/#.*//' -e 's/[[:space:]]*$//' $CFG | grep '^CONFIG_')
  [ $bad = 0 ] || { rm -f "$O/.config"; exit 1; }
  grep '^CONFIG_' "$O/.config" > "$O/hosted-resolved.config"
  echo "config: $(grep -c '^CONFIG_' $CFG) requested, $(wc -l < "$O/hosted-resolved.config") set after dependencies ($O/hosted-resolved.config)"
fi
$K -j"$(nproc)" Image 2>&1 | grep -E 'error|Error' || true
ls -l "$O/arch/arm64/boot/Image"
