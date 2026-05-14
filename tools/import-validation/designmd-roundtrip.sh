#!/usr/bin/env bash
# DESIGN.md (Google design.md, Apache-2.0) import validation harness.
#
# DESIGN.md describes a *design system*, not a screen — so this harness
# is intentionally simpler than the screen-export harnesses. It runs the
# parser/lint/diff/Tailwind tests and, with --coverage, drives the same
# focused diff-coverage check the other parsers use.

set -euo pipefail

PULP_DIR="${PULP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
BUILD_DIR="${PULP_BUILD_DIR:-$PULP_DIR/build-designmd-import}"
BUILD_TYPE="${PULP_BUILD_TYPE:-Debug}"
BUILD_JOBS="${PULP_BUILD_JOBS:-1}"
PARSER_ONLY=0
SKIP_BUILD=0
COVERAGE=0

usage() {
  cat <<'EOF'
Usage:
  tools/import-validation/designmd-roundtrip.sh [--parser-only] [--skip-build]
  tools/import-validation/designmd-roundtrip.sh --coverage

Env:
  PULP_DIR            Pulp checkout path
  PULP_BUILD_DIR      CMake build dir
  PULP_BUILD_TYPE     CMake build type (default: Debug)
  PULP_BUILD_JOBS     CMake build parallelism (default: 1)
  PULP_DIFF_COVER_CTEST_REGEX  override the focused coverage CTest regex
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --parser-only) PARSER_ONLY=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --coverage) COVERAGE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

red() { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }

command -v cmake >/dev/null || { red "cmake is required"; exit 2; }

if [[ $COVERAGE -eq 1 ]]; then
  default_ctest_regex='designmd|parse_designmd|lint_designmd|diff_designmd|export_tailwind'
  export PULP_DIFF_COVER_CTEST_REGEX="${PULP_DIFF_COVER_CTEST_REGEX:-$default_ctest_regex}"
  bash "$PULP_DIR/tools/scripts/local_diff_cover.sh" \
    pulp-test-design-import-designmd
  green "DESIGN.md parser diff coverage passed"
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
    --target pulp-test-design-import-designmd
fi

TEST_BIN="$(find_test_exe pulp-test-design-import-designmd)"
"$TEST_BIN" '[designmd]'
green "DESIGN.md parser tests passed"

# DESIGN.md has no screen output, so there is no screenshot-diff phase.
# Phase 3 (post #1307) will add a round-trip emitter test that compares
# the output of `export_designmd(parse_designmd(file).ir.tokens)` to a
# canonical golden DESIGN.md — wire that here when it lands.

if [[ $PARSER_ONLY -eq 1 ]]; then
  exit 0
fi
exit 0
