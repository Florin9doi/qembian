#!/usr/bin/env bash
# Decompile one N95 XIP binary with headless Ghidra (no GUI, no MCP).
#   tools/ghidra/decompile.sh NAME.dll [OUTDIR]   -> OUTDIR/NAME.c (+ NAME.elf for the Ghidra GUI)
#   VTABLES="0x... 0x..." tools/ghidra/decompile.sh ...   also walks those vtables (virtual methods never called directly)
#   tools/ghidra/decompile.sh path/to/file.elf [OUTDIR]  (e.g. a ROFS binary from tools/e32load.py)
# GHIDRA_HOME defaults to the newest ~/ghidra_* install.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
DEVDIR="$(python3 -I tools/device.py paths dir)"   # the device ($SYBORG_DEVICE / the registry default)
NAME="$1"; OUT="${2:-$DEVDIR/ghidra}"; mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"   # absolute: Ghidra runs elsewhere
# the newest install that has analyzeHeadless (~/ghidra_scripts is not one; bug 2026-11-19)
GHIDRA_HOME="${GHIDRA_HOME:-$(ls -d $HOME/ghidra_*/support/analyzeHeadless | sort -V | tail -1 | xargs dirname | xargs dirname)}"
# VTABLES="0x80f0d634 0x80f0d700": create functions at those vtables' slots first (tools/ghidra/VtableFuncs.java)
PRE=(); [[ -n "${VTABLES:-}" ]] && PRE=(-preScript VtableFuncs.java $VTABLES)
if [[ "$NAME" == *.elf ]]; then cp "$NAME" "$OUT/"; NAME=$(basename "$NAME" .elf)
else python3 -I tools/xip2elf.py "$DEVDIR/core-flat.img" "$DEVDIR/core.json" "$NAME" "$OUT/$NAME.elf"; fi
PROJ=$(mktemp -d)
"$GHIDRA_HOME/support/analyzeHeadless" "$PROJ" p -import "$OUT/$NAME.elf" -overwrite \
  -scriptPath "$ROOT/tools/ghidra" "${PRE[@]}" -postScript ExportDecompiled.java "$OUT/$NAME.c" \
  -deleteProject > "$OUT/$NAME.ghidra.log" 2>&1 || { tail -20 "$OUT/$NAME.ghidra.log"; exit 1; }
rm -rf "$PROJ"
grep -c '^// ====' "$OUT/$NAME.c" | sed "s#^#$OUT/$NAME.c: functions #"
