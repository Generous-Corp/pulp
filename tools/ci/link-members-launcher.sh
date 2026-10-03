#!/bin/sh
# Linker launcher that keeps the object list of every executable's link.
#
#   link-members-launcher.sh <build-root> <link command...>
#
# Runs the link command with one extra `-Wl,-map,<file>` and exits with the
# linker's status. After a successful executable or loadable-module (`-bundle`,
# a CMake MODULE_LIBRARY such as a plug-in bundle) link it keeps the head of the
# map (output path and "# Object files:" list, not the multi-megabyte symbol
# table) as <build-root>/link-members/<name>-<id>.objects, and the link
# arguments as .args, for tools/ci/link_members.py to read; the map itself is
# deleted. Shared libraries and partial links are run untouched, and
# so is any link whose record directory cannot be written: recording never
# changes whether a link succeeds. See tools/cmake/PulpLinkMaps.cmake.

root=$1
shift
out=""
prev=""
for arg in "$@"; do
    [ "$prev" = "-o" ] && out=$arg
    case $arg in
        -dynamiclib|-shared|-r) exec "$@" ;;
    esac
    prev=$arg
done
[ -n "$out" ] || exec "$@"

dir="$root/link-members"
mkdir -p "$dir" 2>/dev/null
{ [ -d "$dir" ] && [ -w "$dir" ]; } || exec "$@"
case $out in
    /*) abs=$out ;;
    *) abs=$PWD/$out ;;
esac
id=$(printf '%s' "$abs" | cksum | cut -d' ' -f1)
base="$dir/${out##*/}-$id"
rm -f "$base.objects" "$base.args"

"$@" "-Wl,-map,$base.map"
rc=$?
if [ "$rc" -eq 0 ] && [ -f "$base.map" ]; then
    if { printf '# Cwd: %s\n' "$PWD"; sed -n '/^# Sections:/q;p' "$base.map"; } > "$base.objects.tmp" 2>/dev/null \
        && printf '%s\n' "$@" > "$base.args.tmp" 2>/dev/null; then
        mv -f "$base.objects.tmp" "$base.objects" 2>/dev/null
        mv -f "$base.args.tmp" "$base.args" 2>/dev/null
    fi
    rm -f "$base.objects.tmp" "$base.args.tmp"
fi
rm -f "$base.map"
exit "$rc"
