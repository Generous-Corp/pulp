#!/usr/bin/env bash
# Self-tests for codex_review_signal.sh.
#
# Each case stubs `gh` on PATH so the assertions are about the script's own
# discrimination and never about the network or a live pull request. The cases
# that matter most are the two that look alike from the outside: a clean review
# (reaction only, no comment) must read as reviewed, and an unreachable API must
# NOT read as unreviewed.
#
#   tools/scripts/test_codex_review_signal.sh
#
# Runs under bash 3.2 (macOS default) — no mapfile, no associative arrays.

set -uo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/codex_review_signal.sh"
[ -x "$SCRIPT" ] || { echo "not executable: $SCRIPT" >&2; exit 1; }

MARKER='<!-- codex-pull-request-review-summary -->'
CODEX='chatgpt-codex-connector[bot]'

pass=0
fail=0

# make_gh <comments-body> <reactions-body> [comments-rc] [reactions-rc]
# Builds a stub `gh` that answers the two endpoints the script calls.
make_gh() {
    local d
    d="$(mktemp -d)"
    printf '%s' "$1" > "$d/comments.txt"
    printf '%s' "$2" > "$d/reactions.txt"
    cat > "$d/gh" <<STUB
#!/usr/bin/env bash
for a in "\$@"; do
    case "\$a" in
        */comments) exec_rc=${3:-0}; body="$d/comments.txt" ;;
        */reactions) exec_rc=${4:-0}; body="$d/reactions.txt" ;;
    esac
done
[ "\${exec_rc:-0}" -eq 0 ] || exit "\$exec_rc"
cat "\$body"
STUB
    chmod +x "$d/gh"
    echo "$d"
}

# check <name> <expected-exit> <expected-substring> <stub-dir>
check() {
    local name="$1" want="$2" needle="$3" stub="$4" out rc
    out="$(PATH="$stub:$PATH" /bin/bash "$SCRIPT" owner/repo 1 2>&1)"
    rc=$?
    if [ "$rc" -ne "$want" ]; then
        printf '  FAIL  %s\n        expected exit %s, got %s\n        output: %s\n' \
            "$name" "$want" "$rc" "$out"
        fail=$((fail + 1))
        return
    fi
    case "$out" in
        *"$needle"*) ;;
        *)
            printf '  FAIL  %s\n        expected output to contain: %s\n        output: %s\n' \
                "$name" "$needle" "$out"
            fail=$((fail + 1))
            return
            ;;
    esac
    printf '  ok    %s\n' "$name"
    pass=$((pass + 1))
}

stub="$(make_gh "some preamble $MARKER trailing" "")"
check "summary comment counts as reviewed" 0 "review summary comment present" "$stub"

# The clean-review case. Codex says "found nothing" with a reaction and no
# comment; reading comments alone would call this PR unreviewed.
stub="$(make_gh "" "+1")"
check "clean-review reaction counts as reviewed" 0 "clean-review reaction present" "$stub"

stub="$(make_gh "" "")"
check "no comment and no reaction is unreviewed" 1 "no Codex signal" "$stub"

# A reaction that is not the clean-review signal must not be mistaken for one.
stub="$(make_gh "" "eyes")"
check "non-thumbs-up reaction is not a review" 1 "no Codex signal" "$stub"

# `+1` must match the whole line: a reaction such as `+1000` is not it.
stub="$(make_gh "" "+1000")"
check "reaction must match exactly" 1 "no Codex signal" "$stub"

# An unreachable API is an error, never a silent "unreviewed": conflating the
# two would let an outage masquerade as a finding.
stub="$(make_gh "" "" 1 0)"
check "unreadable comments is an error" 2 "cannot read comments" "$stub"

stub="$(make_gh "" "" 0 1)"
check "unreadable reactions is an error" 2 "cannot read reactions" "$stub"

# The marker only counts from Codex itself; the stub returns bodies already
# filtered by login, so this asserts the script asks `gh` to filter by login.
if grep -q 'select(.user.login == ' "$SCRIPT"; then
    printf '  ok    %s\n' "queries filter on the Codex login"
    pass=$((pass + 1))
else
    printf '  FAIL  %s\n' "queries filter on the Codex login"
    fail=$((fail + 1))
fi

printf '\ncodex-review-signal selftest: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
