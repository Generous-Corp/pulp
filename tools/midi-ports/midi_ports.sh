#!/usr/bin/env bash
# Run the built pulp-midi-ports probe. Looks in $PULP_BUILD_DIR, then ./build.
# Build it first with: pulp build --target pulp-midi-ports
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${PULP_BUILD_DIR:-$root/build}"
bin="$build/tools/midi-ports/pulp-midi-ports"
if [ ! -x "$bin" ]; then
    echo "pulp-midi-ports is not built in $build; run: pulp build --target pulp-midi-ports" >&2
    exit 3
fi
exec "$bin" "$@"
