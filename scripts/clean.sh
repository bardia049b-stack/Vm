#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# clean.sh -- remove build output, and with --distclean the generated artefacts
#             a developer might otherwise have to remember to delete.
#
#     ./scripts/clean.sh              # objects, libraries, ./rvm, tests binary
#     ./scripts/clean.sh --distclean  # + dtb/rvm.dtb, disk.img, coverage, caches
#     ./scripts/clean.sh --dry-run    # print what would be removed
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DIST=0
DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --distclean|--all) DIST=1; shift ;;
        --dry-run|-n)      DRY=1; shift ;;
        -h|--help)         sed -n '3,11p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

log() { printf '\033[1;34m[clean]\033[0m %s\n' "$*"; }

log "running make clean"
if [ "$DRY" = 1 ]; then
    make -n clean
else
    make clean
fi

if [ "$DIST" = 1 ]; then
    TARGETS=(
        dtb/rvm.dtb
        disk.img
        vmlinux
        build/coverage.info
        "*.gcda" "*.gcno"
    )
    for t in "${TARGETS[@]}"; do
        for f in $t; do
            [ -e "$f" ] || continue
            if [ "$DRY" = 1 ]; then
                log "would remove $f"
            else
                log "removing $f"
                rm -rf -- "$f"
            fi
        done
    done
fi

log "done"
