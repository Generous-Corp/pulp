#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/scripts/repeat-until-fail.sh [count] -- <command> [args...]

Examples:
  tools/scripts/repeat-until-fail.sh -- ctest --test-dir build -R "SeqLock concurrent stress test"
  tools/scripts/repeat-until-fail.sh 100 -- ctest --test-dir build -R "OSC sender/receiver loopback"

If count is omitted, the command repeats until it fails. The current iteration is
exposed to the child process as REPEAT_UNTIL_FAIL_ITERATION.
EOF
}

count=""
if [[ $# -gt 0 && "$1" != "--" ]]; then
  count="$1"
  shift
  if ! [[ "$count" =~ ^[0-9]+$ ]] || [[ "$count" -eq 0 ]]; then
    echo "error: count must be a positive integer" >&2
    usage >&2
    exit 2
  fi
fi

if [[ $# -eq 0 || "$1" != "--" ]]; then
  usage >&2
  exit 2
fi
shift

if [[ $# -eq 0 ]]; then
  echo "error: missing command" >&2
  usage >&2
  exit 2
fi

iteration=1
while true; do
  echo "=== Iteration ${iteration} ==="
  if ! REPEAT_UNTIL_FAIL_ITERATION="${iteration}" "$@"; then
    echo "FAILED at iteration ${iteration}" >&2
    exit 1
  fi

  if [[ -n "${count}" && "${iteration}" -ge "${count}" ]]; then
    echo "PASS:${count}"
    exit 0
  fi

  iteration=$((iteration + 1))
done
