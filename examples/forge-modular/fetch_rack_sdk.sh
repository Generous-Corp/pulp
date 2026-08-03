#!/usr/bin/env bash
# Fetch the VCV Rack SDK so Forge Modular can build modules on this Mac.
#
#   fetch_rack_sdk.sh            # fetch if missing
#   fetch_rack_sdk.sh --check    # report what is present, change nothing
#   fetch_rack_sdk.sh --force    # re-fetch even if present
#
# WHY THIS DOWNLOADS RATHER THAN SHIPS
#
# The Rack SDK is VCV's, and it is GPLv3. Pulp is MIT and does not put GPL code
# in a shipped artifact, so the installer carries none. Your machine fetches it
# from VCV directly, which is not redistribution: we never host or hand on a
# copy. That is the same arrangement the repo already uses for the VST3 and
# AudioUnit SDKs -- developer-supplied, never committed.
#
# A module you build against it links GPLv3. That is fine for your own use; if
# you ever DISTRIBUTE a generated module, its licence follows from that.
#
# PATCH generation does not need any of this. It needs Python and the model
# CLI, nothing else. Only MODULE generation compiles C++.

set -uo pipefail

# Pinned, not "latest": a toolchain that changes under you turns a working
# install into an intermittent one. Bump deliberately, and record it in
# DEPENDENCIES.md / tools/deps/manifest.json like every other pin.
#
# CONFIRM BEFORE SHIPPING: the exact URL and filename VCV publish, and that
# their terms permit an automated fetch. The SDK is published for plugin
# development so this is very likely fine -- but confirm it, do not assume it.
SDK_VERSION="${PULP_RACK_SDK_VERSION:-2.6.6}"
SDK_ARCH="mac-arm64"
SDK_URL="${PULP_RACK_SDK_URL:-https://vcvrack.com/downloads/Rack-SDK-${SDK_VERSION}-${SDK_ARCH}.zip}"

DEST="$HOME/Library/Application Support/Forge Modular/sdk"
SDK_DIR="$DEST/Rack-SDK"
MODE=fetch
case "${1:-}" in
    --check) MODE=check ;;
    --force) MODE=force ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    "") ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
esac

have_sdk() { [ -f "$SDK_DIR/include/rack.hpp" ]; }

# The compiler is Apple's and we do not redistribute it either. Name it and the
# one command that fixes it -- a missing toolchain must never surface as a
# blank failure, which is exactly the defect this product already had once.
report_compiler() {
    if xcode-select -p >/dev/null 2>&1; then
        echo "  compiler: Xcode Command Line Tools present"
    else
        echo "  compiler: NOT installed."
        echo "            Run:  xcode-select --install"
        echo "            Module building needs it; patch generation does not."
    fi
}

if [ "$MODE" = check ]; then
    if have_sdk; then echo "  Rack SDK: $SDK_DIR"; else echo "  Rack SDK: not installed"; fi
    report_compiler
    exit 0
fi

if have_sdk && [ "$MODE" != force ]; then
    echo "Rack SDK already present at $SDK_DIR"
    report_compiler
    exit 0
fi

echo "Fetching the VCV Rack SDK ($SDK_VERSION, $SDK_ARCH) from vcvrack.com."
echo "It is VCV's and GPLv3; it is downloaded from them, not shipped by us."

mkdir -p "$DEST" || { echo "cannot create $DEST" >&2; exit 1; }
TMP="$(mktemp -d)" || exit 1
trap 'rm -rf "$TMP"' EXIT

# --fail so an HTML error page is not unpacked as if it were a zip: without it
# a 404 lands as a "successful" download of something that is not an SDK, and
# the failure surfaces later as a confusing build error.
if ! curl --fail --location --silent --show-error \
        --connect-timeout 20 --max-time 600 \
        -o "$TMP/sdk.zip" "$SDK_URL"; then
    echo "Could not download the SDK from $SDK_URL" >&2
    echo "Check your connection, or set PULP_RACK_SDK_URL and try again." >&2
    exit 1
fi

# Prove it is a zip before trusting it.
if ! unzip -tq "$TMP/sdk.zip" >/dev/null 2>&1; then
    echo "The download is not a valid zip archive. Refusing to unpack it." >&2
    exit 1
fi

rm -rf "$SDK_DIR"
if ! unzip -q "$TMP/sdk.zip" -d "$DEST"; then
    echo "Could not unpack the SDK." >&2
    exit 1
fi
# The archive's top-level directory name has varied across releases; settle it
# here so everything downstream can rely on one path.
if [ ! -d "$SDK_DIR" ]; then
    top="$(find "$DEST" -maxdepth 1 -type d -name 'Rack-SDK*' | head -1)"
    [ -n "$top" ] && [ "$top" != "$SDK_DIR" ] && mv "$top" "$SDK_DIR"
fi

if have_sdk; then
    echo "  Rack SDK installed at $SDK_DIR"
    report_compiler
    exit 0
fi
echo "Unpacked, but $SDK_DIR/include/rack.hpp is missing -- the archive layout" >&2
echo "is not what was expected. Leaving it in place for inspection." >&2
exit 1
