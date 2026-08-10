#!/usr/bin/env bash

# The parent must cover both registrar discovery and the complete validation
# run. A fixed short timeout can kill a healthy worker after inventory succeeds
# on a loaded machine, turning harness scheduling into a false product failure.
auval_worker_timeout_seconds() {
  if [[ $# -ne 2 ]]; then
    return 2
  fi

  local configured=$1
  local discovery_timeout=$2
  if [[ ! $discovery_timeout =~ ^[0-9]+$ ]]; then
    return 2
  fi

  if [[ -n $configured ]]; then
    if [[ ! $configured =~ ^[1-9][0-9]*$ ]]; then
      return 2
    fi
    printf '%s\n' "$configured"
    return 0
  fi

  # Preserve the discovery allowance and then give auval a separate bounded
  # validation window. Default production policy: 30 + 90 = 120 seconds.
  printf '%s\n' "$((discovery_timeout + 90))"
}
