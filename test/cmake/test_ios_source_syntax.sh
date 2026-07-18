#!/usr/bin/env bash
set -euo pipefail

root=${1:?usage: test_ios_source_syntax.sh <source-root> [build-dir]}
build_dir=${2:-$root/build}

if [[ $(uname -s) != Darwin ]] || ! command -v xcrun >/dev/null 2>&1; then
    echo "SKIP: iOS source syntax gate requires macOS with Xcode"
    exit 77
fi
if ! sdk=$(xcrun --sdk iphonesimulator --show-sdk-path 2>/dev/null); then
    echo "SKIP: iPhone Simulator SDK is not installed"
    exit 77
fi

includes=()
while IFS= read -r include_dir; do
    includes+=("-I$include_dir")
done < <(find "$root/core" -mindepth 2 -maxdepth 2 -type d -name include | sort)
includes+=("-I$root/core/format/src")

# window_host_ios.mm reaches CHOC through the event-loop headers. CMake's
# configured FetchContent tree is the canonical dependency location in CI.
choc_root=
if [[ -d "$build_dir/_deps/choc-src/choc" ]]; then
    choc_root="$build_dir/_deps/choc-src"
else
    choc_dir=$(find "$build_dir/_deps" -type d -path '*/choc/containers' -print -quit 2>/dev/null || true)
    if [[ -n "$choc_dir" ]]; then
        choc_root=${choc_dir%/choc/containers}
    fi
fi
if [[ -z "$choc_root" ]]; then
    echo "ERROR: configured CHOC source not found under $build_dir/_deps" >&2
    exit 1
fi
includes+=("-I$choc_root")

sources=()
# Discover conventionally named iOS translation units so adding a new backend
# does not require a second hand-maintained manifest in this test.
while IFS= read -r source; do
    sources+=("${source#"$root"/}")
done < <(find "$root/core" -type f \
    \( -name '*ios*.cpp' -o -name '*ios*.mm' \) | sort)

# These shared sources contain iOS-only branches or are selected exclusively by
# iOS CMake despite having platform-neutral names.
sources+=(
    core/platform/platform/posix/child_process_posix.cpp
    core/platform/src/file_dialog_stub.cpp
    core/platform/src/popup_menu_stub.cpp
    core/render/src/render_loop_apple.mm
    core/view/src/threejs_resources_apple.mm
)

for source in "${sources[@]}"; do
    echo "iOS syntax: $source"
    xcrun --sdk iphonesimulator clang++ \
        -std=c++23 \
        -fblocks \
        -fsyntax-only \
        -target arm64-apple-ios16.4-simulator \
        -isysroot "$sdk" \
        -DPULP_IOS=1 \
        -DPULP_PERMISSIONS_HAS_BACKEND=1 \
        "${includes[@]}" \
        "$root/$source"
done

echo "OK: iOS-conditional source syntax sweep passed"
