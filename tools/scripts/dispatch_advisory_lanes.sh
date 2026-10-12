#!/usr/bin/env bash
# Dispatch Pulp's hosted macOS advisory workflows for an approved PR head.
set -euo pipefail

if [[ $# -ne 1 || -z ${1} ]]; then
  echo "usage: $0 <head-ref-or-sha>" >&2
  exit 2
fi

head="$1"
repo="${PULP_GITHUB_REPO:-Generous-Corp/pulp}"
gh_bin="${GH_BIN:-gh}"

if [[ ! "$head" =~ ^[A-Za-z0-9._/-]+$ ]]; then
  echo "dispatch-advisory-lanes: invalid head ref" >&2
  exit 2
fi

ref="$head"
if [[ "$head" =~ ^[0-9a-fA-F]{40}$ ]]; then
  ref="$($gh_bin api "repos/${repo}/commits/${head}/branches-where-head" --jq '.[0].name')"
  if [[ -z "$ref" ]]; then
    echo "dispatch-advisory-lanes: no branch resolves to approved head ${head}" >&2
    exit 1
  fi
fi

workflows=(
  coverage.yml
  sanitizers.yml
  intel-portability.yml
  scene3d-advisory.yml
  examples-validation.yml
  control-shipping-native.yml
)

for workflow in "${workflows[@]}"; do
  echo "dispatch-advisory-lanes: ${workflow} @ ${head} (ref ${ref})"
  "$gh_bin" workflow run "$workflow" --repo "$repo" --ref "$ref"
done
