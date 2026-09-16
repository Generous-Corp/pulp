#!/usr/bin/env bash
# Report whether Codex has FINISHED reviewing a pull request's current head.
#
# Three states get conflated if you read this carelessly, and each needs a
# different response:
#   * a review completed for the commit you care about;
#   * a review was acknowledged and has not finished (or never will); and
#   * nothing was ever asked.
#
# Acknowledgement is not completion. Codex posts its review-summary comment and
# reacts with EYES the moment a review is requested, before it knows what it can
# do, so the comment's existence proves nothing. Completion shows in that same
# comment's status cell reaching **Completed**.
#
# Completion is also per-commit. The summary names the commit it reviewed, so a
# head SHA passed as the third argument is required to appear alongside
# **Completed**; without that binding, a review of an earlier push would answer
# for code nobody has looked at. A clean review submits no review object at all
# (only this comment and a THUMBS_UP), so the comment is the one signal that
# covers both outcomes and can still be tied to a commit.
#
# THUMBS_UP is reported rather than required: it separates "reviewed, no
# findings" from "reviewed, left comments", which is worth printing, but it
# carries no commit and so cannot prove anything about a particular head.
#
# Exits 0 when a review completed, 1 when none has, and 2 on any `gh` failure —
# an unreachable API must never be indistinguishable from an unreviewed PR.
#
# Usage: codex_review_signal.sh <owner/repo> <pr-number> [head-sha]

set -euo pipefail

repo="${1:?usage: codex_review_signal.sh <owner/repo> <pr-number> [head-sha]}"
pr="${2:?usage: codex_review_signal.sh <owner/repo> <pr-number> [head-sha]}"
head_sha="${3:-}"

codex_login="${CODEX_LOGIN:-chatgpt-codex-connector[bot]}"
completed_marker="${COMPLETED_MARKER:-**Completed**}"

if ! comments=$(gh api "/repos/${repo}/issues/${pr}/comments" --paginate \
        --jq ".[] | select(.user.login == \"${codex_login}\") | .body | gsub(\"\\n\"; \" \")"); then
    echo "codex-review-signal: cannot read comments on ${repo}#${pr}" >&2
    exit 2
fi

# The summary renders the commit as a 7-character short SHA in backticks.
short_sha=""
[ -n "$head_sha" ] && short_sha="${head_sha:0:7}"

completed=""
while IFS= read -r body; do
    case "$body" in
        *"$completed_marker"*) ;;
        *) continue ;;
    esac
    if [ -n "$short_sha" ]; then
        case "$body" in
            *"\`${short_sha}\`"*) ;;
            *) continue ;;
        esac
    fi
    completed="yes"
    break
done <<EOF
$comments
EOF

if [ -n "$completed" ]; then
    verdict="left comments"
    if reactions=$(gh api "/repos/${repo}/issues/${pr}/reactions" --paginate \
            --jq ".[] | select(.user.login == \"${codex_login}\") | .content"); then
        printf '%s' "$reactions" | grep -qx '+1' && verdict="no findings"
    fi
    if [ -n "$short_sha" ]; then
        echo "codex-review-signal: review complete for ${short_sha} on ${repo}#${pr} (${verdict})"
    else
        echo "codex-review-signal: review complete on ${repo}#${pr} (${verdict})"
    fi
    exit 0
fi

# Not complete. Say which kind of not-complete, because only one is worth
# waiting on: a request in flight, versus a PR nobody ever asked about.
if ! reactions=$(gh api "/repos/${repo}/issues/${pr}/reactions" --paginate \
        --jq ".[] | select(.user.login == \"${codex_login}\") | .content"); then
    echo "codex-review-signal: cannot read reactions on ${repo}#${pr}" >&2
    exit 2
fi

if printf '%s' "$reactions" | grep -qx 'eyes' || [ -n "$comments" ]; then
    echo "codex-review-signal: review requested but not complete on ${repo}#${pr}"
else
    echo "codex-review-signal: no Codex signal on ${repo}#${pr}"
fi
exit 1
