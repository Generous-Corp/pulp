#!/usr/bin/env bash
#
# Unit test for tools/mcp/pulp-mcp-launcher. Verifies the fix in pulp #1821:
# the launcher (referenced from .mcp.json so Claude Code can resolve the
# pulp-mcp binary regardless of cwd) finds the source-tree build, falls
# back to $PATH, and emits a useful diagnostic when neither is available.
#
# Run directly:
#   bash test/test_pulp_mcp_launcher.sh
#
# Exits 0 on pass, non-zero on fail.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LAUNCHER_SRC="$SCRIPT_DIR/tools/mcp/pulp-mcp-launcher"

if [ ! -x "$LAUNCHER_SRC" ]; then
    echo "FAIL: launcher not executable at $LAUNCHER_SRC" >&2
    exit 1
fi

TMPDIR_T="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_T"' EXIT

# Each case stages the launcher into a fresh fake-repo layout so the
# launcher's BASH_SOURCE-based resolution can be exercised without
# touching the real repo's build/ dir.
stage_fake_repo() {
    local repo="$1"
    mkdir -p "$repo/tools/mcp"
    cp "$LAUNCHER_SRC" "$repo/tools/mcp/pulp-mcp-launcher"
    chmod +x "$repo/tools/mcp/pulp-mcp-launcher"
}

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Case 1: no source-tree binary, nothing on PATH → diagnostic + exit 127.
# ---------------------------------------------------------------------------
case1="$TMPDIR_T/case1"
stage_fake_repo "$case1"

# Override HOME to a clean tmpdir so the launcher's
# `${HOME}/.pulp/bin/pulp-mcp` probe doesn't accidentally find a real
# installed binary on a dev machine that has run `install.sh`.
# Scrubbing PATH alone isn't enough — HOME is searched independently.
set +e
output=$(HOME="$TMPDIR_T/case1-home" PATH=/usr/bin:/bin \
    "$case1/tools/mcp/pulp-mcp-launcher" 2>&1)
rc=$?
set -e

if [ "$rc" -ne 127 ]; then
    fail "case1: expected exit 127 when no binary found, got $rc (output: $output)"
fi
if ! grep -q "cannot locate pulp-mcp binary" <<<"$output"; then
    fail "case1: diagnostic missing 'cannot locate pulp-mcp binary' (output: $output)"
fi
if ! grep -q "$case1/build/tools/mcp/pulp-mcp" <<<"$output"; then
    fail "case1: diagnostic should reference resolved candidate path (output: $output)"
fi
pass "case1: no-binary error path emits readable diagnostic + exits 127"

# ---------------------------------------------------------------------------
# Case 2: source-tree binary present → launcher exec's it (stub echoes a
# canary; PATH is scrubbed so a real pulp-mcp install can't interfere).
# ---------------------------------------------------------------------------
case2="$TMPDIR_T/case2"
stage_fake_repo "$case2"
mkdir -p "$case2/build/tools/mcp"
cat > "$case2/build/tools/mcp/pulp-mcp" <<'STUB'
#!/usr/bin/env bash
echo "PULP_MCP_LAUNCHER_TEST_CANARY"
exit 0
STUB
chmod +x "$case2/build/tools/mcp/pulp-mcp"

set +e
output=$(HOME="$TMPDIR_T/case2-home" PATH=/usr/bin:/bin \
    "$case2/tools/mcp/pulp-mcp-launcher" 2>&1)
rc=$?
set -e

if [ "$rc" -ne 0 ]; then
    fail "case2: expected exit 0 when stub binary runs cleanly, got $rc (output: $output)"
fi
if [ "$output" != "PULP_MCP_LAUNCHER_TEST_CANARY" ]; then
    fail "case2: expected stub canary, got: $output"
fi
pass "case2: source-tree build is exec'd when present"

# ---------------------------------------------------------------------------
# Case 3: source-tree binary absent but pulp-mcp on $PATH → PATH stub runs.
# ---------------------------------------------------------------------------
case3="$TMPDIR_T/case3"
stage_fake_repo "$case3"

pathdir="$TMPDIR_T/case3-path"
mkdir -p "$pathdir"
cat > "$pathdir/pulp-mcp" <<'STUB'
#!/usr/bin/env bash
echo "PULP_MCP_PATH_FALLBACK_CANARY"
exit 0
STUB
chmod +x "$pathdir/pulp-mcp"

set +e
output=$(HOME="$TMPDIR_T/case3-home" PATH="$pathdir:/usr/bin:/bin" \
    "$case3/tools/mcp/pulp-mcp-launcher" 2>&1)
rc=$?
set -e

if [ "$rc" -ne 0 ]; then
    fail "case3: expected exit 0 when PATH fallback runs cleanly, got $rc (output: $output)"
fi
if [ "$output" != "PULP_MCP_PATH_FALLBACK_CANARY" ]; then
    fail "case3: expected PATH-fallback canary, got: $output"
fi
pass "case3: \$PATH fallback runs when source-tree build is absent"

# ---------------------------------------------------------------------------
# Case 4: argv is forwarded — launcher must exec, not interpret args.
# ---------------------------------------------------------------------------
case4="$TMPDIR_T/case4"
stage_fake_repo "$case4"
mkdir -p "$case4/build/tools/mcp"
cat > "$case4/build/tools/mcp/pulp-mcp" <<'STUB'
#!/usr/bin/env bash
echo "ARGS:$*"
exit 0
STUB
chmod +x "$case4/build/tools/mcp/pulp-mcp"

set +e
output=$(HOME="$TMPDIR_T/case4-home" PATH=/usr/bin:/bin \
    "$case4/tools/mcp/pulp-mcp-launcher" --foo bar baz 2>&1)
rc=$?
set -e

if [ "$rc" -ne 0 ]; then
    fail "case4: expected exit 0, got $rc (output: $output)"
fi
if [ "$output" != "ARGS:--foo bar baz" ]; then
    fail "case4: launcher did not forward argv as-is (output: $output)"
fi
pass "case4: argv forwarded verbatim"

echo "OK — all 4 cases passed."
