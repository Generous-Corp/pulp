#!/usr/bin/env bash
# Report whether Codex has responded to a pull request.
#
# Codex has two distinct "I reviewed this" signals, and only checking one of
# them turns "reviewed, nothing to say" into a false "never reviewed":
#   * a summary comment carrying the codex-pull-request-review-summary marker,
#     posted whenever a review runs; and
#   * a THUMBS_UP reaction on the pull request itself, which is how it reports a
#     clean review.
#
# Exits 0 when either signal is present, 1 when neither is. Any `gh` failure is
# an error (exit 2), never a silent "no signal" — an unreachable API must not be
# indistinguishable from an unreviewed PR.
#
# Usage: codex_review_signal.sh <owner/repo> <pr-number>

set -euo pipefail

repo="${1:?usage: codex_review_signal.sh <owner/repo> <pr-number>}"
pr="${2:?usage: codex_review_signal.sh <owner/repo> <pr-number>}"

codex_login="${CODEX_LOGIN:-chatgpt-codex-connector[bot]}"
review_marker="${REVIEW_MARKER:-<!-- codex-pull-request-review-summary -->}"

if ! comments=$(gh api "/repos/${repo}/issues/${pr}/comments" --paginate \
        --jq ".[] | select(.user.login == \"${codex_login}\") | .body"); then
    echo "codex-review-signal: cannot read comments on ${repo}#${pr}" >&2
    exit 2
fi

if printf '%s' "$comments" | grep -qF "$review_marker"; then
    echo "codex-review-signal: review summary comment present on ${repo}#${pr}"
    exit 0
fi

if ! reactions=$(gh api "/repos/${repo}/issues/${pr}/reactions" --paginate \
        --jq ".[] | select(.user.login == \"${codex_login}\") | .content"); then
    echo "codex-review-signal: cannot read reactions on ${repo}#${pr}" >&2
    exit 2
fi

if printf '%s' "$reactions" | grep -qx '+1'; then
    echo "codex-review-signal: clean-review reaction present on ${repo}#${pr}"
    exit 0
fi

echo "codex-review-signal: no Codex signal on ${repo}#${pr}"
exit 1
