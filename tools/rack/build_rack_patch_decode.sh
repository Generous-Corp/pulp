#!/usr/bin/env bash
# Build the self-contained, read-only zstd/tar decoder used by rack_open.py.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/rack_patch_decode}"
VENDOR="$HERE/vendor/zstd-1.5.7"

if [ ! -f "$VENDOR/zstddeclib.c" ] || [ ! -f "$VENDOR/LICENSE" ]; then
  echo "Pinned zstd decoder sources are incomplete at $VENDOR" >&2
  exit 1
fi

mkdir -p "$(dirname "$OUT")"
BUILD="$(mktemp -d)"
STAGED="$(mktemp "$(dirname "$OUT")/.rack_patch_decode.XXXXXX")"
trap 'rm -rf "$BUILD"; rm -f "$STAGED"' EXIT
"${CC:-/usr/bin/cc}" -std=c99 -O2 -DNDEBUG \
  -c "$VENDOR/zstddeclib.c" -o "$BUILD/zstddeclib.o"
"${CXX:-/usr/bin/c++}" -std=c++17 -O2 -DNDEBUG \
  -Wall -Wextra -Wpedantic -Werror \
  "$HERE/rack_patch_decode.cpp" "$BUILD/zstddeclib.o" -o "$STAGED"
chmod 0755 "$STAGED"
mv -f "$STAGED" "$OUT"
