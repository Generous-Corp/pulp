#!/usr/bin/env bash
# Prove that each Apple bundle can be code-signed and that the signature then
# verifies strictly, even when it was built with CODE_SIGNING_ALLOWED=NO.
#
#   check_apple_bundle_signing.sh <bundle> [<bundle> ...]
#
# Each bundle is copied (ditto, so extended attributes survive) and the copy is
# ad-hoc signed the way Xcode and `pulp ship sign` sign a product: inside out,
# every nested bundle and loose dylib (deepest first) and then the bundle
# itself, each with `codesign --force --sign -` and never `--deep`. `--deep`
# would hide the failure this check exists for: it signs any data file that
# codesign mistakes for code by storing a signature in that file's extended
# attributes, which a real build signature does not do. The copy is then
# checked with `codesign --verify --deep --strict`. The original is never
# modified.
#
# Unsigned builds hide layout mistakes that only signing rejects. A shallow
# (iOS-family) bundle with a top-level Resources/ directory is the known case:
# codesign then reports "bundle format unrecognized", or calls every other file
# at the root unsigned code, and the extension cannot be installed.
#
# Exit status: 0 when every bundle signs and verifies, 1 when one does not,
# 2 on a usage error.
set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: check_apple_bundle_signing.sh <bundle> [<bundle> ...]" >&2
    exit 2
fi

# Absolute: codesign --verify misreports a relative path ("No such process").
work=$(cd "$(mktemp -d "${TMPDIR:-/tmp}/apple-bundle-signing.XXXXXX")" && pwd -P)
trap 'rm -rf "$work"' EXIT

# True when a nested bundle names a main executable in its Info.plist.
has_executable() {
    local plist
    for plist in "$1/Contents/Info.plist" "$1/Info.plist"; do
        if [[ -f "$plist" ]]; then
            /usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" >/dev/null 2>&1
            return
        fi
    done
    return 1
}

status=0
index=0
for bundle in "$@"; do
    if [[ ! -d "$bundle" ]]; then
        echo "ERROR: $bundle is not a bundle directory" >&2
        exit 2
    fi
    index=$((index + 1))
    copy="$work/$index/$(basename "$bundle")"
    mkdir -p "$work/$index"
    ditto "$bundle" "$copy"
    failed=0
    while IFS= read -r target; do
        if [[ "$target" != "$copy" && -d "$target" && "$target" != *.framework ]] \
            && ! has_executable "$target"; then
            continue  # a resource-only bundle (Settings.bundle) carries no code to sign
        fi
        if ! sign_output=$(codesign --force --sign - "$target" 2>&1); then
            echo "ERROR: $bundle cannot be code-signed (${target#"$work/$index/"}):" >&2
            echo "$sign_output" | sed "s|$work/$index/||g" >&2
            failed=1
            break
        fi
    done < <(
        # Nested bundles and loose dylibs, deepest first, then the bundle.
        find "$copy" -mindepth 1 \( -type d \( -name "*.appex" -o -name "*.app" \
            -o -name "*.framework" -o -name "*.bundle" -o -name "*.xpc" \) \
            -o -type f -name "*.dylib" \) -print |
            awk '{ print gsub("/", "/"), $0 }' | sort -rn | cut -d' ' -f2-
        echo "$copy"
    )
    if [[ $failed -ne 0 ]]; then
        status=1
        continue
    fi
    if ! verify_output=$(codesign --verify --deep --strict "$copy" 2>&1); then
        echo "ERROR: $bundle signed but does not verify strictly:" >&2
        echo "$verify_output" | sed "s|$work/$index/||g" >&2
        status=1
        continue
    fi
    echo "OK: $(basename "$bundle") signs ad hoc and verifies (--deep --strict)"
done
exit "$status"
