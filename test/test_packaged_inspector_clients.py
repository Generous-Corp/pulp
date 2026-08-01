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
    mcp_config: Path,
) -> None:
    rust = install / "pulp"
    cpp = install / "pulp-cpp"
    mcp = install / "pulp-mcp"
    exact_selector = selector_args(ready)

    def run_cli(arguments: list[str]) -> subprocess.CompletedProcess[str]:
        result = run_checked([str(rust), *arguments], cwd=client_cwd, env=env)
        require(
            f"fallthrough → {cpp}" in result.stderr,
            "Rust client did not delegate to its extracted sibling:\n" + result.stderr,
        )
        return result

    listed_result = run_cli(["inspect", "list", "--json"])
    listed = parse_json_output(listed_result, "packaged Rust inspect list")
    require(listed.get("schemaVersion") == "pulp.inspect.sessions.v1", str(listed))
    require(len(listed.get("sessions", [])) == 1, f"unexpected sessions: {listed!r}")
    session = listed["sessions"][0]
    require(session.get("sessionId") == ready["session_id"], str(session))
    require(session.get("instanceId") == ready["instance_id"], str(session))
    require(session.get("publicationId") == ready["publication_id"], str(session))
    require(session.get("pluginId") == ready["plugin_id"], str(session))
    capabilities_result = run_cli(
        [
            "inspect",
            "capabilities",
            "--json",
            *exact_selector,
        ]
    )
    capabilities = parse_json_output(capabilities_result, "packaged CLI capabilities")
    require(capabilities.get("schemaVersion") == "pulp.inspect.capabilities.v1", str(capabilities))
    capability_session = capabilities.get("session", {})
    require(capability_session.get("sessionId") == ready["session_id"], str(capabilities))
    require(capability_session.get("instanceId") == ready["instance_id"], str(capabilities))
    require(capability_session.get("publicationId") == ready["publication_id"], str(capabilities))
    require("state.write" in capabilities.get("policy", {}).get("effective", []), str(capabilities))

    parameters_result = run_cli(
        ["inspect", "--json", *exact_selector, "--command", "State.getParameters"]
    )
    parameters = parse_json_output(parameters_result, "packaged CLI parameter read")
    require(abs(parameter_value(parameters) - RESET_PARAMETER_VALUE) < 0.001, str(parameters))

    mutation_result = run_cli(
        [
            "inspect",
            "set-parameter",
            "--id",
            str(PARAMETER_ID),
            "--value",
            str(CLI_PARAMETER_VALUE),
            "--json",
            *exact_selector,
        ]
    )
    mutation = parse_json_output(mutation_result, "packaged Rust parameter mutation")
    require(mutation.get("schemaVersion") == "pulp.inspect.set-parameter.v1", str(mutation))
    require(mutation.get("parameterId") == PARAMETER_ID, str(mutation))
    require(mutation.get("value") == CLI_PARAMETER_VALUE, str(mutation))
    require(mutation.get("result", {}).get("ok") is True, str(mutation))

    reread_result = run_cli(
        ["inspect", "--json", *exact_selector, "--command", "State.getParameters"]
    )
    reread = parse_json_output(reread_result, "packaged CLI parameter reread")
    require(abs(parameter_value(reread) - CLI_PARAMETER_VALUE) < 0.001, str(reread))

    transport_result = run_cli(
        [
            "inspect",
            "set-transport",
            "--playing",
            "false",
            "--position-samples",
            "96000",
            "--tempo-bpm",
            "90",
            "--json",
            *exact_selector,
        ]
    )
    transport = parse_json_output(transport_result, "packaged CLI transport mutation")
    require(transport.get("schemaVersion") == "pulp.inspect.set-transport.v1", str(transport))
    require(transport.get("result", {}).get("applied") is True, str(transport))

    midi_result = run_cli(
        [
            "inspect",
            "inject-midi",
            "--kind",
            "note_on",
            "--channel",
            "3",
            "--note",
            "64",
            "--velocity",
            "99",
            "--json",
            *exact_selector,
        ]
    )
    midi = parse_json_output(midi_result, "packaged CLI MIDI injection")
    require(midi.get("schemaVersion") == "pulp.inspect.inject-midi.v1", str(midi))
    require(midi.get("result", {}).get("accepted") is True, str(midi))

    dom_result = run_cli(
        ["inspect", "--json", *exact_selector, "--command", "DOM.getDocument"]
    )
    dom = parse_json_output(dom_result, "packaged CLI inspector DOM")
    require("REAL STANDALONE INSPECTOR WORKFLOW" in dom_result.stdout, str(dom))
    require(find_dict_with_id(dom, "workflow-gain") is not None, str(dom))

    screenshot_result = run_cli(
        ["inspect", "--json", *exact_selector, "--command", "Capture.screenshot"]
    )
    cli_screenshot = parse_json_output(screenshot_result, "packaged CLI screenshot")
    cli_width, cli_height, cli_digest = require_png(cli_screenshot, "CLI screenshot")

    reset_result = run_cli(
        [
            "inspect",
            "set-parameter",
            "--id",
            str(PARAMETER_ID),
            "--value",
            str(RESET_PARAMETER_VALUE),
            "--json",
            *exact_selector,
        ]
    )
    reset = parse_json_output(reset_result, "packaged CLI state reset")
    require(reset.get("result", {}).get("ok") is True, str(reset))
    reset_read = parse_json_output(
        run_cli(["inspect", "--json", *exact_selector, "--command", "State.getParameters"]),
        "packaged CLI reset verification",
    )
    require(abs(parameter_value(reset_read) - RESET_PARAMETER_VALUE) < 0.001, str(reset_read))

    selector = selector_object(ready)
    requests = [
        json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "packaged-inspector-proof", "version": "1"},
                },
            },
            separators=(",", ":"),
        ),
        mcp_request(2, "pulp_inspect_list", {}),
        mcp_request(3, "pulp_inspect_capabilities", selector),
        mcp_request(4, "pulp_inspect_context", selector),
        mcp_request(5, "pulp_inspect_params", selector),
        mcp_request(
            6,
            "pulp_inspect_set_param",
            {**selector, "id": PARAMETER_ID, "value": MCP_PARAMETER_VALUE},
        ),
        mcp_request(7, "pulp_inspect_params", selector),
        mcp_request(8, "pulp_inspect_dom", selector),
        mcp_request(9, "pulp_inspect_screenshot", selector),
        mcp_request(
            10,
            "pulp_inspect_set_transport",
            {**selector, "playing": False, "position_samples": 96000, "tempo_bpm": 90},
        ),
        mcp_request(
            11,
            "pulp_inspect_inject_midi",
            {**selector, "kind": "note_on", "channel": 3, "note": 64, "velocity": 99},
        ),
    ]
    mcp_env = env.copy()
    mcp_command = marketplace_mcp_command(mcp_config, mcp, mcp_env)
    mcp_result = subprocess.run(
        mcp_command,
        input="\n".join(requests) + "\n",
        cwd=client_cwd,
        env=mcp_env,
        text=True,
        capture_output=True,
        timeout=45,
        check=False,
    )
    if mcp_result.returncode != 0:
        fail(
            f"packaged MCP exited {mcp_result.returncode}\n"
            f"stdout:\n{mcp_result.stdout}\nstderr:\n{mcp_result.stderr}"
        )
    try:
        responses = [json.loads(line) for line in mcp_result.stdout.splitlines() if line]
    except json.JSONDecodeError as error:
        fail(f"packaged MCP emitted invalid JSON: {error}\n{mcp_result.stdout}")
    by_id = {response.get("id"): response for response in responses}
    require(set(by_id) == set(range(1, 12)), f"unexpected MCP responses: {responses!r}")
    require(by_id[1].get("result", {}).get("serverInfo", {}).get("name") == "pulp-mcp", str(by_id[1]))

    structured = {
        request_id: by_id[request_id].get("result", {}).get("structuredContent")
        for request_id in range(2, 12)
    }
    for request_id, payload in structured.items():
        require(isinstance(payload, dict), f"MCP response {request_id} lacks structured content: {by_id[request_id]!r}")
        require(payload.get("ok") is True, f"MCP response {request_id} failed: {payload!r}")

    mcp_sessions = structured[2].get("sessions", [])
    require(len(mcp_sessions) == 1, str(structured[2]))
    require(mcp_sessions[0].get("sessionId") == ready["session_id"], str(structured[2]))
    require(mcp_sessions[0].get("pluginId") == ready["plugin_id"], str(structured[2]))

    for request_id in range(3, 12):
        identity = structured[request_id].get("session", {})
        require_selected_session(identity, ready)

    capability_policy = structured[3].get("result", {})
    require("state.write" in capability_policy.get("effective", []), str(structured[3]))
    require(ready["plugin_id"] in json.dumps(structured[3]), str(structured[3]))
    require(ready["session_id"] in json.dumps(structured[4]), str(structured[4]))
    require(ready["plugin_id"] in json.dumps(structured[4]), str(structured[4]))

    require(
        abs(parameter_value(structured[5].get("result")) - RESET_PARAMETER_VALUE) < 0.001,
        str(structured[5]),
    )
    require(structured[6].get("result", {}).get("ok") is True, str(structured[6]))
    require(
        abs(parameter_value(structured[7].get("result")) - MCP_PARAMETER_VALUE) < 0.001,
        str(structured[7]),
    )

    mcp_dom = structured[8].get("result")
    require("REAL STANDALONE INSPECTOR WORKFLOW" in json.dumps(mcp_dom), str(structured[8]))
    require(find_dict_with_id(mcp_dom, "workflow-gain") is not None, str(structured[8]))

    screenshot = structured[9].get("result", {})
    mcp_width, mcp_height, mcp_digest = require_png(screenshot, "MCP screenshot")
    require(structured[10].get("result", {}).get("applied") is True, str(structured[10]))
    require(structured[11].get("result", {}).get("accepted") is True, str(structured[11]))
    print(
        "proved independent packaged CLI and marketplace-configured MCP "
        "list/capabilities/read/mutate/reread/capture/test-input workflows; "
        f"cli-screenshot={cli_width}x{cli_height} sha256={cli_digest}; "
        f"mcp-screenshot={mcp_width}x{mcp_height} sha256={mcp_digest}"
    )


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
    fixture_output: tuple[str, str] = ("", "")
    with tempfile.TemporaryDirectory(prefix="pulp-packaged-inspector-") as temporary:
        scratch = Path(temporary)
        install = package_and_extract(args, scratch)
        runtime_dir = scratch / "runtime"
        client_cwd = scratch / "empty-client-cwd"
        client_cwd.mkdir()
        ready_path = scratch / "ready.json"
        stop_path = scratch / "stop"
        observation_path = scratch / "test-input-observation.json"
        fixture_env = os.environ.copy()
        fixture_env["PULP_INSPECTOR_RUNTIME_DIR"] = str(runtime_dir)
        for name in ("PULP_INSPECT_PROFILE", "PULP_HEADLESS", "PULP_TEST_MODE", "CI"):
            fixture_env.pop(name, None)
        try:
            fixture = subprocess.Popen(
                [str(args.fixture), "--ready", str(ready_path), "--stop", str(stop_path),
                 "--observation", str(observation_path)],
                cwd=scratch,
                env=fixture_env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            ready = wait_for_ready(fixture, ready_path, 30)
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
                args.mcp_config,
            )
            observation_deadline = time.monotonic() + 5
            observation: dict[str, Any] = {}
            while time.monotonic() < observation_deadline:
                try:
                    observation = json.loads(observation_path.read_text(encoding="utf-8"))
                except (FileNotFoundError, json.JSONDecodeError, OSError):
                    time.sleep(0.02)
                    continue
                if (
                    observation.get("note_on_count", 0) >= 2
                    and observation.get("note_off_count", 0) >= 2
                    and observation.get("transport_match") is True
                ):
                    break
                time.sleep(0.02)
            require(observation.get("note_on_count", 0) >= 2, str(observation))
            require(observation.get("note_off_count", 0) >= 2, str(observation))
            require(observation.get("transport_match") is True, str(observation))
            stop_path.write_text("stop\n", encoding="utf-8")
            fixture_output = fixture.communicate(timeout=30)
            require(
                fixture.returncode == 0,
                f"fixture teardown failed ({fixture.returncode})\n"
                f"stdout:\n{fixture_output[0]}\nstderr:\n{fixture_output[1]}",
            )
            require(not record_path.exists(), f"stale discovery record: {record_path}")
            require(not token_path.exists(), f"stale discovery credential: {token_path}")
            require(lock_path.is_file(), f"ownership lock was not retained: {lock_path}")
            require(
                list(runtime_dir.iterdir()) == [lock_path],
                f"runtime directory contains active residue: {list(runtime_dir.iterdir())!r}",
            )
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
