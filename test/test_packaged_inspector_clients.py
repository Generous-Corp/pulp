#!/usr/bin/env python3
"""Prove packaged inspector clients against a real standalone publication."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile
import time
from typing import Any


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PARAMETER_ID = 42_017
CLI_PARAMETER_VALUE = -1.5
RESET_PARAMETER_VALUE = -6.0
MCP_PARAMETER_VALUE = 3.0


def fail(message: str) -> None:
    raise AssertionError(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def run_checked(
    command: list[str], *, cwd: Path, env: dict[str, str], timeout: float = 30
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if result.returncode != 0:
        fail(
            f"command failed ({result.returncode}): {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def parse_json_output(result: subprocess.CompletedProcess[str], label: str) -> Any:
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        fail(
            f"{label} emitted invalid JSON: {error}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def native_release_platform() -> str:
    machine = platform.machine().lower()
    if machine in {"arm64", "aarch64"}:
        return "darwin-arm64"
    if machine in {"x86_64", "amd64"}:
        return "darwin-x64"
    fail(f"unsupported native macOS architecture: {machine}")


def selector_args(ready: dict[str, str]) -> list[str]:
    return [
        "--session",
        ready["session_id"],
        "--instance",
        ready["instance_id"],
        "--publication",
        ready["publication_id"],
    ]


def selector_object(ready: dict[str, str]) -> dict[str, str]:
    return {
        "session_id": ready["session_id"],
        "instance_id": ready["instance_id"],
        "publication_id": ready["publication_id"],
    }


def find_dict_with_id(value: Any, widget_id: str) -> dict[str, Any] | None:
    if isinstance(value, dict):
        if value.get("id") == widget_id:
            return value
        for child in value.values():
            found = find_dict_with_id(child, widget_id)
            if found is not None:
                return found
    elif isinstance(value, list):
        for child in value:
            found = find_dict_with_id(child, widget_id)
            if found is not None:
                return found
    return None


def mcp_request(request_id: int, name: str, arguments: dict[str, Any]) -> str:
    return json.dumps(
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        },
        separators=(",", ":"),
    )


def wait_for_ready(
    fixture: subprocess.Popen[str], ready_path: Path, timeout: float
) -> dict[str, str]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ready_path.is_file():
            try:
                value = json.loads(ready_path.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, OSError):
                time.sleep(0.02)
                continue
            required = {
                "session_id",
                "instance_id",
                "publication_id",
                "plugin_id",
                "runtime_dir",
            }
            require(required <= value.keys(), f"ready file lacks fields: {value!r}")
            return value
        if fixture.poll() is not None:
            stdout, stderr = fixture.communicate()
            fail(
                f"fixture exited before publishing (code {fixture.returncode})\n"
                f"stdout:\n{stdout}\nstderr:\n{stderr}"
            )
        time.sleep(0.02)
    fail("fixture did not publish a ready record within 30 seconds")


def scrubbed_client_environment(runtime_dir: Path) -> dict[str, str]:
    env = os.environ.copy()
    for name in (
        "PULP_RS_CPP_BINARY",
        "PULP_RS_NO_FALLTHROUGH",
        "PULP_RS_FALLTHROUGH",
        "PULP_USE_CPP",
        "PULP_INSPECT_PROFILE",
        "PULP_HEADLESS",
        "PULP_TEST_MODE",
        "CI",
    ):
        env.pop(name, None)
    for name in tuple(env):
        if name.startswith("DYLD_"):
            env.pop(name, None)
    env.update(
        {
            "PATH": "/usr/bin:/bin",
            "PULP_DEBUG": "1",
            "PULP_INSPECTOR_RUNTIME_DIR": str(runtime_dir),
            "PULP_UPDATE_CHECK_DISABLED": "1",
        }
    )
    return env


def package_and_extract(args: argparse.Namespace, scratch: Path) -> Path:
    archive = scratch / f"pulp-{native_release_platform()}.tar.gz"
    package_env = os.environ.copy()
    result = run_checked(
        [
            sys.executable,
            str(args.packager),
            "--binary",
            str(args.rust),
            "--cpp-binary",
            str(args.cpp),
            "--mcp-binary",
            str(args.mcp),
            "--build-dir",
            str(args.build_dir),
            "--platform",
            native_release_platform(),
            "--out",
            str(archive),
        ],
        cwd=scratch,
        env=package_env,
        timeout=60,
    )
    require(archive.is_file(), f"packager did not create {archive}")
    install = scratch / "install"
    install.mkdir()
    with tarfile.open(archive, "r:gz") as payload:
        members = payload.getmembers()
        names = {member.name for member in members}
        require(len(names) == len(members), "package contains duplicate members")
        require(
            {"pulp", "pulp-cpp", "pulp-mcp"} <= names,
            f"package lacks client binaries: {sorted(names)!r}",
        )
        dylibs = {name for name in names if Path(name).name == "libwgpu_native.dylib"}
        require(len(dylibs) == 1, f"package must contain one wgpu dylib: {sorted(names)!r}")
        require(
            names == {"pulp", "pulp-cpp", "pulp-mcp"} | dylibs,
            f"package layout is not the expected flat payload: {sorted(names)!r}",
        )
        for member in members:
            require(member.name == Path(member.name).name, f"non-flat member: {member.name}")
            require(member.isfile(), f"non-regular package member: {member.name}")
        for member in members:
            source = payload.extractfile(member)
            require(source is not None, f"could not read package member: {member.name}")
            destination = install / member.name
            destination.write_bytes(source.read())
            destination.chmod(member.mode & 0o777)
    for name in ("pulp", "pulp-cpp", "pulp-mcp", "libwgpu_native.dylib"):
        require((install / name).is_file(), f"extracted payload lacks {name}")
    print(result.stdout, end="")
    return install


def require_selected_session(payload: dict[str, Any], ready: dict[str, str]) -> None:
    require(payload.get("session_id") == ready["session_id"], str(payload))
    require(payload.get("instance_id") == ready["instance_id"], str(payload))
    require(payload.get("publication_id") == ready["publication_id"], str(payload))


def parameter_value(parameters: Any) -> float:
    require(isinstance(parameters, list), f"parameter catalog is not an array: {parameters!r}")
    parameter = next(
        (item for item in parameters if item.get("id") == PARAMETER_ID), None
    )
    require(parameter is not None, f"parameter {PARAMETER_ID} is absent: {parameters!r}")
    return float(parameter["value"])


def require_png(screenshot: Any, label: str) -> tuple[int, int, str]:
    require(isinstance(screenshot, dict), f"{label} is not an object: {screenshot!r}")
    require(screenshot.get("mimeType") == "image/png", str(screenshot))
    require(screenshot.get("width", 0) > 1, str(screenshot))
    require(screenshot.get("height", 0) > 1, str(screenshot))
    png = base64.b64decode(screenshot.get("data", ""), validate=True)
    require(png.startswith(PNG_SIGNATURE), f"{label} is not a PNG")
    digest = hashlib.sha256(png).hexdigest()
    require(digest != "0" * 64, f"{label} digest is unexpectedly zero")
    return int(screenshot["width"]), int(screenshot["height"]), digest


def marketplace_mcp_command(
    config_path: Path, packaged_mcp: Path, env: dict[str, str]
) -> list[str]:
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
        server = config["mcpServers"]["pulp"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        fail(f"invalid marketplace MCP configuration {config_path}: {error}")
    require(isinstance(server, dict), f"pulp MCP registration is not an object: {server!r}")
    command_template = server.get("command")
    require(
        command_template == "${PULP_MCP_BINARY}",
        f"marketplace MCP command must use PULP_MCP_BINARY: {command_template!r}",
    )
    configured_env = server.get("env", {})
    require(isinstance(configured_env, dict), f"MCP env is not an object: {configured_env!r}")
    require(not configured_env, f"marketplace MCP config injects unexpected env: {configured_env!r}")
    env["PULP_MCP_BINARY"] = str(packaged_mcp)
    resolved = command_template.replace("${PULP_MCP_BINARY}", env["PULP_MCP_BINARY"])
    require(Path(resolved) == packaged_mcp, f"MCP config resolved outside package: {resolved}")
    require(Path(resolved).is_file(), f"resolved MCP command does not exist: {resolved}")
    return [resolved]



def exercise_clients(
    install: Path,
    client_cwd: Path,
    env: dict[str, str],
    ready: dict[str, str],
    observe_ready: dict[str, str],
    mcp_config: Path,
) -> None:
    rust = install / "pulp"
    cpp = install / "pulp-cpp"
    mcp = install / "pulp-mcp"

    def run_cli(arguments: list[str]) -> subprocess.CompletedProcess[str]:
        result = run_checked([str(rust), *arguments], cwd=client_cwd, env=env)
        require(f"fallthrough → {cpp}" in result.stderr, result.stderr)
        return result

    listed = parse_json_output(run_cli(["inspect", "list", "--json"]), "packaged list")
    identities = {
        (item.get("sessionId"), item.get("instanceId"), item.get("publicationId"))
        for item in listed.get("sessions", [])
    }
    for selected in (ready, observe_ready):
        require(
            (selected["session_id"], selected["instance_id"], selected["publication_id"])
            in identities,
            str(listed),
        )
        capabilities = parse_json_output(
            run_cli(["inspect", "capabilities", "--json", *selector_args(selected)]),
            "packaged capabilities",
        )
        require(capabilities.get("sessionId") == selected["session_id"], str(capabilities))

    removed = subprocess.run(
        [str(rust), "inspect", "--command", "DOM.getDocument"],
        cwd=client_cwd, env=env, text=True, capture_output=True, timeout=30, check=False,
    )
    require(removed.returncode == 2, str(removed))
    require("unknown inspect argument" in removed.stderr, removed.stderr)

    requests = [
        json.dumps({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}, separators=(",", ":")),
        mcp_request(2, "pulp_inspect_list", {}),
        mcp_request(3, "pulp_inspect_capabilities", selector_object(ready)),
        mcp_request(4, "pulp_inspect_dom", selector_object(ready)),
        mcp_request(5, "pulp_motion_snapshot", selector_object(ready)),
    ]
    command = marketplace_mcp_command(mcp_config, mcp, env)
    result = subprocess.run(command, input="\n".join(requests) + "\n", cwd=client_cwd,
                            env=env, text=True, capture_output=True, timeout=30, check=False)
    require(result.returncode == 0, result.stderr)
    responses = {item.get("id"): item for item in map(json.loads, result.stdout.splitlines())}
    require(responses[2].get("result", {}).get("structuredContent", {}).get("ok") is True,
            str(responses[2]))
    require(responses[3].get("result", {}).get("structuredContent", {}).get("ok") is True,
            str(responses[3]))
    require("Unknown tool: pulp_inspect_dom" in json.dumps(responses[4]), str(responses[4]))
    require("Unknown tool: pulp_motion_snapshot" in json.dumps(responses[5]), str(responses[5]))
    print("proved packaged inspector metadata clients and retired raw caller rejection")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rust", type=Path, required=True)
    parser.add_argument("--cpp", type=Path, required=True)
    parser.add_argument("--mcp", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--packager", type=Path, required=True)
    parser.add_argument("--mcp-config", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    for name in ("rust", "cpp", "mcp", "fixture", "packager", "mcp_config"):
        path = getattr(args, name).resolve()
        require(path.is_file(), f"--{name} is not a file: {path}")
        setattr(args, name, path)
    args.build_dir = args.build_dir.resolve()

    fixture: subprocess.Popen[str] | None = None
    observe_fixture: subprocess.Popen[str] | None = None
    fixture_output: tuple[str, str] = ("", "")
    with tempfile.TemporaryDirectory(prefix="pulp-packaged-inspector-") as temporary:
        scratch = Path(temporary)
        install = package_and_extract(args, scratch)
        runtime_dir = scratch / "runtime"
        client_cwd = scratch / "empty-client-cwd"
        client_cwd.mkdir()
        develop_dir = scratch / "develop"
        observe_dir = scratch / "observe"
        develop_dir.mkdir()
        observe_dir.mkdir()
        ready_path = develop_dir / "ready.json"
        stop_path = develop_dir / "stop"
        observation_path = develop_dir / "test-input-observation.json"
        observe_ready_path = observe_dir / "ready.json"
        observe_stop_path = observe_dir / "stop"
        fixture_env = os.environ.copy()
        fixture_env["PULP_INSPECTOR_RUNTIME_DIR"] = str(runtime_dir)
        for name in (
            "PULP_INSPECT_PROFILE",
            "PULP_INSPECT_RUNTIME_EVAL",
            "PULP_HEADLESS",
            "PULP_TEST_MODE",
            "CI",
        ):
            fixture_env.pop(name, None)
        try:
            fixture = subprocess.Popen(
                [str(args.fixture), "--ready", str(ready_path), "--stop", str(stop_path),
                 "--observation", str(observation_path), "--wait-until-stop"],
                cwd=scratch,
                env=fixture_env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            observe_fixture = subprocess.Popen(
                [str(args.fixture), "--ready", str(observe_ready_path),
                 "--stop", str(observe_stop_path), "--profile", "observe",
                 "--wait-until-stop"],
                cwd=scratch,
                env=fixture_env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            ready = wait_for_ready(fixture, ready_path, 30)
            observe_ready = wait_for_ready(observe_fixture, observe_ready_path, 30)
            require(ready["runtime_dir"] == str(runtime_dir), str(ready))
            require(ready["plugin_id"] == "com.pulp.test.standalone-inspector-workflow", str(ready))

            stem = f"{len(ready['session_id'])}-{ready['session_id']}-{ready['instance_id']}"
            record_path = runtime_dir / f"{stem}.json"
            token_path = runtime_dir / f"{stem}.token"
            lock_path = runtime_dir / f"{stem}.lock"
            require(record_path.is_file(), f"missing live discovery record {record_path}")
            require(token_path.is_file(), f"missing live credential {token_path}")
            require(lock_path.is_file(), f"missing live ownership lock {lock_path}")

            exercise_clients(
                install,
                client_cwd,
                scrubbed_client_environment(runtime_dir),
                ready,
                observe_ready,
                args.mcp_config,
            )
            stop_path.write_text("stop\n", encoding="utf-8")
            observe_stop_path.write_text("stop\n", encoding="utf-8")
            fixture_output = fixture.communicate(timeout=30)
            observe_output = observe_fixture.communicate(timeout=30)
            require(
                fixture.returncode == 0,
                f"fixture teardown failed ({fixture.returncode})\n"
                f"stdout:\n{fixture_output[0]}\nstderr:\n{fixture_output[1]}",
            )
            require(not record_path.exists(), f"stale discovery record: {record_path}")
            require(not token_path.exists(), f"stale discovery credential: {token_path}")
            require(lock_path.is_file(), f"ownership lock was not retained: {lock_path}")
            require(
                len(list(runtime_dir.iterdir())) == 2
                and all(path.suffix == ".lock" for path in runtime_dir.iterdir()),
                f"runtime directory contains active residue: {list(runtime_dir.iterdir())!r}",
            )
            require(observe_fixture.returncode == 0, f"observe fixture teardown failed: {observe_output!r}")
            print("proved fixture teardown left only its inert ownership lock")
        finally:
            if fixture is not None and fixture.poll() is None:
                stop_path.write_text("stop\n", encoding="utf-8")
                try:
                    fixture_output = fixture.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    fixture.terminate()
                    try:
                        fixture_output = fixture.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        fixture.kill()
                        fixture_output = fixture.communicate(timeout=5)
            if observe_fixture is not None and observe_fixture.poll() is None:
                observe_stop_path.write_text("stop\n", encoding="utf-8")
                try:
                    observe_fixture.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    observe_fixture.terminate()
                    observe_fixture.communicate(timeout=5)
            if fixture is not None and fixture.returncode not in (None, 0):
                print(f"fixture stdout:\n{fixture_output[0]}", file=sys.stderr)
                print(f"fixture stderr:\n{fixture_output[1]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
