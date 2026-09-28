#!/usr/bin/env bash
# Unit test for hooks/scripts/check-pulp-cli.sh.
#
# Mirrors test/test_pulp_mcp_launcher.sh's case-driven shell-test
# pattern. We scrub PATH per case and stand up a temp PULP_CHECK_CWD
# pointing at a synthetic build tree, so the hook script's three
# states (pulp on PATH / source-tree-only / nothing) are exercised
# deterministically without depending on what the developer has
# installed on their machine.
#
# Usage:
#   bash test/test_check_pulp_cli_hook.sh
#
# CTest registration in test/CMakeLists.txt mirrors the launcher
# test wiring (~line 90). The hook is informational only — it must
# always exit 0, so a failure in any case here means we'd start
# blocking Claude Code session init, which would be a real
# regression.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK="$SCRIPT_DIR/hooks/scripts/check-pulp-cli.sh"

if [ ! -x "$HOOK" ]; then
    echo "FAIL: hook missing or not executable: $HOOK"
    exit 1
fi

# Shared per-case scaffold. Each case gets a fresh tempdir; PATH is
# scrubbed to /usr/bin:/bin so a globally-installed pulp can't pollute
# results. PULP_CHECK_CWD points the hook at the per-case fake build
# tree so cases 2 + 3 are independent.
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

# ── Case 1: `pulp` is on PATH → silent exit 0 ───────────────────────────────
case1=$(mktemp -d)
mkdir -p "$case1/bin"
cat > "$case1/bin/pulp" <<'EOF'
#!/usr/bin/env bash
echo "stub pulp"
EOF
chmod +x "$case1/bin/pulp"

# Empty PULP_CHECK_CWD so source-tree branch can't fire even by accident.
out=$(PATH="$case1/bin:/usr/bin:/bin" PULP_CHECK_CWD="$case1/empty-cwd" "$HOOK" 2>&1)
status=$?
if [ "$status" -ne 0 ]; then
    fail "case1: hook exited $status (expected 0)"
fi
if [ -n "$out" ]; then
    fail "case1: pulp on PATH should produce no output, got: $out"
fi
pass "case1: pulp on PATH → silent success"
rm -rf "$case1"

# ── Case 2: pulp NOT on PATH but source build exists → contributor message ──
case2=$(mktemp -d)
mkdir -p "$case2/build"
cat > "$case2/build/pulp" <<'EOF'
#!/usr/bin/env bash
echo "fake source-tree pulp"
EOF
chmod +x "$case2/build/pulp"

out=$(PATH="/usr/bin:/bin" PULP_CHECK_CWD="$case2" "$HOOK" 2>&1)
status=$?
if [ "$status" -ne 0 ]; then
    fail "case2: hook exited $status (expected 0)"
fi
if ! grep -q "source-tree build" <<<"$out"; then
    fail "case2: missing 'source-tree build' phrase in output: $out"
fi
if ! grep -q "ln -s" <<<"$out"; then
    fail "case2: missing 'ln -s' suggestion in output"
fi
if ! grep -q "$case2/build/pulp" <<<"$out"; then
    fail "case2: output should reference the actual found binary path"
fi
pass "case2: source-tree build → contributor symlink message"
rm -rf "$case2"

# ── Case 2b: source build is pulp-cpp (Rust binary missing) → still works ───
case2b=$(mktemp -d)
mkdir -p "$case2b/build/tools/cli"
cat > "$case2b/build/tools/cli/pulp-cpp" <<'EOF'
#!/usr/bin/env bash
echo "fake pulp-cpp"
EOF
chmod +x "$case2b/build/tools/cli/pulp-cpp"

out=$(PATH="/usr/bin:/bin" PULP_CHECK_CWD="$case2b" "$HOOK" 2>&1)
status=$?
if [ "$status" -ne 0 ]; then
    fail "case2b: hook exited $status (expected 0)"
fi
if ! grep -q "$case2b/build/tools/cli/pulp-cpp" <<<"$out"; then
    fail "case2b: should fall back to pulp-cpp when build/pulp is absent"
fi
pass "case2b: pulp-cpp fallback → contributor symlink message"
rm -rf "$case2b"

# ── Case 3: nothing → install banner ────────────────────────────────────────
case3=$(mktemp -d)  # empty cwd — no build/, no bin/

out=$(PATH="/usr/bin:/bin" PULP_CHECK_CWD="$case3" "$HOOK" 2>&1)
status=$?
if [ "$status" -ne 0 ]; then
    fail "case3: hook exited $status (expected 0)"
fi
if ! grep -q "curl -fsSL" <<<"$out"; then
    fail "case3: missing curl install command in output: $out"
fi
if ! grep -q "generouscorp.com/pulp/install.sh" <<<"$out"; then
    fail "case3: missing install URL in output"
fi
if grep -q "source-tree build" <<<"$out"; then
    fail "case3: should not show source-tree message when no build/ exists"
fi
pass "case3: nothing installed → install banner"
rm -rf "$case3"

# ── Case 4: invariant — exit code is ALWAYS 0 even on broken cwd ─────────────
# A non-existent PULP_CHECK_CWD should not crash the hook (hooks must
# never block Claude session init).
#
# pulp #2000 — capture the hook's exit code into `status`
# directly via `|| status=$?`, NOT via `out=$(...) || true; status=$?`.
# The latter pattern always reports 0 because `true` is what `$?` sees,
# so a regression where the hook started returning non-zero would slip
# through the gate undetected.
status=0
PATH="/usr/bin:/bin" PULP_CHECK_CWD="/nonexistent/path/$(date +%s)" "$HOOK" >/dev/null 2>&1 || status=$?
if [ "$status" -ne 0 ]; then
    fail "case4: hook exited $status with bad PULP_CHECK_CWD; must always be 0"
fi
pass "case4: bad PULP_CHECK_CWD → still exits 0"

# ── Case 5: source-build path that is NOT executable → fall through to case 3 ──
# A `build/pulp` that exists but lacks +x means the user has stale build
# artifacts; treating it as a usable binary would be a lie.
case5=$(mktemp -d)
mkdir -p "$case5/build"
echo "not executable" > "$case5/build/pulp"
chmod -x "$case5/build/pulp"

out=$(PATH="/usr/bin:/bin" PULP_CHECK_CWD="$case5" "$HOOK" 2>&1)
status=$?
if [ "$status" -ne 0 ]; then
    fail "case5: hook exited $status (expected 0)"
fi
if grep -q "source-tree build" <<<"$out"; then
    fail "case5: non-executable build/pulp should NOT trigger contributor branch"
fi
if ! grep -q "curl -fsSL" <<<"$out"; then
    fail "case5: non-executable build/pulp should fall through to install banner"
fi
pass "case5: non-executable source build → install banner (no false-positive)"
rm -rf "$case5"

# ── Case 6: STDOUT vs STDERR — banner must go to stderr ─────────────────────
# Claude Code surfaces hook STDERR in the session UI without treating it
# as a tool failure. Sending banner to stdout would either be silenced
# or misclassified.
case6=$(mktemp -d)
stdout_file=$(mktemp)
stderr_file=$(mktemp)

PATH="/usr/bin:/bin" PULP_CHECK_CWD="$case6" "$HOOK" >"$stdout_file" 2>"$stderr_file"
if [ -s "$stdout_file" ]; then
    fail "case6: banner should not appear on stdout (got: $(cat "$stdout_file"))"
fi
if [ ! -s "$stderr_file" ]; then
    fail "case6: banner should appear on stderr (was empty)"
fi
if ! grep -q "curl -fsSL" "$stderr_file"; then
    fail "case6: stderr should contain install banner"
fi
pass "case6: install banner on stderr (correct stream for Claude UI)"
rm -rf "$case6" "$stdout_file" "$stderr_file"

# ── Case 7: a `pulp` on PATH far older than the Pulp checkout → STALE CLI ────
# Old CLIs configure a current checkout with their own defaults, so the hook
# names the gap and the update command. `--session-start` puts the banner on
# stdout (SessionStart output becomes agent context); the Setup call keeps it
# on stderr.
make_checkout() {
    mkdir -p "$1/core" "$1/bin"
    printf 'cmake_minimum_required(VERSION 3.24)\n# pin before project(),\nproject(Pulp\n    VERSION 0.876.1\n    LANGUAGES C CXX)\n' \
        > "$1/CMakeLists.txt"
}
make_pulp() {
    printf '#!/usr/bin/env bash\n[ "$1" = version ] && echo "pulp v%s" && echo "Claude plugin: v0.1.0"\n' "$2" \
        > "$1/bin/pulp"
    chmod +x "$1/bin/pulp"
}

case7=$(mktemp -d)
make_checkout "$case7"
make_pulp "$case7" 0.305.0
stdout_file=$(mktemp)
stderr_file=$(mktemp)
status=0
PATH="$case7/bin:/usr/bin:/bin" PULP_CHECK_CWD="$case7/core" "$HOOK" --session-start \
    >"$stdout_file" 2>"$stderr_file" || status=$?
[ "$status" -eq 0 ] || fail "case7: hook exited $status (expected 0)"
grep -q "STALE CLI" "$stdout_file" || fail "case7: no STALE CLI banner on stdout: $(cat "$stdout_file")"
grep -q "571 releases behind" "$stdout_file" || fail "case7: banner should count 571 releases: $(cat "$stdout_file")"
grep -q "generouscorp.com/pulp/install.sh" "$stdout_file" || fail "case7: banner lacks the update command"
grep -q "governed-build.sh cmake --build build --target pulp-rust-cli" "$stdout_file" ||
    fail "case7: banner lacks the governed bootstrap"
if [ -s "$stderr_file" ]; then
    fail "case7: --session-start should keep stderr empty: $(cat "$stderr_file")"
fi
out=$(PATH="$case7/bin:/usr/bin:/bin" PULP_CHECK_CWD="$case7" "$HOOK" 2>&1 >/dev/null)
grep -q "STALE CLI" <<<"$out" || fail "case7: Setup mode should print the banner on stderr"
pass "case7: stale pulp on PATH inside a Pulp checkout → STALE CLI banner"
rm -f "$stdout_file" "$stderr_file"

# ── Case 8: the same checkout with a current CLI → silent ────────────────────
make_pulp "$case7" 0.870.0
out=$(PATH="$case7/bin:/usr/bin:/bin" PULP_CHECK_CWD="$case7" "$HOOK" --session-start 2>&1)
[ -z "$out" ] || fail "case8: a CLI within the threshold should be silent, got: $out"
pass "case8: current pulp → silent"

# ── Case 9: a stale CLI outside a Pulp checkout → silent ─────────────────────
make_pulp "$case7" 0.305.0
printf 'project(VersionFixture VERSION 9.0.0)\n' > "$case7/CMakeLists.txt"
out=$(PATH="$case7/bin:/usr/bin:/bin" PULP_CHECK_CWD="$case7" "$HOOK" --session-start 2>&1)
[ -z "$out" ] || fail "case9: a non-Pulp project should be silent, got: $out"
pass "case9: stale pulp outside a Pulp checkout → silent"
rm -rf "$case7"

# ── Case 10: --session-start with no pulp at all → silent ────────────────────
# The install banner belongs to the Setup hook; SessionStart must not repeat it.
case10=$(mktemp -d)
out=$(PATH="/usr/bin:/bin" PULP_CHECK_CWD="$case10" "$HOOK" --session-start 2>&1)
[ -z "$out" ] || fail "case10: --session-start without pulp should be silent, got: $out"
pass "case10: --session-start without pulp → silent"
rm -rf "$case10"

echo ""
echo "All 11 cases passed."
