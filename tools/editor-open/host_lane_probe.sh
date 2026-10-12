#!/bin/bash
# Run pulp-host-lane-probe (see host_lane_probe.mm): load built plug-in
# bundles into one host process and record load, audio with the editor
# closed/open, the editor lifecycle read-back and the dyld census. Builds the
# probe into the build dir if it is missing.
#
#   tools/editor-open/host_lane_probe.sh [--build-dir DIR] -- \
#       --plugin build/CLAP/SpectralLab.clap --out /tmp/host-obs
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build_dir="$root/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=$2; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
probe="$build_dir/tools/editor-open/pulp-host-lane-probe"
if [ ! -x "$probe" ]; then
    echo "host_lane_probe: building $probe" >&2
    "$root/tools/ci/governed-build.sh" cmake --build "$build_dir" --target pulp-host-lane-probe >&2
fi
exec "$probe" "$@"
