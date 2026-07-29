#!/usr/bin/env python3
"""Smoke the real pulp-mcp executable through its stdio protocol."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    return 1


def run_rpc(binary: Path, requests: list[dict[str, object]]) -> list[dict[str, object]]:
    payload = "\n".join(json.dumps(request, separators=(",", ":")) for request in requests) + "\n"
    rpc = subprocess.run(
        [str(binary)], input=payload, check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
    )
    if rpc.returncode != 0:
        raise RuntimeError(
            f"stdio RPC exited {rpc.returncode}: stdout={rpc.stdout!r} stderr={rpc.stderr!r}"
        )
    return [json.loads(line) for line in rpc.stdout.splitlines() if line]


def tool_call(request_id: int, name: str, arguments: dict[str, object]) -> dict[str, object]:
    return {
        "jsonrpc": "2.0", "id": request_id, "method": "tools/call",
        "params": {"name": name, "arguments": arguments},
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()

    binary = args.binary
    if not binary.is_file():
        return fail(f"pulp-mcp binary does not exist: {binary}")

    try:
        version = subprocess.run(
            [str(binary), "--version"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )
    except OSError as exc:
        return fail(f"could not execute {binary}: {exc}")

    if version.returncode != 0:
        return fail(
            f"--version exited {version.returncode}: "
            f"stdout={version.stdout!r} stderr={version.stderr!r}"
        )
    if not re.fullmatch(r"pulp-mcp \d+\.\d+\.\d+\s*", version.stdout):
        return fail(f"unexpected --version output: {version.stdout!r}")

    payload = "\n".join(
        [
            '{"jsonrpc":"2.0","id":1,"method":"initialize"}',
            '{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
            "",
        ]
    )
    rpc = subprocess.run(
        [str(binary)],
        input=payload,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    if rpc.returncode != 0:
        return fail(
            f"stdio RPC exited {rpc.returncode}: "
            f"stdout={rpc.stdout!r} stderr={rpc.stderr!r}"
        )

    lines = [line for line in rpc.stdout.splitlines() if line]
    if len(lines) != 2:
        return fail(f"expected 2 JSON-RPC response lines, got {len(lines)}: {rpc.stdout!r}")

    try:
        initialize = json.loads(lines[0])
        tools_list = json.loads(lines[1])
    except json.JSONDecodeError as exc:
        return fail(f"response line is not valid single-line JSON: {exc}: {rpc.stdout!r}")

    server_info = initialize.get("result", {}).get("serverInfo", {})
    if server_info.get("name") != "pulp-mcp":
        return fail(f"initialize response missing pulp-mcp serverInfo: {initialize!r}")
    if not re.fullmatch(r"\d+\.\d+\.\d+", str(server_info.get("version", ""))):
        return fail(f"initialize response has bad serverInfo.version: {initialize!r}")

    tools = tools_list.get("result", {}).get("tools")
    if not isinstance(tools, list):
        return fail(f"tools/list response missing tools array: {tools_list!r}")
    names = {tool.get("name") for tool in tools if isinstance(tool, dict)}
    if "pulp_compat" not in names:
        return fail(f"tools/list response missing pulp_compat: {tools_list!r}")

    # A handle from a terminated server must never alias the first session in
    # its replacement. These are two real OS processes, not two stores in one
    # test binary, so this pins the restart boundary clients actually cross.
    project = Path(__file__).parent / "fixtures" / "timeline" / "v1" / "minimal.json"
    project_json = project.read_text(encoding="utf-8")
    try:
        first = run_rpc(binary, [tool_call(10, "pulp_timeline_project_open", {"project": project_json})])
        old_handle = first[0]["result"]["structuredContent"]["session_id"]
        second = run_rpc(
            binary,
            [
                tool_call(11, "pulp_timeline_project_open", {"project": project_json}),
                tool_call(12, "pulp_timeline_diff", {"session_id": old_handle}),
            ],
        )
        new_handle = second[0]["result"]["structuredContent"]["session_id"]
        stale_result = second[1]["result"]
    except (KeyError, IndexError, json.JSONDecodeError, OSError, RuntimeError) as exc:
        return fail(f"two-process timeline session regression failed to run: {exc}")
    if old_handle == new_handle:
        return fail(f"timeline session handle aliased across server processes: {old_handle!r}")
    if not stale_result.get("isError") or "unknown or expired" not in json.dumps(stale_result):
        return fail(f"replacement server accepted prior process handle: {stale_result!r}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
