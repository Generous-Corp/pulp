#!/usr/bin/env bash
# What a release stamps, and what it refuses.
#
#   examples/forge-modular/test_version_stamp.sh
#
# Driven against fabricated bundles rather than a real build, so the rule that
# an installed 0.12.7 must not answer 0.11.0 can be exercised in a second
# instead of behind a twenty-minute link.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/version_stamp.sh"

bad=0
ok()   { printf '  ok     %s\n' "$1"; }
fail() { printf '  WRONG  %s\n' "$1"; bad=$((bad + 1)); }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# A bundle carrying the version its BUILD produced, which is not the version
# being released. That gap is the whole defect: package.sh took --version, used
# it for the .pkg name, and left every bundle saying whatever CMake said.
make_bundle() {   # <name> <version>
    local dir="$WORK/$1"
    mkdir -p "$dir/Contents/MacOS"
    cat > "$dir/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleShortVersionString</key><string>$2</string>
<key>CFBundleVersion</key><string>$2</string>
</dict></plist>
PLIST
    echo "$dir"
}

# 1. An unstamped bundle is REFUSED, rather than shipped saying the wrong thing.
app="$(make_bundle 'Forge Modular.app' 0.11.0)"
if check_bundle_version "$app" 0.12.8 >/dev/null 2>&1; then
    fail "a bundle reporting 0.11.0 passed a 0.12.8 release"
else
    ok "a bundle that disagrees with --version is refused"
fi

# 2. Stamping it makes it agree, read back by PlistBuddy rather than assumed.
if stamp_bundle_version "$app" 0.12.8 && check_bundle_version "$app" 0.12.8; then
    got=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
          "$app/Contents/Info.plist")
    [ "$got" = "0.12.8" ] && ok "the app reports the released version" \
                          || fail "stamped, but reports \"$got\""
else
    fail "stamping the app did not take"
fi

# 3. CFBundleVersion too. A bundle whose display version moved and whose build
#    version did not is two answers to one question.
got=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" \
      "$app/Contents/Info.plist")
[ "$got" = "0.12.8" ] && ok "CFBundleVersion moves with it" \
                      || fail "CFBundleVersion is still \"$got\""

# 4. ALL THREE PLUG-IN FORMATS, not only the app. A DAW showing 0.11.0 beside
#    an app showing 0.12.8 is the same unanswerable question in a worse place.
for fmt in 'Forge Modular.component' 'Forge Modular.vst3' 'Forge Modular.clap'; do
    b="$(make_bundle "$fmt" 0.11.0)"
    stamp_bundle_version "$b" 0.12.8 >/dev/null
    check_bundle_version "$b" 0.12.8 >/dev/null 2>&1 \
        && ok "$fmt reports the released version" \
        || fail "$fmt did not take the stamp"
done

# 5. A bundle with no Info.plist at all fails loudly rather than silently
#    stamping nothing.
mkdir -p "$WORK/husk/Contents"
if stamp_bundle_version "$WORK/husk" 0.12.8 >/dev/null 2>&1; then
    fail "a bundle with no Info.plist was stamped anyway"
else
    ok "a bundle with no Info.plist is refused"
fi

# 6. The toolchain stamp: a version and a packaged-at, in that order, so the
#    app can decide which copy of the generator is newer without an mtime.
tools="$WORK/tools"; mkdir -p "$tools"
write_toolchain_stamp "$tools" 0.12.8 2026-08-04T10:00:00Z
[ "$(sed -n 1p "$tools/VERSION")" = "0.12.8" ] \
    && ok "the toolchain stamp names the release" \
    || fail "the toolchain stamp's first line is not the version"
[ "$(sed -n 2p "$tools/VERSION")" = "2026-08-04T10:00:00Z" ] \
    && ok "the toolchain stamp records when it was packaged" \
    || fail "the toolchain stamp has no packaged-at"

# 7. install_toolchain.sh must not STRIP a release stamp when it syncs from a
#    source checkout, which has none. Without this, a developer's install makes
#    the destination look older than the release and lose to it forever.
grep -q 'if \[ "\$part" = "tools/rack" \] && \[ ! -f "\$SRC/\$part/VERSION" \]' \
     "$HERE/../../tools/rack/install_toolchain.sh" \
    && ok "an install from a checkout leaves an existing stamp alone" \
    || fail "install_toolchain.sh would delete the destination's VERSION"

# 8. The SDK version belongs to the selected release artifact. Pulp's real root
#    project declaration spans multiple lines, so the old one-line sed fallback
#    returned an empty string and made the default packaging path reject every
#    valid SDK.
mkdir -p "$WORK/multiline-root" "$WORK/pulp-sdk"
printf '%s\n' 'project(Pulp' '    VERSION 0.999.0' ')' \
    > "$WORK/multiline-root/CMakeLists.txt"
printf '0.790.1\n' > "$WORK/pulp-sdk/version.txt"
got="$(cd "$WORK/multiline-root" && \
    resolve_pulp_sdk_version "$WORK/pulp-sdk" "")"
[ "$got" = "0.790.1" ] \
    && ok "the default Pulp SDK version comes from the selected SDK" \
    || fail "the multiline root made the selected SDK version resolve as \"$got\""

# An explicit release override remains available, but it does not change where
# the default comes from.
got="$(resolve_pulp_sdk_version "$WORK/pulp-sdk" "0.790.2")"
[ "$got" = "0.790.2" ] \
    && ok "an explicit Pulp SDK version override remains authoritative" \
    || fail "the explicit SDK version override resolved as \"$got\""

echo
if [ "$bad" -eq 0 ]; then echo "version stamping is correct"; else
    echo "$bad problem(s)"; fi
exit $([ "$bad" -eq 0 ] && echo 0 || echo 1)
