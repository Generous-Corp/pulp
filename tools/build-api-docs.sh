#!/usr/bin/env bash
# build-api-docs.sh — Generate API reference from public headers using Doxygen.
# Output: build/api-docs/html/
#
# Injects the current SDK version from CMakeLists.txt (`project(Pulp VERSION x.y.z)`)
# as Doxygen's PROJECT_NUMBER so `/api/` always shows the right version instead
# of the stale literal baked into docs/doxygen/Doxyfile.
#
# `--contract-only` stops after the strict API-contract pass and skips the
# published HTML render. The contract pass is seconds of work and decides
# whether a public symbol may merge; the HTML render is a preview artifact that
# takes an order of magnitude longer. Splitting them lets the contract run as
# its own fast check without dragging the site build onto the critical path.

set -euo pipefail

CONTRACT_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --contract-only) CONTRACT_ONLY=1 ;;
        *)
            echo "Error: unknown argument: $arg" >&2
            echo "Usage: build-api-docs.sh [--contract-only]" >&2
            exit 2
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DOXYFILE="$ROOT/docs/doxygen/Doxyfile"
TIMELINE_STRICT_DOXYFILE="$ROOT/docs/doxygen/Doxyfile.timeline-strict"
SEQUENCER_API_BASELINE="$ROOT/docs/doxygen/sequencer-api-contract-legacy-baseline.json"
OUTPUT="$ROOT/build/api-docs"
PUBLISH_LOCK="${OUTPUT}.publish.lock"

if ! command -v doxygen &>/dev/null; then
    echo "Error: doxygen not found. Install with: brew install doxygen"
    exit 1
fi

if [ ! -f "$DOXYFILE" ]; then
    echo "Error: Doxyfile not found at $DOXYFILE"
    exit 1
fi
if [ ! -f "$TIMELINE_STRICT_DOXYFILE" ]; then
    echo "Error: strict Timeline Doxyfile not found at $TIMELINE_STRICT_DOXYFILE"
    exit 1
fi
if [ ! -f "$SEQUENCER_API_BASELINE" ]; then
    echo "Error: sequencer API contract baseline not found at $SEQUENCER_API_BASELINE"
    exit 1
fi

# Extract `project(Pulp VERSION x.y.z)` from the root CMakeLists.txt. Uses
# grep/sed so this works under both BSD and GNU userland (macOS + Linux).
# Falls back to the Doxyfile literal if the regex misses — Doxygen still
# produces output, just with the old version.
SDK_VERSION="$(grep -oE 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' "$ROOT/CMakeLists.txt" 2>/dev/null \
    | head -1 \
    | sed -E 's/VERSION[[:space:]]+//' || true)"

if [ -z "$SDK_VERSION" ]; then
    echo "Warning: could not parse SDK VERSION from CMakeLists.txt; using Doxyfile literal"
fi

if [ "$CONTRACT_ONLY" -eq 1 ]; then
    echo "Checking API contracts only (Pulp ${SDK_VERSION:-unknown})..."
else
    echo "Generating API reference (Pulp ${SDK_VERSION:-unknown})..."
fi
mkdir -p "$ROOT/build"

# Run Doxygen from the docs/doxygen directory so relative paths resolve.
# Stream the Doxyfile through stdin with an appended PROJECT_NUMBER override —
# Doxygen treats later assignments as wins, so the Doxyfile stays untouched.
cd "$ROOT/docs/doxygen"
DOXYGEN_LOG="$(mktemp "${TMPDIR:-/tmp}/pulp-doxygen.XXXXXX")"
STAGING_OUTPUT="$(mktemp -d "$ROOT/build/api-docs-stage.XXXXXX")"
STRICT_OUTPUT="$(mktemp -d "$ROOT/build/api-docs-timeline-check.XXXXXX")"
TRUSTED_BASELINE="$(mktemp "${TMPDIR:-/tmp}/pulp-sequencer-api-baseline.XXXXXX")"

cleanup() {
    status=$?
    trap - EXIT
    rm -f "$DOXYGEN_LOG" "$TRUSTED_BASELINE"
    rm -rf "$STAGING_OUTPUT" "$STRICT_OUTPUT"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

TRUSTED_REF="${PULP_API_DOCS_TRUSTED_REF:-origin/main}"
TRUSTED_PATH="docs/doxygen/sequencer-api-contract-legacy-baseline.json"
TRUSTED_BASELINE_ARGS=()
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if [ "${PULP_API_DOCS_SOURCE_ARCHIVE:-0}" = "1" ]; then
        echo "Error: PULP_API_DOCS_SOURCE_ARCHIVE=1 is valid only outside a Git worktree"
        exit 1
    fi
    if [ -z "${PULP_API_DOCS_TRUSTED_REF:-}" ]; then
        if ! git -C "$ROOT" fetch --quiet origin \
            +refs/heads/main:refs/remotes/origin/main; then
            echo "Error: default trusted API-doc baseline ref origin/main could not be refreshed"
            exit 1
        fi
        if ! FETCHED_COMMIT="$(git -C "$ROOT" rev-parse --verify 'FETCH_HEAD^{commit}' 2>/dev/null)" ||
            ! REFRESHED_COMMIT="$(git -C "$ROOT" rev-parse --verify 'refs/remotes/origin/main^{commit}' 2>/dev/null)" ||
            [ "$FETCHED_COMMIT" != "$REFRESHED_COMMIT" ]; then
            echo "Error: default trusted API-doc baseline ref origin/main did not match the fetched main commit"
            exit 1
        fi
    fi
    if ! TRUSTED_COMMIT="$(git -C "$ROOT" rev-parse --verify "${TRUSTED_REF}^{commit}" 2>/dev/null)"; then
        echo "Error: configured trusted API-doc baseline ref cannot be resolved: $TRUSTED_REF"
        exit 1
    fi
    if ! TRUSTED_ENTRY="$(git -C "$ROOT" ls-tree --name-only "$TRUSTED_COMMIT" -- "$TRUSTED_PATH")"; then
        echo "Error: configured trusted API-doc baseline ref cannot be read: $TRUSTED_REF"
        exit 1
    fi
    if [ "$TRUSTED_ENTRY" = "$TRUSTED_PATH" ]; then
        if ! git -C "$ROOT" show "$TRUSTED_COMMIT:$TRUSTED_PATH" >"$TRUSTED_BASELINE"; then
            echo "Error: configured trusted API-doc baseline cannot be read from $TRUSTED_REF"
            exit 1
        fi
        TRUSTED_BASELINE_ARGS=(--trusted-baseline "$TRUSTED_BASELINE")
    elif [ -z "$TRUSTED_ENTRY" ]; then
        echo "Trusted ref $TRUSTED_REF has no sequencer API baseline; verified bootstrap mode enabled"
        TRUSTED_BASELINE_ARGS=(--allow-missing-trusted-baseline)
    else
        echo "Error: trusted API-doc baseline lookup returned an unexpected path: $TRUSTED_ENTRY"
        exit 1
    fi
else
    if [ "${PULP_API_DOCS_SOURCE_ARCHIVE:-0}" != "1" ]; then
        echo "Error: no Git worktree; set PULP_API_DOCS_SOURCE_ARCHIVE=1 for an explicit source archive"
        exit 1
    fi
    echo "Explicit source-archive mode: trusted sequencer API baseline is unavailable"
    TRUSTED_BASELINE_ARGS=(--allow-missing-trusted-baseline)
fi

# CI installs whatever Doxygen ubuntu ships (1.9.8 at time of writing) while a
# dev machine usually has a much newer Homebrew build. They do not agree on
# every diagnostic — 1.9.8 errors on an `@param` block Doxygen attaches to a
# friend declaration with unnamed parameters, and 1.17 does not — so a local
# pass is necessary but NOT sufficient. Print the version so a CI-only failure
# is immediately recognizable as a version difference rather than a mystery.
echo "Checking exhaustive Timeline API contracts (local $(doxygen --version)," \
     "CI uses ubuntu's package — a local pass does not guarantee CI passes)..."
if ! {
    cat "$TIMELINE_STRICT_DOXYFILE"
    echo "OUTPUT_DIRECTORY = \"$STRICT_OUTPUT\""
} | doxygen - >"$DOXYGEN_LOG" 2>&1; then
    cat "$DOXYGEN_LOG"
    echo "Error: strict Timeline API documentation check failed"
    exit 1
fi
if ! python3 "$ROOT/tools/scripts/timeline_api_docs_check.py" \
    "$STRICT_OUTPUT/xml" \
    --baseline "$SEQUENCER_API_BASELINE" \
    --strict-config "$TIMELINE_STRICT_DOXYFILE" \
    --html-config "$DOXYFILE" \
    ${TRUSTED_BASELINE_ARGS[@]+"${TRUSTED_BASELINE_ARGS[@]}"}; then
    exit 1
fi

if [ "$CONTRACT_ONLY" -eq 1 ]; then
    echo "API contracts OK (--contract-only: published HTML render skipped)"
    exit 0
fi

if [ -n "$SDK_VERSION" ]; then
    if {
        cat "$DOXYFILE"
        echo "PROJECT_NUMBER = $SDK_VERSION"
        echo "OUTPUT_DIRECTORY = \"$STAGING_OUTPUT\""
    } | \
        doxygen - >"$DOXYGEN_LOG" 2>&1; then
        DOXYGEN_STATUS=0
    else
        DOXYGEN_STATUS=$?
    fi
else
    if {
        cat "$DOXYFILE"
        echo "OUTPUT_DIRECTORY = \"$STAGING_OUTPUT\""
    } | doxygen - >"$DOXYGEN_LOG" 2>&1; then
        DOXYGEN_STATUS=0
    else
        DOXYGEN_STATUS=$?
    fi
fi
if [ "$DOXYGEN_STATUS" -ne 0 ]; then
    cat "$DOXYGEN_LOG"
    echo "Error: Doxygen exited with status $DOXYGEN_STATUS"
    exit "$DOXYGEN_STATUS"
fi

# Keep successful builds readable even when Doxygen emits thousands of
# diagnostics. Failures print the complete log above.
DOXYGEN_DIAGNOSTIC_LIMIT=40
DOXYGEN_DIAGNOSTIC_COUNT="$(
    grep -Eic '(^|:[[:space:]])(warning|error):' "$DOXYGEN_LOG" || true
)"
if [ "$DOXYGEN_DIAGNOSTIC_COUNT" -gt 0 ]; then
    echo "Doxygen completed with $DOXYGEN_DIAGNOSTIC_COUNT diagnostic lines" \
         "(showing first $DOXYGEN_DIAGNOSTIC_LIMIT):"
    grep -Ei '(^|:[[:space:]])(warning|error):' "$DOXYGEN_LOG" \
        | sed -n "1,${DOXYGEN_DIAGNOSTIC_LIMIT}p" || true
    if [ "$DOXYGEN_DIAGNOSTIC_COUNT" -gt "$DOXYGEN_DIAGNOSTIC_LIMIT" ]; then
        echo "... $((DOXYGEN_DIAGNOSTIC_COUNT - DOXYGEN_DIAGNOSTIC_LIMIT))" \
             "additional diagnostic lines omitted"
    fi
fi

# Validate the staged result before replacing the last known-good tree.
if [ -d "$STAGING_OUTPUT/html" ]; then
    page_count=$(find "$STAGING_OUTPUT/html" -name "*.html" | wc -l | tr -d ' ')
    if [ "$page_count" -eq 0 ]; then
        echo "Error: Doxygen generated no HTML pages"
        exit 1
    fi
    for expected_symbol in DocumentSession Project MasterTransport PlaybackProgramCompiler \
                           SequenceProcessor ExportPlan Scale SequencerUiHost ArrangerView \
                           PianoRollView; do
        if ! grep -R -q "$expected_symbol" "$STAGING_OUTPUT/html"; then
            echo "Error: generated API reference is missing $expected_symbol"
            exit 1
        fi
    done
else
    echo "Error: no output generated"
    exit 1
fi

# A deterministic backup makes a process killed between the two renames
# recoverable by the next publisher. The checker owns the cross-platform lock,
# path-type validation, recovery, and atomic directory replacement.
python3 "$ROOT/tools/scripts/timeline_api_docs_check.py" \
    --publish-staging "$STAGING_OUTPUT" \
    --publish-output "$OUTPUT" \
    --publish-lock "$PUBLISH_LOCK"
echo "Generated $page_count HTML pages in $OUTPUT/html/"
