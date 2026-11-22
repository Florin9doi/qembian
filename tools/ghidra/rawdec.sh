#!/usr/bin/env bash
# Decompile raw module bytes (cut from an EKA1 ROM at their address) with headless Ghidra, no GUI or MCP:
#   tools/ghidra/rawdec.sh BIN BASE OUT.c [arm|thumb] [ENTRY ...]
# BASE is the load address (0x5000f970); arm = ARMv5T code, thumb = Thumb (TMode set first, functions seeded
# at every push {.., lr}). Writes OUT.c (every function, "// ==== NAME @ ADDR" headers) and OUT.c.log;
# LISTING=1 also writes the disassembly to OUT.c.lst.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$(realpath "$1")"; BASE="$2"; OUT="$(realpath -m "$3")"; MODE="${4:-arm}"; shift 4 2>/dev/null || shift $#
GHIDRA_HOME="${GHIDRA_HOME:-$(ls -d $HOME/ghidra_*/support/analyzeHeadless | sort -V | tail -1 | xargs dirname | xargs dirname)}"
PROJ=$(mktemp -d)
"$GHIDRA_HOME/support/analyzeHeadless" "$PROJ" p -import "$BIN" -overwrite \
  -loader BinaryLoader -loader-baseAddr "$BASE" -processor ARM:LE:32:v5t \
  -scriptPath "$ROOT/tools/ghidra" -preScript RawSeed.java "$MODE" "$@" -postScript ExportDecompiled.java "$OUT" ${LISTING:+-postScript ExportListing.java "$OUT.lst"} \
  -deleteProject > "$OUT.log" 2>&1 || { tail -20 "$OUT.log"; exit 1; }
rm -rf "$PROJ"
grep -c '^// ====' "$OUT" | sed "s#^#$OUT: functions #"
