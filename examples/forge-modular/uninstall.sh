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
USER_PLUGINS="$HOME/Library/Audio/Plug-Ins"
SYSTEM_PLUGINS="/Library/Audio/Plug-Ins"

# Every path this product creates, in one list, so nothing is remembered only
# in somebody's head. Keep in step with package.sh and the first-run SDK fetch.
SOFTWARE=(
    "/Applications/Forge Modular.app"
    "$SYSTEM_PLUGINS/Components/Forge Modular.component"
    "$SYSTEM_PLUGINS/VST3/Forge Modular.vst3"
    "$SYSTEM_PLUGINS/CLAP/Forge Modular.clap"
    # Clean up legacy per-user builds too; released installers use /Library.
    "$USER_PLUGINS/Components/Forge Modular.component"
    "$USER_PLUGINS/VST3/Forge Modular.vst3"
    "$USER_PLUGINS/CLAP/Forge Modular.clap"
    "$SUPPORT/Forge Modular/tools"
    "$SUPPORT/Forge Modular/Rack-SDK"
    "$SUPPORT/Forge Modular/runs"
    # The library catalog, the module index, the cached entitlements and the
    # built patch gate. All derived, and the entitlements file holds a VCV
    # token, which is the one thing here a person would mind being left behind.
    "$HOME/.cache/forge-modular"
)

# The Rack pack exists in TWO shapes and both are a real installation: the
# installer places a `ForgeModular-<version>-mac-<arch>.vcvplugin` archive, and
# Rack unpacks it into a `ForgeModular/` directory the first time it starts.
# Removing only the directory left a user who had installed and never opened
# Rack unable to remove the modules at all -- and the archive would then be
# unpacked by the next Rack launch, so the software came back.
#
# The architecture is not assumed: a Mac can carry an Intel and an Apple
# Silicon plugin folder, and Rack names one directory per platform key.
while IFS= read -r p; do
    SOFTWARE+=("$p")
done < <(find "$SUPPORT/Rack2" -mindepth 2 -maxdepth 2 -path '*/plugins-*/*' \
              \( -name 'ForgeModular' -o -name 'ForgeModular-*.vcvplugin' \) \
              2>/dev/null)

# What the user MADE. Only touched with --all.
WORK=(
    "$SUPPORT/Forge Modular/projects"
    "$SUPPORT/Forge Modular/examples"
    "$SUPPORT/Forge Modular/patches"
    "$SUPPORT/Forge Modular/replaced-rack-packs"
)

gone=0
kept=0
failed=0

remove() {
    local path="$1"
    if [ ! -e "$path" ] && [ ! -L "$path" ]; then return; fi
    if [ "$DRY" -eq 1 ]; then
        echo "  would remove  $path"
        gone=$((gone + 1))
    elif [[ "$path" = /Applications/* || "$path" = /Library/* ]] && \
         [ "$(id -u)" -ne 0 ]; then
        if /usr/bin/sudo /bin/rm -rf "$path"; then
            echo "  removed  $path"; gone=$((gone + 1))
        else
            echo "  FAILED   $path" >&2; failed=$((failed + 1))
        fi
    else
        if rm -rf "$path"; then
            echo "  removed  $path"; gone=$((gone + 1))
        else
            echo "  FAILED   $path" >&2; failed=$((failed + 1))
        fi
    fi
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
if [ "$DRY" -eq 1 ]; then
    echo "$gone item(s) would be removed."
elif [ "$gone" -eq 0 ] && [ "$failed" -eq 0 ]; then
    echo "Nothing to remove. Forge Modular is not installed."
elif [ "$gone" -gt 0 ]; then
    echo "Removed $gone item(s)."
else
    echo "No requested item was removed."
fi
[ "$kept" -gt 0 ] && echo "Kept your patches and projects. Use --all to remove those too."
if [ "$failed" -gt 0 ]; then
    echo "$failed item(s) could not be removed." >&2
    exit 1
fi
exit 0
