#!/usr/bin/env bash
# format_baseline_capture.sh — capture plugin-format validation baselines.
#
# Capture golden output from auval (Audio Unit), pluginval (VST3), and
# clap-validator (CLAP) on a representative Pulp plugin. Normalize the
# output (strip timestamps, system info, ephemeral paths) and write to
# test/fixtures/format-baseline/. The committed baselines are diffed by
# format_baseline_diff.py in CI whenever a PR touches core/format/ or
# core/host/plugin_slot_*.
#
# Why this exists: format-adapter bugs that compile clean can still
# manifest only in a real DAW. The validators
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
#   --diag-dir <dir>  also write per-validator diagnostics here: the raw
#                     (un-normalized) output and the exit code. Kept out of
#                     --output because every file there is treated as a
#                     validator fixture to diff.
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
DIAG_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) BUILD_FIRST=1; shift;;
        --plugin) PLUGIN="$2"; shift 2;;
        --output) OUTPUT_DIR="$2"; shift 2;;
        --diag-dir) DIAG_DIR="$2"; shift 2;;
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
if [[ -n "$DIAG_DIR" ]]; then
    mkdir -p "$DIAG_DIR"
fi

RAW_TMP="$(mktemp -t pulp-baseline-raw)"
trap 'rm -f "$RAW_TMP"' EXIT

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

# ── Validator runner ───────────────────────────────────────────────────
# Run one validator and record its combined output, normalized, at
# $outfile. Returns 0 when the validator exited 0.
#
# A non-zero exit means one of two very different things — the validator
# ran and reported findings, or it never ran at all (bundle unresolvable,
# binary missing its runtime) — and the exit code alone does not tell them
# apart. So echo the code and an excerpt of the output to the log: without
# them a failure here is a dead end, because the only other copy of the
# output lives in a temp dir the caller deletes.
run_validator() {
    local label="$1" outfile="$2"
    shift 2
    local rc=0
    "${NO_EDITOR_ENV[@]}" "$@" >"$RAW_TMP" 2>&1 || rc=$?
    normalize <"$RAW_TMP" >"$outfile"

    if [[ -n "$DIAG_DIR" ]]; then
        local stem
        stem="$(basename "$outfile" .txt)"
        cp "$RAW_TMP" "$DIAG_DIR/${stem}.raw.txt"
        printf '%s\n' "$rc" >"$DIAG_DIR/${stem}.exit"
    fi

    if [[ $rc -ne 0 ]]; then
        echo "[baseline] $label: exit $rc — first 20 lines of its output:" >&2
        sed -n '1,20p' "$RAW_TMP" | sed 's|^|[baseline]   |' >&2
        if [[ ! -s "$RAW_TMP" ]]; then
            echo "[baseline]   (no output at all — the validator did not run)" >&2
        fi
        return 1
    fi
    return 0
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
            if run_validator "auval" "$OUTPUT_DIR/${PLUGIN}.au.txt" \
                    auval -v "$type" "$subtype" "$manuf"; then
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
        if run_validator "pluginval" "$OUTPUT_DIR/${PLUGIN}.vst3.txt" \
                pluginval --validate "$VST3_BUNDLE" --strictness-level 3 --skip-gui-tests; then
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
        if run_validator "clap-validator" "$OUTPUT_DIR/${PLUGIN}.clap.txt" \
                clap-validator validate "$CLAP_BUNDLE"; then
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
    # Every validator that ran exited non-zero. Their output was still
    # written to $OUTPUT_DIR (and to --diag-dir when set) — each one's exit
    # code and an excerpt are in the log above.
    echo "[baseline] All $failed validator(s) exited non-zero. Their output is in $OUTPUT_DIR" >&2
    if [[ -n "$DIAG_DIR" ]]; then
        echo "[baseline] Raw output + exit codes: $DIAG_DIR" >&2
    fi
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
