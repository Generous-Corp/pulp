#!/usr/bin/env bash
#
# Validate one freshly built AUv2 component on macOS.
#
# CMake's linker signature does not seal bundle resources, so a fresh
# AudioComponentRegistrar can ignore an otherwise valid build. Re-sign the
# copied bundle, reset the registrar once, and retry the exact validation until
# discovery settles or the bounded deadline expires.

set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 COMPONENT TYPE SUBTYPE MANUFACTURER" >&2
  exit 2
fi

source_component=$1
component_type=$2
component_subtype=$3
component_manufacturer=$4

if [[ ! -d "$source_component" ]]; then
  echo "AU component does not exist: $source_component" >&2
  exit 2
fi

component_name=$(basename "$source_component")
if [[ "$component_name" != *.component ]]; then
  echo "Invalid AU component name: $component_name" >&2
  exit 2
fi

components_dir="$HOME/Library/Audio/Plug-Ins/Components"
test_component="$components_dir/${component_name%.component}.auvaltest.component"
auval_log=$(mktemp -t pulp-auval.XXXXXX)

cleanup() {
  rm -rf -- "$test_component"
  rm -f -- "$auval_log"
}
trap cleanup EXIT

mkdir -p "$components_dir"
rm -rf -- "$test_component"
ditto "$source_component" "$test_component"

# Seal the complete copied bundle. A linker-only ad-hoc signature fails strict
# verification once Info.plist and other resources are present.
codesign --force --deep --sign - "$test_component"
codesign --verify --deep --strict "$test_component"

killall -KILL AudioComponentRegistrar 2>/dev/null || true

deadline=$((SECONDS + 30))
while true; do
  auval -v "$component_type" "$component_subtype" "$component_manufacturer" \
    >"$auval_log" 2>&1 || true

  if grep -q 'AU VALIDATION SUCCEEDED' "$auval_log"; then
    cat "$auval_log"
    exit 0
  fi

  if (( SECONDS >= deadline )); then
    cat "$auval_log" >&2
    echo "AU discovery/validation deadline expired for: ${component_type} ${component_subtype} ${component_manufacturer}" >&2
    exit 1
  fi
  sleep 1
done
