#!/usr/bin/env bash
# Replay a saved agent-authored panel through the whole pipeline and score it.
#
# Iterating on the RENDERER does not need a model in the loop. The agent's job
# — turning a brief into markup — is done and its output is checked in; every
# round after that is Chromium solving the same HTML and Skia trying to match
# it. Re-prompting for each round costs money, takes minutes, and changes the
# subject under test, which is the one thing a fidelity comparison cannot
# afford.
#
#   tools/import-validation/replay-agent-panel.sh                    # magneto
#   tools/import-validation/replay-agent-panel.sh <panel-dir> [WxH]
#
# Prints the gate result and the fidelity number, and leaves the Chrome
# reference and the native render side by side for eyeballing.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PANEL_DIR="${1:-$REPO/test/fixtures/agent-panels/magneto}"
SIZE="${2:-760x717}"
PACK="${PULP_REPLAY_PACK:-tape-machine}"
FORGE="${PULP_FORGE_REPO:-/Volumes/Workshop/Code/forge-design-fit-20260801}"
IMPORTER="$REPO/build/tools/import-design/pulp-import-design"

[ -f "$PANEL_DIR/panel.html" ] || { echo "no panel.html in $PANEL_DIR"; exit 2; }
[ -x "$IMPORTER" ] || { echo "importer not built: $IMPORTER"; exit 2; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/pulp-replay-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cp "$PANEL_DIR/panel.html" "$WORK/panel.html"

# The page links `styles.css` and the pack's @font-face rules pull `fonts/*`.
# Both are served relative to the page, and a 404 on either aborts the capture
# with `capture-source-unresolved` — which reads like a broken design rather
# than a missing asset. Stage them the way Forge's own import does.
PACK_CSS="$FORGE/build/$PACK.flat.css"
PACK_FONTS="$FORGE/data/design_systems/$PACK/fonts"
if [ -f "$PACK_CSS" ]; then
    cp "$PACK_CSS" "$WORK/styles.css"
    [ -d "$PACK_FONTS" ] && mkdir -p "$WORK/fonts" && cp "$PACK_FONTS"/*.ttf "$WORK/fonts/" 2>/dev/null
else
    echo "note: pack '$PACK' not found at $PACK_CSS — the panel will import unstyled"
fi

echo "── import ────────────────────────────────────────────────"
# --allow-browser-network lets the page load its own sibling assets.
"$IMPORTER" --file "$WORK/panel.html" --output "$WORK/panel.ir.json" \
    --emit ir-json --native-panel-lowering --allow-browser-network \
    --render-size "$SIZE" > "$WORK/import.log" 2>&1
IMPORT_RC=$?
if [ $IMPORT_RC -ne 0 ]; then
    echo "import FAILED (exit $IMPORT_RC):"
    tail -5 "$WORK/import.log"
    exit 1
fi
grep -E "^(Similarity|Validation)" "$WORK/import.log" || true

echo
echo "── pipeline stages ───────────────────────────────────────"
python3 "$REPO/tools/import-validation/check_pipeline_stages.py" "$WORK/panel.ir.json"
GATE_RC=$?

echo
echo "── fidelity vs its own Chrome capture ────────────────────"
CAP="$WORK/panel.ir-browser-capture"
# The pack puts 120px of padding on <body>, so the captured document is larger
# than the panel. Crop to the panel's own box or the scorer refuses on a size
# mismatch — correctly, since resizing would erase the 1px features it exists
# to detect.
#
# The crop comes from the RENDER's own pixels, not from $SIZE: the render is
# sized by the IR root, which is the solved layout and rarely a whole number
# (716.2 here). Deriving it from the requested viewport instead put the crop
# 2px out and the scorer refused — the argument and the artifact are allowed
# to disagree, so only the artifact can be trusted.
read -r CROP_W CROP_H <<<"$(python3 - "$CAP/validation-proof/render/render.png" \
    "$CAP/capture.json" <<'PY'
import json, struct, sys
with open(sys.argv[1], "rb") as f:
    f.read(16)
    w, h = struct.unpack(">II", f.read(8))
try:
    dpr = json.load(open(sys.argv[2])).get("device_scale_factor") or 2
except Exception:
    dpr = 2
print(int(w / dpr), int(h / dpr))
PY
)"
python3 "$REPO/tools/import-validation/score_native_panel.py" \
    --capture "$CAP" --render "$CAP/validation-proof/render/render.png" \
    --crop "120,120,${CROP_W},${CROP_H}" \
    --label "$(basename "$PANEL_DIR")" > "$WORK/score.json" 2>&1
SCORE_RC=$?
python3 - "$WORK/score.json" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(open(sys.argv[1]).read()[:400]); sys.exit(0)
tot, fail = d["total_ink_px"], d["failing_px_scored"]
print("  FIDELITY %.4f   (%s of %s ink px differ at dE %.1f)"
      % (1 - fail / tot, format(fail, ","), format(tot, ","), d["delta_e"]))
print("  calibration ok:", d["calibration"]["ok"], d["calibration"]["per_channel_median_delta"])
def classes(o, p=""):
    if isinstance(o, dict):
        if "fail_px" in o and "nodes" in o: yield p.split("/")[-1], o
        for k, v in o.items(): yield from classes(v, p + "/" + k)
rows = sorted(classes(d), key=lambda kv: -kv[1]["fail_px"])
if rows: print("  worst classes:")
for name, v in rows[:6]:
    if v["fail_px"]:
        print("    %-10s %9s px  %2d/%-2d nodes  %5.1f%% of its ink"
              % (name, format(v["fail_px"], ","), v["failing_nodes"], v["nodes"],
                 100 * v["failing_fraction"]))
PY

OUT="${PULP_REPLAY_OUT:-$REPO/build/replay/$(basename "$PANEL_DIR")}"
mkdir -p "$OUT"
cp "$CAP/browser.png" "$OUT/chrome.png" 2>/dev/null
cp "$CAP/validation-proof/render/render.png" "$OUT/native.png" 2>/dev/null
cp "$CAP/validation-proof/diff/diff.png" "$OUT/diff.png" 2>/dev/null
cp "$WORK/panel.ir.json" "$OUT/panel.ir.json" 2>/dev/null
echo
echo "── artifacts ─────────────────────────────────────────────"
echo "  $OUT/chrome.png   what Chrome drew  (the oracle)"
echo "  $OUT/native.png   what Skia drew"
echo "  $OUT/diff.png     where they differ"
echo "  $OUT/panel.ir.json"

[ $GATE_RC -eq 0 ] && [ $SCORE_RC -eq 0 ] || exit 1
