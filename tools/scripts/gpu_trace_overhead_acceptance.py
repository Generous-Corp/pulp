#!/usr/bin/env python3
"""Measure installed CLI/MCP latency for closed GPU trace analysis.

This is an offline-tool benchmark. It deliberately does not grade producer
instrumentation overhead: compile-out, idle-session, and active-capture costs
need a real traced product workload and remain a separate acceptance surface.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import random
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import sdk_provenance


EXPECTED_EXIT = {"pass": 0, "fail": 1, "unavailable": 2, "unverified": 2}
A2T_ANALYZER_SOURCE_PATHS = {
    ".agents/skills/trace-sql/pulp_gpu_health_transitions.sql",
    ".agents/skills/trace-sql/pulp_gpu_probe_correlation.sql",
    ".agents/skills/trace-sql/pulp_gpu_startup_breakdown.sql",
    "experimental/pulp-rs/src/cmd/trace.rs",
    "experimental/pulp-rs/src/cmd/trace_dispatch.rs",
    "experimental/pulp-rs/src/cmd/trace_gpu_analysis.rs",
    "experimental/pulp-rs/src/cmd/trace_fetch.rs",
    "experimental/pulp-rs/tests/run_trace_gpu_analysis_integration.py",
    "experimental/pulp-rs/tests/trace_gpu_analysis_tool_test.rs",
    "tools/deps/manifest.json",
    "tools/mcp/mcp_trace_tools.cpp",
    "tools/mcp/pulp_mcp.cpp",
    "tools/scripts/gpu_trace_overhead_acceptance.py",
    "tools/scripts/verify_gpu_trace_overhead_acceptance.py",
}

PINNED_PROCESSOR_VERSION = "v57.2"
PROCESSOR_PLATFORM = {
    ("Darwin", "arm64"): "mac-arm64",
    ("Darwin", "aarch64"): "mac-arm64",
    ("Darwin", "x86_64"): "mac-amd64",
    ("Linux", "x86_64"): "linux-amd64",
    ("Linux", "amd64"): "linux-amd64",
    ("Linux", "aarch64"): "linux-arm64",
    ("Linux", "arm64"): "linux-arm64",
    ("Windows", "AMD64"): "windows-amd64",
    ("Windows", "x86_64"): "windows-amd64",
}
PROCESSOR_SHA256 = {
    "mac-arm64": "98a41b80e9f60da0373d64aff6455681f8c26b7c391ae5736324a5b11e3dacc2",
    "mac-amd64": "c0f61397901da47cbe1bb9a0843624f7c2038ac92176ce15e3736ce9aa0afef0",
    "linux-amd64": "55ba613fc6d4f71df81eee2dbfc293020063655c241b3e314bff75345b802684",
    "linux-arm64": "1dcc1d9aaff2eb92e8bc58f1957e4e445600294bd61dbc09345c1018c5ff0868",
    "windows-amd64": "100334b6091596fbc97f872556849a5747bf47a7f7190c485ba8cea8d2409c7b",
}
SEMANTIC_FIELDS = (
    "schema",
    "question",
    "verdict",
    "capture_complete",
    "unavailable_reason",
    "dominant_stage",
    "observed_categories",
    "category_scope",
    "contributors",
    "cold_start_contributors",
    "steady_state_contributors",
    "scheduler_evidence_available",
    "capture_integrity",
    "evidence_ids",
    "next_actions",
    "ui_correlation",
)
FIXTURE_REPLAY = (
    ("healthy-health", "healthy.pftrace", "gpu-health", "pass", "health-transition", None),
    ("compile-failure", "compile-failure.pftrace", "gpu-health", "fail", "shader-compile", "fix-shader-compile"),
    ("blank-readback", "blank-readback-failure.pftrace", "gpu-probe", "fail", "readback", "inspect-readback-oracle"),
    ("device-loss", "device-loss.pftrace", "gpu-health", "fail", "device-loss", "recreate-lost-device"),
    ("acquire-present", "acquire-present-wall-time-only.pftrace", "gpu-startup", "unverified", "acquire", "capture-scheduler-evidence"),
    ("first-frame", "first-frame-pipeline-upload-stall.pftrace", "gpu-startup", "unverified", "pipeline-prepare", "inspect-pipeline-signature"),
    ("incomplete", "incomplete.pftrace", "gpu-startup", "unavailable", None, "complete-and-flush-capture"),
    ("wrong-category", "wrong-category.pftrace", "gpu-health", "unavailable", None, "capture-required-gpu-category"),
)
FIXTURE_SOURCE_PATHS = {
    f"test/fixtures/perfetto-gpu/{filename}"
    for _case, filename, _question, _verdict, _dominant, _action in FIXTURE_REPLAY
}
PLAN_PATH = "research/2026-08-27-vgpu-gpu-ux-inspiration-audit-and-plan.md"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def percentile(values: list[float], quantile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("cannot calculate a percentile of no samples")
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def mad(values: list[float]) -> float:
    center = statistics.median(values)
    return statistics.median(abs(value - center) for value in values)


def summary(values_ns: list[int]) -> dict[str, float | int]:
    values_ms = [value / 1_000_000.0 for value in values_ns]
    return {
        "count": len(values_ms),
        "median_ms": round(statistics.median(values_ms), 6),
        "p95_ms": round(percentile(values_ms, 0.95), 6),
        "min_ms": round(min(values_ms), 6),
        "max_ms": round(max(values_ms), 6),
        "mad_ms": round(mad(values_ms), 6),
    }


def bootstrap_median_delta_ci(
    cli_ns: list[int], mcp_ns: list[int], *, samples: int = 10_000, seed: int = 0xA2
) -> dict[str, Any]:
    if len(cli_ns) != len(mcp_ns) or not cli_ns:
        raise ValueError("paired CLI/MCP samples are required")
    deltas_ms = [(mcp - cli) / 1_000_000.0 for cli, mcp in zip(cli_ns, mcp_ns)]
    rng = random.Random(seed)
    medians = []
    for _ in range(samples):
        medians.append(
            statistics.median(deltas_ms[rng.randrange(len(deltas_ms))] for _ in deltas_ms)
        )
    return {
        "method": "paired bootstrap median delta, 10000 resamples, deterministic seed 0xA2",
        "mcp_minus_cli_median_ms": round(statistics.median(deltas_ms), 6),
        "confidence_level": 0.95,
        "ci_low_ms": round(percentile(medians, 0.025), 6),
        "ci_high_ms": round(percentile(medians, 0.975), 6),
    }


def parse_analysis(payload: Any, *, surface: str) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise RuntimeError(f"{surface} returned a non-object")
    if payload.get("schema") != "pulp.trace-gpu-analysis.v1":
        raise RuntimeError(f"{surface} returned the wrong schema")
    verdict = payload.get("verdict")
    if verdict not in EXPECTED_EXIT:
        raise RuntimeError(f"{surface} returned an invalid verdict")
    return payload


def semantic_projection(payload: dict[str, Any]) -> dict[str, Any]:
    return {
        key: payload.get(key)
        for key in SEMANTIC_FIELDS
        if key in payload
    }


def normalized_semantic_projection(payload: dict[str, Any], trace_label: str) -> dict[str, Any]:
    projection = semantic_projection(payload)
    correlation = projection.get("ui_correlation")
    if isinstance(correlation, dict) and isinstance(correlation.get("open_command"), str):
        correlation = dict(correlation)
        correlation["open_command"] = f"pulp trace open {trace_label}"
        projection["ui_correlation"] = correlation
    return projection


class McpSession:
    def __init__(self, executable: Path, environment: dict[str, str]):
        self.process = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=environment,
        )
        if self.process.stdin is None or self.process.stdout is None:
            raise RuntimeError("failed to open pulp-mcp pipes")
        self._next_id = 1

    def _request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        request_id = self._next_id
        self._next_id += 1
        request: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise RuntimeError(f"pulp-mcp exited before responding: {stderr}")
        response = json.loads(line)
        if response.get("id") != request_id or "result" not in response:
            raise RuntimeError(f"invalid pulp-mcp response: {response}")
        return response

    def initialize(self) -> None:
        response = self._request("initialize")
        if response["result"].get("protocolVersion") != "2024-11-05":
            raise RuntimeError("pulp-mcp returned an unexpected protocol version")
        assert self.process.stdin is not None
        notification = {"jsonrpc": "2.0", "method": "notifications/initialized"}
        self.process.stdin.write(json.dumps(notification, separators=(",", ":")) + "\n")
        self.process.stdin.flush()

    def call(self, question: str, trace: Path) -> tuple[int, dict[str, Any]]:
        params = {
                "name": "pulp_trace_analyze",
                "arguments": {"question": question, "trace": str(trace)},
        }
        start = time.perf_counter_ns()
        response = self._request("tools/call", params)
        elapsed = time.perf_counter_ns() - start
        result = response["result"]
        structured = parse_analysis(result.get("structuredContent"), surface="MCP")
        should_error = EXPECTED_EXIT[structured["verdict"]] != 0
        if bool(result.get("isError", False)) != should_error:
            raise RuntimeError("MCP isError did not preserve the typed verdict status")
        try:
            text_payload = json.loads(result["content"][0]["text"])
        except (IndexError, KeyError, TypeError, json.JSONDecodeError) as error:
            raise RuntimeError("MCP text evidence is missing or malformed") from error
        if text_payload != structured:
            raise RuntimeError("MCP text and structured trace evidence disagree")
        return elapsed, structured

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)
        stderr = self.process.stderr.read() if self.process.stderr else ""
        if self.process.returncode != 0:
            raise RuntimeError(f"pulp-mcp exited {self.process.returncode}: {stderr}")


def run_cli(
    executable: Path, question: str, trace: Path, environment: dict[str, str]
) -> tuple[int, dict[str, Any]]:
    start = time.perf_counter_ns()
    run = subprocess.run(
        [str(executable), "trace", question, "--trace", str(trace), "--json"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
        check=False,
    )
    elapsed = time.perf_counter_ns() - start
    try:
        payload = parse_analysis(json.loads(run.stdout), surface="CLI")
    except Exception as error:
        raise RuntimeError(f"CLI output was invalid: {error}; stderr={run.stderr!r}") from error
    expected = EXPECTED_EXIT[payload["verdict"]]
    if run.returncode != expected:
        raise RuntimeError(f"CLI exit {run.returncode} did not match verdict {payload['verdict']}")
    return elapsed, payload


def system_profiler_identity() -> dict[str, Any]:
    identity: dict[str, Any] = {
        "operating_system": platform.platform(),
        "architecture": platform.machine(),
        "python": platform.python_version(),
    }
    try:
        run = subprocess.run(
            ["system_profiler", "SPHardwareDataType", "SPDisplaysDataType", "-json"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=20,
            check=True,
        )
        profile = json.loads(run.stdout)
        hardware = profile.get("SPHardwareDataType", [{}])[0]
        displays = profile.get("SPDisplaysDataType", [])
        identity["model_name"] = hardware.get("machine_name")
        identity["model_identifier"] = hardware.get("machine_model")
        identity["chip"] = hardware.get("chip_type")
        identity["gpu_adapters"] = [
            {
                "name": item.get("sppci_model"),
                "vendor": item.get("spdisplays_vendor"),
                "metal_support": item.get("spdisplays_metal"),
            }
            for item in displays
        ]
    except (OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        identity["system_profiler_error"] = str(error)
    return identity


def validate_paths(cli: Path, mcp: Path, trace: Path, processor: Path) -> None:
    for label, path in (("CLI", cli), ("MCP", mcp), ("trace", trace), ("processor", processor)):
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"{label} path is not a file: {path}")
    if cli.parent.resolve() != mcp.parent.resolve():
        raise ValueError("CLI and MCP must be installed as siblings in one prefix/bin")
    if cli.name != "pulp" or mcp.name != "pulp-mcp":
        raise ValueError("installed sibling names must be pulp and pulp-mcp")
    if not os.access(cli, os.X_OK) or not os.access(mcp, os.X_OK) or not os.access(processor, os.X_OK):
        raise ValueError("CLI, MCP, and trace processor must be executable regular files")


def _git_text(repository: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False, capture_output=True, text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise ValueError(f"git {' '.join(arguments)} failed: {detail}")
    return completed.stdout.strip()


def clean_source_identity(repository: Path, revision: str) -> dict[str, Any]:
    """Bind a recording to the exact clean checkout that built both fronts."""
    repository = repository.resolve()
    top = Path(_git_text(repository, "rev-parse", "--show-toplevel")).resolve()
    if top != repository:
        raise ValueError("repository must be its exact Git worktree root")
    head = _git_text(repository, "rev-parse", "HEAD")
    if head != revision:
        raise ValueError("source-revision must equal the exact recording checkout HEAD")
    status = _git_text(
        repository,
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
        "--ignore-submodules=untracked",
    )
    if status:
        raise ValueError("A2T recording checkout must be completely clean")
    origin = _git_text(repository, "config", "--get", "remote.origin.url")
    if not re.fullmatch(
        r"(?:git@github\.com:|https://github\.com/)Generous-Corp/pulp(?:\.git)?", origin
    ):
        raise ValueError("A2T source checkout must use the canonical Generous-Corp/pulp origin")
    return {
        "repository": "Generous-Corp/pulp",
        "revision": head,
        "clean": True,
        "status_sha256": hashlib.sha256(status.encode()).hexdigest(),
    }


def plan_identity(
    planning_repository: Path, revision: str, expected_sha256: str
) -> dict[str, Any]:
    """Resolve the accepted plan from immutable Git object bytes."""
    planning_repository = planning_repository.resolve()
    origin = _git_text(planning_repository, "config", "--get", "remote.origin.url")
    if not re.fullmatch(
        r"(?:git@github\.com:|https://github\.com/)danielraffel/pulp-planning(?:\.git)?",
        origin,
    ):
        raise ValueError("planning repository does not use the canonical pulp-planning origin")
    if not valid_lower_hex(revision, 40) or not valid_lower_hex(expected_sha256, 64):
        raise ValueError("plan revision and SHA-256 must be exact lowercase digests")
    completed = subprocess.run(
        ["git", "-C", str(planning_repository), "show", f"{revision}:{PLAN_PATH}"],
        check=False,
        capture_output=True,
    )
    if completed.returncode != 0:
        raise ValueError("accepted plan cannot be resolved at the declared revision")
    digest = hashlib.sha256(completed.stdout).hexdigest()
    if digest != expected_sha256:
        raise ValueError("accepted plan bytes do not match the declared SHA-256")
    blob = _git_text(planning_repository, "rev-parse", f"{revision}:{PLAN_PATH}")
    if not valid_lower_hex(blob, 40):
        raise ValueError("accepted plan does not resolve to an exact Git blob")
    return {
        "repository": "danielraffel/pulp-planning",
        "revision": revision,
        "path": PLAN_PATH,
        "blob": blob,
        "sha256": digest,
    }


def _project_version(repository: Path) -> str:
    payload = (repository / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"(?s)\bproject\s*\(\s*Pulp\b.*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        payload,
    )
    if match is None:
        raise ValueError("source CMakeLists.txt has no canonical Pulp version")
    return match.group(1)


def installed_source_identity(
    repository: Path, revision: str, cli: Path, mcp: Path
) -> dict[str, Any]:
    """Verify the installed siblings carry the clean Release source stamp."""
    prefix = cli.parent.parent.resolve()
    if mcp.parent.parent.resolve() != prefix:
        raise ValueError("installed CLI and MCP do not share one prefix")
    build_info_path = prefix / "include/pulp/runtime/build_info.hpp"
    try:
        build_info = sdk_provenance.verify_installed_build_info(
            prefix,
            expected_version=_project_version(repository),
            expected_source_sha=revision,
        )
    except sdk_provenance.ProvenanceError as error:
        raise ValueError(f"installed build provenance is invalid: {error}") from error
    return {
        "prefix_role": "isolated-clean-release-install",
        "build_info_role": "installed-prefix/include/pulp/runtime/build_info.hpp",
        "build_info_sha256": sha256(build_info_path),
        "build_info": build_info,
        "source_revision": revision,
    }


def _processor_source_contract(repository: Path) -> tuple[str, dict[str, str]]:
    source = (
        repository / "experimental/pulp-rs/src/cmd/trace_fetch.rs"
    ).read_text(encoding="utf-8")
    version_match = re.search(
        r'pub const PINNED_VERSION: &str = "([^"]+)";', source
    )
    pins = dict(re.findall(
        r'platform:\s*"([^"]+)"[\s\S]*?sha256:\s*"([0-9a-f]{64})"', source
    ))
    if version_match is None or version_match.group(1) != PINNED_PROCESSOR_VERSION:
        raise ValueError("Rust trace fetcher Perfetto version differs from the acceptance pin")
    if pins != PROCESSOR_SHA256:
        raise ValueError("Rust trace fetcher platform digests differ from the acceptance pins")
    manifest = json.loads((repository / "tools/deps/manifest.json").read_text(encoding="utf-8"))
    perfetto = [row for row in manifest.get("dependencies", []) if row.get("name") == "Perfetto"]
    if len(perfetto) != 1 or perfetto[0].get("version") != PINNED_PROCESSOR_VERSION:
        raise ValueError("Perfetto SDK manifest and trace processor version are not matched")
    return version_match.group(1), pins


def trace_processor_identity(
    repository: Path,
    processor: Path,
    *,
    system: str | None = None,
    machine: str | None = None,
) -> dict[str, Any]:
    """Prove the executable is Pulp's SDK-matched immutable processor."""
    version, pins = _processor_source_contract(repository)
    host = (system or platform.system(), machine or platform.machine())
    platform_key = PROCESSOR_PLATFORM.get(host)
    if platform_key is None:
        raise ValueError(f"no SDK-matched trace processor pin for host {host[0]}/{host[1]}")
    filename = "trace_processor_shell.exe" if platform_key == "windows-amd64" else "trace_processor_shell"
    if tuple(processor.parts[-5:]) != (
        "tools", "trace-processor", version, platform_key, filename
    ):
        raise ValueError("trace processor is not at the canonical Pulp pinned-cache path")
    digest = sha256(processor)
    if digest != pins[platform_key]:
        raise ValueError("trace processor bytes do not match the SDK-matched platform pin")
    completed = subprocess.run(
        [str(processor), "--version"], check=False, capture_output=True, text=True, timeout=10,
    )
    version_output = (completed.stdout + completed.stderr).strip()
    if completed.returncode != 0 or not version_output.startswith(f"Perfetto {version}-"):
        raise ValueError("trace processor did not report the SDK-matched Perfetto version")
    return {
        "role": f"pulp-home pinned {version} trace_processor_shell",
        "version": version,
        "platform": platform_key,
        "sha256": digest,
        "bytes": processor.stat().st_size,
        "version_output": version_output,
        "pin_source": "experimental/pulp-rs/src/cmd/trace_fetch.rs",
        "sdk_source": "tools/deps/manifest.json",
    }


def run_fixture_replay(
    cli: Path, mcp: Path, repository: Path, environment: dict[str, str]
) -> list[dict[str, Any]]:
    """Replay the required A2T fixture matrix through both installed fronts."""
    fixture_root = repository / "test/fixtures/perfetto-gpu"
    session = McpSession(mcp, environment)
    session.initialize()
    rows: list[dict[str, Any]] = []
    try:
        for case, filename, question, verdict, dominant, action in FIXTURE_REPLAY:
            trace = fixture_root / filename
            _, cli_first = run_cli(cli, question, trace, environment)
            _, cli_second = run_cli(cli, question, trace, environment)
            _, mcp_result = session.call(question, trace)
            label = f"test/fixtures/perfetto-gpu/{filename}"
            projections = [
                normalized_semantic_projection(value, label)
                for value in (cli_first, cli_second, mcp_result)
            ]
            if projections[0] != projections[1]:
                raise RuntimeError(f"{case}: checked SQL view was not rerunnable")
            if projections[0] != projections[2]:
                raise RuntimeError(f"{case}: installed CLI/MCP semantic parity failed")
            result = projections[0]
            if result.get("verdict") != verdict or result.get("dominant_stage") != dominant:
                raise RuntimeError(f"{case}: fixture no longer produces its intended verdict/stage")
            actions = result.get("next_actions")
            first_action = actions[0].get("code") if isinstance(actions, list) and actions else None
            if action is not None and first_action != action:
                raise RuntimeError(f"{case}: fixture no longer produces its intended next action")
            rows.append({
                "case": case,
                "question": question,
                "trace": {"role": f"repository/{label}", "sha256": sha256(trace), "bytes": trace.stat().st_size},
                "cli_rerun": "pass",
                "cli_mcp_parity": "pass",
                "semantic_result": result,
            })
    finally:
        session.close()
    return rows


def commit_inventory(repository: Path, revision: str) -> dict[str, Any]:
    if not valid_lower_hex(revision, 40):
        raise ValueError("A2T implementation revision must be an exact lowercase 40-hex commit")
    run = subprocess.run(
        ["git", "-C", str(repository), "diff-tree", "--no-commit-id", "--name-only", "-r", revision],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if run.returncode != 0:
        raise ValueError(f"cannot inventory A2T implementation revision: {run.stderr.strip()}")
    paths = [line for line in run.stdout.splitlines() if line]
    producer_prefixes = ("core/runtime/", "core/render/", "core/view/", "core/format/", "inspect/")
    producer_paths = [path for path in paths if path.startswith(producer_prefixes)]
    patch = subprocess.run(
        ["git", "-C", str(repository), "show", revision],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    patch_id_run = subprocess.run(
        ["git", "patch-id", "--stable"], input=patch.stdout,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if patch.returncode != 0 or patch_id_run.returncode != 0 or not patch_id_run.stdout:
        raise ValueError("cannot calculate stable patch id for A2T implementation revision")
    patch_id_output = (
        patch_id_run.stdout.decode() if isinstance(patch_id_run.stdout, bytes)
        else patch_id_run.stdout
    )
    patch_id = patch_id_output.split()[0]
    return {
        "method": "git diff-tree --no-commit-id --name-only -r",
        "implementation_revision": revision,
        "stable_patch_id": patch_id,
        "changed_path_count": len(paths),
        "changed_paths": paths,
        "producer_prefixes_checked": list(producer_prefixes),
        "added_or_changed_producer_paths": producer_paths,
        "no_added_producer_call_sites": not producer_paths,
    }


def valid_lower_hex(value: str, length: int) -> bool:
    return len(value) == length and all(c in "0123456789abcdef" for c in value)


def git_blobs(repository: Path, revision: str, paths: set[str]) -> dict[str, str]:
    completed = subprocess.run(
        ["git", "ls-tree", revision, "--", *sorted(paths)],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    if completed.returncode != 0:
        return {}
    blobs: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        try:
            metadata, path = line.split("\t", 1)
            _mode, kind, value = metadata.split()
        except ValueError:
            continue
        if kind == "blob" and valid_lower_hex(value, 40):
            blobs[path] = value
    return blobs


def checkout_blobs(repository: Path, paths: set[str]) -> dict[str, str]:
    completed = subprocess.run(
        ["git", "hash-object", "--stdin-paths"], cwd=repository,
        input="".join(f"{path}\n" for path in sorted(paths)),
        check=False, capture_output=True, text=True,
    )
    values = completed.stdout.splitlines() if completed.returncode == 0 else []
    if len(values) != len(paths):
        return {}
    return {
        path: value for path, value in zip(sorted(paths), values)
        if valid_lower_hex(value, 40)
    }


def measured_source_paths(repository: Path, trace: Path) -> set[str]:
    try:
        relative_trace = trace.resolve().relative_to(repository.resolve()).as_posix()
    except ValueError as error:
        raise ValueError("A2T trace must live in the measured source repository") from error
    if not relative_trace.startswith("test/fixtures/perfetto-gpu/"):
        raise ValueError("A2T trace must be a checked-in Perfetto GPU fixture")
    if relative_trace not in FIXTURE_SOURCE_PATHS:
        raise ValueError("A2T trace must be one of the required checked-in replay fixtures")
    return A2T_ANALYZER_SOURCE_PATHS | FIXTURE_SOURCE_PATHS


def source_binding(repository: Path, revision: str, trace: Path) -> dict[str, Any]:
    paths = measured_source_paths(repository, trace)
    historical = git_blobs(repository, revision, paths)
    head = git_blobs(repository, "HEAD", paths)
    checkout = checkout_blobs(repository, paths)
    if set(historical) != paths:
        raise ValueError("cannot resolve the exact A2T source set at source-revision")
    for path in sorted(paths):
        if head.get(path) != historical[path]:
            raise ValueError(f"current HEAD A2T source blob drift for {path}")
        if checkout.get(path) != historical[path]:
            raise ValueError(f"current checkout A2T source blob drift for {path}")
    return {"integration_head": revision, "source_blobs": historical}


def source_binding_errors(receipt: Any, repository: Path) -> list[str]:
    if not isinstance(receipt, dict):
        return ["A2T receipt must be an object"]
    errors: list[str] = []
    revision = receipt.get("source_revision")
    integration_head = receipt.get("integration_head")
    if not isinstance(integration_head, str) or not valid_lower_hex(integration_head, 40):
        errors.append("integration_head must be an exact Git SHA")
    elif integration_head != revision:
        errors.append("integration_head does not match source_revision")
    role = ((receipt.get("artifacts") or {}).get("trace") or {}).get("role")
    if not isinstance(role, str) or not role.startswith("repository/"):
        errors.append("trace role does not identify a repository fixture")
        return errors
    trace_path = role.removeprefix("repository/")
    if trace_path not in FIXTURE_SOURCE_PATHS:
        errors.append("trace role does not identify a required replay fixture")
    expected = A2T_ANALYZER_SOURCE_PATHS | FIXTURE_SOURCE_PATHS
    declared = receipt.get("source_blobs")
    if not isinstance(declared, dict) or set(declared) != expected:
        errors.append("source_blobs does not bind the exact A2T analyzer/SQL/fixture set")
        return errors
    historical = (
        git_blobs(repository, integration_head, expected)
        if isinstance(integration_head, str) and valid_lower_hex(integration_head, 40)
        else {}
    )
    head = git_blobs(repository, "HEAD", expected)
    checkout = checkout_blobs(repository, expected)
    for path in sorted(expected):
        value = declared.get(path)
        if not isinstance(value, str) or not valid_lower_hex(value, 40):
            errors.append(f"source blob {path} must be an exact Git blob SHA")
            continue
        if historical.get(path) != value:
            errors.append(f"source blob mismatch for {path}")
        if head.get(path) != value:
            errors.append(f"current HEAD source blob drift for {path}")
        if checkout.get(path) != value:
            errors.append(f"current checkout source blob drift for {path}")
    return errors


def source_revisions_match(source_revision: str, mcp_source_revision: str) -> bool:
    return bool(mcp_source_revision) and mcp_source_revision == source_revision


def preserve_human_perfetto_ui_correlation(
    prior_receipt: Any,
    *,
    question: str,
    trace_sha256: str,
    semantic_result: dict[str, Any],
) -> dict[str, Any]:
    """Carry an already accepted visual review across an exact-trace rerun."""
    if question != "gpu-startup":
        raise ValueError("human Perfetto UI correlation applies only to gpu-startup")
    if not isinstance(prior_receipt, dict):
        raise ValueError("prior human-review receipt must be an object")
    protocol = prior_receipt.get("protocol")
    acceptance = prior_receipt.get("acceptance")
    artifacts = prior_receipt.get("artifacts")
    human = prior_receipt.get("human_perfetto_ui_correlation")
    if not isinstance(protocol, dict) or protocol.get("question") != "gpu-startup":
        raise ValueError("prior human-review receipt must be for gpu-startup")
    if (
        not isinstance(acceptance, dict)
        or acceptance.get("human_perfetto_ui_correlation") != "pass"
    ):
        raise ValueError("prior human-review receipt lacks passing human Perfetto UI acceptance")
    if not isinstance(human, dict):
        raise ValueError("prior human-review receipt lacks the root correlation object")
    human_digest = human.get("artifact_sha256")
    prior_trace = artifacts.get("trace") if isinstance(artifacts, dict) else None
    prior_trace_digest = prior_trace.get("sha256") if isinstance(prior_trace, dict) else None
    if not isinstance(human_digest, str) or not valid_lower_hex(human_digest, 64):
        raise ValueError("prior human-review receipt has an invalid artifact SHA-256")
    if prior_trace_digest != human_digest:
        raise ValueError("prior human review is not bound to its receipt trace artifact")
    if human_digest != trace_sha256:
        raise ValueError("prior human-review receipt is not bound to the measured trace")
    for field in ("reviewer", "reviewed_utc", "ui_revision", "delivery"):
        if not isinstance(human.get(field), str) or not human[field].strip():
            raise ValueError(f"prior human-review receipt lacks nonempty {field}")
    if not isinstance(human.get("observed_spans"), list) or not human["observed_spans"]:
        raise ValueError("prior human-review receipt lacks observed span details")
    contributors = semantic_result.get("contributors")
    if not isinstance(contributors, list) or len(contributors) < 2:
        raise ValueError("current semantic result lacks two ranked contributors")
    observed_by_stage: dict[str, dict[str, Any]] = {}
    for observed in human["observed_spans"]:
        if not isinstance(observed, dict):
            raise ValueError("prior human-review observed span must be an object")
        name = observed.get("name")
        if not isinstance(name, str) or not name.startswith("gpu_"):
            raise ValueError("prior human-review observed span has no canonical GPU name")
        observed_by_stage[name.removeprefix("gpu_").replace("_", "-")] = observed
    for contributor in contributors[:2]:
        if not isinstance(contributor, dict) or not isinstance(contributor.get("stage"), str):
            raise ValueError("current semantic contributor is malformed")
        observed = observed_by_stage.get(contributor["stage"])
        if observed is None:
            raise ValueError(
                f"prior human review did not observe current contributor {contributor['stage']}"
            )
        compared = {
            "duration_ns": "duration_ns",
            "evidence_id": "gpu_evidence_id",
            "frame_index": "frame_index",
            "sequence": "sequence",
            "health_state": "health_state",
        }
        for semantic_field, observed_field in compared.items():
            if observed.get(observed_field) != contributor.get(semantic_field):
                raise ValueError(
                    f"prior human review disagrees with current {contributor['stage']} "
                    f"{semantic_field}"
                )
    return human


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--mcp", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--trace-processor", type=Path, required=True)
    parser.add_argument(
        "--question", choices=("gpu-startup", "gpu-health", "gpu-probe"), required=True
    )
    parser.add_argument("--warmups", type=int, default=5)
    parser.add_argument("--trials", type=int, default=30)
    parser.add_argument("--fresh-start-trials", type=int, default=20)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--mcp-source-revision", required=True)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    parser.add_argument(
        "--a2t-implementation-revision", action="append", required=True,
        help="Exact non-producer A2T implementation commit; repeat for hardening commits",
    )
    parser.add_argument("--equivalent-a2t-revision", action="append", default=[])
    parser.add_argument("--plan-revision", required=True)
    parser.add_argument("--plan-sha256", required=True)
    parser.add_argument("--planning-repository", type=Path, required=True)
    parser.add_argument("--routing-inventory", type=Path)
    parser.add_argument(
        "--prior-human-review-receipt", type=Path,
        help=(
            "Prior accepted gpu-startup A2T receipt whose exact root visual-review "
            "object is carried forward only when its trace SHA-256 matches"
        ),
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    cli = args.cli.resolve()
    mcp = args.mcp.resolve()
    trace = args.trace.resolve()
    processor = args.trace_processor.resolve()
    validate_paths(cli, mcp, trace, processor)
    if args.warmups < 0 or args.trials < 2 or args.fresh_start_trials < 1:
        parser.error("warmups >= 0, trials >= 2, and fresh-start-trials >= 1 are required")
    if not valid_lower_hex(args.plan_revision, 40):
        parser.error("plan-revision must be a lowercase 40-character Git revision")
    if not valid_lower_hex(args.plan_sha256, 64):
        parser.error("plan-sha256 must be a lowercase 64-character SHA-256 digest")
    if not valid_lower_hex(args.source_revision, 40):
        parser.error("source-revision must be a lowercase 40-character Git revision")
    if not valid_lower_hex(args.mcp_source_revision, 40):
        parser.error("mcp-source-revision must be a lowercase 40-character Git revision")
    if not source_revisions_match(args.source_revision, args.mcp_source_revision):
        parser.error(
            "mcp-source-revision must equal source-revision; the measured CLI and "
            "MCP must come from one source checkout"
        )

    repository = args.repository.resolve()
    try:
        source_identity = clean_source_identity(repository, args.source_revision)
        installed_identity = installed_source_identity(
            repository, args.source_revision, cli, mcp
        )
        binding = source_binding(repository, args.source_revision, trace)
        processor_identity = trace_processor_identity(repository, processor)
        accepted_plan = plan_identity(
            args.planning_repository, args.plan_revision, args.plan_sha256
        )
    except ValueError as error:
        parser.error(str(error))

    trace_sha256 = sha256(trace)
    prior_receipt = None
    if args.prior_human_review_receipt:
        try:
            prior_receipt = json.loads(
                args.prior_human_review_receipt.read_text(encoding="utf-8")
            )
        except (OSError, json.JSONDecodeError) as error:
            parser.error(f"invalid prior-human-review-receipt: {error}")

    environment = os.environ.copy()
    environment["PULP_TRACE_PROCESSOR"] = str(processor)
    environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    load_average_start = list(os.getloadavg()) if hasattr(os, "getloadavg") else None

    fixture_replay = run_fixture_replay(cli, mcp, repository, environment)

    persistent = McpSession(mcp, environment)
    persistent.initialize()
    reference_projection: dict[str, Any] | None = None
    try:
        for _ in range(args.warmups):
            _, cli_payload = run_cli(cli, args.question, trace, environment)
            _, mcp_payload = persistent.call(args.question, trace)
            if semantic_projection(cli_payload) != semantic_projection(mcp_payload):
                raise RuntimeError("CLI/MCP warm-up semantic parity failed")
            reference_projection = normalized_semantic_projection(
                cli_payload, "test/fixtures/perfetto-gpu/" + trace.name
            )

        rows = []
        cli_samples: list[int] = []
        mcp_samples: list[int] = []
        for trial in range(args.trials):
            if trial % 2 == 0:
                cli_ns, cli_payload = run_cli(cli, args.question, trace, environment)
                mcp_ns, mcp_payload = persistent.call(args.question, trace)
                order = "cli-first"
            else:
                mcp_ns, mcp_payload = persistent.call(args.question, trace)
                cli_ns, cli_payload = run_cli(cli, args.question, trace, environment)
                order = "mcp-first"
            if semantic_projection(cli_payload) != semantic_projection(mcp_payload):
                raise RuntimeError(f"CLI/MCP semantic parity failed at trial {trial + 1}")
            reference_projection = normalized_semantic_projection(
                cli_payload, "test/fixtures/perfetto-gpu/" + trace.name
            )
            cli_samples.append(cli_ns)
            mcp_samples.append(mcp_ns)
            rows.append(
                {"trial": trial + 1, "order": order,
                 "cli_duration_ns": cli_ns, "mcp_duration_ns": mcp_ns}
            )
    finally:
        persistent.close()

    fresh_rows = []
    fresh_cli: list[int] = []
    fresh_mcp: list[int] = []
    for trial in range(args.fresh_start_trials):
        if trial % 2 == 0:
            cli_ns, cli_payload = run_cli(cli, args.question, trace, environment)
            start = time.perf_counter_ns()
            session = McpSession(mcp, environment)
            try:
                session.initialize()
                _, mcp_payload = session.call(args.question, trace)
            finally:
                session.close()
            mcp_ns = time.perf_counter_ns() - start
            order = "cli-first"
        else:
            start = time.perf_counter_ns()
            session = McpSession(mcp, environment)
            try:
                session.initialize()
                _, mcp_payload = session.call(args.question, trace)
            finally:
                session.close()
            mcp_ns = time.perf_counter_ns() - start
            cli_ns, cli_payload = run_cli(cli, args.question, trace, environment)
            order = "mcp-first"
        if semantic_projection(cli_payload) != semantic_projection(mcp_payload):
            raise RuntimeError(f"fresh CLI/MCP semantic parity failed at trial {trial + 1}")
        fresh_cli.append(cli_ns)
        fresh_mcp.append(mcp_ns)
        fresh_rows.append(
            {"trial": trial + 1, "order": order, "cli_duration_ns": cli_ns,
             "mcp_process_initialize_request_shutdown_duration_ns": mcp_ns}
        )

    cli_summary = summary(cli_samples)
    mcp_summary = summary(mcp_samples)
    noise_floor_ms = max(float(cli_summary["mad_ms"]), float(mcp_summary["mad_ms"]))
    confidence = bootstrap_median_delta_ci(cli_samples, mcp_samples)
    confidence["noise_floor_method"] = "maximum within-surface median absolute deviation"
    confidence["noise_floor_ms"] = round(noise_floor_ms, 6)
    confidence["delta_interpretation"] = (
        "unchanged-within-noise"
        if abs(float(confidence["mcp_minus_cli_median_ms"])) <= noise_floor_ms
        else "measurable"
    )

    implementation_inventories = [
        commit_inventory(args.repository.resolve(), revision)
        for revision in args.a2t_implementation_revision
    ]
    if not all(item["no_added_producer_call_sites"] for item in implementation_inventories):
        raise RuntimeError(
            "an A2T implementation revision contains producer paths; run the product overhead gate"
        )
    equivalent_revisions = []
    for revision in args.equivalent_a2t_revision:
        equivalent = commit_inventory(args.repository.resolve(), revision)
        if equivalent["stable_patch_id"] != implementation_inventories[0]["stable_patch_id"]:
            raise RuntimeError(f"A2T revision {revision} is not patch-equivalent")
        equivalent_revisions.append(
            {"revision": revision, "stable_patch_id": equivalent["stable_patch_id"]}
        )
    implementation_inventories[0]["patch_equivalent_revisions"] = equivalent_revisions
    producer_inventory = {
        "method": "per-commit git diff-tree inventory",
        "implementation_revisions": implementation_inventories,
        "no_added_producer_call_sites": True,
        "added_or_changed_producer_paths": [],
    }
    routing_inventory = None
    if args.routing_inventory:
        routing_document = json.loads(args.routing_inventory.read_text())
        if routing_document.get("schema") == "pulp.gpu-trace-overhead-acceptance.v1":
            routing_inventory = routing_document["producer_overhead_disposition"].get(
                "routing_inventory"
            )
        else:
            routing_inventory = routing_document

    if reference_projection is None:
        raise RuntimeError("measured trials produced no reference semantic result")
    human_perfetto_ui_correlation = None
    if prior_receipt is not None:
        try:
            human_perfetto_ui_correlation = preserve_human_perfetto_ui_correlation(
                prior_receipt,
                question=args.question,
                trace_sha256=trace_sha256,
                semantic_result=reference_projection,
            )
        except ValueError as error:
            parser.error(f"invalid prior-human-review-receipt: {error}")

    try:
        if clean_source_identity(repository, args.source_revision) != source_identity:
            raise ValueError("source identity changed during A2T recording")
        if source_binding(repository, args.source_revision, trace) != binding:
            raise ValueError("source binding changed during A2T recording")
        if installed_source_identity(repository, args.source_revision, cli, mcp) != installed_identity:
            raise ValueError("installed CLI/MCP provenance changed during A2T recording")
        if trace_processor_identity(repository, processor) != processor_identity:
            raise ValueError("trace processor identity changed during A2T recording")
    except ValueError as error:
        parser.error(str(error))

    evidence = {
        "schema": "pulp.gpu-trace-overhead-acceptance.v2",
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source_revision": args.source_revision,
        "mcp_source_revision": args.mcp_source_revision,
        "integration_head": binding["integration_head"],
        "source_blobs": binding["source_blobs"],
        "source_identity": source_identity,
        "installed_source_identity": installed_identity,
        "accepted_plan": accepted_plan,
        "scope": "offline-installed-cli-mcp-analysis",
        "producer_overhead_disposition": {
            "status": "not-applicable-no-added-producer-cost",
            "reason": "A2T adds offline analysis and no generic render producer call sites, so Horizon A adds no producer runtime work to grade.",
            "evidence": producer_inventory,
            "routing_inventory": routing_inventory,
            "required_followup": "B6 must run the three-state 5-warmup/30-trial and 20 fresh-process protocol when Vellum producer instrumentation is added.",
            "formal_plan_status": "accepted-canonical-plan",
            "formal_plan_revision": args.plan_revision,
            "formal_plan_sha256": args.plan_sha256,
            "formal_plan_note": "The canonical plan accepts the no-added-producer disposition for Horizon A and preserves the full product producer/xrun protocol for B6; offline timing remains analyzer evidence, not product-capture evidence.",
        },
        "machine": system_profiler_identity(),
        "adapter_relevance": "recorded for host provenance only; saved-trace analysis performs no GPU work",
        "artifacts": {
            "install_prefix_role": "isolated-measurement-prefix",
            "sibling_binding": {
                "verified_same_resolved_parent": cli.parent.resolve() == mcp.parent.resolve(),
                "mechanism": "pulp-mcp resolves installed-prefix/bin/pulp from its own absolute executable identity; PATH excludes the prefix and checkout binaries",
            },
            "cli": {"role": "installed-prefix/bin/pulp", "sha256": sha256(cli), "bytes": cli.stat().st_size},
            "mcp": {"role": "installed-prefix/bin/pulp-mcp", "sha256": sha256(mcp), "bytes": mcp.stat().st_size},
            "trace": {"role": "repository/test/fixtures/perfetto-gpu/" + trace.name,
                      "sha256": trace_sha256, "bytes": trace.stat().st_size},
            "trace_processor": processor_identity,
        },
        "protocol": {
            "question": args.question, "warmups": args.warmups,
            "measured_paired_trials": args.trials,
            "fresh_start_paired_trials": args.fresh_start_trials,
            "order": "alternating cli-first/mcp-first",
            "mcp_lifecycle": "persistent trials exclude initialize; fresh trials include process launch, initialize, request, and graceful shutdown",
            "environment_path": environment["PATH"],
        },
        "measurement_environment": {
            "load_average_start": load_average_start,
            "load_average_end": list(os.getloadavg()) if hasattr(os, "getloadavg") else None,
            "interpretation": "shared-host contention is retained in raw samples; no latency budget is inferred",
        },
        "semantic_result": reference_projection,
        "fixture_replay": fixture_replay,
        "human_perfetto_ui_correlation": human_perfetto_ui_correlation,
        "measured": {
            "cli": cli_summary, "persistent_mcp_request": mcp_summary,
            "confidence": confidence, "raw_samples": rows,
        },
        "fresh_start": {
            "cli": summary(fresh_cli),
            "mcp_process_initialize_request_shutdown": summary(fresh_mcp),
            "raw_samples": fresh_rows,
        },
        "acceptance": {
            "terminal_status": (
                "pass"
                if human_perfetto_ui_correlation is not None
                else "nonterminal-missing-human-perfetto-correlation"
            ),
            "semantic_parity": "pass", "same_installed_prefix": "pass",
            "human_perfetto_ui_correlation": (
                "pass" if human_perfetto_ui_correlation is not None
                else "unverified-no-human-perfetto-ui-correlation"
            ),
            "offline_latency_budget": "unverified-no-ratified-budget",
            "producer_overhead_budget": "not-applicable-horizon-a-no-producer-delta",
            "xrun_check": "not-applicable-offline-no-audio-thread",
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "acceptance": evidence["acceptance"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
