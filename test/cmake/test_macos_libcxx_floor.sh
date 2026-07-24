#!/usr/bin/env bash
set -euo pipefail

repo_root="${1:?repo root is required}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    exit 77
fi

compiler="$(xcrun --find clang++)"
sdk_spec="${PULP_MACOS_FLOOR_SDK:-${2:-}}"
if [[ -z "${sdk_spec}" ]]; then
    sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
elif [[ -d "${sdk_spec}" ]]; then
    sdk_path="${sdk_spec}"
else
    sdk_path="$(xcrun --sdk "${sdk_spec}" --show-sdk-path)"
fi

scratch="$(mktemp -d "${TMPDIR:-/tmp}/pulp-macos-floor.XXXXXX")"
trap 'rm -rf "${scratch}"' EXIT

probe="${scratch}/libcxx_floor.cpp"
broken="${scratch}/broken.cpp"

printf '%s\n' \
    '#include <format>' \
    '#include <string>' \
    'int main() { auto value = std::format("{}", 1); return value.empty(); }' \
    > "${probe}"

while IFS=$'\t' read -r arch floor; do
    object="${scratch}/libcxx_floor_${arch}.o"
    "${compiler}" \
        -std=c++20 \
        -arch "${arch}" \
        -mmacosx-version-min="${floor}" \
        -isysroot "${sdk_path}" \
        -c "${probe}" \
        -o "${object}"
    echo "macOS libc++ floor ${floor} compiles std::format for ${arch} with SDK ${sdk_path}"
done < <(
    /usr/bin/python3 - "${repo_root}/tools/deps/min_os.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    platforms = json.load(handle)["platforms"]

print("arm64", platforms["macos-arm64"]["floor"], sep="\t")
print("x86_64", platforms["macos-x64"]["floor"], sep="\t")
PY
)

macos_15_4_sdk=""
for candidate in \
    "${sdk_path}" \
    "/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk" \
    "$(xcode-select -p)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.4.sdk"; do
    if [[ -d "${candidate}" && "$(basename "${candidate}")" == "MacOSX15.4.sdk" ]]; then
        macos_15_4_sdk="${candidate}"
        break
    fi
done

if [[ -n "${macos_15_4_sdk}" ]]; then
    for arch in arm64 x86_64; do
        error_log="${scratch}/known_bad_${arch}.log"
        if "${compiler}" \
            -std=c++20 \
            -arch "${arch}" \
            -mmacosx-version-min=13.3 \
            -isysroot "${macos_15_4_sdk}" \
            -fno-color-diagnostics \
            -fsyntax-only "${probe}" 2>"${error_log}"; then
            echo "macOS 15.4 SDK unexpectedly compiled std::format for ${arch} at known-bad floor 13.3" >&2
            exit 1
        fi
        if ! grep -q "'to_chars' is unavailable: introduced in macOS 13.4" "${error_log}"; then
            echo "macOS 15.4 SDK probe for ${arch} failed for an unexpected reason:" >&2
            sed -n '1,80p' "${error_log}" >&2
            exit 1
        fi
        echo "macOS 15.4 SDK rejects known-bad std::format floor 13.3 for ${arch}"
    done
else
    echo "macOS 15.4 SDK not installed; skipping the 13.3 lower-bound rejection probe"
fi

# Negative control: prove this compiler invocation reports a malformed source
# instead of treating every probe as successful.
printf '%s\n' 'int main( {' > "${broken}"
if "${compiler}" \
    -std=c++20 \
    -isysroot "${sdk_path}" \
    -fsyntax-only "${broken}" >/dev/null 2>&1; then
    echo "macOS libc++ floor probe is not detecting compiler failures" >&2
    exit 1
fi
