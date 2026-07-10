#!/usr/bin/env bash
# Regenerate the editor screenshots the README embeds.
#
# The images are rendered headlessly from the real `Processor::create_view()`
# path, at exactly the size each plug-in reports from `editor_size()` — so a
# screenshot cannot flatter a layout a host would never show, and cannot drift
# from the editor it claims to depict. Run this after any editor change and
# commit the result alongside it.
#
#   examples/bitches-brew/docs/update-screenshots.sh [build-dir]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
build="${1:-$repo/build}"
shots="$build/examples/bitches-brew/ui/brew-shots"

if [[ ! -x "$shots" ]]; then
    echo "no brew-shots at $shots" >&2
    echo "build it first:  cmake --build $build --target brew-shots -j\$(sysctl -n hw.ncpu)" >&2
    exit 2
fi

"$shots" "$here/images"
echo
echo "wrote $(find "$here/images" -name '*.png' | wc -l | tr -d ' ') images to $here/images"
