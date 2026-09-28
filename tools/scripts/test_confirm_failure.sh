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

# ── The --no-build lane, for a test whose subject is interpreted ────────────
#
# A Node/Python/shell test has no object or binary between the edited source and
# the run, so the compiled lane's build-and-fingerprint evidence cannot apply.
# Without these cases the lane could regress into one that never fails, which is
# precisely the shape of instrument this script exists to expose.
make_script_project() {
    local root="$1" meaningful="$2"
    mkdir -p "$root"
    cat > "$root/value.mjs" <<'EOF'
export function answer() { return 42; }
EOF
    if [ "$meaningful" = "meaningful" ]; then
        cat > "$root/value.test.mjs" <<'EOF'
import assert from 'node:assert/strict';
import { answer } from './value.mjs';
assert.equal(answer(), 42);
EOF
    else
        # Imports the subject but asserts nothing about it.
        cat > "$root/value.test.mjs" <<'EOF'
import { answer } from './value.mjs';
answer();
EOF
    fi
    ( cd "$root" \
      && git init -q . \
      && git config user.email t@example.com \
      && git config user.name test \
      && git add -A \
      && git commit -qm fixture ) >/dev/null 2>&1
}

run_script_under_test() {
    local root="$1"
    ( cd "$root" && "$UNDER_TEST" \
        --file value.mjs \
        --break "perl -pi -e 's/return 42;/return 7;/'" \
        --no-build \
        --test "node value.test.mjs" ) >/dev/null 2>&1
    echo $?
}

if command -v node >/dev/null 2>&1; then
    make_script_project "$TMP/script-covered" meaningful
    check "a covering script test is CONFIRMED" 0 \
        "$(run_script_under_test "$TMP/script-covered")"

    make_script_project "$TMP/script-vacuous" vacuous
    check "a vacuous script test is NOT CONFIRMED" 1 \
        "$(run_script_under_test "$TMP/script-vacuous")"

    # --no-build and the build flags describe two different runs. Accepting both
    # would let a caller read a build into a transcript where none happened.
    make_script_project "$TMP/script-conflict" meaningful
    CONFLICT=$( ( cd "$TMP/script-conflict" && "$UNDER_TEST" \
        --file value.mjs --break "perl -pi -e 's/42/7/'" \
        --no-build --build-dir build --test "node value.test.mjs" \
      ) >/dev/null 2>&1; echo $? )
    check "--no-build with --build-dir is rejected" 2 "$CONFLICT"
else
    # A skip is not a pass, so it is reported rather than counted as one.
    printf '  SKIP script-test lane (no node on PATH)\n'
fi

# ── The --python lane, for a Python test ─────────────────────────────────────
#
# Python caches bytecode and accepts a .pyc whose header matches the source's
# mtime (to the second) and size, whatever the content. A stale .pyc compiled
# from different source therefore runs silently against a clean tree. The
# poisoned case plants exactly that: bytecode for `return 41` carrying the
# committed file's own mtime and size. Plain --no-build runs it and cannot even
# get a green baseline (the control); --python must purge and bypass it.
make_python_project() {
    local root="$1" meaningful="$2"
    mkdir -p "$root"
    printf 'def answer():\n    return 42\n' > "$root/value.py"
    if [ "$meaningful" = "meaningful" ]; then
        cat > "$root/test_value.py" <<'EOF'
import sys
from value import answer
sys.exit(0 if answer() == 42 else 1)
EOF
    else
        cat > "$root/test_value.py" <<'EOF'
from value import answer
answer()
EOF
    fi
    ( cd "$root" \
      && git init -q . \
      && git config user.email t@example.com \
      && git config user.name test \
      && git add -A \
      && git commit -qm fixture ) >/dev/null 2>&1
}

poison_pycache() {
    ( cd "$1" && python3 - <<'EOF'
import importlib.util, os, py_compile, struct, tempfile
st = os.stat("value.py")
broken = open("value.py").read().replace("return 42", "return 41")
with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
    f.write(broken)
target = importlib.util.cache_from_source(os.path.abspath("value.py"))
os.makedirs(os.path.dirname(target), exist_ok=True)
py_compile.compile(f.name, cfile=target, doraise=True)
os.unlink(f.name)
raw = bytearray(open(target, "rb").read())
raw[4:16] = struct.pack("<III", 0, int(st.st_mtime), st.st_size & 0xFFFFFFFF)
open(target, "wb").write(bytes(raw))
EOF
    )
}

run_python_under_test() {
    local root="$1" mode="$2"
    ( cd "$root" && "$UNDER_TEST" \
        --file value.py \
        --break "perl -pi -e 's/return 42/return 41/'" \
        "$mode" \
        --test "python3 test_value.py" ) >/dev/null 2>&1
    echo $?
}

make_python_project "$TMP/py-covered" meaningful
check "a covering Python test is CONFIRMED" 0 \
    "$(run_python_under_test "$TMP/py-covered" --python)"

make_python_project "$TMP/py-vacuous" vacuous
check "a vacuous Python test is NOT CONFIRMED" 1 \
    "$(run_python_under_test "$TMP/py-vacuous" --python)"

make_python_project "$TMP/py-poison-control" meaningful
poison_pycache "$TMP/py-poison-control"
check "a planted stale .pyc defeats the plain --no-build lane (control)" 2 \
    "$(run_python_under_test "$TMP/py-poison-control" --no-build)"

make_python_project "$TMP/py-poison" meaningful
poison_pycache "$TMP/py-poison"
check "--python purges and bypasses a planted stale .pyc" 0 \
    "$(run_python_under_test "$TMP/py-poison" --python)"

make_python_project "$TMP/py-conflict" meaningful
PYCONFLICT=$( ( cd "$TMP/py-conflict" && "$UNDER_TEST" \
    --file value.py --break "perl -pi -e 's/42/41/'" \
    --python --build-dir build --test "python3 test_value.py" \
  ) >/dev/null 2>&1; echo $? )
check "--python with --build-dir is rejected" 2 "$PYCONFLICT"

# A test that drives another binary: the fingerprint must follow the edit,
# not the test's own executable. Here the "test" is a shell script that runs
# the compiled subject, the shape of every CLI shell-out suite (the suite is
# one binary, the edited code is compiled into pulp-cpp).
make_shellout_project() {
    local root="$1"
    mkdir -p "$root"
    cat > "$root/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(confirm_failure_shellout_fixture CXX)
set(CMAKE_CXX_STANDARD 17)
add_library(fixture_lib value.cpp)
add_executable(fixture_subject subject.cpp)
target_link_libraries(fixture_subject PRIVATE fixture_lib)
EOF
    cat > "$root/value.hpp" <<'EOF'
int answer();
EOF
    cat > "$root/value.cpp" <<'EOF'
#include "value.hpp"
int answer() { return 42; }
EOF
    cat > "$root/subject.cpp" <<'EOF'
#include "value.hpp"
int main() { return answer() == 42 ? 0 : 1; }
EOF
    cat > "$root/run_test.sh" <<'EOF'
#!/bin/sh
exec ./build/fixture_subject
EOF
    ( cd "$root" \
      && git init -q . \
      && git config user.email t@example.com \
      && git config user.name test \
      && git add -A \
      && git commit -qm fixture ) >/dev/null 2>&1
    cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
}

run_shellout_under_test() {
    local root="$1"; shift
    ( cd "$root" && "$UNDER_TEST" \
        --file value.cpp \
        --break "perl -pi -e 's/return 42;/return 7;/'" \
        --build-dir build \
        --target fixture_subject \
        --test "sh ./run_test.sh" \
        --jobs 2 "$@" ) >/dev/null 2>&1
    echo $?
}

make_shellout_project "$TMP/shellout"
# Without --subject the loop fingerprints `sh`, which never changes, and must
# refuse a verdict rather than bless or blame the test.
check "a shell-out test without --subject is INCONCLUSIVE" 2 \
    "$(run_shellout_under_test "$TMP/shellout")"
check "a shell-out test with --subject is CONFIRMED" 0 \
    "$(run_shellout_under_test "$TMP/shellout" --subject ./build/fixture_subject)"

# An INCONCLUSIVE exit must not leave the subject built from broken source.
# Only the --test binary used to be invalidated on restore, so the next run's
# "baseline" fingerprint was the contaminated artifact, the hash never moved
# when the source was broken, and the loop reported a structural failure that
# read as a harness limitation rather than as stale state. Provoke a restore
# path (a break that changes nothing) and require the subject to be gone.
make_shellout_project "$TMP/shellout-stale"
STALE=$( ( cd "$TMP/shellout-stale" && "$UNDER_TEST" \
    --file value.cpp \
    --break "perl -pi -e 's/no_such_text/x/'" \
    --build-dir build --target fixture_subject \
    --subject ./build/fixture_subject \
    --test "sh ./run_test.sh" --jobs 2 \
  ) >/dev/null 2>&1; echo $? )
check "a no-op break with --subject is INCONCLUSIVE" 2 "$STALE"
if [ -e "$TMP/shellout-stale/build/fixture_subject" ]; then
    printf '  FAIL the subject binary survives an INCONCLUSIVE exit (stale-baseline hazard)\n'
    FAILURES=$((FAILURES + 1))
else
    printf '  ok   the subject binary is invalidated on an INCONCLUSIVE exit\n'
fi

# ── The build dir outside --target is left as it was found ──────────────────
#
# A header break invalidates every object and archive, because the header's
# dependents are unknown; the loop then rebuilds only --target. Anything outside
# that target used to stay deleted, so the next ordinary build silently paid for
# a rebuild the script never declared. Here `other_lib` is outside the target:
# its object and archive must be back afterwards, and the script must say how
# many artifacts it rebuilt and how many it put back.
make_header_project() {
    local root="$1"
    mkdir -p "$root"
    cat > "$root/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(confirm_failure_header_fixture CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(fixture_test test.cpp)
add_library(other_lib other.cpp)
EOF
    cat > "$root/value.hpp" <<'EOF'
inline int answer() { return 42; }
EOF
    cat > "$root/test.cpp" <<'EOF'
#include "value.hpp"
int main() { return answer() == 42 ? 0 : 1; }
EOF
    cat > "$root/other.cpp" <<'EOF'
int other() { return 1; }
EOF
    ( cd "$root" \
      && git init -q . \
      && git config user.email t@example.com \
      && git config user.name test \
      && git add -A \
      && git commit -qm fixture ) >/dev/null 2>&1
    cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
    cmake --build "$root/build" -j 2 >/dev/null 2>&1
}

make_header_project "$TMP/header"
OTHER_OBJ="$(cd "$TMP/header/build" && find . -name 'other.cpp.o' | head -1)"
OTHER_LIB="$(cd "$TMP/header/build" && find . -name 'libother_lib.a' | head -1)"
if [ -z "$OTHER_OBJ" ] || [ -z "$OTHER_LIB" ]; then
    printf '  FAIL the header fixture did not build other_lib (control)\n'
    FAILURES=$((FAILURES + 1))
fi
HEADER_OUT="$(cd "$TMP/header" && "$UNDER_TEST" \
    --file value.hpp \
    --break "perl -pi -e 's/return 42;/return 7;/'" \
    --build-dir build --target fixture_test --test ./build/fixture_test --jobs 2 2>&1)"
check "a covering header test is CONFIRMED" 0 "$?"
for artifact in "$OTHER_OBJ" "$OTHER_LIB"; do
    if [ -n "$artifact" ] && [ -f "$TMP/header/build/$artifact" ]; then
        printf '  ok   %s outside --target is back after the run\n' "$(basename "$artifact")"
    else
        printf '  FAIL %s outside --target was left deleted\n' "$(basename "${artifact:-other_lib}")"
        FAILURES=$((FAILURES + 1))
    fi
done
if printf '%s\n' "$HEADER_OUT" | grep -qE 'build dir: [1-9][0-9]* removed artifact\(s\) rebuilt by --target, [1-9][0-9]* outside it put back'; then
    printf '  ok   the run says what it rebuilt and what it put back\n'
else
    printf '  FAIL the run does not say what it did with the removed artifacts\n'
    FAILURES=$((FAILURES + 1))
fi
if find "$TMP/header/build" -name '.confirm-failure-stash.*' | grep -q .; then
    printf '  FAIL a stash directory is left in the build dir\n'
    FAILURES=$((FAILURES + 1))
else
    printf '  ok   no stash directory is left behind\n'
fi

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
