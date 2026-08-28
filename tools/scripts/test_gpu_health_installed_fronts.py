#!/usr/bin/env python3
"""Exercise installed GPU doctor fronts from PATH and an unrelated cwd."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str], *, cwd: Path, env: dict[str, str], stdin: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, env=env, input=stdin,
                          capture_output=True, text=True, check=False)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--require-cpp", action="store_true")
    parser.add_argument("--require-rust", action="store_true")
    args = parser.parse_args()
    bindir = args.prefix.resolve() / "bin"
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join((str(bindir), "/usr/bin", "/bin"))

    with tempfile.TemporaryDirectory(prefix="pulp-gpu-installed-fronts-") as td:
        cwd = Path(td)
        cpp_cli = bindir / ("pulp-cpp.exe" if os.name == "nt" else "pulp-cpp")
        executables: list[str] = []
        if cpp_cli.is_file():
            executables.append("pulp-cpp")
        elif args.require_cpp:
            raise AssertionError(f"configured C++ GPU CLI is missing: {cpp_cli}")
        rust_cli = bindir / ("pulp.exe" if os.name == "nt" else "pulp")
        if rust_cli.is_file():
            executables.append("pulp")
        elif args.require_rust:
            raise AssertionError(f"configured Rust CLI is missing: {rust_cli}")
        for executable in executables:
            result = run([executable, "doctor", "gpu", "--no-render", "--json"],
                         cwd=cwd, env=env)
            assert result.returncode == 2, (executable, result.returncode, result.stderr)
            evidence = json.loads(result.stdout)
            assert evidence["schema"] == "pulp.gpu-health-result.v1"
            assert evidence["render_requested"] is False
            assert evidence["verdict"] == "unverified"

        requests = "\n".join((
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}),
            json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
                "name": "pulp_gpu_doctor", "arguments": {"no_render": True}}}),
        )) + "\n"
        mcp = run(["pulp-mcp"], cwd=cwd, env=env, stdin=requests)
        assert mcp.returncode == 0, (mcp.returncode, mcp.stderr, mcp.stdout)
        messages = [json.loads(line) for line in mcp.stdout.splitlines() if line.strip()]
        response = next(message for message in messages if message.get("id") == 2)
        result = response["result"]
        assert result["isError"] is True
        if cpp_cli.is_file():
            assert result["structuredContent"]["exit_code"] == 2
            evidence = result["structuredContent"]["evidence"]
            assert evidence["schema"] == "pulp.gpu-health-result.v1"
            assert evidence["render_requested"] is False
            assert evidence["verdict"] == "unverified"
        else:
            assert "structuredContent" not in result
            error = json.loads(result["content"][0]["text"])
            assert error["error"]["code"] == "cli-unavailable"

    print("gpu_health_installed_fronts_verified "
          f"cli={','.join(executables)} mcp=path-unrelated-cwd")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
