#!/bin/bash
# Place the Forge Modular pack where VCV Rack looks for it.
#
# Rack loads plug-ins from the USER's Application Support, which a package
# payload cannot address: an installer writes as root, to absolute paths, and
# "the user" is not a path. So the .vcvplugin rides inside the app bundle and
# something has to move it afterwards. This is that something.
#
#   install_pack.sh --source <app bundle or dir> --home <HOME> [--owner uid:gid]
#                   [--dry-run]
#
# ONE implementation with two callers, because two would disagree:
#
#   the installer's postinstall  runs as root, so it passes --home and --owner
#                                for the console user, whose $HOME it does not
#                                have
#   patch.py, on every run       runs as the user, so it passes its own $HOME
#                                and no --owner
#
# The second exists because a postinstall that fails aborts the whole install,
# so it is written to never fail; a generation that finds no pack can repair it
# quietly instead. Neither can be dropped: the first is what makes Rack show
# the modules before anything else is run, the second is what recovers if it
# did not.

set -uo pipefail

SOURCE=""
DEST_HOME=""
OWNER=""
DRY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --source) SOURCE="${2:-}"; shift 2 ;;
        --home)   DEST_HOME="${2:-}"; shift 2 ;;
        --owner)  OWNER="${2:-}"; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        *) echo "install_pack: unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$SOURCE" ] || { echo "install_pack: --source is required" >&2; exit 2; }
[ -n "$DEST_HOME" ] || { echo "install_pack: --home is required" >&2; exit 2; }

# --source may be the app bundle or the directory holding the pack. Accepting
# both means neither caller has to know the bundle's internal layout, and the
# layout can move without two call sites needing the same edit.
if [ -d "$SOURCE/Contents/Resources/rack" ]; then
    SRC_DIR="$SOURCE/Contents/Resources/rack"
elif [ -d "$SOURCE" ]; then
    SRC_DIR="$SOURCE"
else
    echo "install_pack: no such source: $SOURCE" >&2
    exit 1
fi

# A glob is the obvious spelling and is wrong twice over: zsh aborts the whole
# command when one matches nothing, and every path here contains "Forge
# Modular.app" and "Application Support", so an unquoted `$(find …)` in a for
# loop splits them on the space and looks for a directory called "Forge". Read
# whole lines, never split them.
PACK="$(find "$SRC_DIR" -maxdepth 1 -name '*.vcvplugin' 2>/dev/null | sort | tail -1)"
if [ -z "$PACK" ]; then
    echo "install_pack: no .vcvplugin in $SRC_DIR" >&2
    exit 1
fi

BASE="$(basename "$PACK")"

# ForgeModular-2.0.0-mac-arm64.vcvplugin -> slug, version, platform key.
#
# Parsed from the NAME rather than hardcoded, because the name is what Rack
# itself keys on: the platform suffix names the plugin directory, and a pack
# built for arm64 dropped into plugins-mac-x64 is a plugin Rack silently will
# not load. A hardcoded "arm64" here would ship a working installer that is
# wrong the moment an Intel or Linux build exists.
SLUG="${BASE%%-*}"
REST="${BASE#*-}"                  # 2.0.0-mac-arm64.vcvplugin
REST="${REST%.vcvplugin}"          # 2.0.0-mac-arm64
VERSION="${REST%%-*}"              # 2.0.0
PLATFORM="${REST#*-}"              # mac-arm64

if [ -z "$SLUG" ] || [ -z "$VERSION" ] || [ -z "$PLATFORM" ] || [ "$VERSION" = "$REST" ]; then
    echo "install_pack: cannot read slug/version/platform out of '$BASE'" >&2
    exit 1
fi

DEST_DIR="$DEST_HOME/Library/Application Support/Rack2/plugins-$PLATFORM"

# What is already there, and is it newer than what we carry?
#
# Rack unpacks a .vcvplugin into a directory named for the slug and removes the
# archive, so BOTH shapes are a real installation and both have to be read. A
# user who built a newer pack from inside the app has the unpacked form, and
# overwriting it with the installer's copy would silently undo their work.
installed_version() {
    local unpacked="$DEST_DIR/$SLUG/plugin.json"
    if [ -f "$unpacked" ]; then
        sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
            "$unpacked" | head -1
        return
    fi
    local archive n
    archive="$(find "$DEST_DIR" -maxdepth 1 -name "$SLUG-*.vcvplugin" 2>/dev/null | sort | tail -1)"
    if [ -n "$archive" ]; then
        n="${archive##*/}"; n="${n#*-}"
        echo "${n%%-*}"
    fi
}

HAVE="$(installed_version)"
if [ -n "$HAVE" ] && [ "$HAVE" = "$VERSION" ]; then
    # Version alone cannot distinguish the stock pack from a pack rebuilt by
    # the user: generated packs intentionally retain the plugin's compatible
    # 2.0.0 version. Preserve that identity across installer upgrades.
    if [ -f "$DEST_DIR/.ForgeModular-user-pack" ] || \
       [ -f "$DEST_DIR/$SLUG/.forge-generated-pack" ]; then
        echo "install_pack: keeping user-generated $SLUG $HAVE"
        exit 0
    fi
fi
if [ -n "$HAVE" ] && [ "$HAVE" != "$VERSION" ]; then
    # sort -V puts the lower version first, so "the newest is the one already
    # there and it is not ours" means leave it alone.
    NEWEST="$(printf '%s\n%s\n' "$HAVE" "$VERSION" | sort -V | tail -1)"
    if [ "$NEWEST" = "$HAVE" ]; then
        echo "install_pack: keeping $SLUG $HAVE, which is newer than $VERSION"
        exit 0
    fi
fi

if [ "$DRY" -eq 1 ]; then
    echo "install_pack: would place $BASE in $DEST_DIR"
    exit 0
fi

mkdir -p "$DEST_DIR" || {
    echo "install_pack: could not create $DEST_DIR" >&2; exit 1; }

# Replace an older copy of OUR pack only. Anything else in that directory is
# somebody else's plugin and is none of our business.
find "$DEST_DIR" -maxdepth 1 -name "$SLUG-*.vcvplugin" 2>/dev/null \
    | while IFS= read -r old; do
        [ "$old" = "$DEST_DIR/$BASE" ] && continue
        rm -f "$old"
    done

# THE UNPACKED DIRECTORY IS WHAT RACK LOADS, AND IT WINS OVER THE ARCHIVE.
#
# Rack unpacks a .vcvplugin into `<slug>/` and then loads that directory. So
# placing a new archive beside a stale unpacked copy changes nothing at all:
# this script printed "placed", exited 0, and Rack went on running the old
# code. Verified -- an install over an existing unpacked plugin left the
# previous binary live while every signal said it had succeeded, which is the
# worst shape a packaging step can have.
#
# Moved aside rather than deleted. A user who built their own pack from inside
# the app has it in exactly this form, and silently destroying it would be the
# other failure. They get it back under a dated name and are told where.
if [ -d "$DEST_DIR/$SLUG" ]; then
    BACKUP_ROOT="$DEST_HOME/Library/Application Support/Forge Modular/replaced-rack-packs"
    mkdir -p "$BACKUP_ROOT" || {
        echo "install_pack: could not create $BACKUP_ROOT" >&2; exit 1; }
    ASIDE="$BACKUP_ROOT/$SLUG-$(date +%Y%m%d-%H%M%S)"
    if mv "$DEST_DIR/$SLUG" "$ASIDE" 2>/dev/null; then
        echo "install_pack: moved the previously unpacked $SLUG aside to $ASIDE"
        echo "              (Rack loads the unpacked directory, so leaving it"
        echo "               would have kept the old plugin live)"
        [ -n "$OWNER" ] && chown -R "$OWNER" "$ASIDE" 2>/dev/null
    else
        echo "install_pack: could not move aside $DEST_DIR/$SLUG -- Rack will" >&2
        echo "              keep loading it and this install will NOT take" >&2
        exit 1
    fi
fi

# Copy to a temporary name and rename, so a Rack starting up mid-copy never
# sees a half-written archive under a name it will try to unpack.
TMP="$DEST_DIR/.$BASE.part"
rm -f "$TMP"
cp "$PACK" "$TMP" || { echo "install_pack: copy failed" >&2; rm -f "$TMP"; exit 1; }
mv -f "$TMP" "$DEST_DIR/$BASE" || {
    echo "install_pack: could not place $BASE" >&2; rm -f "$TMP"; exit 1; }
rm -f "$DEST_DIR/.ForgeModular-user-pack"

# The app appends its @-mention install log to `runs/` with a shell redirect,
# which fails outright if the directory is absent -- so on a fresh machine the
# very first module fetch did nothing at all while the window said it was
# fetching. Created here because this is the one step that runs before the user
# can reach any of it.
mkdir -p "$DEST_HOME/Library/Application Support/Forge Modular/runs" \
         "$DEST_HOME/Library/Application Support/Forge Modular/patches" 2>/dev/null

# The module index and library catalog are keyed to nothing in particular and
# go stale silently: a cache written a week ago does not contain a plugin
# published since, so a correctly typed module name resolves to nothing and
# reads as the search being broken. An install is the one moment we know the
# software changed, so it is the right moment to drop them.
rm -f "$DEST_HOME/.cache/forge-modular/modules.json" \
      "$DEST_HOME/.cache/forge-modular/library.json"

# Root wrote these. Without this the user cannot update or remove their own
# plugin directory, and Rack cannot unpack the archive it was given.
if [ -n "$OWNER" ]; then
    chown "$OWNER" "$DEST_DIR/$BASE" 2>/dev/null
    chown -R "$OWNER" "$DEST_HOME/Library/Application Support/Rack2" 2>/dev/null
    chown -R "$OWNER" "$DEST_HOME/Library/Application Support/Forge Modular" 2>/dev/null
fi

echo "install_pack: placed $SLUG $VERSION in $DEST_DIR"
exit 0
