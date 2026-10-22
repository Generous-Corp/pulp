#!/bin/bash
# Record the FDN reverb's reference fingerprints from a released engine.
#
#   tools/audio/fdn_reference/generate.sh [git-ref]   (default v0.1004.0)
#
# Extracts that ref's core/ headers into a temporary directory, builds
# fingerprint_driver.cpp against them, and prints the envelopes as C++
# initializers for test/test_fdn_reverb_block_engine.cpp's kReference table.
# v0.1004.0 is the last release whose FdnReverb ran the sample-at-a-time
# network the block engine replaced. Pass the working tree's own engine with
# --current to measure it the same way.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
ref="${1:-v0.1004.0}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
if [ "$ref" = "--current" ]; then
    src="$root"
else
    git -C "$root" archive "$ref" core | tar -x -C "$work"
    src="$work"
fi
includes=()
for dir in "$src"/core/*/include; do includes+=("-I$dir"); done
"${CXX:-c++}" -std=c++20 -O2 -DNDEBUG "${includes[@]}" "$here/fingerprint_driver.cpp" \
    -o "$work/fingerprint"
"$work/fingerprint"
