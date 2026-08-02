#!/usr/bin/env bash
#
# Exercise confirm_failure.sh against a real, tiny CMake project.
#
# A gate script that is never executed is not a gate, so these build and run
# genuine binaries rather than asserting on the script's text. Each case sets up
# a throwaway git repo so the script's git-based restore is the real one.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNDER_TEST="$SCRIPT_DIR/confirm_failure.sh"

FAILURES=0
check() {
    local what="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        printf '  ok   %s\n' "$what"
    else
        printf '  FAIL %s (expected %s, got %s)\n' "$what" "$expected" "$actual"
        FAILURES=$((FAILURES + 1))
    fi
}

# A project whose test either does or does not depend on the value it checks.
make_project() {
    local root="$1" meaningful="$2"
    mkdir -p "$root"
    cat > "$root/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(confirm_failure_fixture CXX)
set(CMAKE_CXX_STANDARD 17)
add_library(fixture_lib value.cpp)
add_executable(fixture_test test.cpp)
target_link_libraries(fixture_test PRIVATE fixture_lib)
EOF
    cat > "$root/value.hpp" <<'EOF'
int answer();
EOF
    cat > "$root/value.cpp" <<'EOF'
#include "value.hpp"
int answer() { return 42; }
EOF
    if [ "$meaningful" = "meaningful" ]; then
        cat > "$root/test.cpp" <<'EOF'
#include "value.hpp"
int main() { return answer() == 42 ? 0 : 1; }
EOF
    else
        # Passes no matter what answer() returns — the shape of a test that
        # cannot fail, which is exactly what this script exists to expose.
        cat > "$root/test.cpp" <<'EOF'
#include "value.hpp"
int main() { (void)answer(); return 0; }
EOF
    fi
    ( cd "$root" \
      && git init -q . \
      && git config user.email t@example.com \
      && git config user.name test \
      && git add -A \
      && git commit -qm fixture ) >/dev/null 2>&1
    cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
}

run_under_test() {
    local root="$1"
    ( cd "$root" && "$UNDER_TEST" \
        --file value.cpp \
        --break "perl -pi -e 's/return 42;/return 7;/'" \
        --build-dir build \
        --target fixture_test \
        --test ./build/fixture_test \
        --jobs 2 ) >/dev/null 2>&1
    echo $?
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "confirm_failure.sh"

# A test that genuinely depends on the value: breaking it must be caught.
make_project "$TMP/covered" meaningful
check "a covering test is CONFIRMED" 0 "$(run_under_test "$TMP/covered")"

# A test that cannot fail: the script must say so rather than bless it.
make_project "$TMP/uncovered" vacuous
check "a vacuous test is NOT CONFIRMED" 1 "$(run_under_test "$TMP/uncovered")"

# A dirty file cannot be restored exactly, so the loop must refuse to run.
make_project "$TMP/dirty" meaningful
echo "// uncommitted" >> "$TMP/dirty/value.cpp"
check "a dirty file is INCONCLUSIVE" 2 "$(run_under_test "$TMP/dirty")"

# A break pattern that matches nothing would otherwise look like a passing
# control, since nothing changed and the test still passes.
make_project "$TMP/nomatch" meaningful
NOMATCH=$( ( cd "$TMP/nomatch" && "$UNDER_TEST" \
    --file value.cpp \
    --break "perl -pi -e 's/no_such_text/x/'" \
    --build-dir build --target fixture_test --test ./build/fixture_test --jobs 2 \
  ) >/dev/null 2>&1; echo $? )
check "a break that changes nothing is INCONCLUSIVE" 2 "$NOMATCH"

# The tree must be left exactly as it was found, whatever the verdict.
if git -C "$TMP/uncovered" diff --quiet; then
    printf '  ok   the tree is restored after a NOT CONFIRMED run\n'
else
    printf '  FAIL the tree is left dirty after a NOT CONFIRMED run\n'
    FAILURES=$((FAILURES + 1))
fi

if [ "$FAILURES" -eq 0 ]; then
    echo "all confirm_failure.sh cases passed"
    exit 0
fi
echo "$FAILURES case(s) failed"
exit 1
