#!/usr/bin/env bash
# pulp jsx-instrument-import experiment (2026-05-17).
#
# End-to-end JSX import smoke harness:
#   1. Audit source-contract extraction from JSX/CSS.
#   2. Run the Node + esbuild transform on a JSX fixture.
#   3. Build the JSX runtime test target.
#   4. Run the C++ smoke test (asserts >9 IR nodes + Chainer text).
#   5. Optionally render through pulp-screenshot for a visual artifact.
#
# Designed to fail loudly with a clear "Node not installed" or "fixture
# missing" diagnostic so it's clear which gate failed.

set -euo pipefail

PULP_DIR="${PULP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
FIXTURE="${PULP_JSX_FIXTURE:-$PULP_DIR/planning/fixtures/jsx/chainer-instrument.jsx}"
BUILD_DIR="${PULP_BUILD_DIR:-$PULP_DIR/build}"
BUNDLE_OUT="${PULP_JSX_BUNDLE_OUT:-/tmp/pulp-jsx-bundle.js}"
CONTRACT_AUDIT_OUT="${PULP_JSX_CONTRACT_AUDIT_OUT:-/tmp/pulp-jsx-contract-audit.json}"
SCREENSHOT_OUT="${PULP_JSX_SCREENSHOT_OUT:-/tmp/pulp-jsx-render.png}"
WIDTH="${PULP_JSX_WIDTH:-1280}"
HEIGHT="${PULP_JSX_HEIGHT:-800}"
SCALE="${PULP_JSX_SCALE:-2}"
PARSER_ONLY=0
SKIP_BUILD=0
SKIP_CONTRACT_AUDIT=0
SKIP_SCREENSHOT=0

usage() {
    cat <<'EOF'
Usage:
  tools/import-validation/jsx-roundtrip.sh [options]

Options:
  --parser-only      Only run the C++ smoke test (skip screenshot)
  --skip-build       Skip the cmake --build step (assumes binaries fresh)
  --skip-contract-audit Skip the JSX/CSS source-contract audit
  --skip-screenshot  Run transform + smoke but not pulp-screenshot
  --help             Show this help

Env:
  PULP_DIR              Pulp checkout path
  PULP_BUILD_DIR        CMake build dir (default: $PULP_DIR/build)
  PULP_JSX_FIXTURE      JSX fixture path (default: chainer-instrument.jsx)
  PULP_JSX_BUNDLE_OUT   Bundle output path (default: /tmp/pulp-jsx-bundle.js)
  PULP_JSX_CONTRACT_AUDIT_OUT Contract audit JSON path (default: /tmp/pulp-jsx-contract-audit.json)
  PULP_JSX_SCREENSHOT_OUT Screenshot output (default: /tmp/pulp-jsx-render.png)
  PULP_JSX_WIDTH        Screenshot width (default: 1280)
  PULP_JSX_HEIGHT       Screenshot height (default: 800)
  PULP_JSX_SCALE        Screenshot scale factor (default: 2)
EOF
}

for arg in "$@"; do
    case "$arg" in
        --parser-only)     PARSER_ONLY=1; SKIP_SCREENSHOT=1 ;;
        --skip-build)      SKIP_BUILD=1 ;;
        --skip-contract-audit) SKIP_CONTRACT_AUDIT=1 ;;
        --skip-screenshot) SKIP_SCREENSHOT=1 ;;
        --help|-h)         usage; exit 0 ;;
        *) echo "Unknown arg: $arg" >&2; usage; exit 2 ;;
    esac
done

log() { printf '\033[1;36m[jsx-roundtrip]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[jsx-roundtrip] FAIL:\033[0m %s\n' "$*" >&2; exit 1; }

# ─── 1. Preflight ────────────────────────────────────────────────────────
[ -f "$FIXTURE" ] || fail "fixture not found: $FIXTURE"
command -v node >/dev/null 2>&1 || fail "node is required (install Node.js >= 18)"
NODE_VER=$(node --version)
log "node: $NODE_VER"
log "fixture: $FIXTURE"

TRANSFORM="$PULP_DIR/tools/import-design/jsx-runtime/jsx-transform.mjs"
[ -f "$TRANSFORM" ] || fail "transform script missing: $TRANSFORM"
CONTRACT_AUDIT="$PULP_DIR/tools/import-design/jsx-runtime/jsx-contract-audit.mjs"
[ -f "$CONTRACT_AUDIT" ] || fail "contract audit script missing: $CONTRACT_AUDIT"

if [ ! -d "$PULP_DIR/tools/import-design/jsx-runtime/node_modules" ]; then
    log "installing jsx-runtime npm deps (first run only)..."
    (cd "$PULP_DIR/tools/import-design/jsx-runtime" && npm install) || \
        fail "npm install failed in tools/import-design/jsx-runtime"
fi

# ─── 2. Audit JSX/CSS source contract ───────────────────────────────────
if [ "$SKIP_CONTRACT_AUDIT" -eq 0 ]; then
    log "auditing source contract $FIXTURE → $CONTRACT_AUDIT_OUT"
    node "$CONTRACT_AUDIT" --in "$FIXTURE" --json "$CONTRACT_AUDIT_OUT" --fail-on-weak-proof \
        2>&1 | sed 's/^/    /'
    [ -f "$CONTRACT_AUDIT_OUT" ] || fail "contract audit produced no output"
    log "contract audit: $CONTRACT_AUDIT_OUT"
fi

# ─── 3. Transform JSX → bundle ──────────────────────────────────────────
log "transforming $FIXTURE → $BUNDLE_OUT"
node "$TRANSFORM" --in "$FIXTURE" --out "$BUNDLE_OUT" --verbose 2>&1 | sed 's/^/    /'
[ -f "$BUNDLE_OUT" ] || fail "transform produced no output"
SIZE=$(wc -c < "$BUNDLE_OUT")
log "bundle: $SIZE bytes"

# ─── 4. Build the smoke test ────────────────────────────────────────────
if [ "$SKIP_BUILD" -eq 0 ]; then
    log "building pulp-test-design-import-jsx-runtime"
    cmake --build "$BUILD_DIR" --target pulp-test-design-import-jsx-runtime -j8 \
        2>&1 | tail -5 | sed 's/^/    /'
fi

TEST_BIN="$BUILD_DIR/test/pulp-test-design-import-jsx-runtime"
[ -x "$TEST_BIN" ] || fail "test binary missing: $TEST_BIN (run cmake first)"

# ─── 5. Run the smoke test ──────────────────────────────────────────────
log "running smoke test"
PULP_JSX_BUNDLE="$BUNDLE_OUT" "$TEST_BIN" "[jsx]" --reporter compact 2>&1 | sed 's/^/    /'

# ─── 6. Optionally render a screenshot ──────────────────────────────────
if [ "$SKIP_SCREENSHOT" -eq 0 ]; then
    SHOT_BIN="$BUILD_DIR/tools/screenshot/pulp-screenshot"
    if [ ! -x "$SHOT_BIN" ]; then
        log "WARN: pulp-screenshot missing at $SHOT_BIN — skipping render"
    else
        log "rendering $BUNDLE_OUT → $SCREENSHOT_OUT (${WIDTH}x${HEIGHT}@${SCALE}x)"
        "$SHOT_BIN" --script "$BUNDLE_OUT" --output "$SCREENSHOT_OUT" \
            --width "$WIDTH" --height "$HEIGHT" --scale "$SCALE" --backend skia \
            2>&1 | sed 's/^/    /'
        [ -f "$SCREENSHOT_OUT" ] || fail "screenshot was not produced"
        PNG_SIZE=$(wc -c < "$SCREENSHOT_OUT")
        log "screenshot: ${PNG_SIZE} bytes at $SCREENSHOT_OUT"
    fi
fi

log "OK — jsx-roundtrip smoke complete"
