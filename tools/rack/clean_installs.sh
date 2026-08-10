#!/usr/bin/env bash
# Remove the artifacts an install leaves behind on a machine.
#
#   clean_installs.sh          # dry run: list what would go, and the total
#   clean_installs.sh --yes    # remove it
#
# Two kinds of litter, both of which have cost real time:
#
#   * SUFFIXED COPIES -- "Forge Modular.vst3.prev",
#     "Forge Modular.app.backup-20260731-1219", ".signed-backup". Nothing in
#     this repository creates these; they are hand-made "let me keep the old
#     one" copies from interactive sessions. They accumulated to 513 MB on one
#     machine and 628 MB on another. The build directory is the previous build
#     and git is the history, so a copy beside the installed bundle is never the
#     only record of anything.
#   * A COPY IN ~/Applications. macOS searches it as well as /Applications and
#     the home one SHADOWS the installed copy for Spotlight and the Dock, with
#     nothing in the running window to say which answered. A fix was tested
#     against the wrong binary and reported as not working, on two machines.
#
# Deliberately narrow. It only ever removes paths whose name begins with one of
# the four bundle names, and it refuses any path whose name IS one of them --
# except ~/Applications, which is the shadowing case and is named separately.
# It never touches /Applications, the plug-in bundles themselves, the module
# pack, the projects, or anything a user made.

set -uo pipefail
shopt -s nullglob

APPLY=0
case "${1:-}" in
    --yes)  APPLY=1 ;;
    "")     ;;
    *) echo "usage: $(basename "$0") [--yes]" >&2; exit 2 ;;
esac

NAMES=("Forge Modular.app" "Forge Modular.component"
       "Forge Modular.vst3" "Forge Modular.clap")

live_name() { # is this path named exactly like a live bundle?
    local base; base="$(basename "$1")"
    for n in "${NAMES[@]}"; do [ "$base" = "$n" ] && return 0; done
    return 1
}

candidates=()

# Suffixed copies beside every place a bundle is installed.
for p in "$HOME/Applications/Forge Modular.app".* \
         /Applications/"Forge Modular.app".* \
         "$HOME/Library/Audio/Plug-Ins/Components/Forge Modular.component".* \
         "$HOME/Library/Audio/Plug-Ins/VST3/Forge Modular.vst3".* \
         "$HOME/Library/Audio/Plug-Ins/CLAP/Forge Modular.clap".*; do
    live_name "$p" || candidates+=("$p")
done

# The shadowing copy itself. Named on its own so the "never a live name" rule
# above stays absolute everywhere else.
[ -e "$HOME/Applications/Forge Modular.app" ] && \
    candidates+=("$HOME/Applications/Forge Modular.app")

if [ ${#candidates[@]} -eq 0 ]; then
    echo "nothing to clean"
    exit 0
fi

total=0
for p in "${candidates[@]}"; do
    kb=$(du -sk "$p" 2>/dev/null | cut -f1)
    total=$((total + kb))
    why="stale copy"
    [ "$(basename "$p")" = "Forge Modular.app" ] && \
        why="shadows /Applications for Spotlight and the Dock"
    printf "  %5s MB  %-46s  %s\n" "$((kb / 1024))" "${p/#$HOME/~}" "$why"
done
printf "  %5s MB  total\n" "$((total / 1024))"

if [ "$APPLY" -eq 0 ]; then
    echo
    echo "dry run. Remove them with: $(basename "$0") --yes"
    exit 0
fi

echo
for p in "${candidates[@]}"; do
    rm -rf "$p" && echo "  removed ${p/#$HOME/~}"
done
printf "  %s MB reclaimed\n" "$((total / 1024))"
