#!/usr/bin/env bash
# Phase 6.6.2 v0.dev runtime-import validation harness.
#
# This is staged before the parser lands so the C-1 PR has a repeatable
# command once #1859 and #1863 are merged. It intentionally does not
# capture a golden screenshot by itself; provide one with --reference.

set -euo pipefail

PULP_DIR="${PULP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
DEFAULT_PLANNING_FIXTURE="$PULP_DIR/planning/fixtures/v0-dev/audio-control-panel.tsx"
DEFAULT_TEST_FIXTURE="$PULP_DIR/test/fixtures/v0-dev/audio-control-panel.tsx"
if [[ -n "${PULP_V0_FIXTURE:-}" ]]; then
  FIXTURE="$PULP_V0_FIXTURE"
elif [[ -f "$DEFAULT_PLANNING_FIXTURE" ]]; then
  FIXTURE="$DEFAULT_PLANNING_FIXTURE"
else
  FIXTURE="$DEFAULT_TEST_FIXTURE"
fi
BUILD_DIR="${PULP_BUILD_DIR:-$PULP_DIR/build-v0-import}"
BUILD_TYPE="${PULP_BUILD_TYPE:-Debug}"
BUILD_JOBS="${PULP_BUILD_JOBS:-1}"
REFERENCE="${PULP_V0_REFERENCE:-$PULP_DIR/planning/screenshots/REFERENCE-v0-dev-audio-control-panel.png}"
OUT="${PULP_V0_OUT:-$PULP_DIR/planning/screenshots/v0-dev-audio-control-panel-latest.png}"
THRESHOLD="${PULP_HARNESS_THRESHOLD:-0.85}"
PARSER_ONLY=0
SKIP_BUILD=0
COVERAGE=0

usage() {
  cat <<'EOF'
Usage:
  tools/import-validation/v0-roundtrip.sh [--parser-only] [--skip-build]
  tools/import-validation/v0-roundtrip.sh --coverage
  tools/import-validation/v0-roundtrip.sh --reference path/to/reference.png

Env:
  PULP_DIR              Pulp checkout path
  PULP_BUILD_DIR        CMake build dir
  PULP_BUILD_TYPE       CMake build type (default: Debug)
  PULP_BUILD_JOBS       CMake build parallelism (default: 1)
  PULP_V0_FIXTURE       v0 TSX fixture path (defaults to planning fixture,
                        falls back to test fixture when planning is unavailable)
  PULP_V0_REFERENCE     reference screenshot path
  PULP_V0_OUT           candidate screenshot path
  PULP_HARNESS_THRESHOLD screenshot similarity threshold
  PULP_DIFF_COVER_CTEST_REGEX override the focused coverage CTest regex
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --parser-only) PARSER_ONLY=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --coverage) COVERAGE=1; shift ;;
    --reference) REFERENCE="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

red() { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

[[ -f "$FIXTURE" ]] || { red "missing v0 fixture: $FIXTURE"; exit 2; }
command -v cmake >/dev/null || { red "cmake is required"; exit 2; }

if [[ $COVERAGE -eq 1 ]]; then
  default_ctest_regex='parse_v0_dev_react|WidgetBridge __pulpRuntimeImport__ dispatches v0|WidgetBridge __pulpRuntimeImport__ surfaces parse failure'
  export PULP_DIFF_COVER_CTEST_REGEX="${PULP_DIFF_COVER_CTEST_REGEX:-$default_ctest_regex}"
  bash "$PULP_DIR/tools/scripts/local_diff_cover.sh" \
    pulp-test-design-import pulp-test-widget-bridge
  green "v0 parser diff coverage passed"
  exit 0
fi

find_test_exe() {
  local name="$1"
  local candidate
  for candidate in "$BUILD_DIR/test/$name" "$BUILD_DIR/test/$BUILD_TYPE/$name"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  red "missing built test executable: $name"
  exit 2
}

if [[ $SKIP_BUILD -eq 0 ]]; then
  cmake -S "$PULP_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS" \
    --target pulp-test-design-import pulp-test-widget-bridge
fi

DESIGN_IMPORT_TEST="$(find_test_exe pulp-test-design-import)"
WIDGET_BRIDGE_TEST="$(find_test_exe pulp-test-widget-bridge)"

"$DESIGN_IMPORT_TEST" '[phase-6.6.2]'
"$WIDGET_BRIDGE_TEST" '[phase-6.6.2]'

if [[ $PARSER_ONLY -eq 1 ]]; then
  green "v0 parser tests passed (parser-only)"
  exit 0
fi

if [[ ! -f "$REFERENCE" ]]; then
  yellow "reference screenshot not found: $REFERENCE"
  yellow "Phase 6.6.2 screenshot diff is skipped until a v0 runtime render is captured."
  exit 77
fi

if [[ ! -f "$OUT" ]]; then
  yellow "candidate screenshot not found: $OUT"
  yellow "Run the runtime-import demo/capture step first, then re-run this harness."
  exit 77
fi

python3 "$PULP_DIR/tools/import-validation/diff_against_reference.py" \
  "$REFERENCE" "$OUT" --threshold "$THRESHOLD"
