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

ran=$((ran + 1))
new_world 2.0.0 mac-arm64
DEST="$W/home/Library/Application Support/Rack2/plugins-mac-arm64"
mkdir -p "$DEST"
printf 'stale stock pack\n' > "$DEST/ForgeModular-2.0.0-mac-arm64.vcvplugin"
"$PLACER" --source "$W/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
if grep -q 'stub pack' "$DEST/ForgeModular-2.0.0-mac-arm64.vcvplugin"; then
    ok "an unmarked same-version stock archive is refreshed"
else
    fail "a stale unmarked same-version archive was mistaken for user work"
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

# An unpacked stock pack is moved out of Rack's executable search path before
# the replacement archive lands. It remains recoverable as user work and the
# ordinary uninstaller keeps it unless --all is explicitly requested.
ran=$((ran + 1))
new_world 2.0.0 mac-arm64
DEST="$W/home/Library/Application Support/Rack2/plugins-mac-arm64"
BACKUPS="$W/home/Library/Application Support/Forge Modular/replaced-rack-packs"
mkdir -p "$DEST/ForgeModular"
printf '{"slug":"ForgeModular","version":"1.0.0"}\n' > "$DEST/ForgeModular/plugin.json"
"$PLACER" --source "$W/Forge Modular.app" --home "$W/home" >/dev/null 2>&1
if [ ! -d "$DEST/ForgeModular" ] && \
   [ -f "$DEST/ForgeModular-2.0.0-mac-arm64.vcvplugin" ] && \
   find "$BACKUPS" -mindepth 1 -maxdepth 1 -type d -name 'ForgeModular-*' \
        | grep -q .; then
    ok "replaced unpacked packs leave Rack's search path but remain recoverable"
else
    fail "a replaced unpacked pack stayed executable or was destroyed"
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

# HOME is controlled by that user. The package script must drop root before
# traversing it, otherwise a pre-created Rack2 symlink becomes a privileged
# write primitive even when the final archive itself is harmless.
ran=$((ran + 1))
if grep -q '/usr/bin/sudo -H -u "\$CONSOLE_USER"' "$POSTINSTALL" && \
   ! grep -q -- '--owner' "$POSTINSTALL"; then
    ok "postinstall drops to the console user before writing below HOME"
else
    fail "postinstall traverses user-controlled HOME paths while privileged"
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

# Prove discovery rather than merely checking that a glob is written down. The
# generated directory and the not-yet-unpacked archive are both real installed
# shapes, and both live one level below Rack's platform directory.
ran=$((ran + 1))
W="$(mktemp -d)"
RACK_HOME="$W/home/Library/Application Support/Rack2"
mkdir -p "$RACK_HOME/plugins-mac-arm64/ForgeModular" \
         "$RACK_HOME/plugins-mac-x64"
printf 'pack\n' > "$RACK_HOME/plugins-mac-x64/ForgeModular-2.0.0-mac-x64.vcvplugin"
out="$(HOME="$W/home" /bin/bash "$FM/uninstall.sh" --dry-run 2>&1)"
if echo "$out" | grep -Fq "$RACK_HOME/plugins-mac-arm64/ForgeModular" && \
   echo "$out" | grep -Fq "$RACK_HOME/plugins-mac-x64/ForgeModular-2.0.0-mac-x64.vcvplugin"; then
    ok "uninstall discovers unpacked and archived Rack packs in a scratch home"
else
    fail "uninstall did not discover both Rack pack shapes: $out"
fi
/bin/rm -rf "$W"

# Released component packages install globally, not under the user's Library.
# The uninstaller must name all three real payloads and elevate only when it
# actually removes a system path.
ran=$((ran + 1))
if grep -q 'SYSTEM_PLUGINS="/Library/Audio/Plug-Ins"' \
        "$FM/uninstall.sh" && \
   grep -q '\$SYSTEM_PLUGINS/Components/Forge Modular.component' \
        "$FM/uninstall.sh" && \
   grep -q '\$SYSTEM_PLUGINS/VST3/Forge Modular.vst3' \
        "$FM/uninstall.sh" && \
   grep -q '\$SYSTEM_PLUGINS/CLAP/Forge Modular.clap' \
        "$FM/uninstall.sh" && \
   grep -q '/usr/bin/sudo /bin/rm -rf' "$FM/uninstall.sh"; then
    ok "uninstall.sh removes the system-wide AU, VST3 and CLAP payloads"
else
    fail "uninstall.sh does not mirror the released system plug-in payloads"
fi

# A failed deletion is not a successful uninstall. Use a scratch HOME and a
# failing rm so no real installation can be touched.
ran=$((ran + 1))
W="$(mktemp -d)"
mkdir -p "$W/bin" \
    "$W/home/Library/Audio/Plug-Ins/Components/Forge Modular.component"
printf '#!/bin/bash\nexit 1\n' > "$W/bin/rm"
chmod +x "$W/bin/rm"
out="$(PATH="$W/bin:/usr/bin:/bin" HOME="$W/home" \
    /bin/bash "$FM/uninstall.sh" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q 'could not be removed' && \
   ! echo "$out" | grep -q 'Nothing to remove' && \
   [ -d "$W/home/Library/Audio/Plug-Ins/Components/Forge Modular.component" ]; then
    ok "uninstall failure is reported and returns non-zero"
else
    fail "uninstall failure was reported as success: rc=$rc; $out"
fi
/bin/rm -rf "$W"

# Packaging is architecture-coherent: artifact selection and Installer's host
# declaration are derived from the same target architecture.
ran=$((ran + 1))
if grep -q -- '--rack-plugin is required' "$FM/package.sh" && \
   grep -q 'wrong Rack pack identity' "$FM/package.sh" && \
   grep -q -- '--architectures "\$INSTALLER_ARCH"' "$FM/package.sh"; then
    ok "package requires a named current pack and one target architecture"
else
    fail "package can discover stale packs or mix target architectures"
fi

# A plausible archive on the right platform is still stale when its embedded
# manifest is not the manifest generated by this tree.
ran=$((ran + 1))
W="$(mktemp -d)"
mkdir -p "$W/stale/ForgeModular"
printf '%s\n' '{"slug":"ForgeModular","version":"2.0.0","modules":[]}' \
    > "$W/stale/ForgeModular/plugin.json"
/usr/bin/tar --zstd -cf "$W/ForgeModular-2.0.0-mac-arm64.vcvplugin" \
    -C "$W/stale" ForgeModular
out="$("$FM/package.sh" --build-dir "$W/build" --out "$W/out" \
       --architecture arm64 \
       --rack-plugin "$W/ForgeModular-2.0.0-mac-arm64.vcvplugin" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && \
   echo "$out" | grep -q 'embedded plugin.json does not match the current tree'; then
    ok "package rejects a same-platform Rack pack from a different tree"
else
    fail "package accepted a stale same-platform Rack pack: rc=$rc; $out"
fi
/bin/rm -rf "$W"

# Exact basename identity is part of provenance. A renamed archive cannot be
# substituted even when its plugin.json bytes are current.
ran=$((ran + 1))
W="$(mktemp -d)"
mkdir -p "$W/current/ForgeModular"
cp "$FM/plugin.json" "$W/current/ForgeModular/plugin.json"
/usr/bin/tar --zstd -cf "$W/Other-2.0.0-mac-arm64.vcvplugin" \
    -C "$W/current" ForgeModular
out="$("$FM/package.sh" --build-dir "$W/build" --out "$W/out" \
       --architecture arm64 \
       --rack-plugin "$W/Other-2.0.0-mac-arm64.vcvplugin" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q 'wrong Rack pack identity'; then
    ok "package rejects a renamed Rack pack"
else
    fail "package accepted a Rack pack with the wrong build identity: rc=$rc; $out"
fi
/bin/rm -rf "$W"

# Matching metadata does not make an archive a current build. It must be owned
# by this checkout's Release CMake tree so package.sh can rebuild and compare
# the packed binary to the just-built target.
ran=$((ran + 1))
W="$(mktemp -d)"
mkdir -p "$W/rack/ForgeModular"
cp "$FM/plugin.json" "$W/rack/ForgeModular/plugin.json"
printf 'old binary\n' > "$W/rack/ForgeModular/plugin.dylib"
/usr/bin/tar --zstd -cf "$W/rack/ForgeModular-2.0.0-mac-arm64.vcvplugin" \
    -C "$W/rack" ForgeModular
out="$("$FM/package.sh" --build-dir "$W/build" --out "$W/out" \
       --architecture arm64 \
       --rack-plugin "$W/rack/ForgeModular-2.0.0-mac-arm64.vcvplugin" 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && echo "$out" | grep -q 'has no owning CMake build'; then
    ok "package rejects matching metadata without current-build provenance"
else
    fail "package accepted an unattested stale binary: rc=$rc; $out"
fi
/bin/rm -rf "$W"

ran=$((ran + 1))
if grep -q 'byte-identical to the current build' "$FM/verify_package.sh" && \
   grep -q 'PACKS\[@\].*ne 1' "$FM/verify_package.sh" && \
   grep -q 'byte-identical to the rebuilt Forge target' \
        "$FM/verify_package.sh" && \
   grep -q 'ForgeModular_Standalone ForgeModular_AU' "$FM/package.sh" && \
   grep -q 'provenance binds the current source tree, SDK and binary' \
        "$FM/verify_package.sh"; then
    ok "finished-package verification pins one exact current Rack build"
else
    fail "finished-package verification can accept a substituted Rack pack"
fi

# The Forge rebuild is only reproducible when its exact released Pulp SDK and
# locally rebuilt text shaper are bound into the package provenance.
ran=$((ran + 1))
if grep -q 'FORGE_PULP_SDK_SOURCE_SHA is required' "$FM/package.sh" && \
   grep -q 'FORGE_PULP_SDK_CONTENT_SHA256 is required' "$FM/package.sh" && \
   grep -q -- '--content-sha256 "\$expected_sdk_content"' "$FM/package.sh" && \
   grep -q 'sdk_identity.py' "$FM/package.sh" && \
   grep -q 'build_shape_text.sh' "$FM/package.sh" && \
   grep -q 'shape_text_sha256' "$FM/verify_package.sh"; then
    ok "package binds the exact Pulp SDK and rebuilt shape_text helper"
else
    fail "package can accept an unpinned SDK, unverified content, or stale shape_text helper"
fi

# The vocabulary extractor reads the checked capability manifest at runtime.
# Both the signed seed and the writable first-run toolchain must carry it, and
# finished-package verification must reject every payload that omits it.
ran=$((ran + 1))
if grep -q 'docs/status/agent-capabilities.json' "$FM/package.sh" && \
   grep -q 'docs/status/agent-capabilities.json' "$REPO/tools/rack/install_toolchain.sh" && \
   [ "$(grep -c 'Contents/Resources/docs/status/agent-capabilities.json' \
        "$FM/verify_package.sh")" -eq 2 ]; then
    ok "package carries the capability manifest through seed and writable toolchains"
else
    fail "package can strand dsp_vocabulary.py without its capability manifest"
fi

ran=$((ran + 1))
if [ "$(grep -c '^trap ' "$FM/package.sh")" -eq 1 ] && \
   grep -q 'STAGED_ROOT.*STAGE.*CHECK' "$FM/package.sh"; then
    ok "one packaging exit trap cleans every staging directory"
else
    fail "a later packaging trap can strand an earlier staging directory"
fi

# Signing identities may live only in the standard keychain environment file.
# It has to be sourced (and the preflight run) before strict expansion reads
# either hash, or a valid signing machine fails before the helper can repair it.
ran=$((ran + 1))
source_line=$(grep -nF 'source "$HOME/.config/pulp/secrets/keychain.env"' \
    "$FM/package.sh" | head -1 | cut -d: -f1)
preflight_line=$(grep -nF '"$REPO/tools/scripts/ensure_signing_ready.sh"' \
    "$FM/package.sh" | head -1 | cut -d: -f1)
identity_line=$(grep -nF ': "${PULP_SIGN_IDENTITY_HASH:?' \
    "$FM/package.sh" | head -1 | cut -d: -f1)
if [ -n "$source_line" ] && [ -n "$preflight_line" ] && [ -n "$identity_line" ] && \
   [ "$source_line" -lt "$identity_line" ] && \
   [ "$preflight_line" -lt "$identity_line" ]; then
    ok "package loads and preflights signing identities before requiring them"
else
    fail "package requires signing identities before setup can provide them"
fi

# CMake builds stage the same runtime before package.sh runs. Prove this path
# uses the correct tools/rack layout and cannot carry ignored reference books.
ran=$((ran + 1))
W="$(mktemp -d)"
mkdir -p "$W/source/nested/.corpus" "$W/source/nested/.git" \
         "$W/source/nested/__pycache__"
printf 'generator\n' > "$W/source/patch.py"
printf 'book\n' > "$W/source/nested/.corpus/reference.pdf"
printf 'repo\n' > "$W/source/nested/.git/config"
printf 'cache\n' > "$W/source/nested/__pycache__/cache.pyc"
cmake -DSOURCE="$W/source" -DDEST="$W/bundle/Resources/tools/rack" \
      -P "$FM/stage_toolchain.cmake" >/dev/null 2>&1
if [ -f "$W/bundle/Resources/tools/rack/patch.py" ] && \
   ! find "$W/bundle" \( -name .corpus -o -name .git -o -name __pycache__ \) \
        -print -quit | grep -q . && \
   grep -q 'stage_toolchain.cmake' "$FM/CMakeLists.txt" && \
   grep -q 'Resources/tools/rack' "$FM/CMakeLists.txt" && \
   grep -q '"Resources" / "tools" / "rack"' \
        "$FM/app/src/engine_client.cpp"; then
    ok "CMake stages and the app resolves tools/rack without local corpora or caches"
else
    fail "bundle staging and app resolution disagree or leaked local material"
fi
/bin/rm -rf "$W"

ran=$((ran + 1))
if grep -q -- '--license "\$PKG_LICENSE_FILE"' "$FM/package.sh" && \
   grep -q 'the distribution declares a license consent pane' "$FM/verify_package.sh"; then
    ok "package passes consent text to the installer and verifies the pane"
else
    fail "package configures consent without wiring or artifact verification"
fi

echo
echo "$((ran - bad))/$ran correct"
[ "$bad" -eq 0 ] || exit 1
exit 0
