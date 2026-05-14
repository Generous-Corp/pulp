#!/usr/bin/env bash
# quick-spectr-cycle.sh — ONE command, ~10 seconds: rebuild pulp-react in the
# current worktree, rebuild Spectr's editor.js, capture native screenshot,
# diff vs the cached webview baseline. Print PASS/FAIL + similarity score.
#
# This is the tight visual-iteration loop. Run it on every pulp-react edit
# during the Spectr import-flow chase. The webview baseline is captured
# once (cached in planning/screenshots/REFERENCE-spectr-editor-html.png);
# we only re-capture if editor.html changes.
#
# Usage:
#   quick-spectr-cycle.sh                         # use current $PWD worktree's pulp-react
#   quick-spectr-cycle.sh --worktree /tmp/foo     # explicit worktree
#   quick-spectr-cycle.sh --label flex-fix        # label the output PNG
#   quick-spectr-cycle.sh --no-build              # skip the pulp-react rebuild
#
# Exit codes:
#   0  similarity ≥ threshold (PASS)
#   1  similarity below threshold (FAIL — gap is real)
#   2  pipeline broke

set -euo pipefail

PULP="${PULP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && git rev-parse --show-toplevel 2>/dev/null || pwd)}"
SPECTR="${SPECTR_DIR:-$(cd "$PULP/.." && pwd)/spectr}"
[[ -d "$SPECTR/native-react" ]] || { echo "[quickcycle] FATAL: SPECTR_DIR=$SPECTR has no native-react/; set SPECTR_DIR env var" >&2; exit 2; }
REFERENCE="${PULP}/planning/screenshots/REFERENCE-spectr-editor-html.png"
SCREENSHOTS="${PULP}/planning/screenshots"
THRESHOLD="${PULP_HARNESS_THRESHOLD:-0.85}"
WORKTREE=""
LABEL="iter"
DO_BUILD=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --worktree) WORKTREE="$2"; shift 2 ;;
    --label)    LABEL="$2"; shift 2 ;;
    --no-build) DO_BUILD=0; shift ;;
    -h|--help)  sed -n '/^# /,/^$/p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# Auto-detect worktree from $PWD if not given
if [[ -z "$WORKTREE" ]]; then
  if [[ -d "${PWD}/packages/pulp-react" ]]; then
    WORKTREE="$PWD"
  else
    WORKTREE="$PULP"
  fi
fi

PULP_REACT="${WORKTREE}/packages/pulp-react"
[[ -d "$PULP_REACT" ]] || { echo "FATAL: no pulp-react at $PULP_REACT" >&2; exit 2; }
[[ -f "$REFERENCE" ]] || { echo "FATAL: missing $REFERENCE" >&2; exit 2; }

TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_PNG="${SCREENSHOTS}/spectr-quickcycle-${LABEL}-${TS}.png"
SPECTR_NODE_MODULES="${SPECTR}/native-react/node_modules/@pulp/react"

log() { printf '[quickcycle] %s\n' "$*"; }

if [[ $DO_BUILD -eq 1 ]]; then
  log "[1/4] Building @pulp/react from $WORKTREE…"
  ( cd "$PULP_REACT" && npm run build >/tmp/pulp-react-build.log 2>&1 ) \
    || { log "FATAL: pulp-react build failed; see /tmp/pulp-react-build.log"; exit 2; }

  log "[2/4] Syncing dist → Spectr node_modules…"
  cp -R "${PULP_REACT}/dist/." "${SPECTR_NODE_MODULES}/dist/" \
    || { log "FATAL: dist sync failed"; exit 2; }

  log "[3/4] Rebundling Spectr editor.js (build:port — real Spectr port)…"
  ( cd "${SPECTR}/native-react" && npm run build:port >/tmp/spectr-bundle.log 2>&1 ) \
    || { log "FATAL: Spectr bundle failed; see /tmp/spectr-bundle.log"; exit 2; }
else
  log "[1-3/4] Skipped (--no-build)."
fi

log "[4/4] Capturing native render → $OUT_PNG"
"${PULP}/build/tools/screenshot/pulp-screenshot" \
  --script "${SPECTR}/native-react/dist/editor.js" \
  --output "$OUT_PNG" \
  --width 1280 --height 800 --scale 2.0 --theme dark \
  >/tmp/pulp-screenshot.log 2>&1 \
  || { log "FATAL: pulp-screenshot failed; see /tmp/pulp-screenshot.log"; exit 2; }

log "Diffing vs webview baseline…"
# Run the diff and capture its real exit status. We deliberately avoid `|| true`
# here so $? reflects the python script's verdict (0 = PASS, non-zero = FAIL).
# `set -e` is suspended for this single command via `if`, so a failing diff
# does not abort the script before we print the summary.
if SCORE_LINE="$(python3 "${PULP}/tools/import-validation/diff_against_reference.py" \
  "$REFERENCE" "$OUT_PNG" --threshold "$THRESHOLD" 2>&1)"; then
  RESULT=0
else
  RESULT=$?
fi
echo "$SCORE_LINE"

echo ""
echo "=== quickcycle summary ==="
echo "Worktree: $WORKTREE"
echo "Native:   $OUT_PNG"
echo "Baseline: $REFERENCE"
echo "Threshold: $THRESHOLD"
exit "$RESULT"
