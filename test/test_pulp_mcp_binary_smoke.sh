#!/bin/bash
# Smoke the real pulp-mcp executable, not just the in-process unit harness.
# This catches split-source/link/package-adjacent regressions where the test
# fixture compiles but the shipped MCP server binary no longer starts or speaks
# the stdio JSON-RPC protocol.

set -euo pipefail

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

if [ "$#" -ne 1 ]; then
    fail "usage: $0 /path/to/pulp-mcp"
fi

bin="$1"
if [ ! -x "$bin" ]; then
    fail "pulp-mcp binary is not executable: $bin"
fi

version_output="$("$bin" --version)"
if ! grep -Eq '^pulp-mcp [0-9]+\.[0-9]+\.[0-9]+' <<<"$version_output"; then
    fail "unexpected --version output: $version_output"
fi

rpc_output="$(
    printf '%s\n' \
        '{"jsonrpc":"2.0","id":1,"method":"initialize"}' \
        '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
    | "$bin"
)"

if ! grep -q '"serverInfo":{"name":"pulp-mcp","version":"' <<<"$rpc_output"; then
    fail "initialize response missing serverInfo: $rpc_output"
fi

if ! grep -q '"tools":\[' <<<"$rpc_output"; then
    fail "tools/list response missing tools array: $rpc_output"
fi

if ! grep -q '"name":"pulp_compat"' <<<"$rpc_output"; then
    fail "tools/list response missing pulp_compat: $rpc_output"
fi

if grep -q $'\r' <<<"$rpc_output"; then
    fail "bare JSON-RPC output contains carriage returns"
fi
