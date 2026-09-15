#!/usr/bin/env bash
# Self-tests for format_changed.sh.
#
# Each case builds a throwaway git repo and a FAKE clang-format that marks the
# line ranges it was asked to format, so the assertions are about the wrapper's
# plumbing — which files it picks, which `--lines=` ranges it derives from the
# hunks, and that untouched lines are never handed to the formatter — and never
# about which real clang-format this machine happens to have.
#
#   tools/scripts/test_format_changed.sh
#
# Runs under bash 3.2 (macOS default) — no mapfile, no associative arrays.

set -uo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/format_changed.sh"
[ -x "$SCRIPT" ] || { echo "not executable: $SCRIPT" >&2; exit 1; }

pass=0
fail=0

# A clang-format stand-in. Prefixes every line inside the requested
# `--lines=a:b` ranges (all lines when none are given) with `F:`, honours -i,
# logs its argv, and reports the version FAKE_MAJOR (default 21).
make_fake() {
    local dir="$1"
    mkdir -p "$dir"
    cat > "$dir/clang-format" <<'EOF'
#!/usr/bin/env bash
log="${FAKE_LOG:-/dev/null}"
echo "$@" >> "$log"
if [ "${1:-}" = "--version" ]; then
    echo "clang-format version ${FAKE_MAJOR:-21}.0.0 (fake)"
    exit 0
fi
inplace=0; ranges=""; file=""
for a in "$@"; do
    case "$a" in
        -i) inplace=1 ;;
        --lines=*) ranges="$ranges ${a#--lines=}" ;;
        --style=*) ;;
        *) file="$a" ;;
    esac
done
out="$(awk -v R="$ranges" '
    BEGIN { n = split(R, parts, " "); for (i = 1; i <= n; i++) { split(parts[i], ab, ":"); lo[i] = ab[1]; hi[i] = ab[2] } }
    {
        hit = (n == 0)
        for (i = 1; i <= n; i++) if (NR >= lo[i] && NR <= hi[i]) hit = 1
        if (hit && $0 !~ /^F:/) print "F:" $0; else print
    }' "$file")"
if [ "$inplace" -eq 1 ]; then printf '%s\n' "$out" > "$file"; else printf '%s\n' "$out"; fi
EOF
    chmod +x "$dir/clang-format"
}

# Build a repo on branch `work` whose `main` holds a 5-line C++ file plus
# `.clang-format`. Echoes the repo path.
make_repo() {
    local d
    d="$(mktemp -d)"
    git -C "$d" init -q -b main
    git -C "$d" config user.email t@example.com
    git -C "$d" config user.name test
    echo "BasedOnStyle: LLVM" > "$d/.clang-format"
    mkdir -p "$d/core" "$d/external/vendor"
    printf 'line1\nline2\nline3\nline4\nline5\n' > "$d/core/a.cpp"
    printf 'v1\nv2\n' > "$d/external/vendor/v.cpp"
    echo "seed" > "$d/README.md"
    git -C "$d" add .clang-format core external README.md >/dev/null
    git -C "$d" commit -qm seed
    git -C "$d" checkout -q -b work
    echo "$d"
}

# run <repo> <fake-dir> [args...]  → stdout+stderr in $out, exit in $rc
run() {
    local repo="$1" fake="$2"; shift 2
    out="$(cd "$repo" && PATH=/usr/bin:/bin FAKE_LOG="$repo/fake.log" PULP_CLANG_FORMAT="$fake/clang-format" \
        PULP_CLANG_FORMAT_CANDIDATES="" /bin/bash "$SCRIPT" --base main "$@" 2>&1)"
    rc=$?
}

ok()   { printf '  ok    %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  FAIL  %s\n        %s\n' "$1" "$2"; printf '%s\n' "${out:-}" | sed 's/^/        | /'; fail=$((fail + 1)); }

expect_rc() { # <name> <want>
    if [ "$rc" -eq "$2" ]; then ok "$1"; else bad "$1" "expected exit $2, got $rc"; fi
}
expect_out() { # <name> <needle>
    if grep -Fq -- "$2" <<<"$out"; then ok "$1"; else bad "$1" "output missing: $2"; fi
}
expect_no_out() { # <name> <needle>
    if grep -Fq -- "$2" <<<"$out"; then bad "$1" "output must not contain: $2"; else ok "$1"; fi
}

echo "format_changed.sh self-tests"

# ── no binary anywhere → exit 3 with install guidance ───────────────────────
repo="$(make_repo)"
out="$(cd "$repo" && PATH=/usr/bin:/bin PULP_CLANG_FORMAT="" PULP_CLANG_FORMAT_CANDIDATES="" \
    /bin/bash "$SCRIPT" --base main 2>&1)"; rc=$?
expect_rc "no clang-format → exit 3" 3
expect_out "no clang-format → names the pinned major" "pinned major 21"
expect_out "no clang-format → labelled INFRASTRUCTURE, not a formatting verdict" "INFRASTRUCTURE: no clang-format found"
rm -rf "$repo"

# ── nothing changed → exit 0, nothing invoked ───────────────────────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
run "$repo" "$fake" --check
expect_rc "clean tree → exit 0" 0
expect_out "clean tree → says nothing to format" "nothing to format"
rm -rf "$repo"

# ── one changed line: only that line is judged (the whole point) ────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
printf 'line1\nline2\nCHANGED\nline4\nline5\n' > "$repo/core/a.cpp"
run "$repo" "$fake" --check
expect_rc "--check with a dirty changed line → exit 1" 1
expect_out "--check diff covers the changed line" "+F:CHANGED"
expect_no_out "--check diff never touches an untouched line" "+F:line1"
if grep -q -- '--lines=3:3' "$repo/fake.log"; then ok "hunk → --lines=3:3"; else bad "hunk → --lines=3:3" "log: $(cat "$repo/fake.log")"; fi
run "$repo" "$fake"
expect_rc "rewrite → exit 0" 0
if [ "$(cat "$repo/core/a.cpp")" = "$(printf 'line1\nline2\nF:CHANGED\nline4\nline5')" ]; then
    ok "rewrite formats the changed line and leaves the other four byte-identical"
else
    bad "rewrite formats the changed line and leaves the other four byte-identical" "$(cat "$repo/core/a.cpp")"
fi
rm -rf "$repo"

# ── a multi-line hunk maps to an inclusive range ────────────────────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
printf 'line1\nA\nB\nC\nline5\n' > "$repo/core/a.cpp"
run "$repo" "$fake" --check
if grep -q -- '--lines=2:4' "$repo/fake.log"; then ok "3-line hunk → --lines=2:4"; else bad "3-line hunk → --lines=2:4" "log: $(cat "$repo/fake.log")"; fi
expect_no_out "3-line hunk leaves line5 alone" "+F:line5"
rm -rf "$repo"

# ── two hunks in one file → two ranges, both in one invocation ──────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
printf 'X\nline2\nline3\nline4\nY\n' > "$repo/core/a.cpp"
run "$repo" "$fake" --check
if grep -q -- '--lines=1:1 --lines=5:5' "$repo/fake.log"; then ok "two hunks → two --lines on one call"; else bad "two hunks → two --lines on one call" "log: $(cat "$repo/fake.log")"; fi
expect_no_out "two hunks leave the middle alone" "+F:line3"
rm -rf "$repo"

# ── commits since base count, not only the working tree ────────────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
printf 'line1\nline2\nline3\nline4\nCOMMITTED\n' > "$repo/core/a.cpp"
git -C "$repo" commit -qam "touch line 5"
run "$repo" "$fake" --check
expect_rc "committed-since-base change → still judged" 1
if grep -q -- '--lines=5:5' "$repo/fake.log"; then ok "committed hunk → --lines=5:5"; else bad "committed hunk → --lines=5:5" "log: $(cat "$repo/fake.log")"; fi
rm -rf "$repo"

# ── an untracked new file is formatted whole ────────────────────────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
printf 'n1\nn2\n' > "$repo/core/new.cpp"
run "$repo" "$fake" --check
expect_rc "new file → judged" 1
expect_out "new file → whole file formatted (last line)" "+F:n2"
if grep -q -- '--lines=' "$repo/fake.log"; then bad "new file → no --lines" "log: $(cat "$repo/fake.log")"; else ok "new file → no --lines"; fi
rm -rf "$repo"

# ── vendored and non-C++ changes are ignored ────────────────────────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
printf 'v1\nVCHANGED\n' > "$repo/external/vendor/v.cpp"
echo "doc change" >> "$repo/README.md"
printf 'p1\n' > "$repo/core/notes.py"
run "$repo" "$fake" --check
expect_rc "external/ + .md + .py changes → nothing to format" 0
expect_out "external/ + .md + .py changes → says so" "nothing to format"
rm -rf "$repo"

# ── another major warns but still runs ──────────────────────────────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
printf 'line1\nline2\nCHANGED\nline4\nline5\n' > "$repo/core/a.cpp"
out="$(cd "$repo" && PATH=/usr/bin:/bin FAKE_MAJOR=19 FAKE_LOG="$repo/fake.log" PULP_CLANG_FORMAT="$fake/clang-format" \
    PULP_CLANG_FORMAT_CANDIDATES="" /bin/bash "$SCRIPT" --base main --check 2>&1)"; rc=$?
expect_rc "clang-format 19 → still runs (exit 1 on the dirty line)" 1
expect_out "clang-format 19 → warns about the pinned major" "expected clang-format 21"
rm -rf "$repo"

# ── path restriction ────────────────────────────────────────────────────────
repo="$(make_repo)"; fake="$repo/fake"; make_fake "$fake"
printf 'line1\nline2\nCHANGED\nline4\nline5\n' > "$repo/core/a.cpp"
printf 'o1\n' > "$repo/core/other.cpp"
run "$repo" "$fake" --check -- core/other.cpp
expect_no_out "path restriction → a.cpp not judged" "+F:CHANGED"
expect_out "path restriction → other.cpp judged" "+F:o1"
rm -rf "$repo"

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
