#!/usr/bin/env bash
# format_baseline_capture.sh — companion-track U-3.
#
# Capture golden output from auval (Audio Unit), pluginval (VST3), and
# clap-validator (CLAP) on a representative Pulp plugin. Normalize the
# output (strip timestamps, system info, ephemeral paths) and write to
# test/fixtures/format-baseline/. The committed baselines are diffed by
# format_baseline_diff.py in CI whenever a PR touches core/format/ or
# core/host/plugin_slot_*.
#
# Why this exists: format-adapter bugs that compile clean often slip
# through PR review until they manifest in a real DAW. The validators
# (auval/pluginval/clap-validator) catch a meaningful subset of these,
# but their output isn't currently captured or diffed. This script makes
# the validator output a first-class fixture so silent regressions get
# caught at PR time.
#
# Usage:
#   tools/scripts/format_baseline_capture.sh [--build] [--plugin <name>]
#
# Flags:
#   --build           configure + build the plugin before capturing
#                     (default: assume plugins are already built in ./build/)
#   --plugin <name>   plugin slug to validate (default: PulpDrums)
#   --output <dir>    output directory (default: test/fixtures/format-baseline)
#
# Exit codes:
#   0 — all available validators captured successfully
#   1 — capture failure
#   2 — no validators available on this host
#
# Validator availability:
#   auval           macOS only, ships with the OS
#   pluginval       installed via brew or built from source
#   clap-validator  cargo install clap-validator (or download release)

set -euo pipefail

BUILD_FIRST=0
PLUGIN="PulpEffect"
OUTPUT_DIR="test/fixtures/format-baseline"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) BUILD_FIRST=1; shift;;
        --plugin) PLUGIN="$2"; shift 2;;
        --output) OUTPUT_DIR="$2"; shift 2;;
        *) echo "Unknown flag: $1" >&2; exit 2;;
    esac
done

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

if [[ $BUILD_FIRST -eq 1 ]]; then
    echo "[baseline] Configuring + building $PLUGIN" >&2
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build --target "$PLUGIN" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >/dev/null
fi

mkdir -p "$OUTPUT_DIR"

captured=0
failed=0
NO_EDITOR_ENV=(
    env
    PULP_DISABLE_PLUGIN_EDITOR=1
    PULP_HEADLESS=1
    PULP_TEST_MODE=1
)

# ── Normalizer ─────────────────────────────────────────────────────────
# Strip lines that are inherently non-deterministic (timestamps, host
# paths, dylib load addresses, cache directories, validator version,
# CPU/OS info). Keep the structural pass/fail signal that we care about.
normalize() {
    sed -E \
        -e 's|[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.+Z-]+|<TIMESTAMP>|g' \
        -e 's|/Users/[^/[:space:]]+|/Users/<USER>|g' \
        -e 's|/private/var/folders/[^[:space:]"]+|/private/var/folders/<TMP>|g' \
        -e 's|0x[0-9a-fA-F]{8,}|0x<ADDR>|g' \
        -e 's|version [0-9]+\.[0-9]+(\.[0-9]+)?|version <VER>|gi' \
        -e 's|elapsed: [0-9.]+s|elapsed: <DURATION>|g' \
        -e 's|elapsed [0-9.]+ ?s|elapsed <DURATION>|g' \
        -e 's|[0-9]+ ?ms|<MS>ms|g'
}

# ── auval (AU) ─────────────────────────────────────────────────────────
if command -v auval >/dev/null 2>&1; then
    AU_BUNDLE="$HOME/Library/Audio/Plug-Ins/Components/${PLUGIN}.component"
    if [[ -d "$AU_BUNDLE" ]]; then
        echo "[baseline] auval: $PLUGIN" >&2
        # auval -v takes type:subtype:manufacturer. We extract those from
        # the bundle's Info.plist when available; fall back to a wildcard
        # listing of all installed AUs and grep for the plugin name.
        if /usr/libexec/PlistBuddy -c "Print :AudioComponents:0" \
                "$AU_BUNDLE/Contents/Info.plist" >/dev/null 2>&1; then
            type=$(/usr/libexec/PlistBuddy -c "Print :AudioComponents:0:type" "$AU_BUNDLE/Contents/Info.plist")
            subtype=$(/usr/libexec/PlistBuddy -c "Print :AudioComponents:0:subtype" "$AU_BUNDLE/Contents/Info.plist")
            manuf=$(/usr/libexec/PlistBuddy -c "Print :AudioComponents:0:manufacturer" "$AU_BUNDLE/Contents/Info.plist")
            if "${NO_EDITOR_ENV[@]}" auval -v "$type" "$subtype" "$manuf" 2>&1 \
                | normalize > "$OUTPUT_DIR/${PLUGIN}.au.txt"; then
                captured=$((captured + 1))
            else
                failed=$((failed + 1))
            fi
        else
            echo "[baseline] auval: skipping — Info.plist missing AudioComponents key" >&2
        fi
    else
        echo "[baseline] auval: skipping — $AU_BUNDLE not installed" >&2
    fi
else
    echo "[baseline] auval: not available (macOS only)" >&2
fi

# ── pluginval (VST3) ───────────────────────────────────────────────────
if command -v pluginval >/dev/null 2>&1; then
    VST3_BUNDLE="$HOME/Library/Audio/Plug-Ins/VST3/${PLUGIN}.vst3"
    if [[ -d "$VST3_BUNDLE" ]]; then
        echo "[baseline] pluginval: $PLUGIN" >&2
        # --strictness-level 5 is the most thorough but slow; level 3
        # catches most issues and runs in seconds.
        if "${NO_EDITOR_ENV[@]}" pluginval --validate "$VST3_BUNDLE" --strictness-level 3 --skip-gui-tests 2>&1 \
            | normalize > "$OUTPUT_DIR/${PLUGIN}.vst3.txt"; then
            captured=$((captured + 1))
        else
            failed=$((failed + 1))
        fi
    else
        echo "[baseline] pluginval: skipping — $VST3_BUNDLE not installed" >&2
    fi
else
    echo "[baseline] pluginval: not available" >&2
fi

# ── clap-validator (CLAP) ──────────────────────────────────────────────
if command -v clap-validator >/dev/null 2>&1; then
    CLAP_BUNDLE="$HOME/Library/Audio/Plug-Ins/CLAP/${PLUGIN}.clap"
    if [[ -e "$CLAP_BUNDLE" ]]; then
        echo "[baseline] clap-validator: $PLUGIN" >&2
        if "${NO_EDITOR_ENV[@]}" clap-validator validate "$CLAP_BUNDLE" 2>&1 \
            | normalize > "$OUTPUT_DIR/${PLUGIN}.clap.txt"; then
            captured=$((captured + 1))
        else
            failed=$((failed + 1))
        fi
    else
        echo "[baseline] clap-validator: skipping — $CLAP_BUNDLE not installed" >&2
    fi
else
    echo "[baseline] clap-validator: not available — install via 'cargo install clap-validator'" >&2
fi

if [[ $captured -eq 0 && $failed -eq 0 ]]; then
    echo "[baseline] No validators available on this host." >&2
    exit 2
fi

if [[ $failed -gt 0 && $captured -eq 0 ]]; then
    # All available validators failed. That's a hard fail — no
    # signal at all.
    echo "[baseline] All $failed validator(s) failed — no captured output." >&2
    exit 1
fi

if [[ $failed -gt 0 ]]; then
    # Partial: some validators captured cleanly, others crashed.
    # Common cause: pluginval SIGKILL on unsigned bundles (fixed by
    # ad-hoc codesign upstream), or clap-validator missing from
    # PATH. Continue with what we have so the gate can still diff
    # the surviving lanes.
    echo "[baseline] WARN: $failed validator(s) failed; $captured captured into $OUTPUT_DIR" >&2
    exit 0
fi

echo "[baseline] Captured $captured validator(s) into $OUTPUT_DIR" >&2
