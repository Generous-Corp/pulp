#!/usr/bin/env bash
# Verdict for the `macos` merge-group bootstrap job (build.yml
# `macos-merge-group`), the job that owns the required `macos` check when the
# native macOS leg is absent.
#
# Inputs (the `needs.*` values the workflow passes as env):
#   PROVIDER_RESULT    needs.resolve-provider.result
#   CLASSIFY_RESULT    needs.classify.result
#   NATIVE_REQUIRED    needs.classify.outputs.native_build_required
#   RECEIPT_RESULT     needs.protected-receipt-reuse.result
#   RECEIPT_REUSED     needs.protected-receipt-reuse.outputs.macos_reused
#
# A dependency that FAILED, was skipped, or has no result fails the gate
# closed. A CANCELLED preamble is different: GitHub cancels a job it never
# assigned a runner to, and failing closed on that turned a hosted-runner
# outage into a red required check, so the queue ejected the PR, re-batched,
# and met the same wait. This job already has a runner, so it classifies the
# merge group itself (classify_in_job, the same classifier scripts the classify
# job runs) and proceeds on that answer:
#   - nothing native to build  -> the skip-safe pass, as if classify had said so
#   - native build required    -> fails closed: the native leg and the receipt
#                                  reuse both wait on classify, so neither ran
#   - the classifier fails     -> fails closed
# A cancelled resolve-provider only decides how the native leg would run, so it
# does not block a group with nothing native to build.
#
# CLASSIFY_FALLBACK_CMD replaces classify_in_job in tests; it must print
# `native_build_required=true|false` as its last line.
set -euo pipefail

provider="${PROVIDER_RESULT:-}"
classify="${CLASSIFY_RESULT:-}"
native="${NATIVE_REQUIRED:-}"

fail_closed() {
    echo "$1 — failing macos gate closed"
    exit 1
}

# The classify job's classification, run here: the receipt-change decision,
# the event-correct base, and the tree diff. The exact-generated-version-bump
# fast path only ever lowers the answer, so leaving it out is conservative.
classify_in_job() {
    export PATH="/opt/homebrew/bin:/usr/local/bin:$HOME/.local/bin:$PATH"
    local ci_python out a2t base classification required
    ci_python="$(python3 tools/ci/find_python311.py)"
    out="$(mktemp)"
    a2t="$(GITHUB_OUTPUT="$out" "$ci_python" tools/scripts/a2t_structural_verification_ci.py \
        --classify-receipt-change)"
    rm -f "$out"
    echo "$a2t" >&2
    if [[ "$a2t" == *"verify=true"* ]]; then
        echo "native_build_required=true"
        return
    fi
    base="$("$ci_python" tools/scripts/resolve_classify_base.py)"
    if ! [[ "$base" =~ ^[0-9a-fA-F]{40}$ ]]; then
        echo "no immutable event base ($base); the native build is required" >&2
        echo "native_build_required=true"
        return
    fi
    git cat-file -e "$base^{commit}" 2>/dev/null \
        || git fetch --no-tags --depth=1 origin "$base" >&2 || true
    classification="$(env GITHUB_OUTPUT='' "$ci_python" tools/scripts/classify_changes.py \
        --mode=diff --comparison=trees --base "$base" --json)"
    required="$("$ci_python" -c \
        'import json,sys; print(str(json.load(sys.stdin)["native_build_required"]).lower())' \
        <<<"$classification")"
    echo "native_build_required=$required"
}

case "$provider" in
    success|cancelled) ;;
    *) fail_closed "provider resolution did not succeed (${provider:-no result})" ;;
esac

case "$classify" in
    success)
        if [ -z "$native" ]; then
            fallback_reason="classify succeeded but published no classification"
        fi
        ;;
    cancelled)
        fallback_reason="preamble not acquired (infrastructure): classify was cancelled"
        ;;
    *) fail_closed "classify did not succeed (${classify:-no result})" ;;
esac

if [ -n "${fallback_reason:-}" ]; then
    echo "$fallback_reason; classifying the merge group in this job"
    echo "::notice title=macos bootstrap classified in-job::$fallback_reason"
    if [ -n "${CLASSIFY_FALLBACK_CMD:-}" ]; then
        verdict="$(bash -c "$CLASSIFY_FALLBACK_CMD")" || fail_closed "the in-job classifier failed"
    else
        verdict="$(classify_in_job)" || fail_closed "the in-job classifier failed"
    fi
    case "${verdict##*$'\n'}" in
        native_build_required=false) native=false ;;
        native_build_required=true)
            fail_closed "the in-job classifier requires the native build, which never started because classify was not acquired"
            ;;
        *) fail_closed "the in-job classifier gave no classification" ;;
    esac
    echo "in-job classification: native_build_required=$native"
fi

if [ "$native" = "true" ]; then
    if [ "${RECEIPT_RESULT:-}" != "success" ] || [ "${RECEIPT_REUSED:-}" != "true" ]; then
        fail_closed "protected receipt decision unavailable"
    fi
    echo "Exact-tree protected PR receipt revalidated for this merge group — macos gate ✓"
    REASON="an exact-tree protected receipt from an earlier run was reused"
else
    echo "Skip-safe merge group — macos gate ✓ (native matrix omitted)"
    REASON="the merge group changed no native build input, so the matrix was skipped"
fi
# A green `macos` here is indistinguishable from a real one in the
# checks list, and reasoning from it as though the suite passed has
# already cost hours. Say so in the job's own output.
{
    echo "### ⚠️ macos gate passed WITHOUT running the test suite"
    echo
    echo "This check is green because **$REASON**, not because any test"
    echo "executed. No ctest run happened in this job — it has no build"
    echo "and no test step. Do not read it as evidence that the suite"
    echo "passes on this tree."
    echo
    echo "To find a run where the suite actually executed:"
    echo '```'
    echo "python3 tools/scripts/gate_suite_executed.py --run-id <run-id>"
    echo '```'
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
echo "::notice title=macos gate did not run the suite::$REASON"
if [ "$native" = "true" ]; then
    echo '::notice title=shipyard-test-tier::{"schema":"shipyard-test-tier/v1","tier":"receipt-reused"}'
else
    echo '::notice title=shipyard-test-tier::{"schema":"shipyard-test-tier/v1","tier":"not-required"}'
fi
