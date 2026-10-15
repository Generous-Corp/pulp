#!/usr/bin/env bash
# Package the Unix CLI tarball exactly as a release does.
#
# release-cli.yml, release-dry-run.yml and the release-path PR gate all call
# this script, so a change to how a release packages is a change to what the
# dry-run and the PR gate test. They used to carry hand copies: the dry-run's
# copy fell behind (no materialized JSX siblings) and went red on 2026-10-12
# unnoticed, and the PR gate never packaged at all, so a FetchContent layout
# change that broke Linux packaging passed it and stalled four releases.
# tools/scripts/test_release_package_step.py holds every caller to this script.
#
# Usage: tools/ci/release_package_cli.sh <platform> [<release tag>]
#   <platform>     release matrix platform, e.g. linux-x64, darwin-arm64
#   <release tag>  vX.Y.Z the version floors are judged against; defaults to
#                  the project VERSION in CMakeLists.txt (dry-run, PR gate)
# Environment:
#   PULP_RELEASE_PACKAGER  package_cli.py to run (release-cli points this at
#                          main's copy on a backfill dispatch)
#   PULP_CONTROL_BROKER_FLOOR, PULP_CONTROL_STANDALONE_HOST_FLOOR
#                          default to tools/scripts/release_product_matrix.json
#   PULP_NODE_RUNTIME_EXECUTABLE  bundled with import-design when set
# Writes pulp-<platform>.tar.gz in the current directory (the repo root).
set -euo pipefail

platform="${1:?usage: release_package_cli.sh <platform> [<release tag>]}"
release_tag="${2:-}"
python="${PYTHON:-python3}"

if [ -z "$release_tag" ]; then
  release_tag="v$(sed -n 's/^[[:space:]]*VERSION \([0-9][0-9.]*\)[[:space:]]*$/\1/p' CMakeLists.txt | head -n 1)"
fi
[ "$release_tag" != "v" ] || { echo "::error::no release tag and no project VERSION in CMakeLists.txt" >&2; exit 1; }

matrix_floor() {
  "$python" -c 'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8"))[sys.argv[2]])' \
    tools/scripts/release_product_matrix.json "$1"
}
broker_floor="${PULP_CONTROL_BROKER_FLOOR:-$(matrix_floor control_broker_floor)}"
standalone_floor="${PULP_CONTROL_STANDALONE_HOST_FLOOR:-$(matrix_floor control_standalone_host_floor)}"
PULP_RELEASE_PACKAGER="${PULP_RELEASE_PACKAGER:-tools/scripts/package_cli.py}"

at_least() {
  "$python" -c 'import sys; v=lambda s: tuple(map(int, s.removeprefix("v").split("."))); raise SystemExit(0 if v(sys.argv[1]) >= v(sys.argv[2]) else 1)' "$1" "$2"
}

import_design_args=()
control_broker_args=()
control_standalone_host_args=()
if [ -x build/tools/import-design/pulp-import-design ] \
    && [ -d build/tools/import-design/browser_capture-v1 ]; then
  import_design_args=(
    --import-design-binary build/tools/import-design/pulp-import-design
    --import-design-runtime-dir build/tools/import-design/browser_capture-v1
  )
  if at_least "$release_tag" "0.918.0"; then
    test -f build/tools/import-design/jsx-runtime/materialized_binding_contract.mjs
    test -f build/tools/import-design/jsx-runtime/materialized_source_emitter.mjs
    test -f build/tools/import-design/jsx-runtime/materialized_ids_map.mjs
    import_design_args+=(
      --import-design-contract build/tools/import-design/jsx-runtime/materialized_binding_contract.mjs
      --import-design-source-emitter build/tools/import-design/jsx-runtime/materialized_source_emitter.mjs
      --import-design-ids-map build/tools/import-design/jsx-runtime/materialized_ids_map.mjs
    )
  else
    import_design_args+=(--allow-missing-import-design-contract)
  fi
  if [ -n "${PULP_NODE_RUNTIME_EXECUTABLE:-}" ]; then
    import_design_args+=(
      --node-runtime "$PULP_NODE_RUNTIME_EXECUTABLE"
      --node-license "$(dirname "$PULP_NODE_RUNTIME_EXECUTABLE")/node.LICENSE"
    )
  fi
fi
if [[ "$platform" == darwin-* ]] && at_least "$release_tag" "$broker_floor"; then
  control_broker_args=(--control-broker-binary build/inspect/pulp-control-broker)
fi
if [[ "$platform" == darwin-* ]] && at_least "$release_tag" "$standalone_floor"; then
  control_standalone_host_args=(
    --control-standalone-host-binary build/inspect/pulp-control-standalone-host
    --control-standalone-manifest build/inspect/pulp-control-standalone-host.inspector-capabilities.json
  )
fi

"$python" "$PULP_RELEASE_PACKAGER" \
  --binary build/pulp \
  --cpp-binary build/tools/cli/pulp-cpp \
  --mcp-binary build/tools/mcp/pulp-mcp \
  ${control_broker_args[@]+"${control_broker_args[@]}"} \
  ${control_standalone_host_args[@]+"${control_standalone_host_args[@]}"} \
  ${import_design_args[@]+"${import_design_args[@]}"} \
  --build-dir build \
  --platform "$platform" \
  --out "pulp-${platform}.tar.gz"
test -s "pulp-${platform}.tar.gz" || { echo "::error::package_cli produced no tarball" >&2; exit 1; }
