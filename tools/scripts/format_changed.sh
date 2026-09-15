#!/usr/bin/env bash
# Diff-scoped clang-format: format (or check) ONLY the lines you changed.
#
# The committed tree does not round-trip under `.clang-format` — 85% of files
# (3,743 of 4,415 as of 2026-09-14) reflow under every clang-format 19/21
# binary, because the config was authored without ever being applied. A
# whole-file `clang-format -i` on a file you touched therefore rewrites
# hundreds of lines nobody asked you to change, and nothing in CI runs
# clang-format at all, so there is no gate to tell you which side is right.
#
# This wrapper is the honest middle: it reads the hunks that differ from a
# base ref (committed since the base AND uncommitted, plus untracked new
# files) and passes them to clang-format as `--lines=` ranges, so untouched
# lines are never judged and never rewritten. New files are formatted whole.
#
#   tools/scripts/format_changed.sh                 # rewrite changed lines vs origin/main
#   tools/scripts/format_changed.sh --check         # print the diff, exit 1 if any
#   tools/scripts/format_changed.sh --base HEAD~1   # different base
#   tools/scripts/format_changed.sh -- core/x.cpp   # restrict to paths
#
# Binary resolution, first hit wins: --binary, $PULP_CLANG_FORMAT, `clang-format`
# on PATH, then the toolchains that ship one on a Mac (Homebrew llvm@21, the
# selected Xcode toolchain, CommandLineTools) and /usr/lib/llvm-21 on Linux.
# The pinned major is 21: every macOS toolchain in 2026 ships LLVM 21, and the
# three local 21.x binaries were measured byte-identical over the whole tree
# (clang-format 19 differed on 2 of 4,415 files). Another major warns; it does
# not fail, because no gate depends on this and a warning beats hand-formatting.
#
# Exit: 0 clean / rewritten · 1 --check found changes · 2 usage or repo error ·
#       3 no clang-format found.
#
# Runs under bash 3.2 (macOS default) — no mapfile, no associative arrays.

set -uo pipefail

PULP_CLANG_FORMAT_MAJOR="${PULP_CLANG_FORMAT_MAJOR:-21}"

check=0
base=""
binary="${PULP_CLANG_FORMAT:-}"
paths=""

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --check) check=1 ;;
        --base) shift; [ $# -gt 0 ] || { echo "format_changed: --base needs a ref" >&2; exit 2; }; base="$1" ;;
        --base=*) base="${1#--base=}" ;;
        --binary) shift; [ $# -gt 0 ] || { echo "format_changed: --binary needs a path" >&2; exit 2; }; binary="$1" ;;
        --binary=*) binary="${1#--binary=}" ;;
        -h|--help) usage; exit 0 ;;
        --) shift; paths="$*"; break ;;
        -*) echo "format_changed: unknown flag: $1" >&2; usage >&2; exit 2 ;;
        *) paths="$paths $1" ;;
    esac
    shift
done

root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "format_changed: not inside a git repository" >&2
    exit 2
}
cd "$root" || exit 2
[ -f .clang-format ] || {
    echo "format_changed: no .clang-format at $root" >&2
    exit 2
}

# ── Resolve the binary ──────────────────────────────────────────────────────
# PULP_CLANG_FORMAT_CANDIDATES (colon-separated dirs) overrides the built-in
# fallback list; set it empty to disable fallbacks (the self-test does).
resolve_binary() {
    if [ -n "$binary" ]; then
        [ -x "$binary" ] && { echo "$binary"; return 0; }
        echo "format_changed: clang-format not executable: $binary" >&2
        return 1
    fi
    if command -v clang-format >/dev/null 2>&1; then
        command -v clang-format
        return 0
    fi
    local dirs d
    if [ "${PULP_CLANG_FORMAT_CANDIDATES+set}" = set ]; then
        dirs="$PULP_CLANG_FORMAT_CANDIDATES"
    else
        dirs="/opt/homebrew/opt/llvm@${PULP_CLANG_FORMAT_MAJOR}/bin:/opt/homebrew/opt/llvm/bin"
        if command -v xcode-select >/dev/null 2>&1; then
            d="$(xcode-select -p 2>/dev/null || true)"
            [ -n "$d" ] && dirs="$dirs:$d/Toolchains/XcodeDefault.xctoolchain/usr/bin:$d/usr/bin"
        fi
        dirs="$dirs:/Library/Developer/CommandLineTools/usr/bin"
        dirs="$dirs:/usr/lib/llvm-${PULP_CLANG_FORMAT_MAJOR}/bin:/usr/local/opt/llvm@${PULP_CLANG_FORMAT_MAJOR}/bin"
    fi
    local IFS=:
    for d in $dirs; do
        [ -n "$d" ] && [ -x "$d/clang-format" ] && { echo "$d/clang-format"; return 0; }
    done
    return 1
}

bin="$(resolve_binary)" || {
    cat >&2 <<EOF
format_changed: no clang-format found.
  Install one (pinned major ${PULP_CLANG_FORMAT_MAJOR}):
    macOS:  brew install llvm@${PULP_CLANG_FORMAT_MAJOR}   (or: xcode-select --install)
    Linux:  apt install clang-format-${PULP_CLANG_FORMAT_MAJOR}
  or point PULP_CLANG_FORMAT / --binary at one.
EOF
    exit 3
}

version_line="$("$bin" --version 2>/dev/null | head -1)"
major="$(printf '%s\n' "$version_line" | sed -n 's/.*clang-format version \([0-9][0-9]*\)\..*/\1/p')"
if [ -z "$major" ]; then
    echo "format_changed: could not parse a version from: $bin ($version_line)" >&2
elif [ "$major" != "$PULP_CLANG_FORMAT_MAJOR" ]; then
    echo "format_changed: WARNING: $bin is clang-format $major; expected clang-format ${PULP_CLANG_FORMAT_MAJOR} (output may differ on a few files)" >&2
fi
echo "format_changed: using $bin ($version_line)" >&2

# ── Pick the base ───────────────────────────────────────────────────────────
if [ -z "$base" ]; then
    if git rev-parse --verify -q origin/main >/dev/null; then
        base=origin/main
    else
        base=HEAD
    fi
fi
git rev-parse --verify -q "$base^{commit}" >/dev/null || {
    echo "format_changed: unknown base ref: $base" >&2
    exit 2
}
echo "format_changed: base $base" >&2

# ── Collect candidate files ─────────────────────────────────────────────────
is_source() {
    case "$1" in
        *.cpp|*.hpp|*.h|*.mm|*.cc|*.cxx) ;;
        *) return 1 ;;
    esac
    case "/$1" in
        /external/*|*/external/*|*/build/*|*/_deps/*|*/generated/*) return 1 ;;
    esac
    return 0
}

# shellcheck disable=SC2086  # $paths is a deliberate word list
changed="$( { git diff --name-only --diff-filter=ACMR "$base" -- $paths; git ls-files --others --exclude-standard -- $paths; } | sort -u )"
untracked="$(git ls-files --others --exclude-standard -- $paths)"

is_untracked() {
    printf '%s\n' "$untracked" | grep -Fxq -- "$1"
}

# Lines of `--lines=a:b` for the hunks of $1 that differ from base.
hunk_ranges() {
    git diff -U0 "$base" -- "$1" | sed -n 's/^@@ -[0-9][0-9,]* +\([0-9][0-9,]*\) @@.*/\1/p' | while IFS= read -r range; do
        start="${range%%,*}"
        if [ "$range" = "$start" ]; then
            count=1
        else
            count="${range#*,}"
        fi
        [ "$count" -gt 0 ] || continue
        end=$((start + count - 1))
        printf -- '--lines=%s:%s\n' "$start" "$end"
    done
}

dirty=0
touched=0
printf '%s\n' "$changed" | while IFS= read -r f; do
    [ -n "$f" ] || continue
    is_source "$f" || continue
    [ -f "$f" ] || continue
    if is_untracked "$f" || ! git cat-file -e "$base:$f" 2>/dev/null; then
        ranges=""
    else
        ranges="$(hunk_ranges "$f")"
        [ -n "$ranges" ] || continue
    fi
    # shellcheck disable=SC2086  # ranges are one --lines= per word
    if [ "$check" -eq 1 ]; then
        if ! "$bin" --style=file $ranges "$f" | diff -u --label "a/$f" --label "b/$f" "$f" - ; then
            echo "DIRTY $f"
        fi
        echo "TOUCHED $f"
    else
        "$bin" --style=file -i $ranges "$f" || echo "FAILED $f"
        echo "TOUCHED $f"
    fi
done > "${TMPDIR:-/tmp}/format_changed.$$" 2>&1
rc=$?
report="${TMPDIR:-/tmp}/format_changed.$$"

# The loop ran in a subshell (pipe), so tally from its report rather than from
# variables it could not hand back.
touched=$(grep -c '^TOUCHED ' "$report")
dirty=$(grep -c '^DIRTY ' "$report")
failed=$(grep -c '^FAILED ' "$report")
grep -v -E '^(TOUCHED|DIRTY|FAILED) ' "$report"
rm -f "$report"

if [ "$touched" -eq 0 ]; then
    echo "format_changed: no changed C++ lines vs $base — nothing to format." >&2
    exit 0
fi
if [ "$failed" -gt 0 ]; then
    echo "format_changed: clang-format failed on $failed file(s)" >&2
    exit 2
fi
if [ "$check" -eq 1 ]; then
    if [ "$dirty" -gt 0 ]; then
        echo "format_changed: $dirty of $touched changed file(s) need formatting on the lines you touched. Run tools/scripts/format_changed.sh to fix." >&2
        exit 1
    fi
    echo "format_changed: $touched changed file(s) clean on touched lines." >&2
    exit 0
fi
echo "format_changed: formatted changed lines in $touched file(s)." >&2
exit "$rc"
