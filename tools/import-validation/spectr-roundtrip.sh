#!/usr/bin/env bash
# spectr-roundtrip.sh — re-import editor.html via pulp, rebuild Spectr native
# bridge, launch, capture, diff against REFERENCE. The full A→D loop in one
# command, so we can re-run after every Pulp importer fix and instantly see
# whether the gap narrowed.
#
# Usage:
#   tools/import-validation/spectr-roundtrip.sh
#   tools/import-validation/spectr-roundtrip.sh --skip-import   # use existing bundle
#   tools/import-validation/spectr-roundtrip.sh --skip-build    # use existing build
#   tools/import-validation/spectr-roundtrip.sh --skip-capture  # use existing screenshot
#
# Env:
#   PULP_HARNESS_THRESHOLD  similarity threshold for PASS (default 0.85)
#   PULP_DIR               override pulp checkout path (default /Users/danielraffel/Code/pulp)
#   SPECTR_DIR             override spectr checkout path (default /Users/danielraffel/Code/spectr)
#
# Exit codes:
#   0  PASS  — Spectr native render matches reference within tolerance
#   1  FAIL  — render diverges (the gap; what we're working to close)
#   2  ERROR — pipeline broke (build fail, crash, missing tool)

set -euo pipefail

PULP="${PULP_DIR:-/Users/danielraffel/Code/pulp}"
SPECTR="${SPECTR_DIR:-/Users/danielraffel/Code/spectr}"
EDITOR_HTML="$SPECTR/resources/editor.html"
REFERENCE="$PULP/planning/screenshots/REFERENCE-spectr-editor-html.png"
OUT_DIR="$PULP/planning/screenshots"
OUT="$OUT_DIR/spectr-native-latest.png"
THRESHOLD="${PULP_HARNESS_THRESHOLD:-0.85}"

SKIP_IMPORT=0
SKIP_BUILD=0
SKIP_CAPTURE=0
for arg in "$@"; do
  case "$arg" in
    --skip-import) SKIP_IMPORT=1 ;;
    --skip-build)  SKIP_BUILD=1 ;;
    --skip-capture) SKIP_CAPTURE=1 ;;
    -h|--help) sed -n '/^# /,/^$/p' "$0"; exit 0 ;;
  esac
done

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yel()   { printf '\033[33m%s\033[0m\n' "$*"; }

# Sanity
[[ -f "$REFERENCE" ]] || { red "ERROR: missing reference $REFERENCE — capture via Chrome first"; exit 2; }
[[ -f "$EDITOR_HTML" ]] || { red "ERROR: missing $EDITOR_HTML"; exit 2; }
which pulp >/dev/null || { red "ERROR: pulp CLI not in PATH"; exit 2; }
which python3 >/dev/null || { red "ERROR: python3 required for diff"; exit 2; }

# Freshness — refuse to validate from a checkout behind origin/main.
# Lesson from 2026-05-15: a roundtrip ran from a feature branch 175 commits
# behind. The diff score reflected stale framework code, not main. Bypass
# with PULP_FRESHNESS_BYPASS=1 if you specifically want to validate a feature
# branch's code.
( cd "$PULP" && "$PULP/tools/scripts/check_workspace_freshness.sh" ) || {
  red "ERROR: refusing to run roundtrip against stale checkout (see freshness output above)"
  exit 2
}

mkdir -p "$OUT_DIR"

# ── [1/5] Re-import via pulp ───────────────────────────────────────────────
if [[ $SKIP_IMPORT -eq 0 ]]; then
  echo "[1/5] Re-import editor.html via pulp import-design…"
  cd "$PULP"
  # Workaround for "not in a Pulp project directory": run from the Pulp tree
  # with an absolute path to spectr's HTML.
  if ! pulp import-design --from claude --file "$EDITOR_HTML" >/tmp/spectr-rt-import.log 2>&1; then
    red "ERROR: pulp import-design failed"
    tail -20 /tmp/spectr-rt-import.log
    exit 2
  fi
  grep -E "elements:|elements " /tmp/spectr-rt-import.log | head -1
  # Park output under planning baselines + diff vs last-run baseline
  STAMP=$(date +%Y%m%d-%H%M%S)
  BASE="$PULP/planning/import-baselines/$STAMP"
  mkdir -p "$BASE"
  mv ui.js bridge_handlers.cpp classnames.json "$BASE/" 2>/dev/null || true
  ls "$BASE/" | head -5
  echo "  Baselined to: $BASE"
else
  yel "[1/5] Skipped re-import (--skip-import)"
fi

# ── [2/5] Build Spectr standalone (native bridge + GPU) ────────────────────
if [[ $SKIP_BUILD -eq 0 ]]; then
  echo "[2/5] Build Spectr (native bridge + GPU)…"
  cd "$SPECTR"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPECTR_NATIVE_EDITOR=ON \
    >/tmp/spectr-rt-cmake.log 2>&1 || {
    red "ERROR: cmake configure failed"
    tail -30 /tmp/spectr-rt-cmake.log
    exit 2
  }
  cmake --build build --config Release -j"$(sysctl -n hw.ncpu)" \
    >/tmp/spectr-rt-build.log 2>&1 || {
    red "ERROR: build failed"
    tail -30 /tmp/spectr-rt-build.log
    exit 2
  }
  green "  build OK ($(stat -f %z "$SPECTR/build/Spectr.app/Contents/MacOS/Spectr" 2>/dev/null) bytes)"
else
  yel "[2/5] Skipped build (--skip-build)"
fi

# ── [3-4/5] Candidate capture ──────────────────────────────────────────────
if [[ $SKIP_CAPTURE -eq 0 ]]; then
  red "ERROR: automatic inspector screenshot capture was retired."
  red "  Capture the candidate through the canonical control platform, save it to $OUT,"
  red "  then rerun with --skip-capture."
  exit 2
fi
yel "[3-4/5] Using externally captured candidate (--skip-capture)"

# ── [5/5] Diff against reference ───────────────────────────────────────────
echo "[5/5] Diff native render against REFERENCE…"
[[ -f "$OUT" ]] || { red "ERROR: no candidate screenshot at $OUT"; exit 2; }
if python3 "$PULP/tools/import-validation/diff_against_reference.py" \
   "$REFERENCE" "$OUT" --threshold "$THRESHOLD"; then
  green ""
  green "✓ ROUND-TRIP PASS — native render matches editor.html reference"
  green "  threshold=$THRESHOLD  candidate=$OUT"
  exit 0
else
  red ""
  red "✗ ROUND-TRIP FAIL — native render diverges from reference"
  red "  this is the gap; the next Pulp fix should narrow it"
  red ""
  echo "Side-by-side:"
  echo "  reference:  $REFERENCE"
  echo "  candidate:  $OUT"
  echo ""
  echo "Possible Pulp issues to file:"
  echo "  - importer IR too shallow (today: 9-11 elements vs hundreds)"
  echo "  - format gap: importer emits createCol DSL, Spectr expects React+JSX"
  echo "  - sandbox runtime can't expand React tree"
  exit 1
fi
