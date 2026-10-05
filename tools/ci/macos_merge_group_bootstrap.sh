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
# A dependency that FAILED, or has no result, fails the gate closed. One that
# was CANCELLED (a merge group re-batched, or a runner was never acquired) is
# not a verdict about the tree: failing closed there turns infrastructure into
# a red required check, the queue ejects the PR, re-batches, and the next group
# meets the same wait. So a cancelled dependency cancels this run instead, and
# the required check reads cancelled. If the cancellation does not take hold
# within CANCEL_WAIT_SECONDS, the gate still fails closed: this script never
# exits 0 on a dependency that did not succeed.
#
# Cancelling uses POST /repos/{repo}/actions/runs/{id}/cancel with GH_TOKEN
# (the job needs `actions: write`). BOOTSTRAP_CANCEL_CMD replaces that request
# in tests.
set -euo pipefail

provider="${PROVIDER_RESULT:-}"
classify="${CLASSIFY_RESULT:-}"
native="${NATIVE_REQUIRED:-}"
wait_seconds="${CANCEL_WAIT_SECONDS:-120}"

fail_closed() {
    echo "$1 — failing macos gate closed"
    exit 1
}

request_cancel() {
    if [ -n "${BOOTSTRAP_CANCEL_CMD:-}" ]; then
        bash -c "$BOOTSTRAP_CANCEL_CMD"
        return
    fi
    curl -fsS -X POST \
        -H "Authorization: Bearer ${GH_TOKEN:?GH_TOKEN is required to cancel the run}" \
        -H "Accept: application/vnd.github+json" \
        "${GITHUB_API_URL:-https://api.github.com}/repos/${GITHUB_REPOSITORY:?}/actions/runs/${GITHUB_RUN_ID:?}/cancel" \
        >/dev/null
}

# A failure anywhere outranks a cancellation: that is a verdict. So is a
# missing or skipped result.
case "$provider" in
    success|cancelled) ;;
    *) fail_closed "provider resolution did not succeed (${provider:-no result})" ;;
esac
case "$classify" in
    success|cancelled) ;;
    *) fail_closed "classify did not succeed (${classify:-no result})" ;;
esac

if [ "$provider" = cancelled ] || [ "$classify" = cancelled ]; then
    echo "provider=$provider classify=$classify: a dependency was cancelled, not failed;"
    echo "cancelling this run so the required check reads cancelled and the queue re-batches."
    echo "::notice title=macos bootstrap cancelled its run::provider=$provider classify=$classify"
    if ! request_cancel; then
        fail_closed "could not request cancellation of run ${GITHUB_RUN_ID:-?}"
    fi
    echo "cancel requested for run ${GITHUB_RUN_ID:-?}; waiting up to ${wait_seconds}s for it to take hold"
    sleep "$wait_seconds"
    fail_closed "run was not cancelled within ${wait_seconds}s"
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
