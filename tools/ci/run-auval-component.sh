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
inventory_log=$(mktemp -t pulp-auval-inventory.XXXXXX)
auval_log=$(mktemp -t pulp-auval.XXXXXX)
inventory_pid=0

stop_inventory() {
  if (( inventory_pid <= 0 )); then
    return
  fi

  kill -TERM "$inventory_pid" 2>/dev/null || true
  local grace_deadline=$((SECONDS + 2))
  while kill -0 "$inventory_pid" 2>/dev/null &&
        (( SECONDS < grace_deadline )); do
    sleep 0.1
  done
  if kill -0 "$inventory_pid" 2>/dev/null; then
    kill -KILL "$inventory_pid" 2>/dev/null || true
  fi
  wait "$inventory_pid" 2>/dev/null || true
  inventory_pid=0
}

cleanup() {
  stop_inventory
  rm -rf -- "$test_component"
  rm -f -- "$inventory_log"
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

scan_inventory() {
  : >"$inventory_log"
  auval -a >"$inventory_log" 2>&1 &
  inventory_pid=$!
  local attempt_deadline=$((SECONDS + 15))
  if (( attempt_deadline > deadline )); then
    attempt_deadline=$deadline
  fi

  while kill -0 "$inventory_pid" 2>/dev/null; do
    if (( SECONDS >= attempt_deadline )); then
      stop_inventory
      return 124
    fi
    sleep 1
  done

  local rc=0
  wait "$inventory_pid" || rc=$?
  inventory_pid=0
  return "$rc"
}

while true; do
  scan_inventory || true
  if awk -v type="$component_type" \
         -v subtype="$component_subtype" \
         -v manufacturer="$component_manufacturer" \
         '$1 == type && $2 == subtype && $3 == manufacturer { found = 1 }
          END { exit(found ? 0 : 1) }' "$inventory_log"; then
    break
  fi

  if (( SECONDS >= deadline )); then
    cat "$inventory_log" >&2
    echo "AU discovery deadline expired for: ${component_type} ${component_subtype} ${component_manufacturer}" >&2
    exit 1
  fi
  sleep 1
done

auval -v "$component_type" "$component_subtype" "$component_manufacturer" \
  >"$auval_log" 2>&1 || true
cat "$auval_log"
if ! grep -q 'AU VALIDATION SUCCEEDED' "$auval_log"; then
  echo "AU validation did not reach its terminal success marker" >&2
  exit 1
fi
