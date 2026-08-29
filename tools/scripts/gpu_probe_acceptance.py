#!/usr/bin/env python3
"""Record exact-head A2 all-four installed CLI/MCP and Forge acceptance."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import gpu_trace_overhead_acceptance as provenance
import verify_gpu_probe_acceptance as verifier


RECIPES = {
    "compute": "gpu-compute.magnitude.v1",
    "stft": "gpu-audio.stft.v1",
    "renderer": "renderer3d.hardcoded-cube.v1",
    "threejs": "threejs.multi-pass.v1",
}
MISSING_FORGE_PATH_CANARIES = {
    "gpu_audio": "stft",
    "threejs": "threejs",
}
SYSTEM_PATH = "/usr/bin:/bin:/usr/sbin:/sbin"
MAX_COMMAND_OUTPUT = 4 * 1024 * 1024


class AcceptanceError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_bounded(
    command: list[str], *, cwd: Path, environment: dict[str, str], timeout: int
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command, cwd=cwd, env=environment, check=False,
        capture_output=True, text=True, timeout=timeout,
    )
    if len(completed.stdout.encode()) + len(completed.stderr.encode()) > MAX_COMMAND_OUTPUT:
        raise AcceptanceError(f"command exceeded bounded output: {command[0]}")
    return completed


def cmake_cache(build_dir: Path) -> dict[str, str]:
    path = build_dir / "CMakeCache.txt"
    try:
        payload = path.read_text(encoding="utf-8")
    except OSError as error:
        raise AcceptanceError(f"cannot read CMake cache {path}: {error}") from error
    if len(payload.encode()) > 4 * 1024 * 1024:
        raise AcceptanceError("CMake cache exceeds 4 MiB")
    values: dict[str, str] = {}
    for line in payload.splitlines():
        match = re.match(r"^([^#/:=]+)(?::[^=]+)?=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def require_release_pulp_build(build_dir: Path, repository: Path) -> dict[str, str]:
    values = cmake_cache(build_dir)
    try:
        home = Path(values["CMAKE_HOME_DIRECTORY"]).resolve()
    except KeyError as error:
        raise AcceptanceError("Pulp CMake cache lacks CMAKE_HOME_DIRECTORY") from error
    if home != repository.resolve() or values.get("CMAKE_BUILD_TYPE") != "Release":
        raise AcceptanceError("Pulp build must be a single-config Release of the exact source")
    requirements = {
        "PULP_ENABLE_GPU": "ON",
        "PULP_ENABLE_SCENE3D": "ON",
        "PULP_ENABLE_THREEJS_RUNTIME": "ON",
        "PULP_ENABLE_JS": "ON",
        "PULP_JS_ENGINE": "v8",
        "PULP_BUILD_RUST_CLI": "ON",
        "PULP_RUST_CLI_PROFILE": "release",
        "PULP_HAS_THREEJS": "TRUE",
    }
    for key, expected in requirements.items():
        if values.get(key) != expected:
            raise AcceptanceError(f"Pulp build requires {key}={expected}")
    return values


def refresh_install(
    repository: Path, build_dir: Path, prefix: Path, build_environment: dict[str, str]
) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    cache = require_release_pulp_build(build_dir, repository)
    build = run_bounded(
        ["cmake", "--build", str(build_dir), "--target",
         "pulp-rust-cli", "pulp-cli", "pulp-mcp", "--parallel"],
        cwd=repository, environment=build_environment, timeout=3600,
    )
    if build.returncode != 0:
        raise AcceptanceError(f"Pulp CLI/MCP refresh failed: {build.stderr[-2000:]}")
    install = run_bounded(
        ["cmake", "--install", str(build_dir), "--prefix", str(prefix)],
        cwd=repository, environment=build_environment, timeout=1800,
    )
    if install.returncode != 0:
        raise AcceptanceError(f"Pulp install refresh failed: {install.stderr[-2000:]}")
    installed_paths = {
        "installed_rust_cli": prefix / "bin/pulp",
        "installed_cpp_delegate": prefix / "bin/pulp-cpp",
        "installed_mcp": prefix / "bin/pulp-mcp",
    }
    build_paths = {
        "installed_rust_cli": build_dir / "pulp",
        "installed_cpp_delegate": build_dir / "tools/cli/pulp-cpp",
        "installed_mcp": build_dir / "tools/mcp/pulp-mcp",
    }
    binaries: dict[str, dict[str, Any]] = {}
    for role in installed_paths:
        installed_path = installed_paths[role]
        built_path = build_paths[role]
        if (
            not installed_path.is_file() or installed_path.is_symlink()
            or not built_path.is_file() or built_path.is_symlink()
            or not os.access(installed_path, os.X_OK)
        ):
            raise AcceptanceError(f"{role} is not a regular refreshed executable")
        installed_digest = sha256(installed_path)
        build_digest = sha256(built_path)
        if installed_digest != build_digest:
            raise AcceptanceError(f"{role} installed bytes differ from the refreshed build")
        binaries[role] = {
            "sha256": installed_digest,
            "bytes": installed_path.stat().st_size,
            "build_output_sha256": build_digest,
        }
    if binaries["installed_rust_cli"]["sha256"] == binaries["installed_cpp_delegate"]["sha256"]:
        raise AcceptanceError("installed pulp is a C++ copy, not the required Rust front")
    revision = provenance._git_text(repository, "rev-parse", "HEAD")
    installed_identity = provenance.installed_source_identity(
        repository, revision, installed_paths["installed_rust_cli"], installed_paths["installed_mcp"]
    )
    return ({
        "cmake_cache_sha256": sha256(build_dir / "CMakeCache.txt"),
        "cmake_home_revision": revision,
        "cmake_build_type": cache["CMAKE_BUILD_TYPE"],
        "rust_profile": cache["PULP_RUST_CLI_PROFILE"],
        "feature_contract": {
            key: cache[key] for key in (
                "PULP_ENABLE_GPU", "PULP_ENABLE_SCENE3D",
                "PULP_ENABLE_THREEJS_RUNTIME", "PULP_ENABLE_JS",
                "PULP_JS_ENGINE", "PULP_BUILD_RUST_CLI", "PULP_HAS_THREEJS",
            )
        },
        "build_info": installed_identity["build_info"],
        "build_info_sha256": installed_identity["build_info_sha256"],
        "build_install_binary_identity": "pass",
    }, binaries)


def canonical_result(value: dict[str, Any]) -> dict[str, Any]:
    copied = json.loads(json.dumps(value))
    copied.pop("gpu_evidence_id", None)
    return copied


def bind_missing_forge_path_canaries(
    cli_results: dict[str, dict[str, dict[str, Any]]],
    mcp_results: dict[str, dict[str, dict[str, Any]]],
) -> dict[str, dict[str, Any]]:
    """Bind Forge's missing paths to the A2 executions that actually cover them."""
    canaries: dict[str, dict[str, Any]] = {}
    for canary, group in MISSING_FORGE_PATH_CANARIES.items():
        cli = cli_results.get(group, {})
        mcp = mcp_results.get(group, {})
        recipe = RECIPES[group]
        if (
            cli.get("run1", {}).get("verdict") != "pass"
            or cli.get("run2", {}).get("verdict") != "pass"
            or cli.get("negative", {}).get("verdict") != "fail"
            or mcp.get("positive", {}).get("verdict") != "pass"
            or mcp.get("negative", {}).get("verdict") != "fail"
            or canonical_result(cli.get("run1", {}))
            != canonical_result(mcp.get("positive", {}))
            or canonical_result(cli.get("negative", {}))
            != canonical_result(mcp.get("negative", {}))
        ):
            raise AcceptanceError(f"{canary} missing-path canary was not actually exercised")
        group_index = list(RECIPES).index(group)
        canaries[canary] = {
            "status": "pass",
            "recipe": recipe,
            "cli_positive_files": [f"{group}-run1.json", f"{group}-run2.json"],
            "cli_negative_file": f"{group}-negative.json",
            "mcp_positive_response_id": 2 + (2 * group_index),
            "mcp_negative_response_id": 3 + (2 * group_index),
        }
    return canaries


def run_cli_probe(
    cli: Path,
    recipe: str,
    artifacts: Path,
    *,
    negative: bool,
    cwd: Path,
    environment: dict[str, str],
) -> dict[str, Any]:
    command = [str(cli), "gpu", "probe", "--recipe", recipe,
               "--artifacts", str(artifacts), "--json"]
    if negative:
        command.append("--negative-control")
    completed = run_bounded(command, cwd=cwd, environment=environment, timeout=300)
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise AcceptanceError(f"installed CLI returned malformed probe JSON: {error}") from error
    expected_exit = 1 if negative else 0
    expected_verdict = "fail" if negative else "pass"
    if (
        completed.returncode != expected_exit or result.get("verdict") != expected_verdict
        or result.get("recipe_id") != recipe
        or bool(result.get("mutation")) != negative
    ):
        raise AcceptanceError(f"installed CLI did not preserve {recipe} typed status")
    return result


class McpSession:
    def __init__(self, executable: Path, cwd: Path, environment: dict[str, str]):
        self.process = subprocess.Popen(
            [str(executable)], cwd=cwd, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self.next_id = 1
        self.transcript: list[dict[str, Any]] = []

    def request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        request_id = self.next_id
        self.next_id += 1
        request: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params
        assert self.process.stdin is not None and self.process.stdout is not None
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        ready, _, _ = select.select([self.process.stdout], [], [], 310)
        if not ready:
            raise AcceptanceError("installed MCP response exceeded five minutes")
        line = self.process.stdout.readline()
        if not line or len(line.encode()) > 1024 * 1024:
            raise AcceptanceError("installed MCP returned no bounded response")
        response = json.loads(line)
        if response.get("id") != request_id or not isinstance(response.get("result"), dict):
            raise AcceptanceError("installed MCP returned an incoherent response")
        self.transcript.append(response)
        return response

    def initialize(self) -> None:
        response = self.request("initialize")
        if response["result"].get("protocolVersion") != "2024-11-05":
            raise AcceptanceError("installed MCP protocol version drifted")
        assert self.process.stdin is not None
        self.process.stdin.write(
            '{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
        )
        self.process.stdin.flush()

    def probe(self, recipe: str, artifacts: Path, *, negative: bool) -> dict[str, Any]:
        response = self.request("tools/call", {
            "name": "pulp_gpu_probe",
            "arguments": {
                "recipe": recipe, "artifacts": str(artifacts),
                "negative_control": negative,
            },
        })
        result = response["result"]
        structured = result.get("structuredContent")
        if not isinstance(structured, dict) or not isinstance(structured.get("evidence"), dict):
            raise AcceptanceError("installed MCP lacks structured probe evidence")
        evidence = structured["evidence"]
        expected_exit = 1 if negative else 0
        if structured.get("exit_code") != expected_exit or bool(result.get("isError", False)) != negative:
            raise AcceptanceError("installed MCP did not preserve typed probe status")
        try:
            text_evidence = json.loads(result["content"][0]["text"])
        except (IndexError, KeyError, TypeError, json.JSONDecodeError) as error:
            raise AcceptanceError("installed MCP text evidence is malformed") from error
        if text_evidence != evidence:
            raise AcceptanceError("installed MCP text and structured probe evidence differ")
        return evidence

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)
        stderr = self.process.stderr.read() if self.process.stderr else ""
        if self.process.returncode != 0 or len(stderr.encode()) > MAX_COMMAND_OUTPUT:
            raise AcceptanceError(f"installed MCP exited abnormally: {stderr[-1000:]}")


def forge_source_identity(repository: Path, pulp_revision: str) -> dict[str, Any]:
    if provenance._git_text(repository, "rev-parse", "HEAD") != verifier.EXPECTED_FORGE_REVISION:
        raise AcceptanceError("Forge checkout is not the accepted exact revision")
    origin = provenance._git_text(repository, "config", "--get", "remote.origin.url")
    if not re.fullmatch(
        r"(?:git@github\.com:|https://github\.com/)Generous-Corp/forge(?:\.git)?", origin
    ):
        raise AcceptanceError("Forge checkout does not use the canonical origin")
    staged = provenance._git_text(repository, "diff", "--cached", "--name-only")
    changed = provenance._git_text(repository, "diff", "--name-only")
    untracked = provenance._git_text(repository, "ls-files", "--others", "--exclude-standard")
    if staged or changed.splitlines() != ["PULP_SDK_REF"] or untracked:
        raise AcceptanceError("Forge may differ only by an unstaged PULP_SDK_REF overlay")
    ref = (repository / "PULP_SDK_REF").read_text(encoding="utf-8").strip()
    if ref != pulp_revision:
        raise AcceptanceError("Forge PULP_SDK_REF overlay does not name this Pulp revision")
    original_blob = provenance._git_text(
        repository, "rev-parse", f"{verifier.EXPECTED_FORGE_REVISION}:PULP_SDK_REF"
    )
    return {
        "repository": "Generous-Corp/forge",
        "revision": verifier.EXPECTED_FORGE_REVISION,
        "pulp_sdk_ref_overlay": {
            "path": "PULP_SDK_REF", "content": ref, "original_blob": original_blob,
        },
        "all_other_tracked_files_clean": True,
    }


def parse_forge_stamp(path: Path) -> dict[str, str]:
    data = path.read_bytes()
    if len(data) > 16 * 1024:
        raise AcceptanceError("FORGE_BUILD_INFO exceeds 16 KiB")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AcceptanceError("FORGE_BUILD_INFO is not UTF-8") from error
    values: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            raise AcceptanceError("FORGE_BUILD_INFO contains a malformed line")
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[a-z_]+", key) or key in values:
            raise AcceptanceError("FORGE_BUILD_INFO contains duplicate/invalid keys")
        values[key] = value
    required = {"schema", "version", "packaged", "product", "product_id",
                "role", "format", "build", "pulp_sdk"}
    if not required.issubset(values):
        raise AcceptanceError("FORGE_BUILD_INFO lacks required identity fields")
    return values


def prove_forge(
    repository: Path,
    build_dir: Path,
    prefix: Path,
    pulp_revision: str,
    staging: Path,
    build_environment: dict[str, str],
    runtime_environment: dict[str, str],
) -> dict[str, Any]:
    identity = forge_source_identity(repository, pulp_revision)
    cache = cmake_cache(build_dir)
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != repository.resolve():
        raise AcceptanceError("Forge build is not configured from the exact Forge checkout")
    if cache.get("CMAKE_BUILD_TYPE") != "Release":
        raise AcceptanceError("Forge build must be single-config Release")
    expected_pulp_dir = (prefix / "lib/cmake/Pulp").resolve()
    if Path(cache.get("Pulp_DIR", "")).resolve() != expected_pulp_dir:
        raise AcceptanceError("Forge build does not consume the accepted Pulp install")
    build = run_bounded(
        ["cmake", "--build", str(build_dir), "--target", "ForgeModular_Standalone", "--parallel"],
        cwd=repository, environment=build_environment, timeout=3600,
    )
    if build.returncode != 0:
        raise AcceptanceError(f"Forge Modular refresh failed: {build.stderr[-2000:]}")
    stamps = []
    for candidate in build_dir.rglob("FORGE_BUILD_INFO"):
        if len(stamps) >= 64:
            raise AcceptanceError("Forge build contains too many build-info candidates")
        if candidate.is_file() and not candidate.is_symlink():
            values = parse_forge_stamp(candidate)
            if values.get("product") == "Forge Modular" and values.get("format") == "Standalone application":
                stamps.append((candidate, values))
    if len(stamps) != 1:
        raise AcceptanceError("Forge build must contain one exact Modular standalone stamp")
    stamp_path, stamp = stamps[0]
    bundle = stamp_path.parents[2]
    if bundle.suffix != ".app":
        raise AcceptanceError("Forge Modular stamp is not inside an app bundle")
    executables = [path for path in (bundle / "Contents/MacOS").iterdir()
                   if path.is_file() and not path.is_symlink() and os.access(path, os.X_OK)]
    if len(executables) != 1:
        raise AcceptanceError("Forge Modular bundle lacks one exact executable")
    binary = executables[0]
    signed = run_bounded(
        ["/usr/bin/codesign", "--verify", "--deep", "--strict", str(bundle)],
        cwd=repository, environment=runtime_environment, timeout=60,
    )
    if signed.returncode != 0:
        raise AcceptanceError(f"Forge Modular signature verification failed: {signed.stderr}")
    screenshot = staging / "forge-modular-screenshot.png"
    capture = run_bounded(
        [str(binary), "--screenshot", str(screenshot)],
        cwd=repository, environment=runtime_environment, timeout=300,
    )
    if capture.returncode != 0:
        raise AcceptanceError(f"Forge Modular screenshot smoke failed: {capture.stderr[-2000:]}")
    metrics = verifier._png_metrics(screenshot)
    doctor_path = staging / "forge-gpu-doctor.json"
    doctor = run_bounded(
        [str(prefix / "bin/pulp"), "doctor", "gpu", "--json"],
        cwd=repository, environment=runtime_environment, timeout=300,
    )
    if doctor.returncode != 0:
        raise AcceptanceError(f"Forge-cwd GPU doctor failed: {doctor.stderr[-2000:]}")
    try:
        doctor_payload = json.loads(doctor.stdout)
    except json.JSONDecodeError as error:
        raise AcceptanceError("Forge-cwd GPU doctor returned malformed JSON") from error
    doctor_path.write_text(json.dumps(doctor_payload, indent=2, sort_keys=True) + "\n")
    if forge_source_identity(repository, pulp_revision) != identity:
        raise AcceptanceError("Forge source identity changed during downstream proof")
    return {
        **identity,
        "cmake_cache_sha256": sha256(build_dir / "CMakeCache.txt"),
        "build_target": "ForgeModular_Standalone",
        "bundle_build_info": stamp,
        "bundle_build_info_sha256": sha256(stamp_path),
        "bundle_binary_sha256": sha256(binary),
        "codesign_verify": "pass",
        "screenshot_metrics": metrics,
    }


def source_blobs(repository: Path, revision: str) -> dict[str, str]:
    paths = verifier.EXPECTED_SOURCE_BLOBS_V2
    historical = verifier._git_blobs(revision, paths)
    head = verifier._git_blobs("HEAD", paths)
    checkout = verifier._checkout_blobs(paths)
    if set(historical) != paths or historical != head or historical != checkout:
        raise AcceptanceError("A2 recorder/verifier/recipe source is not exact-head clean")
    return historical


def ensure_outside_checkout(path: Path, repositories: tuple[Path, ...]) -> None:
    resolved = path.resolve()
    for repository in repositories:
        try:
            resolved.relative_to(repository.resolve())
        except ValueError:
            continue
        raise AcceptanceError("A2 output/execution path must be outside every checkout")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--install-prefix", type=Path, required=True)
    parser.add_argument("--planning-repository", type=Path, required=True)
    parser.add_argument("--plan-revision", required=True)
    parser.add_argument("--plan-sha256", required=True)
    parser.add_argument("--forge-repository", type=Path, required=True)
    parser.add_argument("--forge-build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args(argv)

    repository = args.repository.resolve()
    build_dir = args.build_dir.resolve()
    prefix = args.install_prefix.resolve()
    planning = args.planning_repository.resolve()
    forge_repository = args.forge_repository.resolve()
    forge_build = args.forge_build_dir.resolve()
    output = args.output_dir.absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        parser.error("output-dir must be a new path under an existing directory")
    try:
        script_repository = SCRIPT_DIR.parents[1].resolve()
        if repository != script_repository:
            raise AcceptanceError(
                "repository must be the exact checkout containing this recorder/verifier"
            )
        for generated_path in (build_dir, prefix, forge_build):
            ensure_outside_checkout(
                generated_path, (repository, planning, forge_repository)
            )
        ensure_outside_checkout(output, (repository, planning, forge_repository))
        revision = provenance._git_text(repository, "rev-parse", "HEAD")
        source_identity = provenance.clean_source_identity(repository, revision)
        accepted_plan = provenance.plan_identity(
            planning, args.plan_revision, args.plan_sha256
        )
        blobs = source_blobs(repository, revision)
        build_environment = os.environ.copy()
        for inherited_override in (
            "PULP_SKIP_RUST_CLI",
            "PULP_USE_CPP",
            "PULP_RS_CPP_BINARY",
            "PULP_RS_FALLTHROUGH",
            "PULP_RS_NO_FALLTHROUGH",
        ):
            build_environment.pop(inherited_override, None)
        runtime_environment = build_environment.copy()
        runtime_environment["PATH"] = SYSTEM_PATH
        install_provenance, binaries = refresh_install(
            repository, build_dir, prefix, build_environment
        )
        cli = prefix / "bin/pulp"
        mcp = prefix / "bin/pulp-mcp"
        with tempfile.TemporaryDirectory(prefix="pulp-a2-execution-") as execution_name, \
             tempfile.TemporaryDirectory(prefix="pulp-a2-receipt-", dir=output.parent) as staging_name:
            execution = Path(execution_name).resolve()
            staging = Path(staging_name).resolve()
            ensure_outside_checkout(execution, (repository, planning, forge_repository))
            git_probe = subprocess.run(
                ["git", "-C", str(execution), "rev-parse", "--is-inside-work-tree"],
                check=False, capture_output=True, text=True,
            )
            if git_probe.returncode == 0:
                raise AcceptanceError("fresh execution directory unexpectedly belongs to Git")
            cli_results: dict[str, dict[str, dict[str, Any]]] = {}
            for group, recipe in RECIPES.items():
                cli_results[group] = {}
                for suffix, negative in (("run1", False), ("run2", False), ("negative", True)):
                    result = run_cli_probe(
                        cli, recipe, execution / f"{group}-cli-{suffix}-artifacts",
                        negative=negative, cwd=execution,
                        environment=runtime_environment,
                    )
                    cli_results[group][suffix] = result
                    (staging / f"{group}-{suffix}.json").write_text(
                        json.dumps(result, indent=2, sort_keys=True) + "\n"
                    )
                if canonical_result(cli_results[group]["run1"]) != canonical_result(cli_results[group]["run2"]):
                    raise AcceptanceError(f"{recipe} installed CLI rerun was nondeterministic")
            mcp_results: dict[str, dict[str, dict[str, Any]]] = {
                group: {} for group in RECIPES
            }
            session = McpSession(mcp, execution, runtime_environment)
            try:
                session.initialize()
                for group, recipe in RECIPES.items():
                    for suffix, negative in (("positive", False), ("negative", True)):
                        evidence = session.probe(
                            recipe, execution / f"{group}-mcp-{suffix}-artifacts",
                            negative=negative,
                        )
                        mcp_results[group][suffix] = evidence
                        cli_key = "negative" if negative else "run1"
                        if canonical_result(evidence) != canonical_result(cli_results[group][cli_key]):
                            raise AcceptanceError(f"{recipe} installed CLI/MCP parity failed")
            finally:
                session.close()
            (staging / "mcp-transcript.jsonl").write_text(
                "".join(json.dumps(row, sort_keys=True) + "\n" for row in session.transcript)
            )
            missing_path_canaries = bind_missing_forge_path_canaries(
                cli_results, mcp_results
            )
            forge = prove_forge(
                forge_repository, forge_build, prefix, revision, staging,
                build_environment, runtime_environment,
            )
            forge["missing_path_canaries"] = missing_path_canaries
            raw_names = {
                *(f"{group}-{suffix}.json" for group in RECIPES
                  for suffix in ("run1", "run2", "negative")),
                "mcp-transcript.jsonl", "forge-modular-screenshot.png",
                "forge-gpu-doctor.json",
            }
            receipt = {
                "schema": "pulp.gpu-probe-acceptance-receipt.v2",
                "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "integration_head": revision,
                "source_identity": source_identity,
                "source_blobs": blobs,
                "accepted_plan": accepted_plan,
                "execution_context": {
                    "cwd_role": "fresh-temporary-directory-outside-any-checkout",
                    "path": SYSTEM_PATH,
                    "checkout_discovery": "explicitly-rejected-by-git-rev-parse",
                },
                "install_provenance": install_provenance,
                "binaries": binaries,
                "run_groups": {
                    group: {"recipe": recipe, "binary_role": "installed_rust_cli"}
                    for group, recipe in RECIPES.items()
                },
                "raw_sha256": {name: sha256(staging / name) for name in sorted(raw_names)},
                "forge_downstream": forge,
                "acceptance": {
                    "terminal_status": "pass",
                    "all_four_installed_cli": "pass",
                    "all_four_installed_mcp": "pass",
                    "seeded_negative_controls": "pass",
                    "forge_modular_and_missing_path_canaries": "pass",
                },
            }
            if provenance.clean_source_identity(repository, revision) != source_identity:
                raise AcceptanceError("Pulp source identity changed during A2 recording")
            if source_blobs(repository, revision) != blobs:
                raise AcceptanceError("Pulp source binding changed during A2 recording")
            (staging / "receipt.json").write_text(
                json.dumps(receipt, indent=2, sort_keys=True) + "\n"
            )
            problems = verifier.verify(staging)
            if problems:
                raise AcceptanceError("self-verification failed: " + "; ".join(problems))
            Path(staging_name).rename(output)
        print(json.dumps({"output": str(output), "integration_head": revision,
                          "terminal_status": "pass"}, sort_keys=True))
        return 0
    except (AcceptanceError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
