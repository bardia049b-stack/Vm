#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build.sh -- configure and build RVM, plus anything optional that is present.
#
#     ./scripts/build.sh              # ./rvm, warnings as errors
#     ./scripts/build.sh --debug      # -O0 -g and the trace/asan knobs on
#     ./scripts/build.sh --release    # -O3, no debug symbols
#     ./scripts/build.sh --jobs 4     # parallel make
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE="release"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
WERROR=1

while [ $# -gt 0 ]; do
    case "$1" in
        --debug)   MODE="debug"; shift ;;
        --release) MODE="release"; shift ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        --no-werror) WERROR=0; shift ;;
        -h|--help) sed -n '3,12p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

log() { printf '\033[1;34m[build]\033[0m %s\n' "$*"; }

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || { echo "no C compiler ($CC) found" >&2; exit 1; }
log "compiler: $("$CC" --version 2>&1 | head -1)"
log "mode: $MODE, jobs: $JOBS"

EXTRA=""
[ "$WERROR" = 1 ] && EXTRA="WERROR=1"

# Objects from a different mode would be reused silently, so track the mode and
# start over when it changes.
STAMP="build/.mode"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" != "$MODE" ]; then
    log "mode changed from $(cat "$STAMP") to $MODE; rebuilding from scratch"
    make clean >/dev/null
fi
mkdir -p build
echo "$MODE" > "$STAMP"

make -j"$JOBS" MODE="$MODE" $EXTRA rvm

# Optional: compile the device tree if dtc is available.
if command -v dtc >/dev/null 2>&1 && [ -f dtb/rvm.dts ]; then
    log "compiling dtb/rvm.dts"
    make dtb
else
    log "dtc not found; skipping dtb/rvm.dtb (the VM builds one in-process)"
fi

log "done: $(ls -l rvm | awk '{print $5}') bytes"
./rvm --version
