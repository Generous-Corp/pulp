#!/usr/bin/env bash
# Does the Rack pack actually reach Rack?
#
#     examples/forge-modular/test/test_install_pack.sh
#
# The 0.12.4 installer carried the .vcvplugin inside the app bundle and had no
# postinstall in any component, so on a machine that had never seen this source
# Rack showed no Forge modules at all. Every development machine hid it: the
# pack was already in the plugins folder from a build, and an installer that
# places nothing looks exactly like one that places the same file again.
#
# So these run against a SCRATCH HOME. Nothing here reads the real one.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FM="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$FM/../.." && pwd)"
PLACER="$FM/install_pack.sh"
POSTINSTALL="$FM/scripts/postinstall"

bad=0
ran=0

fail() { echo "  WRONG  $1"; bad=$((bad + 1)); }
ok()   { echo "  ok     $1"; }

# A scratch world: a fake app bundle carrying a fake pack, and an empty home.
# The pack's CONTENT is irrelevant to placement, so a stub keeps the test
# independent of whether the Rack SDK is installed on this machine.
new_world() {   # $1 = version  $2 = platform key
    W="$(mktemp -d)"
    mkdir -p "$W/home" "$W/Forge Modular.app/Contents/Resources/rack"
    printf 'stub pack\n' \
        > "$W/Forge Modular.app/Contents/Resources/rack/ForgeModular-$1-$2.vcvplugin"
    cp "$PLACER" "$W/Forge Modular.app/Contents/Resources/install_pack.sh"
    chmod +x "$W/Forge Modular.app/Contents/Resources/install_pack.sh"
}

# ── it places the pack at all ────────────────────────────────────────────────
ran=$((ran + 1))
new_world 2.0.0 mac-arm64
"$PLACER" --source "$W/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
DEST="$W/home/Library/Application Support/Rack2/plugins-mac-arm64"
if [ -f "$DEST/ForgeModular-2.0.0-mac-arm64.vcvplugin" ]; then
    ok "places the pack in Rack's plugin directory"
else
    fail "the pack never reached $DEST — this is the 0.12.4 defect"
fi
rm -rf "$W"

# ── same-version generated content is user work, not stock ──────────────────
ran=$((ran + 1))
new_world 2.0.0 mac-arm64
DEST="$W/home/Library/Application Support/Rack2/plugins-mac-arm64"
mkdir -p "$DEST"
printf 'user generated pack\n' > "$DEST/ForgeModular-2.0.0-mac-arm64.vcvplugin"
printf 'ForgeModular-2.0.0-mac-arm64.vcvplugin\n' > "$DEST/.ForgeModular-user-pack"
out="$("$PLACER" --source "$W/Forge Modular.app" --home "$W/home" 2>&1)"
if grep -q 'user generated pack' "$DEST/ForgeModular-2.0.0-mac-arm64.vcvplugin" && \
   echo "$out" | grep -q 'keeping user-generated'; then
    ok "an installer upgrade preserves a same-version generated archive"
else
    fail "a same-version generated archive was replaced by stock: $out"
fi
rm -rf "$W"

ran=$((ran + 1))
new_world 2.0.0 mac-arm64
DEST="$W/home/Library/Application Support/Rack2/plugins-mac-arm64"
mkdir -p "$DEST/ForgeModular"
printf '{"slug":"ForgeModular","version":"2.0.0"}\n' > "$DEST/ForgeModular/plugin.json"
printf 'generated\n' > "$DEST/ForgeModular/.forge-generated-pack"
out="$("$PLACER" --source "$W/Forge Modular.app" --home "$W/home" 2>&1)"
if [ -d "$DEST/ForgeModular" ] && echo "$out" | grep -q 'keeping user-generated'; then
    ok "an installer upgrade preserves an unpacked generated pack"
else
    fail "an unpacked generated pack was displaced: $out"
fi
rm -rf "$W"

# ── the platform directory comes from the pack's name ────────────────────────
# A pack built for Intel dropped into plugins-mac-arm64 is one Rack silently
# will not load, and a hardcoded "arm64" would produce exactly that the day an
# Intel build exists.
ran=$((ran + 1))
new_world 2.0.0 mac-x64
"$PLACER" --source "$W/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
if [ -f "$W/home/Library/Application Support/Rack2/plugins-mac-x64/ForgeModular-2.0.0-mac-x64.vcvplugin" ]; then
    ok "an x64 pack lands in plugins-mac-x64"
else
    fail "an x64 pack did not land in plugins-mac-x64"
fi
rm -rf "$W"

# ── running it twice is running it once ──────────────────────────────────────
# A postinstall runs on every upgrade, and the app's repair path runs on every
# generation, so a second run has to be a no-op rather than a duplicate.
ran=$((ran + 1))
new_world 2.0.0 mac-arm64
"$PLACER" --source "$W/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
"$PLACER" --source "$W/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
DEST="$W/home/Library/Application Support/Rack2/plugins-mac-arm64"
n=$(find "$DEST" -name '*.vcvplugin' | wc -l | tr -d ' ')
if [ "$n" = "1" ]; then
    ok "idempotent: two runs leave one pack"
else
    fail "two runs left $n packs"
fi
rm -rf "$W"

# ── an older pack replaces a newer one nowhere ───────────────────────────────
# Rack unpacks an archive into a directory and deletes it, so a user who built
# a module from inside the app has the UNPACKED shape. Overwriting that with
# the installer's copy would silently undo their work on every reinstall.
ran=$((ran + 1))
new_world 2.0.0 mac-arm64
DEST="$W/home/Library/Application Support/Rack2/plugins-mac-arm64"
mkdir -p "$DEST/ForgeModular"
printf '{"slug":"ForgeModular","version":"2.4.0"}\n' > "$DEST/ForgeModular/plugin.json"
out="$("$PLACER" --source "$W/Forge Modular.app" --home "$W/home" 2>&1)"
if [ -f "$DEST/ForgeModular-2.0.0-mac-arm64.vcvplugin" ]; then
    fail "clobbered an unpacked 2.4.0 install with 2.0.0"
elif echo "$out" | grep -q "keeping ForgeModular 2.4.0"; then
    ok "refuses to replace a newer unpacked install"
else
    fail "did not place 2.0.0 but did not say why: $out"
fi
rm -rf "$W"

# ── an older ARCHIVE is replaced ─────────────────────────────────────────────
# The other half of the rule. "Never overwrite" would leave a beta user on the
# pack they first installed forever.
ran=$((ran + 1))
new_world 2.0.0 mac-arm64
DEST="$W/home/Library/Application Support/Rack2/plugins-mac-arm64"
mkdir -p "$DEST"
printf 'old\n' > "$DEST/ForgeModular-1.0.0-mac-arm64.vcvplugin"
"$PLACER" --source "$W/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
if [ -f "$DEST/ForgeModular-2.0.0-mac-arm64.vcvplugin" ] && \
   [ ! -f "$DEST/ForgeModular-1.0.0-mac-arm64.vcvplugin" ]; then
    ok "replaces an older pack rather than sitting beside it"
else
    fail "1.0.0 and 2.0.0 both present, or 2.0.0 missing"
fi
rm -rf "$W"

# ── a path with spaces survives ──────────────────────────────────────────────
# Every path in this product contains one: "Forge Modular.app" and
# "Application Support". The first version of the placer split on them and
# reported "no such file or directory" for a file that was plainly there.
ran=$((ran + 1))
new_world 2.0.0 mac-arm64
SPACED="$W/a directory with spaces"
mkdir -p "$SPACED"
mv "$W/Forge Modular.app" "$SPACED/"
"$PLACER" --source "$SPACED/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
if [ -f "$W/home/Library/Application Support/Rack2/plugins-mac-arm64/ForgeModular-2.0.0-mac-arm64.vcvplugin" ]; then
    ok "a source path with spaces still resolves"
else
    fail "spaces in the source path broke placement"
fi
rm -rf "$W"

# ── the stale index is dropped on install ────────────────────────────────────
# A cache from last week does not contain a plugin published since, so a
# correctly typed module name resolves to nothing and reads as a broken search.
# An install is the one moment we know to drop it.
ran=$((ran + 1))
new_world 2.0.0 mac-arm64
mkdir -p "$W/home/.cache/forge-modular"
printf '{}' > "$W/home/.cache/forge-modular/modules.json"
printf '{}' > "$W/home/.cache/forge-modular/library.json"
"$PLACER" --source "$W/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
if [ ! -f "$W/home/.cache/forge-modular/modules.json" ] && \
   [ ! -f "$W/home/.cache/forge-modular/library.json" ]; then
    ok "drops the stale module index on install"
else
    fail "the stale index survived the install"
fi
rm -rf "$W"

# ── the postinstall never fails the installation ─────────────────────────────
# A non-zero postinstall aborts the install and takes the app with it, so a
# machine that cannot receive the modules would end up with no software at all.
ran=$((ran + 1))
W="$(mktemp -d)"
# No app on the volume at all: the worst case the script can meet.
out="$("$POSTINSTALL" pkg / "$W" / 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    ok "postinstall survives a missing app bundle"
else
    fail "postinstall exited $rc — that aborts the whole installation"
fi
rm -rf "$W"

# ── the postinstall does not write into root's home ──────────────────────────
# It runs as root, so $HOME is /var/root. Resolving the real user is the entire
# job, and getting it wrong installs the modules for nobody.
ran=$((ran + 1))
if grep -q 'stat -f%Su /dev/console' "$POSTINSTALL" && \
   grep -q 'NFSHomeDirectory' "$POSTINSTALL"; then
    ok "postinstall resolves the console user and their real home"
else
    fail "postinstall does not resolve the console user's home directory"
fi

# ── uninstall.sh removes what install_pack places ────────────────────────────
# The two sides have to name the same paths. install_pack drops an ARCHIVE;
# uninstall.sh used to remove only the unpacked directory, so a user who never
# started Rack could not remove the modules at all.
ran=$((ran + 1))
if grep -q 'ForgeModular-\*\.vcvplugin' "$FM/uninstall.sh" || \
   grep -q 'ForgeModular\*' "$FM/uninstall.sh"; then
    ok "uninstall.sh removes the placed archive, not only the unpacked form"
else
    fail "uninstall.sh removes the unpacked directory but not the .vcvplugin"
fi

echo
echo "$((ran - bad))/$ran correct"
[ "$bad" -eq 0 ] || exit 1
exit 0
