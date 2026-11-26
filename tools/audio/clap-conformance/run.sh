#!/bin/sh
# Run the CLAP conformance checker from a source checkout.
# usage: run.sh <plugin.clap> [checker options]   (build target: pulp-clap-conformance)
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
bin=${PULP_CLAP_CONFORMANCE:-$root/build/tools/audio/pulp-clap-conformance}
if [ ! -x "$bin" ]; then
    echo "run.sh: $bin not built; run: pulp build --target pulp-clap-conformance" >&2
    exit 2
fi
exec "$bin" "$@"
