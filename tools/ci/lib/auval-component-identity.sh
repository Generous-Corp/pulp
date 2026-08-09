#!/usr/bin/env bash

# Produce a fresh bundle identity for an isolated AU validation copy. The
# AudioComponentRegistrar caches metadata by CFBundleIdentifier even after a
# temporary component is removed, so reusing the product identity can validate
# stale metadata instead of the bundle supplied to the harness.
auval_isolated_bundle_id() {
  if [[ $# -ne 2 || -z $1 || -z $2 ]]; then
    return 2
  fi

  local source_bundle_id=$1
  local run_token=$2
  if [[ ! $source_bundle_id =~ ^[A-Za-z0-9.-]+$ ||
        ! $run_token =~ ^[A-Za-z0-9-]+$ ]]; then
    return 2
  fi

  printf '%s.pulp-auvaltest.%s\n' "$source_bundle_id" "$run_token"
}
