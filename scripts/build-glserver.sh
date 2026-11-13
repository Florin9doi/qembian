#!/usr/bin/env bash
# Build the host GL server of the hosted EKA2 board: src/hosted/glserver (syborg_gl.c from the QEMU board,
# unchanged, against a QEMU shim). Needs what the QEMU board's GL needs: epoxy, glib, the syvg library
# (scripts/build-syvg.sh -> build/syvg/libsyvg.a). Output: build/hosted/glserver/glserver
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
[ -f build/syvg/libsyvg.a ] || scripts/build-syvg.sh
O=build/hosted/glserver
mkdir -p "$O"
HW=src/qemu-syborg/hw/arm
SRCS=(src/hosted/glserver/glserver.c "$HW"/syborg_gl*.c "$HW"/syborg_gl*.h src/hosted/glserver/shim/qemu_shim.h
      src/hosted/include/board/syborg_glchan.h)
if [ ! -f "$O/glserver" ] || [ -n "$(find "${SRCS[@]}" build/syvg/libsyvg.a -newer "$O/glserver" 2>/dev/null)" ]; then
  # syborg_gl.c includes syvg.h and vg2_fn.h from its own directory in the QEMU tree (build-qemu-modern.sh
  # copies them there): here they come from src/syvg
  ${CC:-gcc} -O2 -g -std=gnu11 -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
    -Isrc/hosted/glserver/shim -I"$HW" -Isrc/syvg -Isrc/hosted/include/board \
    $(pkg-config --cflags glib-2.0 epoxy) \
    -o "$O/glserver" src/hosted/glserver/glserver.c build/syvg/libsyvg.a \
    $(pkg-config --libs glib-2.0 epoxy) -lstdc++ -lm -lpthread
fi
ls -l "$O/glserver"
