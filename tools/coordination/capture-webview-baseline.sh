#!/usr/bin/env bash
# capture-webview-baseline.sh — render editor.html in Chrome headless and
# capture a fresh webview baseline screenshot. Pair with the native render
# from spectr-roundtrip.sh and diff_against_reference.py to detect deltas
# between "what the webview rendering of editor.html looks like" and "what
# our GPU-native runtime-import produces from the same HTML."
#
# The webview is the gold standard. This script makes the baseline
# regenerable on demand so it doesn't go stale when editor.html changes.
#
# Usage:
#   capture-webview-baseline.sh                  # refresh canonical REFERENCE
#   capture-webview-baseline.sh --width 1320 --height 860
#   capture-webview-baseline.sh --output /tmp/foo.png
#   capture-webview-baseline.sh --timestamped    # write a timestamped file
#                                                # instead of overwriting the
#                                                # canonical REFERENCE
#   capture-webview-baseline.sh --diff           # also diff vs native-latest
#
# By default the screenshot is written to the canonical REFERENCE path that
# quick-spectr-cycle.sh compares against, so a single capture run actually
# refreshes the baseline. Use --output to override the destination, or
# --timestamped to keep historical snapshots side-by-side.
#
# Env:
#   PULP_DIR     pulp root (default /Users/danielraffel/Code/pulp)
#   SPECTR_DIR   spectr root (default /Users/danielraffel/Code/spectr)
#   CHROME       path to Chrome binary (default macOS Google Chrome)
#
# Exit codes:
#   0  success (capture wrote a PNG; optional diff produced a score)
#   1  capture failed
#   2  config error (Chrome missing, editor.html missing, etc.)

set -euo pipefail

PULP="${PULP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && git rev-parse --show-toplevel 2>/dev/null || pwd)}"
SPECTR="${SPECTR_DIR:-$(cd "$PULP/.." && pwd)/spectr}"
[[ -d "$SPECTR/resources" ]] || { echo "[capture-webview] FATAL: SPECTR_DIR=$SPECTR has no resources/; set SPECTR_DIR env var" >&2; exit 2; }
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"

WIDTH=1280
HEIGHT=800
DO_DIFF=0
OUT_PNG=""
TIMESTAMPED=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --width)       WIDTH="$2"; shift 2 ;;
    --height)      HEIGHT="$2"; shift 2 ;;
    --output)      OUT_PNG="$2"; shift 2 ;;
    --diff)        DO_DIFF=1; shift ;;
    --timestamped) TIMESTAMPED=1; shift ;;
    -h|--help)     sed -n '/^# /,/^$/p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

EDITOR_HTML="${SPECTR}/resources/editor.html"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
SCREENSHOTS_DIR="${PULP}/planning/screenshots"
# Default: overwrite the canonical REFERENCE that quick-spectr-cycle.sh reads,
# so capturing actually refreshes the baseline. Use --timestamped for the
# legacy "side-by-side history" behaviour, or --output to pick a custom path.
if [[ -z "$OUT_PNG" ]]; then
  if [[ $TIMESTAMPED -eq 1 ]]; then
    OUT_PNG="${SCREENSHOTS_DIR}/webview-baseline-editor-html-${TS}.png"
  else
    OUT_PNG="${SCREENSHOTS_DIR}/REFERENCE-spectr-editor-html.png"
  fi
fi

log() { printf '[capture-webview] %s\n' "$*"; }
die() { printf '[capture-webview] FATAL: %s\n' "$*" >&2; exit 2; }

[[ -x "$CHROME" ]] || die "Chrome not found at $CHROME"
[[ -f "$EDITOR_HTML" ]] || die "editor.html not found at $EDITOR_HTML"
mkdir -p "$SCREENSHOTS_DIR"

log "Rendering $EDITOR_HTML in Chrome headless at ${WIDTH}x${HEIGHT}…"

# --force-device-scale-factor=2 to match the @2x scale pulp-screenshot uses
# so the diff math compares like-for-like resolutions. --hide-scrollbars
# avoids the right-edge scrollbar polluting the pixel comparison.
"$CHROME" \
  --headless=new \
  --disable-gpu \
  --hide-scrollbars \
  --force-device-scale-factor=2 \
  --window-size="${WIDTH},${HEIGHT}" \
  --screenshot="$OUT_PNG" \
  --virtual-time-budget=5000 \
  "file://${EDITOR_HTML}" \
  >/tmp/chrome-headless.log 2>&1 || {
    log "ERROR: Chrome headless failed; see /tmp/chrome-headless.log"
    tail -10 /tmp/chrome-headless.log
    exit 1
  }

[[ -s "$OUT_PNG" ]] || { log "ERROR: screenshot file is empty"; exit 1; }
log "Webview baseline: $OUT_PNG ($(stat -f%z "$OUT_PNG") bytes)"

if [[ $DO_DIFF -eq 1 ]]; then
  NATIVE_LATEST="$(ls -t "${SCREENSHOTS_DIR}"/spectr-1894-flex-fix-REAL-port-*.png 2>/dev/null | head -1)"
  if [[ -z "$NATIVE_LATEST" ]]; then
    NATIVE_LATEST="$(ls -t "${SCREENSHOTS_DIR}"/spectr-native-latest.png 2>/dev/null | head -1)"
  fi
  if [[ -z "$NATIVE_LATEST" || ! -f "$NATIVE_LATEST" ]]; then
    log "WARN: no native screenshot found to diff against"
    exit 0
  fi
  log "Diffing native ($NATIVE_LATEST) vs webview baseline ($OUT_PNG)…"
  python3 "${PULP}/tools/import-validation/diff_against_reference.py" \
    "$OUT_PNG" "$NATIVE_LATEST" --threshold 0.85 || true
fi
