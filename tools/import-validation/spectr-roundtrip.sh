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
#   PULP_CAPTURE_CLI       pulp/pulp-cpp binary with `inspect screenshot` (default: PATH pulp)
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
CAPTURE_CLI="${PULP_CAPTURE_CLI:-$(command -v pulp || true)}"

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
[[ -x "$CAPTURE_CLI" ]] || { red "ERROR: capture CLI is not executable: $CAPTURE_CLI"; exit 2; }
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

# ── [3/5] Launch Spectr directly + verify it's our binary ──────────────────
if [[ $SKIP_CAPTURE -eq 0 ]]; then
  echo "[3/5] Launch Spectr (direct binary, bypass LaunchServices)…"
  pkill -f "Spectr.app/Contents/MacOS/Spectr" 2>/dev/null || true
  sleep 0.5
  BINARY="$SPECTR/build/Spectr.app/Contents/MacOS/Spectr"
  [[ -x "$BINARY" ]] || { red "ERROR: $BINARY missing or not executable"; exit 2; }
  PULP_INSPECT_PROFILE=observe "$BINARY" >/tmp/spectr-rt-runtime.log 2>&1 &
  PID=$!
  sleep 4
  if ! kill -0 $PID 2>/dev/null; then
    red "ERROR: Spectr crashed at launch"
    tail -30 /tmp/spectr-rt-runtime.log
    exit 2
  fi
  # Verify it's our binary path
  CMD=$(ps -p $PID -o command= 2>&1 | head -1)
  if [[ "$CMD" != *"$BINARY"* ]]; then
    red "ERROR: launched process is not the binary we built"
    echo "  expected: $BINARY"
    echo "  running:  $CMD"
    kill $PID 2>/dev/null || true
    exit 2
  fi
  green "  Spectr running (PID=$PID, path verified)"

  # ── [4/5] Capture window ────────────────────────────────────────────────
  echo "[4/5] Capture Spectr window in-process…"
  # Match discovery by PID so another live inspector session cannot be
  # captured accidentally. This path runs inside Spectr and therefore works
  # over SSH without Screen Recording or Accessibility permission.
  IDENTITY=""
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    IDENTITY=$("$CAPTURE_CLI" inspect list --json 2>/dev/null | \
      python3 -c 'import json,sys; p=int(sys.argv[1]); rows=json.load(sys.stdin).get("sessions", []); matches=[r for r in rows if r.get("processId")==p]; print("\t".join((matches[0]["sessionId"], matches[0]["instanceId"], matches[0]["publicationId"]))) if len(matches)==1 else None' "$PID" || true)
    [[ -n "$IDENTITY" ]] && break
    sleep 0.25
  done
  if [[ -z "$IDENTITY" ]]; then
    red "ERROR: Spectr did not publish one inspector session for pid=$PID."
    red "  Ensure it was rebuilt against a Pulp SDK with standalone inspector support."
    kill $PID 2>/dev/null || true
    exit 2
  fi
  IFS=$'\t' read -r SESSION_ID INSTANCE_ID PUBLICATION_ID <<< "$IDENTITY"
  if "$CAPTURE_CLI" inspect screenshot --out "$OUT" \
      --session "$SESSION_ID" --instance "$INSTANCE_ID" \
      --publication "$PUBLICATION_ID"; then
    :
  else
    status=$?
    red "ERROR: in-process Spectr capture failed (status=$status)."
    red "  Status 3 means this standalone host explicitly lacks capture capability."
    kill $PID 2>/dev/null || true
    exit 2
  fi
  [[ -f "$OUT" ]] || { red "ERROR: screenshot was not written to $OUT"; kill $PID 2>/dev/null; exit 2; }
  green "  captured to $OUT ($(stat -f %z "$OUT") bytes)"
  # Leave Spectr running so user can click to test — or kill if env says so
  if [[ -n "${PULP_HARNESS_KILL_AFTER:-}" ]]; then
    kill $PID 2>/dev/null || true
  fi
else
  yel "[3-4/5] Skipped launch+capture (--skip-capture)"
fi

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
