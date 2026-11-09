#!/usr/bin/env bash
#
# Confirm a test actually fails without the fix it claims to cover.
#
# A passing test proves nothing on its own — it may assert the same thing the
# code assumes, or never reach the code at all. The check is to break the fix,
# watch the test fail, put it back, and watch it pass. Done by hand that loop
# has a silent failure mode which has produced wrong conclusions in both
# directions:
#
#   The build does not pick up the edit. Restoring a file and rebuilding within
#   the same filesystem second leaves make comparing equal mtimes and deciding
#   the object is current, so the binary still holds the OLD code. Touching the
#   source does not reliably fix this — the touch lands in the same second. A
#   stale binary during the break step makes the control falsely PASS, which
#   reads as "my test does not cover this" and sends you off to rewrite a test
#   that was fine. A stale binary during the restore step reports a failure that
#   is not real.
#
# So this never trusts the timestamps. It deletes the object files for the
# edited source, and then VERIFIES the compiler actually rebuilt it by looking
# for the file in the build output. If the recompile is not observed, it stops
# and says so rather than reporting a result it cannot stand behind.
#
# Usage:
#   confirm_failure.sh --file <path> --break <sed/perl cmd> \
#                      --build-dir <dir> --target <cmake target> \
#                      --test <command> [--jobs N] [--object <basename>] \
#                      [--subject <binary>] [--keep-logs]
#
# --keep-logs keeps the per-phase test logs after a CONFIRMED run too.
#
# --subject names the compiled binary the edit must reach when it is not the
# test's own executable: a CLI shell-out suite fingerprints as
# pulp-test-cli-shellout while the edited code is compiled into pulp-cpp.
# Without it the loop fingerprints the wrong file, never sees the edit land,
# and reports INCONCLUSIVE by construction - which reads as "the test does not
# cover this" when the truth is "the wrong file was fingerprinted". With
# --subject the test executable is left in place (it only drives the subject),
# so --target may name the subject's target alone, unless the driver links the
# edited source too: then the run stops INCONCLUSIVE and asks for a --target
# that rebuilds the driver as well. The link is read from the build graph, and
# the run is also INCONCLUSIVE when the graph cannot answer: no word of --test
# is a binary in the build dir (a script or ctest wrapping the driver), or a
# header is edited in a Makefile build, which records no header dependencies.
#
# A Catch2 test spec splits on commas, so a --test naming a test case whose
# name contains a comma matches nothing and the baseline comes back
# INCONCLUSIVE. Select such a case by a comma-free tag or a wildcard instead
# ("[my-tag]", "Name up to the comma*").
#
#   confirm_failure.sh --file <path> --break <sed/perl cmd> \
#                      --no-build --test <command>
#
# --no-build is the lane for a test whose subject is INTERPRETED — a Node
# `.test.mjs`, a Python test, a shell script. There is no object, archive, or
# executable between the edited source and the test, so the stale-artifact trap
# this script exists to defeat cannot arise: a fresh process re-reads the file
# at import. The compiled lane's binary fingerprint has no analogue there (the
# interpreter is the same bytes either way), so the evidence that the edit
# reached the test is the edited file's content hash changing, which is checked
# in both lanes. Without this lane every interpreted test in the tree is
# un-negative-controllable by the one instrument the repo mandates for it.
#
# A failing test proves nothing when the broken code never ran. In the compiled
# lane that is a break that does not compile; in the --python lane it is a break
# that no longer parses (SyntaxError, IndentationError) or no longer imports:
# the test then dies at import, which reads exactly like an assertion failure.
# So after the break, --python byte-compiles the edited .py in memory, and, when
# the unbroken module could be loaded standalone at baseline, loads the broken
# one the same way. Either failure is INCONCLUSIVE, never CONFIRMED. The load
# executes the module's top level once more, as the test itself would, in a
# child process with a time limit; a module that cannot be loaded standalone
# at baseline (relative imports, required environment) gets the parse check
# only, and the run says so.
#
# --no-build is mutually exclusive with --build-dir/--target/--jobs/--object:
# a caller must not be able to think a build happened when none did.
#
#   confirm_failure.sh --file <path.py> --break <sed/perl cmd> \
#                      --python --test "python3 tools/scripts/test_foo.py"
#
# --python is the --no-build lane for Python, which is NOT free of the stale
# artifact trap: importlib accepts a cached .pyc whose header matches the
# source's mtime (one-second granularity) and size, never its content. A
# length-preserving break restored within the same second therefore leaves the
# broken bytecode running against a clean tree. So every run in this mode first
# deletes __pycache__ under the edited file's directory and then runs with
# PYTHONDONTWRITEBYTECODE=1 and a fresh, empty PYTHONPYCACHEPREFIX, so no cached
# bytecode from before, or from the previous run, can be imported.
#
# --object names the source whose object file carries the edited file, for a
# source the compiler never sees directly. A JS prelude under core/view/js is
# embedded into web_compat_preludes_gen.cpp at build time, so there is no
# web-compat-foo.js.o to watch for and the recompile evidence must be
# `--object web_compat_preludes_gen.cpp` instead.
#
# The break command is run with the file path appended, e.g.
#   --break "perl -0pi -e 's/policy.priority/0/'"
#
# Exit codes:
#   0  CONFIRMED  — passed before, failed while broken, passes again after
#   1  NOT CONFIRMED — the test passed while the fix was broken; it does not
#      cover the change
#   2  INCONCLUSIVE — the loop could not be run honestly (dirty file, build
#      never picked up the edit, test already failing, restore failed)
#
# Each phase's test output (baseline, broken, restored) goes to its own log in
# a temp directory. A NOT CONFIRMED or INCONCLUSIVE verdict keeps the directory
# and prints the log of the phase that decided it with its tail, so a wrong
# verdict leaves its evidence. CONFIRMED removes it, unless --keep-logs is
# given, in which case it keeps it and prints its path.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GOVERNED_BUILD="$SCRIPT_DIR/../ci/governed-build.sh"

FILE=""
BREAK_CMD=""
BUILD_DIR=""
TARGET=""
TEST_CMD=""
SUBJECT=""
JOBS=""
OBJECT=""
NO_BUILD=0
PYTHON_MODE=0
KEEP_LOGS=0

while [ $# -gt 0 ]; do
    case "$1" in
        --file)      FILE="$2"; shift 2 ;;
        --break)     BREAK_CMD="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --target)    TARGET="$2"; shift 2 ;;
        --subject)   SUBJECT="$2"; shift 2 ;;
        --test)      TEST_CMD="$2"; shift 2 ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        --object)    OBJECT="$2"; shift 2 ;;
        --no-build)  NO_BUILD=1; shift ;;
        --python)    NO_BUILD=1; PYTHON_MODE=1; shift ;;
        --keep-logs) KEEP_LOGS=1; shift ;;
        -h|--help)   sed -n '3,40p' "$0"; exit 0 ;;
        *) echo "confirm-failure: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

missing=""
[ -n "$FILE" ]      || missing="$missing --file"
[ -n "$BREAK_CMD" ] || missing="$missing --break"
[ -n "$TEST_CMD" ]  || missing="$missing --test"
if [ "$NO_BUILD" -eq 0 ]; then
    [ -n "$BUILD_DIR" ] || missing="$missing --build-dir"
    [ -n "$TARGET" ]    || missing="$missing --target"
fi
if [ -n "$missing" ]; then
    echo "confirm-failure: required argument(s):$missing" >&2
    exit 2
fi

if [ "$NO_BUILD" -eq 1 ]; then
    conflict=""
    [ -z "$BUILD_DIR" ] || conflict="$conflict --build-dir"
    [ -z "$TARGET" ]    || conflict="$conflict --target"
    [ -z "$JOBS" ]      || conflict="$conflict --jobs"
    [ -z "$OBJECT" ]    || conflict="$conflict --object"
    if [ -n "$conflict" ]; then
        echo "confirm-failure: --no-build cannot be combined with:$conflict" >&2
        exit 2
    fi
fi

if [ -z "$JOBS" ]; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

say() { printf 'confirm-failure: %s\n' "$1"; }

# Created by the first test run, so a run that stops before any test leaves
# nothing behind.
TEST_LOG_DIR=""
LAST_TEST_LOG=""

# Name the log of the phase that decided the verdict and show its tail.
show_test_log() {
    [ -n "$LAST_TEST_LOG" ] || return 0
    say "test output ($(basename "$LAST_TEST_LOG" .log)): $LAST_TEST_LOG"
    tail -n 20 "$LAST_TEST_LOG" | sed 's/^/    | /'
}
die_inconclusive() { say "INCONCLUSIVE — $1"; show_test_log; exit 2; }

[ -f "$FILE" ] || die_inconclusive "no such file: $FILE"

# Restoring through git is exact, where a hand-kept .bak can silently drift.
if ! git ls-files --error-unmatch "$FILE" >/dev/null 2>&1; then
    die_inconclusive "$FILE is not tracked by git, so it cannot be restored exactly"
fi
if ! git diff --quiet -- "$FILE"; then
    die_inconclusive "$FILE has uncommitted changes; commit or stash them so the
    restore is exact"
fi

BUILD_LOG="$(mktemp)"
# Pristine bytes of the clean file, for a restore that needs no git lock.
PRISTINE="$(mktemp)"
cp -p "$FILE" "$PRISTINE"

# Artifacts the first invalidate() removes are moved here instead of deleted.
# They were built from the committed source, which is what the tree holds again
# after the restore, so any the final --target build did not recreate go back
# where they were on exit. Without this a header run deleted every object and
# archive in the build dir and rebuilt only --target's closure, leaving the rest
# of the tree to rebuild on the next build with nothing saying so.
STASH=""
[ "$NO_BUILD" -eq 1 ] || STASH="$BUILD_DIR/.confirm-failure-stash.$$"
STASHED=0

put_back_stash() {
    [ -n "$STASH" ] && [ -d "$STASH" ] || return 0
    local counts
    counts="$(python3 - "$STASH" "$BUILD_DIR" <<'PY'
import os, sys
stash, build = sys.argv[1], sys.argv[2]
restored = rebuilt = 0
for root, _, files in os.walk(stash):
    for name in files:
        src = os.path.join(root, name)
        dst = os.path.join(build, os.path.relpath(src, stash))
        if os.path.lexists(dst):
            rebuilt += 1
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.rename(src, dst)
        restored += 1
print(restored, rebuilt)
PY
)" || { say "could not put the removed artifacts back; they are under $STASH"; return 1; }
    rm -rf "$STASH"
    set -- $counts
    say "build dir: $2 removed artifact(s) rebuilt by --target, $1 outside it put back as they were"
}

trap 'rm -f "$BUILD_LOG" "$PRISTINE"; put_back_stash' EXIT

# Delete the objects built from this source so the rebuild cannot be skipped on
# a timestamp comparison. Header-only edits have no object of their own, so the
# recompile is verified by observing that SOMETHING was rebuilt.
BASE="$(basename "$FILE")"
IS_HEADER=0
case "$BASE" in *.h|*.hpp|*.hh|*.hxx|*.inc) IS_HEADER=1 ;; esac

# The object to delete and to watch for in the build log. Normally that is the
# edited file's own object; --object redirects it when the edited source is
# embedded into a generated translation unit and has no object of its own.
OBJ_BASE="${OBJECT:-$BASE}"

# The binary the test runs. Verifying an OBJECT recompiled is not enough: the
# archive can relink while the executable's link is skipped, because make
# compares the archive's mtime against the executable's and they can land in the
# same second. That leaves the test running old code with a fresh object beside
# it — the same stale-artifact trap this script exists to catch, one level up.
TEST_BINARY="$(printf '%s' "$TEST_CMD" | awk '{print $1}')"
# What must change when the edit lands: the subject when the test drives
# another binary, otherwise the test executable itself.
FINGERPRINTED="${SUBJECT:-$TEST_BINARY}"

binary_fingerprint() {
    # An interpreted test runs through a shared interpreter whose bytes never
    # change, so fingerprinting it would compare a constant against itself and
    # report "the edit did not reach the test" for every honest run. The edited
    # file's own content hash is the evidence in that lane, and it is checked
    # for both lanes at the break step.
    [ "$NO_BUILD" -eq 1 ] && { echo "source-is-the-artifact"; return; }
    [ -f "$FINGERPRINTED" ] || { echo "absent"; return; }
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$FINGERPRINTED" | awk '{print $1}'
    else
        cksum "$FINGERPRINTED" | awk '{print $1 $2}'
    fi
}

# Push the edited file's mtime a few seconds into the future.
#
# This is the whole reason the hand-run loop is unreliable. macOS ships GNU make
# 3.81, which compares mtimes at ONE-SECOND granularity, and a build rebuilds a
# source only when it is strictly NEWER than its object. An edit landing in the
# same second as the previous build therefore looks current: make prints "Built
# target" and compiles nothing, and the test runs the old code. Deleting objects
# does not save you — the target-level check short-circuits before the missing
# object is ever noticed. `touch` to NOW does not save you either, for exactly
# the same reason.
bump_mtime() {
    local stamp
    stamp="$(date -v+5S '+%Y%m%d%H%M.%S' 2>/dev/null ||
             date -d '+5 seconds' '+%Y%m%d%H%M.%S' 2>/dev/null || true)"
    if [ -n "$stamp" ]; then
        touch -t "$stamp" "$FILE" 2>/dev/null || touch "$FILE"
    else
        touch "$FILE"
    fi
}

# Remove every artifact between the edited source and the test binary.
#
# Bumping the source's mtime is necessary but NOT sufficient: it forces the
# object to recompile, and then the fresh object ties its archive's mtime, and
# the archive ties the executable's, so the cascade stalls one level up and the
# test runs old code with a fresh object sitting beside it. Nothing survives a
# comparison that has no artifact to compare against, so the archives and the
# binary go too.
#
# The first pass moves the pristine artifacts to $STASH rather than deleting
# them, and the exit trap puts back whichever ones the --target build did not
# recreate, so the rest of the build dir ends as it began.
invalidate() {
    [ "$NO_BUILD" -eq 1 ] && return 0
    # With --subject the edit lands in the subject, and the test executable is
    # only the driver that runs it. --target may build the subject alone, so a
    # deleted driver would never come back; leave it and invalidate the
    # subject instead.
    if [ -n "$SUBJECT" ]; then
        rm -f "$SUBJECT" 2>/dev/null || true
    else
        rm -f "$TEST_BINARY" 2>/dev/null || true
    fi
    local objects="${OBJ_BASE}.o"
    # A header's dependents are unknown here, so every object goes.
    [ "$IS_HEADER" -eq 1 ] && objects='*.o'
    if [ "$STASHED" -eq 0 ]; then
        # First pass: these were built from the committed source; keep them.
        STASHED=1
        find "$BUILD_DIR" -path "$STASH" -prune -o \
            \( -name '*.a' -o -name '*.dylib' -o -name '*.so' -o -name "$objects" \) \
            -type f -print0 2>/dev/null |
            python3 -c '
import os, sys
build, stash = sys.argv[1], sys.argv[2]
for p in filter(None, sys.stdin.buffer.read().split(b"\0")):
    p = p.decode()
    dst = os.path.join(stash, os.path.relpath(p, build))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    os.rename(p, dst)
' "$BUILD_DIR" "$STASH" || true
        return 0
    fi
    find "$BUILD_DIR" -path "$STASH" -prune -o \
        \( -name '*.a' -o -name '*.dylib' -o -name '*.so' -o -name "$objects" \) \
        -type f -exec rm -f {} + 2>/dev/null || true
}

# Build, and require evidence that the edited file was actually compiled. This
# is the check the hand-run loop does not have.
build_and_verify_recompile() {
    local phase="$1"
    if [ "$NO_BUILD" -eq 1 ]; then
        if [ "$PYTHON_MODE" -eq 1 ]; then
            say "$phase: no build step -- __pycache__ purged and bytecode caching bypassed, so the test re-reads $BASE"
        else
            say "$phase: no build step -- the test re-reads $BASE from disk"
        fi
        return 0
    fi
    if ! PULP_BUILD_JOBS="$JOBS" bash "$GOVERNED_BUILD" \
        cmake --build "$BUILD_DIR" --target "$TARGET" > "$BUILD_LOG" 2>&1; then
        if grep -qE '\berror:' "$BUILD_LOG"; then
            # A break that does not compile proves nothing about the test: the
            # binary it would run was never built. The caller treats it as
            # INCONCLUSIVE.
            say "$phase: the edit does not compile"
            return 1
        fi
        say "$phase: build failed for a reason other than the edit"
        sed -n '$p' "$BUILD_LOG" >&2
        return 2
    fi
    # Echo the compile line that was matched. The verdict already depends on
    # it, but a reader of the transcript cannot see a grep that happened inside
    # a temp file this script deletes -- and "Built target" is not evidence, so
    # the line itself has to reach the log.
    local evidence=""
    if [ "$IS_HEADER" -eq 1 ]; then
        evidence="$(grep -m1 -E 'Building [A-Z]+ object' "$BUILD_LOG")"
    else
        evidence="$(grep -m1 -F "${OBJ_BASE}.o" "$BUILD_LOG")"
    fi
    if [ -n "$evidence" ]; then
        say "$phase: recompiled -- ${evidence#"${evidence%%[![:space:]]*}"}"
        return 0
    fi
    say "$phase: the build did NOT recompile $OBJ_BASE"
    say "  The binary still holds the old code, so any result now is meaningless."
    say "  This is the stale-object trap: an edit landing in the same filesystem"
    say "  second as the previous build leaves the object looking current."
    return 2
}

# Remove every bytecode cache under the edited file's directory.
purge_pycache() {
    find "$(dirname "$FILE")" -type d -name __pycache__ -prune \
        -exec rm -rf {} + 2>/dev/null || true
}

# Parse the edited file without writing bytecode. Prints the error on failure.
python_parses() {
    python3 -I - "$FILE" <<'PY'
import sys
path = sys.argv[1]
try:
    with open(path, "rb") as stream:
        compile(stream.read(), path, "exec", dont_inherit=True)
except (SyntaxError, ValueError) as exc:
    print(f"{type(exc).__name__}: {exc}")
    sys.exit(1)
PY
}

# Load the edited file standalone in a child process with a time limit.
# Exit 0 loaded, 1 raised while loading (the error is printed), 3 timed out.
python_loads() {
    python3 -I - "$FILE" <<'PY'
import os, signal, subprocess, sys
path = os.path.abspath(sys.argv[1])
probe = r"""
import importlib.util, os, sys
path = sys.argv[1]
sys.path.insert(0, os.path.dirname(path))
name = "_confirm_failure_probe_" + os.path.splitext(os.path.basename(path))[0]
spec = importlib.util.spec_from_file_location(name, path)
module = importlib.util.module_from_spec(spec)
sys.modules[name] = module
spec.loader.exec_module(module)
"""
env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
timeout = float(os.environ.get("CONFIRM_FAILURE_PYTHON_LOAD_TIMEOUT", "60"))
process = subprocess.Popen(
    [sys.executable, "-c", probe, path], cwd=os.path.dirname(path), env=env,
    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True, start_new_session=True)
try:
    stdout, stderr = process.communicate(timeout=timeout)
except subprocess.TimeoutExpired:
    # The module may have spawned descendants. Kill the whole process group,
    # then reap the probe so a timed-out import cannot leak into later phases.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.communicate()
    print(f"timed out after {timeout:g} seconds")
    sys.exit(3)
result = subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)
if result.returncode != 0:
    lines = (result.stderr or result.stdout).strip().splitlines()
    print(lines[-1] if lines else f"exit {result.returncode}")
    sys.exit(1)
PY
}

run_test() {
    [ -n "$TEST_LOG_DIR" ] ||
        TEST_LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/confirm-failure.XXXXXX")"
    LAST_TEST_LOG="$TEST_LOG_DIR/$1.log"
    if [ "$PYTHON_MODE" -eq 1 ]; then
        purge_pycache
        local prefix rc
        prefix="$(mktemp -d)"
        ( export PYTHONDONTWRITEBYTECODE=1 PYTHONPYCACHEPREFIX="$prefix"
          eval "$TEST_CMD" ) > "$LAST_TEST_LOG" 2>&1
        rc=$?
        rm -rf "$prefix"
        return "$rc"
    fi
    ( eval "$TEST_CMD" ) > "$LAST_TEST_LOG" 2>&1
}

restore() {
    # Copy the pristine bytes back rather than "git checkout", which takes
    # .git/index.lock and fails while any other git process holds it; the
    # git diff below (no lock needed) proves the result matches the index.
    cp -p "$PRISTINE" "$FILE" || return 1
    git diff --quiet -- "$FILE" || return 1
    [ "$PYTHON_MODE" -eq 0 ] || purge_pycache
    bump_mtime
    invalidate
}

# ── 1. Positive control: green before we touch anything ──────────────────────
say "checking the test passes before the edit"
invalidate
build_and_verify_recompile "baseline" || die_inconclusive "could not build a clean baseline"
if ! run_test baseline; then
    die_inconclusive "the test already fails before any edit; fix that first"
fi
PYTHON_SOURCE=0
LOAD_PROBE=0
if [ "$PYTHON_MODE" -eq 1 ] && [ "${FILE##*.}" = "py" ]; then
    PYTHON_SOURCE=1
    if python_loads >/dev/null 2>&1; then
        LOAD_PROBE=1
    else
        say "baseline: $BASE does not load standalone, so the broken module gets the parse check only"
    fi
fi
BASELINE_BINARY="$(binary_fingerprint)"
if [ "$BASELINE_BINARY" = "absent" ]; then
    [ -z "$SUBJECT" ] ||
        die_inconclusive "cannot find the subject binary '$SUBJECT' to fingerprint"
    die_inconclusive "cannot find the test binary '$TEST_BINARY' to fingerprint -
    if the test shells out to another compiled binary, name it with
    --subject <path> (e.g. build/tools/cli/pulp-cpp); if it is a script, use
    --no-build"
fi

# With --subject the driver is left in place, so it must not carry the edited
# code itself: --target rebuilds the subject only, and a driver that also links
# the edited object would run the stale copy and could pass with the fix
# broken (a false NOT CONFIRMED).
#
# The answer comes from the build's own graph, never from symbol names. A
# symbol comparison cannot tell a linked object from a shared header: inline
# functions and template instantiations (libc++'s ABI-tagged ones included)
# are emitted into every object that uses them, and Mach-O nm prints those weak
# definitions as strong "T", so any driver including the same headers looked
# like it linked the edited object. Instead the driver's link inputs are walked
# - its objects, and recursively the objects of every library it links - in
# build.ninja (via `ninja -t query`) or in the Makefile generator's link.txt
# files. A source edit reaches the driver when that walk reaches the object
# compiled from it. A header has no object of its own, so a header edit reaches
# the driver when one of those objects depends on it in ninja's dependency log
# (`ninja -t deps`); the Makefile lane has no such log and cannot answer.
#
# Every word of --test that is a file under --build-dir is a driver and is
# checked, so a prefix such as `env` or `VAR=1` does not hide one, and neither
# does a chain such as `./build/subject && ./build/driver`. The subject itself
# is rebuilt by --target and needs no check. A --test with no such word (a
# script, ctest, a command on PATH) may still run a binary that links the
# edited code, so it cannot be checked and the run is INCONCLUSIVE; so is a
# driver that the graph knows but that no object is linked into (a custom
# command's output), since what it runs is unknown.
#
# Exit status: 0 the driver carries the edited code, 1 it does not, 2 the
# graph cannot answer; the reason or the linking object is printed. Any other
# status (a crash) is treated as 2. A link is harmless when --target itself
# rebuilds the driver; the helper then prints "rebuilt-by-target" and exits 1.
driver_link_status() {
    [ -n "$SUBJECT" ] && [ "$NO_BUILD" -eq 0 ] || return 1
    python3 - "$BUILD_DIR" "$FILE" "${OBJ_BASE}.o" "${OBJECT:+1}" \
        "$TEST_CMD" "$TARGET" "$STASH" "$SUBJECT" "$IS_HEADER" <<'PY'
import os, re, shlex, shutil, subprocess, sys

def unknown(reason):
    print(reason)
    sys.exit(2)

def main():
    (build, edited_file, obj_name, by_object, test_cmd, target, stash,
     subject, is_header) = sys.argv[1:10]
    build = os.path.realpath(build)
    edited_file = os.path.realpath(edited_file)
    stash = os.path.realpath(stash) if stash else ""
    is_header = is_header == "1"
    LIBRARY = re.compile(r"\.(a|lib|dylib|so(\.[0-9]+)*)$")
    OBJECT = (".o", ".obj")

    def inside(path, root):
        return os.path.commonpath([path, root]) == root

    # ── Which binary is the driver ──────────────────────────────────────────
    try:
        words = shlex.split(test_cmd)
    except ValueError as exc:
        unknown(f"--test cannot be parsed into words ({exc})")
    # Every word that is a build-dir file may run code, so each one is
    # checked: `./build/subject && ./build/driver` runs both.
    candidates = []
    for word in words:
        path = os.path.realpath(word)
        if os.path.isfile(path) and inside(path, build) and path not in candidates:
            candidates.append(path)
    if not candidates:
        first = words[0] if words else ""
        unknown(f"no word of --test is a file under {build} (the first word is "
                f"'{first}'), so the binary it runs cannot be checked for a copy "
                "of the edited code; name the driver binary itself in --test")
    # The subject is rebuilt by --target, so it needs no check.
    drivers = [c for c in candidates if c != os.path.realpath(subject)]
    if not drivers:
        sys.exit(1)
    driver = drivers[0]  # rebound per driver below; the helpers read it late

    def is_edited_object(path, source_inputs):
        if os.path.basename(path) != obj_name:
            return False
        # --object names a generated translation unit that embeds the edited
        # file, so the object's own source is not the edited file.
        if by_object:
            return True
        return any(os.path.realpath(s) == edited_file for s in source_inputs)

    def node_path(node):
        return os.path.realpath(node if os.path.isabs(node) else os.path.join(build, node))

    # ── Ninja: build.ninja is the graph, `ninja -t` reads it ────────────────
    def ninja_program():
        override = os.environ.get("CONFIRM_FAILURE_NINJA")
        if override is not None:
            return override if os.access(override, os.X_OK) else None
        try:
            with open(os.path.join(build, "CMakeCache.txt")) as cache:
                for line in cache:
                    if line.startswith("CMAKE_MAKE_PROGRAM:"):
                        candidate = line.split("=", 1)[1].strip()
                        if os.access(candidate, os.X_OK):
                            return candidate
        except OSError:
            pass
        return shutil.which("ninja")

    def ninja_tool(ninja, tool, nodes):
        proc = subprocess.run([ninja, "-C", build, "-t", tool, *nodes],
                              capture_output=True, text=True)
        return proc.stdout if proc.returncode == 0 else None

    def ninja_query(ninja, nodes):
        """Explicit and implicit inputs of each node; order-only ones are not
        linked, only sequenced, so they never count. None when ninja fails."""
        out = ninja_tool(ninja, "query", nodes)
        if out is None:
            return None
        inputs, node, section = {}, None, None
        for line in out.splitlines():
            if line and not line.startswith(" ") and line.endswith(":"):
                node, section = line[:-1], None
                inputs[node] = []
            elif line.startswith("  ") and not line.startswith("    "):
                section = line.strip().split(":", 1)[0]
            elif section == "input" and node is not None:
                item = line.strip()
                if item.startswith("||") or item.startswith("|@"):
                    continue
                if item.startswith("| "):
                    item = item[2:]
                inputs[node].append(item)
        return inputs

    def ninja_objects(ninja):
        driver_node = os.path.relpath(driver, build)
        first = ninja_query(ninja, [driver_node])
        if first is None or not first.get(driver_node):
            unknown(f"ninja does not know how {driver_node} is linked")
        seen, frontier, objects = {driver_node}, [driver_node], []
        graph = dict(first)
        while frontier:
            nxt = []
            for node in frontier:
                for item in graph.get(node, []):
                    if item in seen:
                        continue
                    seen.add(item)
                    if item.endswith(OBJECT):
                        objects.append(item)
                    elif LIBRARY.search(item):
                        nxt.append(item)
            if nxt:
                more = ninja_query(ninja, nxt)
                if more is None:
                    unknown("ninja could not describe a library the driver links: "
                            + " ".join(nxt))
                graph.update(more)
            frontier = nxt
        if not objects:
            unknown(f"{driver_node} is not linked from any object (a custom "
                    "command output, for example), so what it runs is unknown")
        return objects

    def ninja_links_source(ninja, objects):
        candidates = [o for o in objects if os.path.basename(o) == obj_name]
        if not candidates:
            return None
        sources = ninja_query(ninja, candidates)
        if sources is None:
            unknown("ninja could not describe " + " ".join(candidates))
        for obj in candidates:
            if is_edited_object(obj, [node_path(s) for s in sources.get(obj, [])]):
                return obj
        return None

    def ninja_links_header(ninja, objects):
        if not objects:
            return None
        out = ninja_tool(ninja, "deps", objects)
        if out is None:
            unknown("`ninja -t deps` failed, so which headers the driver's "
                    "objects include is unknown")
        deps, node = {}, None
        for line in out.splitlines():
            if line and not line.startswith(" "):
                node = line.split(":", 1)[0]
                deps[node] = None if "#deps" not in line else []
            elif line.strip() and node is not None and deps.get(node) is not None:
                deps[node].append(line.strip())
        for obj in objects:
            if deps.get(obj) is None:
                unknown(f"ninja's dependency log has no record for {obj}, so "
                        "whether it includes the edited header is unknown")
            if any(node_path(d) == edited_file for d in deps[obj]):
                return obj
        return None

    def ninja_target_rebuilds_driver(ninja):
        out = ninja_tool(ninja, "inputs", [target])
        if out is None:
            return False
        nodes = {target, *(line.strip() for line in out.splitlines() if line.strip())}
        return any(node_path(n) == driver for n in nodes)

    # ── Makefiles: each target's link.txt holds its link command ───────────
    def make_links():
        links = {}  # output path -> (target .dir, input paths)
        for root, dirs, files in os.walk(build):
            if stash and inside(os.path.realpath(root), stash):
                dirs[:] = []
                continue
            if "link.txt" not in files or not root.endswith(".dir"):
                continue
            cwd = os.path.dirname(os.path.dirname(root))  # parent of CMakeFiles
            outputs, inputs = set(), []
            with open(os.path.join(root, "link.txt")) as stream:
                for line in stream:
                    try:
                        argv = shlex.split(line)
                    except ValueError:
                        continue
                    tokens = []
                    for arg in argv[1:]:
                        if arg.startswith("@"):
                            try:
                                with open(os.path.join(cwd, arg[1:])) as rsp:
                                    tokens += shlex.split(rsp.read())
                            except OSError:
                                pass
                        elif arg.startswith("-Wl,"):
                            tokens += arg.split(",")[1:]
                        else:
                            tokens.append(arg)
                    if "-o" in tokens and tokens.index("-o") + 1 < len(tokens):
                        outputs.add(tokens[tokens.index("-o") + 1])
                    elif argv and os.path.basename(argv[0]).endswith("ar") and len(tokens) >= 2:
                        outputs.add(tokens[1])  # ar <flags> <archive> <objects...>
                    inputs += [t for t in tokens if t.endswith(OBJECT) or LIBRARY.search(t)]
            resolved_outputs = {os.path.realpath(os.path.join(cwd, o)) for o in outputs}
            resolved_inputs = [os.path.realpath(os.path.join(cwd, i)) for i in inputs]
            for out in resolved_outputs:
                links[out] = (root, [i for i in resolved_inputs if i not in resolved_outputs])
        if driver not in links:
            unknown(f"no link.txt in {build} produces {driver}")
        seen, frontier, objects = {driver}, [driver], []
        while frontier:
            nxt = []
            for node in frontier:
                for item in links.get(node, (None, []))[1]:
                    if item in seen:
                        continue
                    seen.add(item)
                    if item.endswith(OBJECT):
                        objects.append(item)
                    elif item in links:
                        nxt.append(item)
            frontier = nxt
        if not objects:
            unknown(f"no object is linked into {driver}, so what it runs is unknown")
        for obj in objects:
            if os.path.basename(obj) != obj_name:
                continue
            # The object's .dir/build.make names its source: "<obj>: <source>".
            obj_dir = os.path.dirname(obj)
            while obj_dir != build and not obj_dir.endswith(".dir"):
                parent = os.path.dirname(obj_dir)
                if parent == obj_dir:
                    break  # outside the build dir; build.make is not found below
                obj_dir = parent
            sources = []
            try:
                with open(os.path.join(obj_dir, "build.make")) as stream:
                    for line in stream:
                        lhs, sep, rhs = line.rstrip("\n").partition(": ")
                        if sep and os.path.realpath(os.path.join(build, lhs)) == obj:
                            sources.append(rhs.strip())
            except OSError:
                sources = [edited_file]  # cannot tell; count it, which refuses
            if is_edited_object(obj, sources):
                return obj, links[driver][0]
        return None, links[driver][0]

    def make_target_rebuilds_driver(driver_dir):
        """CMakeFiles/Makefile2 holds the target graph: "<target>: <dir>/rule"
        names the target's own .dir ("all: <dir>/all" lists each of the
        default target's), and "<dir>/all: <dep>/all" its dependencies. The
        driver is rebuilt when its .dir is in the closure."""
        try:
            with open(os.path.join(build, "CMakeFiles", "Makefile2")) as stream:
                lines = stream.read().splitlines()
        except OSError:
            return False
        starts, edges = set(), {}
        for line in lines:
            lhs, sep, rhs = line.partition(": ")
            if not sep or line.startswith(("\t", "#")):
                continue
            if lhs == target:
                for suffix in ("/rule", "/all"):
                    if rhs.strip().endswith(suffix):
                        starts.add(rhs.strip()[: -len(suffix)])
            if lhs.endswith("/all") and rhs.strip().endswith("/all"):
                edges.setdefault(lhs[: -len("/all")], set()).add(rhs.strip()[: -len("/all")])
        if not starts:
            return False
        seen, frontier = set(starts), list(starts)
        while frontier:
            nxt = [d for node in frontier for d in edges.get(node, ()) if d not in seen]
            seen.update(nxt)
            frontier = nxt
        return any(os.path.realpath(os.path.join(build, d)) == os.path.realpath(driver_dir)
                   for d in seen)

    use_ninja = os.path.exists(os.path.join(build, "build.ninja"))
    if use_ninja:
        ninja = ninja_program()
        if ninja is None:
            unknown("the build dir uses Ninja but no ninja executable was found")
    elif os.path.exists(os.path.join(build, "Makefile")):
        if is_header:
            unknown("the Makefile generator keeps no dependency log to say "
                    "whether the driver includes the edited header")
    else:
        unknown(f"{build} has neither build.ninja nor a Makefile to read the link graph from")
    rebuilt = False
    for driver in drivers:
        if use_ninja:
            objects = ninja_objects(ninja)
            linked = (ninja_links_header(ninja, objects) if is_header
                      else ninja_links_source(ninja, objects))
            harmless = bool(linked) and ninja_target_rebuilds_driver(ninja)
        else:
            linked, driver_dir = make_links()
            harmless = bool(linked) and make_target_rebuilds_driver(driver_dir)
        if linked and not harmless:
            print(f"{linked} in {driver}")
            sys.exit(0)
        rebuilt = rebuilt or harmless
    if rebuilt:
        print("rebuilt-by-target")
    sys.exit(1)

try:
    main()
except SystemExit:
    raise
except BaseException as exc:  # a crash must never read as "does not link"
    unknown(f"the link check failed: {type(exc).__name__}: {exc}")
PY
}
DRIVER_LINK="$(driver_link_status)"
DRIVER_LINK_STATUS=$?
case "$DRIVER_LINK_STATUS" in
    0) die_inconclusive "the test driver also carries $BASE (through
    $DRIVER_LINK), and with --subject the driver is not rebuilt, so it would
    run the unbroken copy. Pass a --target that builds the driver as well as
    the subject (the driver's own target usually does)" ;;
    1) [ "$DRIVER_LINK" != "rebuilt-by-target" ] ||
           say "the test driver carries $BASE, and --target '$TARGET' rebuilds it too" ;;
    *) die_inconclusive "cannot tell whether the test driver carries $BASE:
    ${DRIVER_LINK:-the check exited $DRIVER_LINK_STATUS}. A driver that does
    would run the unbroken copy, so no verdict is possible without that answer" ;;
esac

# ── 2. Break the fix ─────────────────────────────────────────────────────────
say "breaking the fix in $FILE"
BEFORE_HASH="$(git hash-object "$FILE")"
if ! eval "$BREAK_CMD \"$FILE\""; then
    restore
    die_inconclusive "the break command failed to run"
fi
if [ "$(git hash-object "$FILE")" = "$BEFORE_HASH" ]; then
    restore
    die_inconclusive "the break command changed nothing — check the pattern matches"
fi

bump_mtime
invalidate
build_and_verify_recompile "broken"
BUILD_STATUS=$?
if [ "$BUILD_STATUS" -eq 2 ]; then
    restore
    exit 2
fi

BREAK_UNRUNNABLE=""
if [ "$BUILD_STATUS" -eq 0 ] && [ "$PYTHON_SOURCE" -eq 1 ]; then
    if ! PARSE_ERROR="$(python_parses 2>&1)"; then
        BREAK_UNRUNNABLE="no longer parses ($PARSE_ERROR)"
    elif [ "$LOAD_PROBE" -eq 1 ]; then
        LOAD_ERROR="$(python_loads 2>&1)"
        LOAD_STATUS=$?
        [ "$LOAD_STATUS" -eq 0 ] || BREAK_UNRUNNABLE="no longer imports ($LOAD_ERROR)"
    fi
    if [ -n "$BREAK_UNRUNNABLE" ]; then
        say "broken: $BASE $BREAK_UNRUNNABLE"
        BUILD_STATUS=1
    fi
fi

BROKEN_PASSES=0
BROKEN_TEST_LOG=""
if [ "$BUILD_STATUS" -eq 0 ]; then
    # The binary must differ from the one the baseline ran. If it does not, the
    # edit never reached what the test executes and any verdict here would be
    # about the old code.
    if [ "$NO_BUILD" -eq 0 ] &&
       [ "$(binary_fingerprint)" = "$BASELINE_BINARY" ]; then
        restore
        die_inconclusive "'$FINGERPRINTED' is unchanged after breaking $BASE - the
    edit did not reach what the test runs, so no verdict is possible. If the
    test drives a different compiled binary, name it with --subject <path>"
    fi
    if run_test broken; then BROKEN_PASSES=1; fi
    BROKEN_TEST_LOG="$LAST_TEST_LOG"
fi

# ── 3. Put it back and prove we are green again ──────────────────────────────
say "restoring $FILE"
restore || die_inconclusive "could not restore $FILE — the tree is left dirty"
build_and_verify_recompile "restored" || die_inconclusive "could not rebuild after restoring"
if ! run_test restored; then
    die_inconclusive "the test does not pass after restoring; the tree may be inconsistent"
fi

# ── 4. Verdict ───────────────────────────────────────────────────────────────
if [ -n "$BREAK_UNRUNNABLE" ]; then
    die_inconclusive "the broken $BASE $BREAK_UNRUNNABLE, so a failing test would
    only show that it could not load the module, not that it checks the fix.
    Write a --break that still parses and imports (change a value or a branch)"
fi
if [ "$BUILD_STATUS" -eq 1 ]; then
    die_inconclusive "the break does not compile, so the test never ran against
    broken code. Write a --break that still builds (change a value or a branch,
    not a signature)"
fi
if [ "$BROKEN_PASSES" -eq 1 ]; then
    say "NOT CONFIRMED — the test passed with the fix broken."
    say "  It does not cover this change. Common causes: the assertion restates"
    say "  what the code assumes, the broken path is never reached by the inputs"
    say "  under test, or a guard skips the check when the thing it needs is absent."
    LAST_TEST_LOG="$BROKEN_TEST_LOG"
    show_test_log
    exit 1
fi

say "CONFIRMED — the test passes with the fix and fails without it."
if [ "$KEEP_LOGS" -eq 1 ]; then
    say "test output: $TEST_LOG_DIR"
else
    rm -rf "$TEST_LOG_DIR"
fi
exit 0
