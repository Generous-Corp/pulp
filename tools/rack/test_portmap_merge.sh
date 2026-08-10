#!/usr/bin/env bash
# The port-map merge, without Rack.
#
# The scanner needs Rack running and a rack full of modules to exercise. The
# MERGE does not, and the merge is the part with the edge cases -- an empty
# scan, a re-measured module, a map it cannot read. So it lives in a header
# both the module and this test include, and is checked here in a second.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../examples/forge-modular/src"
BIN="$(mktemp -d)/test_portmap_merge"
trap 'rm -rf "$(dirname "$BIN")"' EXIT
if ! c++ -std=c++17 -I "$SRC" "$SRC/test_portmap_merge.cpp" -o "$BIN" 2>&1; then
    echo "test_portmap_merge: failed to compile" >&2
    exit 1
fi
"$BIN"
