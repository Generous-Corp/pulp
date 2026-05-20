#!/usr/bin/env bash
# Phase 6.6.3 Figma Make runtime-import validation harness.
#
# The parser lane is local-first: parser/dispatch tests, a parser-emitted
# runtime render, and diff coverage must pass before a PR is pushed.
# Screenshot comparison runs when a reference is available.

set -euo pipefail

PULP_DIR="${PULP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# pulp #2087 lesson — refuse to run on a stale checkout. Bypass with
# PULP_FRESHNESS_BYPASS=1 to validate a feature branch's code instead.
if ! ( cd "$PULP_DIR" && "$PULP_DIR/tools/scripts/check_workspace_freshness.sh" ); then
  echo "ERROR: refusing to run validation against stale checkout" >&2
  exit 2
fi

DEFAULT_PLANNING_FIXTURE="$PULP_DIR/planning/fixtures/figma/level-meter-panel.tsx"
DEFAULT_TEST_FIXTURE="$PULP_DIR/test/fixtures/figma/level-meter-panel.tsx"
if [[ -n "${PULP_FIGMA_FIXTURE:-}" ]]; then
  FIXTURE="$PULP_FIGMA_FIXTURE"
elif [[ -f "$DEFAULT_PLANNING_FIXTURE" ]]; then
  FIXTURE="$DEFAULT_PLANNING_FIXTURE"
else
  FIXTURE="$DEFAULT_TEST_FIXTURE"
fi
BUILD_DIR="${PULP_BUILD_DIR:-$PULP_DIR/build-figma-import}"
BUILD_TYPE="${PULP_BUILD_TYPE:-Debug}"
BUILD_JOBS="${PULP_BUILD_JOBS:-1}"
REFERENCE="${PULP_FIGMA_REFERENCE:-$PULP_DIR/planning/screenshots/REFERENCE-figma-level-meter-panel.png}"
OUT="${PULP_FIGMA_OUT:-$PULP_DIR/planning/screenshots/figma-level-meter-panel-latest.png}"
THRESHOLD="${PULP_HARNESS_THRESHOLD:-0.85}"
PARSER_ONLY=0
SKIP_BUILD=0
COVERAGE=0

usage() {
  cat <<'EOF'
Usage:
  tools/import-validation/figma-roundtrip.sh [--parser-only] [--skip-build]
  tools/import-validation/figma-roundtrip.sh --coverage
  tools/import-validation/figma-roundtrip.sh --reference path/to/reference.png

Env:
  PULP_DIR              Pulp checkout path
  PULP_BUILD_DIR        CMake build dir
  PULP_BUILD_TYPE       CMake build type (default: Debug)
  PULP_BUILD_JOBS       CMake build parallelism (default: 1)
  PULP_FIGMA_FIXTURE    Figma TSX fixture path (defaults to planning fixture,
                        falls back to test fixture when planning is unavailable)
  PULP_FIGMA_REFERENCE  reference screenshot path
  PULP_FIGMA_OUT        candidate screenshot path
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

[[ -f "$FIXTURE" ]] || { red "missing Figma fixture: $FIXTURE"; exit 2; }
command -v cmake >/dev/null || { red "cmake is required"; exit 2; }

if [[ $COVERAGE -eq 1 ]]; then
  default_ctest_regex='parse_figma_make_react|WidgetBridge __pulpRuntimeImport__ dispatches Figma|WidgetBridge __pulpRuntimeImport__ surfaces parse failure'
  export PULP_DIFF_COVER_CTEST_REGEX="${PULP_DIFF_COVER_CTEST_REGEX:-$default_ctest_regex}"
  bash "$PULP_DIR/tools/scripts/local_diff_cover.sh" \
    pulp-test-design-import pulp-test-design-import-react-runtime pulp-test-widget-bridge-runtime-import
  green "Figma parser diff coverage passed"
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
  build_targets=(pulp-test-design-import pulp-test-design-import-react-runtime pulp-test-widget-bridge-runtime-import)
  if [[ $PARSER_ONLY -eq 0 ]]; then
    build_targets+=(pulp-screenshot)
  fi
  cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS" \
    --target "${build_targets[@]}"
fi

DESIGN_IMPORT_TEST="$(find_test_exe pulp-test-design-import)"
DESIGN_IMPORT_REACT_RUNTIME_TEST="$(find_test_exe pulp-test-design-import-react-runtime)"
WIDGET_BRIDGE_TEST="$(find_test_exe pulp-test-widget-bridge-runtime-import)"

"$DESIGN_IMPORT_TEST" '[phase-6.6.3]'
"$DESIGN_IMPORT_REACT_RUNTIME_TEST" '[phase-6.6.3]'
"$WIDGET_BRIDGE_TEST" '[phase-6.6.3]'

if [[ $PARSER_ONLY -eq 1 ]]; then
  green "Figma parser tests passed (parser-only)"
  exit 0
fi

find_tool_exe() {
  local rel="$1"
  local candidate
  for candidate in "$BUILD_DIR/$rel" "$BUILD_DIR/$BUILD_TYPE/$rel"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  red "missing built tool: $rel"
  exit 2
}

SCREENSHOT_BIN="$(find_tool_exe tools/screenshot/pulp-screenshot)"
TMP_SCRIPT="$(mktemp "${TMPDIR:-/tmp}/pulp-figma-runtime.XXXXXX.js")"
trap 'rm -f "$TMP_SCRIPT"' EXIT
PULP_FIGMA_RUNTIME_JS_OUT="$TMP_SCRIPT" \
  "$DESIGN_IMPORT_TEST" 'parse_figma_make_react runtime bundle materializes with host React shim'

mkdir -p "$(dirname "$OUT")"
"$SCREENSHOT_BIN" \
  --script "$TMP_SCRIPT" \
  --output "$OUT" \
  --width 520 \
  --height 380 \
  --theme dark
green "captured Figma runtime render: $OUT"

if [[ ! -f "$REFERENCE" ]]; then
  yellow "reference screenshot not found: $REFERENCE"
  yellow "Phase 6.6.3 screenshot diff skipped; use the captured render as the reference once reviewed."
  exit 77
fi

if [[ ! -f "$OUT" ]]; then
  yellow "candidate screenshot not found: $OUT"
  yellow "Run the runtime-import demo/capture step first, then re-run this harness."
  exit 77
fi

python3 "$PULP_DIR/tools/import-validation/diff_against_reference.py" \
  "$REFERENCE" "$OUT" --threshold "$THRESHOLD"
