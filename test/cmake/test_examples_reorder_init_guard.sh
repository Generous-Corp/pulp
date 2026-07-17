#!/usr/bin/env bash
# Proves the example build catches an out-of-declaration-order designated
# initializer — the bug class that shipped an unpublishable SDK tag: a `.field`
# init reordered in an example compiled clean on macOS/Clang, then failed BOTH
# Linux (GCC) legs of the release build, where the required PR gate never
# compiles examples (PULP_BUILD_EXAMPLES=OFF).
#
# examples/CMakeLists.txt promotes -Wreorder-init-list to an error under Clang so
# the diagnostic fails everywhere, matching GCC (which already errors by default).
# This test proves that guard is load-bearing, on whatever compiler is present.
#
# Exit: 0 pass, 1 fail, 77 skip (no usable C++ compiler).
set -u

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
    for c in clang++ g++ c++; do command -v "$c" >/dev/null 2>&1 && { CXX="$c"; break; }; done
fi
[ -z "$CXX" ] && { echo "SKIP: no C++ compiler found"; exit 77; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# Mirrors an example's real shape: ParamInfo-like struct, brace-initialized.
cat > "$tmp/good.cpp" <<'EOF'
struct ParamInfo { int id; float range; int kind; };
ParamInfo p = { .id = 1, .range = 2, .kind = 3 };   // declaration order — OK
int main() { return p.kind - 3; }
EOF
cat > "$tmp/bad.cpp" <<'EOF'
struct ParamInfo { int id; float range; int kind; };
ParamInfo p = { .id = 1, .kind = 3, .range = 2 };   // .kind before .range — the bug
int main() { return p.kind - 3; }
EOF

is_clang=0
"$CXX" --version 2>/dev/null | grep -qi clang && is_clang=1

fail() { echo "FAIL: $1"; exit 1; }

# The correctly-ordered init must ALWAYS compile clean under the guard — no
# false positives on well-formed examples.
if [ "$is_clang" -eq 1 ]; then
    "$CXX" -std=c++20 -Werror=reorder-init-list -fsyntax-only "$tmp/good.cpp" 2>/dev/null \
        || fail "declaration-order init rejected under -Werror=reorder-init-list (false positive)"

    # Load-bearing proof: WITHOUT the guard Clang only warns and the bug ships…
    if ! "$CXX" -std=c++20 -Wall -Wextra -fsyntax-only "$tmp/bad.cpp" >/dev/null 2>&1; then
        echo "NOTE: this Clang already errors on the reordered init without the guard"
    fi
    # …WITH the guard it must be an error.
    "$CXX" -std=c++20 -Werror=reorder-init-list -fsyntax-only "$tmp/bad.cpp" >/dev/null 2>&1 \
        && fail "reordered init compiled under -Werror=reorder-init-list (guard not load-bearing)"
    echo "PASS: Clang guard promotes the reordered designated init to an error"
else
    # GCC (and other ISO-conforming front ends): the reordered init is a hard
    # error by DEFAULT — no flag needed. This is the parity the Clang guard
    # restores. Note the flag name is Clang-specific, so it is not passed to GCC.
    "$CXX" -std=c++20 -fsyntax-only "$tmp/good.cpp" 2>/dev/null \
        || fail "declaration-order init rejected by default (unexpected)"
    "$CXX" -std=c++20 -fsyntax-only "$tmp/bad.cpp" >/dev/null 2>&1 \
        && fail "reordered designated init compiled on a non-Clang compiler (expected ISO error)"
    echo "PASS: non-Clang compiler rejects the reordered designated init by default"
fi
exit 0
