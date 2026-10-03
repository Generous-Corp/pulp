#!/usr/bin/env bash
# Keep a reused build directory on one CMake generator.
#
# CMake refuses to configure an existing build directory with a different
# generator ("Does not match the generator used previously"), so a lane that
# starts passing `-G Ninja` would fail on every warm directory configured with
# another one. This removes such a directory first, so the configure that
# follows starts clean: one cold build per directory, once.
#
# The local mac lane needs Ninja because its reuse record reads per-object
# dependencies from Ninja's dependency log (.ninja_deps); the Makefiles
# generator keeps no equivalent with a validity marker, so every object would
# read as unrecorded and nothing could ever be reused.
#
#   require_build_generator.sh <build-dir> <generator>
set -euo pipefail
dir="${1:-}"
want="${2:-}"
if [ -z "$dir" ] || [ -z "$want" ]; then
  echo "usage: require_build_generator.sh <build-dir> <generator>" >&2
  exit 2
fi
cache="$dir/CMakeCache.txt"
# Not configured yet: the configure that follows creates it fresh.
[ -f "$cache" ] || exit 0
have="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache" | head -n 1)"
# A cache that names no generator is left for CMake to report, never wiped.
if [ -z "$have" ] || [ "$have" = "$want" ]; then
  exit 0
fi
# Remove only a real directory strictly inside the checkout this runs from:
# never a symlink (which could point anywhere), never the checkout itself.
if [ -L "$dir" ]; then
  echo "require-build-generator: refusing to remove $dir: it is a symlink" >&2
  exit 1
fi
root="$(pwd -P)"
real="$(cd "$dir" && pwd -P)"
case "$real" in
  "$root"/?*) ;;
  *)
    echo "require-build-generator: refusing to remove $real: not inside $root" >&2
    exit 1
    ;;
esac
echo "require-build-generator: $dir was configured with '$have', not '$want';" \
     "removing it so this configure starts clean"
rm -rf "$real"
