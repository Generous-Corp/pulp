#!/usr/bin/env bash
# Remove Forge Modular from this Mac.
#
# Ships INSIDE the app bundle (Contents/Resources/uninstall.sh) and is linked
# from the app, so there is no second thing to download and no README step. An
# uninstaller you have to go and find is one people do not run; they drag the
# app to the Trash and leave the plug-ins, the Rack modules and the SDK behind.
#
#   uninstall.sh              # remove the software, KEEP patches and projects
#   uninstall.sh --all        # also remove everything you have made
#   uninstall.sh --dry-run    # print what would go, touch nothing
#
# Your work is kept by default. Patches take minutes of model time to make and
# a person uninstalling a beta is usually reinstalling it an hour later; the
# one thing they cannot get back is what they generated.

set -uo pipefail

DRY=0
ALL=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY=1 ;;
        --all)     ALL=1 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

SUPPORT="$HOME/Library/Application Support"
PLUGINS="$HOME/Library/Audio/Plug-Ins"

# Every path this product creates, in one list, so nothing is remembered only
# in somebody's head. Keep in step with package.sh and the first-run SDK fetch.
SOFTWARE=(
    "/Applications/Forge Modular.app"
    "$PLUGINS/Components/Forge Modular.component"
    "$PLUGINS/VST3/Forge Modular.vst3"
    "$PLUGINS/CLAP/Forge Modular.clap"
    "$SUPPORT/Rack2/plugins-mac-arm64/ForgeModular"
    "$SUPPORT/Forge Modular/tools"
    "$SUPPORT/Forge Modular/Rack-SDK"
    "$SUPPORT/Forge Modular/runs"
)

# What the user MADE. Only touched with --all.
WORK=(
    "$SUPPORT/Forge Modular/projects"
    "$SUPPORT/Forge Modular/examples"
)

gone=0
kept=0

remove() {
    local path="$1"
    if [ ! -e "$path" ] && [ ! -L "$path" ]; then return; fi
    if [ "$DRY" -eq 1 ]; then
        echo "  would remove  $path"
    else
        rm -rf "$path" && echo "  removed  $path" || echo "  FAILED   $path" >&2
    fi
    gone=$((gone + 1))
}

echo "Forge Modular uninstaller"
[ "$DRY" -eq 1 ] && echo "(dry run: nothing will be deleted)"
echo

# Refuse to pull the app out from under itself. Deleting a running bundle
# leaves the process on a deleted inode and the next launch on a half-removed
# tree, which reads as a corrupt install rather than an interrupted uninstall.
if pgrep -x "Forge Modular" >/dev/null 2>&1; then
    echo "Forge Modular is running. Quit it first, then run this again." >&2
    exit 1
fi
if pgrep -x "Rack" >/dev/null 2>&1; then
    echo "VCV Rack is running, and it has our modules loaded." >&2
    echo "Quit Rack first, then run this again." >&2
    exit 1
fi

for p in "${SOFTWARE[@]}"; do remove "$p"; done

if [ "$ALL" -eq 1 ]; then
    for p in "${WORK[@]}"; do remove "$p"; done
    # Only now is the product directory empty enough to go.
    remove "$SUPPORT/Forge Modular"
else
    for p in "${WORK[@]}"; do
        [ -e "$p" ] && { echo "  kept     $p"; kept=$((kept + 1)); }
    done
fi

echo
if [ "$gone" -eq 0 ]; then
    echo "Nothing to remove. Forge Modular is not installed."
elif [ "$DRY" -eq 1 ]; then
    echo "$gone item(s) would be removed."
else
    echo "Removed $gone item(s)."
fi
[ "$kept" -gt 0 ] && echo "Kept your patches and projects. Use --all to remove those too."
exit 0
