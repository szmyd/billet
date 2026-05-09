#!/usr/bin/env bash
# Drive tools/compare.py over a directory of collected billet results.
# Picks up *-rr4k.json and *-pg.json across subdirs and emits one
# comparison HTML per profile.
#
# Usage:
#   compare-collected.sh <collected-root> [output-dir]

set -euo pipefail

ROOT="${1:?usage: $0 <collected-root> [output-dir]}"
OUT="${2:-$ROOT}"
mkdir -p "$OUT"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
COMPARE="$REPO_ROOT/tools/compare.py"

if [ ! -x "$COMPARE" ]; then
    echo "tools/compare.py not found at $COMPARE" >&2
    exit 1
fi

mapfile -t rr_files < <(find "$ROOT" -name '*-rr4k.json' | sort)
mapfile -t pg_files < <(find "$ROOT" -name '*-pg.json' | sort)

if [ "${#rr_files[@]}" -gt 0 ]; then
    python3 "$COMPARE" "${rr_files[@]}" \
        -o "$OUT/compare-rr4k.html" \
        --title "random_read_4k comparison"
    echo "wrote $OUT/compare-rr4k.html (${#rr_files[@]} runs)"
fi

if [ "${#pg_files[@]}" -gt 0 ]; then
    python3 "$COMPARE" "${pg_files[@]}" \
        -o "$OUT/compare-pg.html" \
        --title "postgresql comparison"
    echo "wrote $OUT/compare-pg.html (${#pg_files[@]} runs)"
fi

if [ "${#rr_files[@]}" -eq 0 ] && [ "${#pg_files[@]}" -eq 0 ]; then
    echo "no *-rr4k.json or *-pg.json files found under $ROOT" >&2
    exit 2
fi
