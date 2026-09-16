#!/usr/bin/env bash
# Self-tests for codex_review_signal.sh.
#
# Each case stubs `gh` on PATH so the assertions are about the script's own
# discrimination and never about the network or a live pull request. What it has
# to keep apart are states that look alike from outside: a review that completed
# for THIS commit, one that completed for an earlier one, one that was merely
# acknowledged, one nobody asked for, and an API that could not be read.
#
#   tools/scripts/test_codex_review_signal.sh
#
# Runs under bash 3.2 (macOS default) — no mapfile, no associative arrays.

set -uo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/codex_review_signal.sh"
[ -x "$SCRIPT" ] || { echo "not executable: $SCRIPT" >&2; exit 1; }

HEAD=abc1234def5678
OTHER=9999999aaaa

summary() { # summary <status> <short-sha>
    # Single-quoted on purpose: the backticks are literal, matching how Codex
    # renders the commit cell.
    # shellcheck disable=SC2016
    printf '<!-- codex-pull-request-review-summary --> | Code Review | %s | `%s` | Manual request |' "$1" "$2"
}

pass=0
fail=0

# make_gh <comments-body> <reactions-body> [comments-rc] [reactions-rc]
make_gh() {
    local d
    d="$(mktemp -d)"
    printf '%s' "$1" > "$d/comments.txt"
    printf '%s' "$2" > "$d/reactions.txt"
    cat > "$d/gh" <<STUB
#!/usr/bin/env bash
for a in "\$@"; do
    case "\$a" in
        */comments) rc=${3:-0}; body="$d/comments.txt" ;;
        */reactions) rc=${4:-0}; body="$d/reactions.txt" ;;
    esac
done
[ "\${rc:-0}" -eq 0 ] || exit "\$rc"
cat "\$body"
STUB
    chmod +x "$d/gh"
    echo "$d"
}

# check <name> <expected-exit> <expected-substring> <stub-dir> [head-sha]
check() {
    local name="$1" want="$2" needle="$3" stub="$4" sha="${5:-}" out rc
    # Unquoted on purpose: an empty sha must pass NO third argument, which is
    # what exercises the PR-wide fallback. Quoting would pass an empty one.
    # shellcheck disable=SC2086
    out="$(PATH="$stub:$PATH" /bin/bash "$SCRIPT" owner/repo 1 $sha 2>&1)"
    rc=$?
    if [ "$rc" -ne "$want" ]; then
        printf '  FAIL  %s\n        expected exit %s, got %s\n        output: %s\n' \
            "$name" "$want" "$rc" "$out"
        fail=$((fail + 1)); return
    fi
    case "$out" in
        *"$needle"*) ;;
        *)
            printf '  FAIL  %s\n        expected output to contain: %s\n        output: %s\n' \
                "$name" "$needle" "$out"
            fail=$((fail + 1)); return
            ;;
    esac
    printf '  ok    %s\n' "$name"
    pass=$((pass + 1))
}

stub="$(make_gh "$(summary '**Completed**' "${HEAD:0:7}")" "")"
check "completed review for this head is reviewed" 0 "review complete for ${HEAD:0:7}" "$stub" "$HEAD"

# THUMBS_UP is what separates "reviewed, nothing to say" from "reviewed, left
# comments". It is reported, not required — it carries no commit.
stub="$(make_gh "$(summary '**Completed**' "${HEAD:0:7}")" "+1")"
check "thumbs-up is reported as no findings" 0 "no findings" "$stub" "$HEAD"

stub="$(make_gh "$(summary '**Completed**' "${HEAD:0:7}")" "")"
check "absent thumbs-up is reported as comments" 0 "left comments" "$stub" "$HEAD"

# A review of an earlier push must not answer for the current head, or a PR can
# pass while its newest commits have been seen by nobody.
stub="$(make_gh "$(summary '**Completed**' "${OTHER:0:7}")" "+1")"
check "completed review for another head does not count" 1 "requested but not complete" "$stub" "$HEAD"

# Acknowledgement is not completion: the summary comment and the EYES reaction
# both appear the instant a review is requested.
stub="$(make_gh "$(summary '**Running**' "${HEAD:0:7}")" "eyes")"
check "acknowledged-but-running is not reviewed" 1 "requested but not complete" "$stub" "$HEAD"

# A running review and an unasked PR are both unreviewed, but only one is worth
# waiting on, so they must not report the same thing.
stub="$(make_gh "" "")"
check "never asked reads differently from running" 1 "no Codex signal" "$stub" "$HEAD"

# Without a head SHA the check is PR-wide, which is weaker but still honest.
stub="$(make_gh "$(summary '**Completed**' "${OTHER:0:7}")" "")"
check "no head argument falls back to PR-wide" 0 "review complete on" "$stub"

# An unreachable API is an error, never a silent "unreviewed": conflating the
# two would let an outage masquerade as a finding.
stub="$(make_gh "" "" 1 0)"
check "unreadable comments is an error" 2 "cannot read comments" "$stub" "$HEAD"

stub="$(make_gh "" "" 0 1)"
check "unreadable reactions is an error" 2 "cannot read reactions" "$stub" "$HEAD"

# Once completion is established the verdict is a nicety, so a reactions failure
# must not turn a proven review into an error.
stub="$(make_gh "$(summary '**Completed**' "${HEAD:0:7}")" "" 0 1)"
check "reactions failure cannot unprove a completed review" 0 "review complete for" "$stub" "$HEAD"

if grep -q 'select(.user.login == ' "$SCRIPT"; then
    printf '  ok    %s\n' "queries filter on the Codex login"
    pass=$((pass + 1))
else
    printf '  FAIL  %s\n' "queries filter on the Codex login"
    fail=$((fail + 1))
fi

printf '\ncodex-review-signal selftest: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
