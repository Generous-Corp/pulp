#!/usr/bin/env bash
#
# Validate one freshly built AUv2 component on macOS.
#
# CMake's linker signature does not seal bundle resources, so re-sign an
# isolated copy before asking macOS to discover it. Self-hosted runners connect
# over SSH, but AudioComponentRegistrar belongs to the auto-login user's GUI
# bootstrap. Run both inventory and validation there, matching the namespace
# used by Logic and other AU hosts.

set -euo pipefail

# Shared with tools/ci/test_run_auval_component.py so the launchd-exec predicate
# is tested as the code the script actually runs, not a copy of it.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/auval-exec-check.sh"

if [[ ${1:-} == "--gui-worker" ]]; then
  if [[ $# -ne 7 ]]; then
    echo "invalid AU validation worker invocation" >&2
    exit 2
  fi

  component_type=$2
  component_subtype=$3
  component_manufacturer=$4
  inventory_log=$5
  validation_log=$6
  status_file=$7
  auval_bin=${PULP_AUVAL_BIN:-/usr/bin/auval}
  discovery_timeout=${PULP_AU_DISCOVERY_DEADLINE_SECONDS:-30}
  discovery_poll=${PULP_AU_DISCOVERY_POLL_SECONDS:-1}
  discovery_deadline=$((SECONDS + discovery_timeout))

  status=1
  while true; do
    inventory_status=0
    "$auval_bin" -a >"$inventory_log" 2>&1 || inventory_status=$?
    if (( inventory_status == 0 )) && awk -v type="$component_type" \
        -v subtype="$component_subtype" \
        -v manufacturer="$component_manufacturer" \
        '$1 == type && $2 == subtype && $3 == manufacturer { found = 1 }
         END { exit(found ? 0 : 1) }' "$inventory_log"; then
      status=0
      break
    fi
    if (( SECONDS >= discovery_deadline )); then
      echo "AU inventory did not contain: $component_type $component_subtype $component_manufacturer" \
        >>"$validation_log"
      break
    fi
    sleep "$discovery_poll"
  done

  if (( status == 0 )); then
    "$auval_bin" -v "$component_type" "$component_subtype" \
      "$component_manufacturer" >"$validation_log" 2>&1 || status=$?
    if (( status == 0 )) &&
       ! grep -q 'AU VALIDATION SUCCEEDED' "$validation_log"; then
      echo "AU validation did not reach its terminal success marker" \
        >>"$validation_log"
      status=1
    fi
  fi

  status_tmp="${status_file}.tmp"
  printf '%s\n' "$status" >"$status_tmp"
  mv "$status_tmp" "$status_file"
  exit "$status"
fi

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
scratch_dir=$(mktemp -d -t pulp-auval-gui.XXXXXX)
inventory_log="$scratch_dir/inventory.log"
validation_log="$scratch_dir/validation.log"
status_file="$scratch_dir/status"
stdout_log="$scratch_dir/launchd.stdout"
stderr_log="$scratch_dir/launchd.stderr"
agent_plist="$scratch_dir/agent.plist"
uid=$(id -u)
label_suffix=$(printf '%s-%s-%s' "$component_subtype" "$$" "$RANDOM" |
  tr -cd '[:alnum:]-')
agent_label="com.pulp.auval.${label_suffix}"
agent_loaded=0

cleanup() {
  if (( agent_loaded )); then
    launchctl bootout "gui/$uid/$agent_label" >/dev/null 2>&1 || true
  fi
  rm -rf -- "$test_component"
  rm -rf -- "$scratch_dir"
}
trap cleanup EXIT

mkdir -p "$components_dir"
rm -rf -- "$test_component"
ditto "$source_component" "$test_component"

# Seal the complete copied bundle. A linker-only ad-hoc signature fails strict
# verification once Info.plist and other resources are present.
codesign --force --deep --sign - "$test_component"
codesign --verify --deep --strict "$test_component"

# Audio Component discovery is triggered by the Components directory's
# modification time. Restart only this user's registrar; the root daemon belongs
# to the system-wide component directory and must not be required by this test.
touch "$components_dir"
killall -KILL AudioComponentRegistrar 2>/dev/null || true

script_dir=$(cd "$(dirname "$0")" && pwd)
script_path="$script_dir/$(basename "$0")"
plutil -create xml1 "$agent_plist"
plutil -insert Label -string "$agent_label" "$agent_plist"
plutil -insert ProgramArguments -array "$agent_plist"
plutil -insert ProgramArguments.0 -string /bin/bash "$agent_plist"
plutil -insert ProgramArguments.1 -string "$script_path" "$agent_plist"
plutil -insert ProgramArguments.2 -string --gui-worker "$agent_plist"
plutil -insert ProgramArguments.3 -string "$component_type" "$agent_plist"
plutil -insert ProgramArguments.4 -string "$component_subtype" "$agent_plist"
plutil -insert ProgramArguments.5 -string "$component_manufacturer" "$agent_plist"
plutil -insert ProgramArguments.6 -string "$inventory_log" "$agent_plist"
plutil -insert ProgramArguments.7 -string "$validation_log" "$agent_plist"
plutil -insert ProgramArguments.8 -string "$status_file" "$agent_plist"
plutil -insert EnvironmentVariables -dictionary "$agent_plist"
plutil -insert EnvironmentVariables.PULP_DISABLE_PLUGIN_EDITOR -string 1 "$agent_plist"
plutil -insert EnvironmentVariables.PULP_HEADLESS -string 1 "$agent_plist"
plutil -insert EnvironmentVariables.PULP_TEST_MODE -string 1 "$agent_plist"
plutil -insert RunAtLoad -bool true "$agent_plist"
plutil -insert StandardOutPath -string "$stdout_log" "$agent_plist"
plutil -insert StandardErrorPath -string "$stderr_log" "$agent_plist"

launchctl bootstrap "gui/$uid" "$agent_plist"
agent_loaded=1

# launchd may be unable to execute the worker at all — most commonly because the
# checkout lives somewhere launchd is not permitted to read (a worktree under
# /Volumes needs Full Disk Access for launchd; the directory stats fine from the
# shell, so this is invisible until the exec fails). That is not a validation
# result, and waiting the full deadline for a status file that can never appear
# turns it into a misleading "timed out" 55 seconds later.
#
# Detect it precisely: bash reporting it could not execute THIS script. A plugin
# that genuinely fails validation writes a status file and is reported below, so
# this cannot swallow a real failure.
deadline=$((SECONDS + 55))
while [[ ! -f "$status_file" ]] && (( SECONDS < deadline )); do
  if auval_worker_exec_failed "$stderr_log" "$script_path"; then
    break
  fi
  sleep 0.2
done

cat "$inventory_log" 2>/dev/null || true
cat "$validation_log" 2>/dev/null || true
cat "$stdout_log" 2>/dev/null || true
cat "$stderr_log" 2>/dev/null || true

if [[ ! -f "$status_file" ]] && auval_worker_exec_failed "$stderr_log" "$script_path"; then
  echo "AU validation SKIPPED: launchd could not execute $script_path" >&2
  echo "  The AU host has to be launched from the user's GUI session, and launchd" >&2
  echo "  cannot read this path. A checkout under /Volumes is the usual cause —" >&2
  echo "  grant launchd Full Disk Access, or run this validation from a checkout" >&2
  echo "  under your home directory." >&2
  exit 77
fi

if [[ ! -f "$status_file" ]]; then
  echo "AU GUI-bootstrap validation timed out for: $component_type $component_subtype $component_manufacturer" >&2
  exit 1
fi

status=$(<"$status_file")
if [[ "$status" != "0" ]]; then
  echo "AU GUI-bootstrap validation failed with status $status" >&2
  exit "$status"
fi
