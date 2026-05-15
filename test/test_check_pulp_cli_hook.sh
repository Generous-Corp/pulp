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
out=$(PATH="/usr/bin:/bin" PULP_CHECK_CWD="/nonexistent/path/$(date +%s)" "$HOOK" 2>&1) || true
status=$?
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

echo ""
echo "All 7 cases passed."
