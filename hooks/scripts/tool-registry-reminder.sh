#!/bin/bash
# tool-registry-reminder.sh — PostToolUse hook (Edit|Write|Bash).
#
# Catch an agent in the act of hand-rolling a tool Pulp already ships.
#
# This exists because of a real incident: an agent needed to compare an imported
# design against its source, and wrote its own PIL crop script — while
# fidelity_diff.py, montage.py and figma_import_diff.py sat in the tree. The
# tools were documented; the docs just never fired at the moment of need. A
# registry nobody reads is the same bug with more YAML, so this is the delivery
# at the moment the hand-roll is being typed.
#
# Deliberately coarse: the crudest PIL matcher catches the real incident, and a
# false positive costs one line of ignorable text. What it must NOT do is nag
# about legitimate PIL/soundfile use INSIDE the registered tools themselves —
# hence the tool-directory bail-out below.
#
# Advisory only: always exits 0, never blocks a tool call.
#
# Source of truth for what's registered: docs/status/tools.yaml (and the
# generated "Registered tools" digest in CLAUDE.md).

# Payload: TOOL_INPUT env (this repo's existing hook convention, see
# docs-reminder.sh) or the hook JSON on stdin. Support both so the hook works
# regardless of which one the harness supplies.
PAYLOAD="${TOOL_INPUT:-}"
if [ -z "$PAYLOAD" ] && [ ! -t 0 ]; then
    PAYLOAD=$(cat 2>/dev/null)
fi
[ -n "$PAYLOAD" ] || exit 0

read -r -d '' PARSE <<'PY'
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
# Accept either the bare tool_input or the full hook envelope.
ti = d.get("tool_input") if isinstance(d.get("tool_input"), dict) else d
if not isinstance(ti, dict):
    sys.exit(0)
path = ti.get("file_path") or ""
# Write -> content; Edit -> new_string; Bash -> command.
body = " ".join(str(ti.get(k) or "") for k in ("content", "new_string", "command"))
print(path)
print(body.replace("\n", " "))
PY

FILE=$(printf '%s' "$PAYLOAD" | python3 -c "$PARSE" 2>/dev/null | sed -n '1p')
BODY=$(printf '%s' "$PAYLOAD" | python3 -c "$PARSE" 2>/dev/null | sed -n '2p')

[ -n "$BODY" ] || exit 0

# Bail out inside the registered tools themselves — they are SUPPOSED to import
# PIL. Without this the hook would nag on every edit to montage.py.
case "$FILE" in
    */tools/import-design/*|*/tools/import-validation/*|*/tools/harness/*|*/tools/audio/*|*/tools/scripts/*)
        exit 0
        ;;
esac

# Image comparison / cropping / diffing — the incident's exact signature.
if printf '%s' "$BODY" | grep -qE 'from PIL|import PIL|PIL\.Image|Image\.open|ImageChops|ImageDraw|\.crop\(|pixelmatch'; then
    cat <<'EOF'
REGISTERED TOOL: this looks like hand-rolled image comparison/cropping. Pulp already ships these — use them instead of PIL:
  - compare a render to its design source, per node -> tools/import-design/fidelity_diff.py
  - see WHERE two images disagree (4-panel diff)   -> tools/scripts/figma_import_diff.py
  - labeled reference-vs-render montage             -> tools/import-design/montage.py
  - one similarity score + verdict                  -> tools/import-validation/diff_against_reference.py
  - per-region scores (a broken sub-region hides in a whole-image score)
                                                    -> tools/import-validation/diff_against_reference_regions.py
  - prove an importer change didn't regress a design -> tools/import-validation/golden_regression.py
Full registry: docs/status/tools.yaml. Guidance: .agents/skills/import-design/ and .agents/skills/screenshot/.
If none of them fit, say why — then hand-roll.
EOF
    exit 0
fi

# Hand-rolled audio measurement — same disease, other subsystem.
if printf '%s' "$BODY" | grep -qE 'soundfile\.read|scipy\.io\.wavfile|wave\.open|librosa\.load'; then
    cat <<'EOF'
REGISTERED TOOL: this looks like hand-rolled audio measurement. Pulp already ships these:
  - summarize / diagnose / compare / gate a WAV -> pulp audio validate {summarize|doctor|compare|assert}
  - waveform / spectrum of a sample window      -> pulp audio scope --input-wav <f.wav>
  - render a plugin offline (no DAW/device)     -> pulp audio render
  - "did this DSP change sound WORSE, and where?" -> Audio Quality Lab
                                                   (pulp tool install audio-quality-lab)
Full registry: docs/status/tools.yaml. Guidance: .agents/skills/audio-harness/.
If none of them fit, say why — then hand-roll.
EOF
    exit 0
fi

exit 0
