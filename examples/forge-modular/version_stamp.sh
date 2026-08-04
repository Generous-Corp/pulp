#!/usr/bin/env bash
# What a release stamps onto the things it ships, and how it proves it stuck.
#
# `package.sh --version` used to name the .pkg and nothing else, so an
# installed 0.12.7 answered CFBundleShortVersionString 0.11.0 and nothing on
# the machine could say which build it was running. That is how a toolchain
# from an older release shadowed every fix a newer one shipped for four days:
# the two builds were indistinguishable from the outside.
#
# Sourced by package.sh, and driven directly by test_version_stamp.sh, so the
# rule is exercised without a twenty-minute build behind it.

# Set the version a bundle reports. Both keys: CFBundleShortVersionString is
# what a person is shown, CFBundleVersion is what the system compares.
stamp_bundle_version() {   # <bundle> <version>
    local bundle="$1" version="$2" plist="$1/Contents/Info.plist"
    [ -f "$plist" ] || { echo "no Info.plist in $bundle" >&2; return 1; }
    local key
    for key in CFBundleShortVersionString CFBundleVersion; do
        /usr/libexec/PlistBuddy -c "Set :$key $version" "$plist" >/dev/null 2>&1 ||
        /usr/libexec/PlistBuddy -c "Add :$key string $version" "$plist" >/dev/null ||
            return 1
    done
    return 0
}

# Read it back out, and refuse the release if it disagrees. Reading rather than
# trusting the write is the whole point: every delivery failure this project
# has had was a build script reporting success over an artifact nobody looked
# inside.
check_bundle_version() {   # <bundle> <version>
    local got
    got=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
          "$1/Contents/Info.plist" 2>/dev/null || echo "")
    if [ "$got" != "$2" ]; then
        echo "VERSION MISMATCH: $1 reports \"$got\", not \"$2\"." >&2
        echo "  Nothing on the installed machine would be able to say which" >&2
        echo "  build it is running, which is how a stale toolchain hid for" >&2
        echo "  four days. Refusing to package." >&2
        return 1
    fi
    return 0
}

# The toolchain's own stamp: which release laid these files down, and when.
# A file rather than an mtime, because every path this takes is a copy and a
# copy rewrites mtimes.
write_toolchain_stamp() {   # <tools dir> <version> [packaged-at]
    local at="${3:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
    printf '%s\n%s\n' "$2" "$at" > "$1/VERSION"
}
