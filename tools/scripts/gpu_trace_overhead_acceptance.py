#!/usr/bin/env python3
"""Measure installed CLI/MCP latency for closed GPU trace analysis.

This is an offline-tool benchmark. It deliberately does not grade producer
instrumentation overhead: compile-out, idle-session, and active-capture costs
need a real traced product workload and remain a separate acceptance surface.
"""

from __future__ import annotations

import argparse
import ast
from collections import Counter
import ctypes
import errno
import hashlib
import json
import math
import os
import platform
import random
import re
import resource
import secrets
import select
import stat as stat_module
import statistics
import subprocess
import sys
import tempfile
import time
import types
import unicodedata
from pathlib import Path, PurePosixPath
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent


def _load_local_source(module_name: str, filename: str) -> types.ModuleType:
    """Load a trusted sibling from source bytes without consulting ``.pyc``."""
    path = SCRIPT_DIR / filename
    data = path.read_bytes()
    if len(data) > 2 * 1024 * 1024:
        raise RuntimeError(f"local source module is unbounded: {filename}")
    module = types.ModuleType(module_name)
    module.__file__ = str(path)
    module.__package__ = ""
    sys.modules[module_name] = module
    exec(compile(data, str(path), "exec", dont_inherit=True), module.__dict__)
    return module


_load_local_source("json_schema_lite", "json_schema_lite.py")
_load_local_source("sdk_capability_handoff", "sdk_capability_handoff.py")
sdk_provenance = _load_local_source("sdk_provenance", "sdk_provenance.py")


EXPECTED_EXIT = {"pass": 0, "fail": 1, "unavailable": 2, "unverified": 2}
ANALYSIS_TIMEOUT_SECONDS = 300
MCP_RESPONSE_LIMIT_BYTES = 1024 * 1024
OFFLINE_STRUCTURAL_STATUS = "nonterminal-offline-structural-evidence"
NON_HUMAN_IDENTITY_TOKENS = frozenset({
    "agent", "assistant", "automated", "automation", "bot", "chatgpt",
    "claude", "codex", "copilot", "gemini", "gpt", "llm", "model",
    "openai",
})
NON_HUMAN_IDENTITY_PHRASES = frozenset({
    "artificialintelligence", "reasoningsystem",
})
A2T_EXECUTING_SOURCE_PATHS = frozenset({
    "tools/scripts/gpu_trace_overhead_acceptance.py",
    "tools/scripts/verify_gpu_trace_overhead_acceptance.py",
    "tools/scripts/json_schema_lite.py",
    "tools/scripts/sdk_capability_handoff.py",
    "tools/scripts/sdk_provenance.py",
})
A2T_ANALYZER_SOURCE_PATHS = {
    "CMakeLists.txt",
    ".agents/skills/trace-sql/pulp_gpu_health_transitions.sql",
    ".agents/skills/trace-sql/pulp_gpu_probe_correlation.sql",
    ".agents/skills/trace-sql/pulp_gpu_startup_breakdown.sql",
    "core/runtime/include/pulp/runtime/build_info.hpp.in",
    "experimental/pulp-rs/CMakeLists.txt",
    "experimental/pulp-rs/src/cmd/mod.rs",
    "experimental/pulp-rs/src/cmd/trace.rs",
    "experimental/pulp-rs/src/cmd/trace_dispatch.rs",
    "experimental/pulp-rs/src/cmd/trace_gpu_analysis.rs",
    "experimental/pulp-rs/src/cmd/trace_fetch.rs",
    "experimental/pulp-rs/src/cmd/trace_response.rs",
    "experimental/pulp-rs/src/fallthrough.rs",
    "experimental/pulp-rs/src/main.rs",
    "experimental/pulp-rs/tests/run_trace_gpu_analysis_integration.py",
    "experimental/pulp-rs/tests/trace_gpu_analysis_tool_test.rs",
    "tools/deps/manifest.json",
    "tools/cli/CMakeLists.txt",
    "tools/cmake/FindSkia.cmake",
    "tools/cmake/FindV8.cmake",
    "tools/cmake/PulpInstallRules.cmake",
    "tools/mcp/CMakeLists.txt",
    "tools/mcp/mcp_tools.hpp",
    "tools/mcp/mcp_tools_internal.cpp",
    "tools/mcp/mcp_tools_internal.hpp",
    "tools/mcp/mcp_trace_tools.cpp",
    "tools/mcp/pulp_mcp.cpp",
    "tools/scripts/gpu_trace_overhead_acceptance.py",
    "tools/scripts/fetch_skia_for_release.py",
    "tools/scripts/fetch_v8_for_release.py",
    "tools/scripts/gpu_trace_overhead_scope.json",
    "tools/scripts/json_schema_lite.py",
    "tools/scripts/sdk_capability_handoff.py",
    "tools/scripts/sdk_provenance.py",
    "tools/scripts/verify_gpu_trace_overhead_acceptance.py",
}
A2T_SCOPE_MANIFEST_PATH = "tools/scripts/gpu_trace_overhead_scope.json"
A2T_SCOPE_BASE = "d694994433aec73396caffd8bb10bdc77e15379f"
A2T_INTEGRATED_PATCH_EQUIVALENT = "bc1cfaa0aacc881da4c3753ca9d3862f55b571c9"
A2T_SEMANTIC_IDENTIFIERS = (
    "pulp.trace-gpu-analysis.v1",
    "pulp_gpu_startup_breakdown",
    "pulp_gpu_health_transitions",
    "pulp_gpu_probe_correlation",
    "pulp_trace_analyze",
    "gpu_trace_overhead_acceptance.py",
    "debug.gpu_evidence_id",
)
A2T_SEMANTIC_DISCOVERY_PATHS = (
    ".agents/skills/trace-analysis",
    ".agents/skills/trace-sql",
    ".claude/commands/trace.md",
    "core/format",
    "core/render",
    "core/runtime",
    "core/view",
    "docs/guides/tracing.md",
    "docs/guides/troubleshooting.md",
    "docs/reference/cli.md",
    "docs/status",
    "docs/validation/gpu-trace-overhead/README.md",
    "experimental/pulp-rs",
    "inspect",
    "test/cmake/quality_tests.cmake",
    "test/test_mcp_server.cpp",
    "tools/deps/manifest.json",
    "tools/mcp",
    "tools/scripts/cli_mcp_parity_baseline.json",
    "tools/scripts/gpu_trace_overhead_acceptance.py",
    "tools/scripts/gpu_trace_overhead_scope.json",
    "tools/scripts/test_gpu_trace_overhead_acceptance.py",
    "tools/scripts/test_verify_gpu_trace_overhead_acceptance.py",
    "tools/scripts/verify_gpu_trace_overhead_acceptance.py",
)
A2T_FIXED_SCOPE_PATHS = {
    "docs/status/tools.yaml",
    "docs/validation/gpu-trace-overhead/README.md",
    "experimental/pulp-rs/CMakeLists.txt",
    "test/cmake/quality_tests.cmake",
    "tools/deps/manifest.json",
    "tools/mcp/mcp_gpu_tools.cpp",
}
A3_SCOPE_AUTHORITY_PATH = "tools/scripts/gpu_first_visible_a3_acceptance.py"
A2T_PRODUCER_AUTHORITY_SOURCE_PATHS = {A3_SCOPE_AUTHORITY_PATH}
PRODUCT_PRODUCER_ROOTS = (
    "core/runtime", "core/render", "core/view", "core/format", "inspect",
)
PRODUCER_SOURCE_ANNOTATION = '"gpu_evidence_id"'
PRODUCT_TRACE_MACROS = {
    "PULP_TRACE_SCOPE",
    "PULP_TRACE_SCOPE_NAMED",
    "PULP_TRACE_SCOPE_NAMED_ARGS",
    "PULP_TRACE_SCOPE_DYNAMIC",
    "PULP_TRACE_BEGIN",
    "PULP_TRACE_BEGIN_ARGS",
    "PULP_TRACE_COUNTER",
}
MAX_PRODUCT_TRACE_CALL_BYTES = 2048
BUILD_TARGETS = ("pulp-rust-cli", "pulp-cli", "pulp-mcp")
REQUIRED_BUILD_SETTINGS = {
    "CMAKE_BUILD_TYPE": "Release",
    "PULP_ENABLE_GPU": "ON",
    "PULP_ENABLE_SCENE3D": "ON",
    "PULP_ENABLE_THREEJS_RUNTIME": "ON",
    "PULP_ENABLE_JS": "ON",
    "PULP_JS_ENGINE": "v8",
    "PULP_BUILD_RUST_CLI": "ON",
    "PULP_RUST_CLI_PROFILE": "release",
    "PULP_HAS_THREEJS": "TRUE",
}
BUILD_OUTPUT_LIMIT = 4 * 1024 * 1024

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
HUMAN_REVIEW_SCHEMA = "pulp.gpu-trace-human-review.v1"
HUMAN_REVIEW_PATH_PREFIX = "docs/validation/gpu-trace-overhead/human-reviews/"
HUMAN_REVIEW_MAX_BYTES = 256 * 1024
PROVIDER_MANIFEST_PATH = "tools/deps/manifest.json"
SKIA_PROVIDER_TOP_LEVEL = frozenset({
    ".skia-asset-sha256", "VERSION.md", "build", "include", "lib",
    "modules", "share",
})
V8_PROVIDER_TOP_LEVEL = frozenset({
    ".v8-asset-sha256", "LICENSE", "LICENSE.txt", "README", "README.md",
    "VERSION", "VERSION.md", "bin", "data", "icudtl.dat", "include",
    "jniLibs", "lib", "share", "snapshot_blob.bin",
})
SKIA_GPU_PLATFORM_DIRECTORY = re.compile(
    r"^(?:mac|win|linux|android(?:-x86_64)?|ios|wasm)-gpu$"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_descriptor(descriptor: int) -> str:
    digest = hashlib.sha256()
    metadata = os.fstat(descriptor)
    offset = 0
    while offset < metadata.st_size:
        chunk = os.pread(descriptor, min(1024 * 1024, metadata.st_size - offset), offset)
        if not chunk:
            raise ValueError("executable descriptor ended before its claimed size")
        digest.update(chunk)
        offset += len(chunk)
    if os.fstat(descriptor).st_size != metadata.st_size:
        raise ValueError("executable size changed while hashing its descriptor")
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


def paired_delta_confidence(cli_ns: list[int], mcp_ns: list[int]) -> dict[str, Any]:
    """Return the complete recorder confidence object from paired raw durations."""
    cli_summary = summary(cli_ns)
    mcp_summary = summary(mcp_ns)
    noise_floor_ms = max(float(cli_summary["mad_ms"]), float(mcp_summary["mad_ms"]))
    confidence = bootstrap_median_delta_ci(cli_ns, mcp_ns)
    confidence["noise_floor_method"] = "maximum within-surface median absolute deviation"
    confidence["noise_floor_ms"] = round(noise_floor_ms, 6)
    confidence["delta_interpretation"] = (
        "unchanged-within-noise"
        if abs(float(confidence["mcp_minus_cli_median_ms"])) <= noise_floor_ms
        else "measurable"
    )
    return confidence


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


def read_bounded_process_line(
    process: subprocess.Popen[Any],
    stream: Any,
    buffered: bytearray,
    *,
    timeout_seconds: float,
    maximum_bytes: int,
    label: str,
) -> str:
    """Read one newline-delimited response under one wall-clock deadline.

    ``TextIOWrapper.readline`` may block after ``select`` reports only a partial
    line. Reading the descriptor incrementally keeps the deadline authoritative
    and retains bytes after the first newline for the next JSON-RPC response.
    """
    if timeout_seconds <= 0 or maximum_bytes <= 0:
        raise ValueError(f"{label} has an invalid read bound")
    descriptor = stream.fileno()
    deadline = time.monotonic() + timeout_seconds
    while True:
        newline = buffered.find(b"\n")
        if newline >= 0:
            if newline + 1 > maximum_bytes:
                raise ValueError(f"{label} exceeded {maximum_bytes} bytes")
            raw = bytes(buffered[:newline + 1])
            del buffered[:newline + 1]
            try:
                return raw.decode("utf-8")
            except UnicodeDecodeError as error:
                raise ValueError(f"{label} is not UTF-8") from error
        if len(buffered) >= maximum_bytes:
            raise ValueError(f"{label} exceeded {maximum_bytes} bytes")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"{label} exceeded {timeout_seconds:g} seconds")
        ready, _, _ = select.select([descriptor], [], [], remaining)
        if not ready:
            raise TimeoutError(f"{label} exceeded {timeout_seconds:g} seconds")
        chunk = os.read(descriptor, min(64 * 1024, maximum_bytes + 1 - len(buffered)))
        if not chunk:
            if process.poll() is None:
                raise ValueError(f"{label} closed before a complete line")
            raise ValueError(f"{label} process exited before a complete line")
        buffered.extend(chunk)


class McpSession:
    def __init__(
        self, executable: Path, environment: dict[str, str],
        directory_claim: Any | None = None,
    ):
        self.directory_claim = directory_claim
        if self.directory_claim is not None:
            self.directory_claim.assert_current()
        self.process = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=environment,
        )
        if self.directory_claim is not None:
            self.directory_claim.assert_current()
        if self.process.stdin is None or self.process.stdout is None:
            raise RuntimeError("failed to open pulp-mcp pipes")
        self._next_id = 1
        self._terminated_for_timeout = False
        self._stdout_buffer = bytearray()

    def _terminate_bounded(self) -> None:
        if self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)

    def _request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        if self.directory_claim is not None:
            self.directory_claim.assert_current()
        request_id = self._next_id
        self._next_id += 1
        request: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            request["params"] = params
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        try:
            line = read_bounded_process_line(
                self.process,
                self.process.stdout,
                self._stdout_buffer,
                timeout_seconds=ANALYSIS_TIMEOUT_SECONDS,
                maximum_bytes=MCP_RESPONSE_LIMIT_BYTES,
                label="pulp-mcp response",
            )
        except (TimeoutError, ValueError) as error:
            self._terminated_for_timeout = isinstance(error, TimeoutError)
            self._terminate_bounded()
            raise RuntimeError(str(error)) from error
        try:
            response = json.loads(line)
        except json.JSONDecodeError as error:
            raise RuntimeError("pulp-mcp returned malformed JSON") from error
        if self.directory_claim is not None:
            self.directory_claim.assert_current()
        if (
            not isinstance(response, dict)
            or type(response.get("id")) is not int
            or response["id"] != request_id
            or not isinstance(response.get("result"), dict)
        ):
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
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._terminate_bounded()
        stderr = self.process.stderr.read() if self.process.stderr else ""
        if self.process.stdout:
            self.process.stdout.close()
        if self.process.stderr:
            self.process.stderr.close()
        if self.process.returncode != 0 and not self._terminated_for_timeout:
            raise RuntimeError(f"pulp-mcp exited {self.process.returncode}: {stderr}")
        if self.directory_claim is not None:
            self.directory_claim.assert_current()


def run_cli(
    executable: Path, question: str, trace: Path, environment: dict[str, str],
    directory_claim: Any | None = None,
) -> tuple[int, dict[str, Any]]:
    if directory_claim is not None:
        directory_claim.assert_current()
    start = time.perf_counter_ns()
    try:
        run = subprocess.run(
            [str(executable), "trace", question, "--trace", str(trace), "--json"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
            check=False,
            timeout=ANALYSIS_TIMEOUT_SECONDS,
        )
    finally:
        if directory_claim is not None:
            directory_claim.assert_current()
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
    head = assert_exact_live_head(repository, revision)
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


def assert_exact_live_head(repository: Path, revision: str) -> str:
    """Require a fresh recorder run to target the checkout's exact live HEAD."""
    head = _git_text(repository.resolve(), "rev-parse", "HEAD")
    if head != revision:
        raise ValueError("source-revision must equal the exact recording checkout HEAD")
    return head


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


def _cmake_cache(build_dir: Path) -> dict[str, str]:
    path = build_dir / "CMakeCache.txt"
    try:
        payload = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise ValueError(f"cannot read A2T CMake cache: {error}") from error
    if len(payload.encode()) > BUILD_OUTPUT_LIMIT:
        raise ValueError("A2T CMake cache exceeds 4 MiB")
    values: dict[str, str] = {}
    for line in payload.splitlines():
        match = re.match(r"^([^#/:=]+)(?::[^=]+)?=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def _require_external_path(path: Path, repositories: tuple[Path, ...], label: str) -> None:
    for repository in repositories:
        try:
            path.resolve().relative_to(repository.resolve())
        except ValueError:
            continue
        raise ValueError(f"{label} must be outside every protected tree")


def _directory_open_flags() -> int:
    return (
        os.O_RDONLY
        | getattr(os, "O_DIRECTORY", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )


def _directory_identity(value: os.stat_result) -> dict[str, int]:
    if not stat_module.S_ISDIR(value.st_mode):
        raise ValueError("claimed install-prefix identity is not a directory")
    return {"device": value.st_dev, "inode": value.st_ino}


def _assert_directory_path_identity(
    path: Path, descriptor: int, expected: dict[str, int], label: str
) -> None:
    try:
        descriptor_identity = _directory_identity(os.fstat(descriptor))
        path_identity = _directory_identity(os.stat(path, follow_symlinks=False))
    except OSError as error:
        raise ValueError(f"{label} claim is no longer reachable") from error
    if descriptor_identity != expected or path_identity != expected:
        raise ValueError(f"{label} path no longer names the atomically claimed directory")


class RetainedDirectoryClaim:
    def __init__(self, path: Path, descriptor: int, identity: dict[str, int], label: str):
        self.path = path
        self.descriptor = descriptor
        self.identity = identity
        self.label = label
        self.closed = False
        self.file_claims: list[dict[str, Any]] = []
        self.ancestor_claims: list[dict[str, Any]] = []
        self.claimed_directory_identities = {
            (identity["device"], identity["inode"])
        }
        self.monitor: Any | None = None
        self._bind_ancestor_chain(path)

    @staticmethod
    def _file_vnode_flags() -> int:
        required = (
            "KQ_NOTE_DELETE", "KQ_NOTE_WRITE", "KQ_NOTE_EXTEND",
            "KQ_NOTE_ATTRIB", "KQ_NOTE_LINK", "KQ_NOTE_RENAME",
            "KQ_NOTE_REVOKE",
        )
        if not hasattr(select, "kqueue") or any(
            not hasattr(select, name) for name in required
        ):
            raise ValueError("exact file sealing requires macOS kqueue")
        return sum(getattr(select, name) for name in required)

    @staticmethod
    def _directory_vnode_flags() -> int:
        required = ("KQ_NOTE_DELETE", "KQ_NOTE_RENAME", "KQ_NOTE_REVOKE")
        if any(not hasattr(select, name) for name in required):
            raise ValueError("exact path sealing requires macOS kqueue")
        return sum(getattr(select, name) for name in required)

    def _register_monitor(self, descriptor: int, *, is_file: bool) -> None:
        assert self.monitor is not None
        event = select.kevent(
            descriptor,
            filter=select.KQ_FILTER_VNODE,
            flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
            fflags=(
                self._file_vnode_flags() if is_file
                else self._directory_vnode_flags()
            ),
        )
        self.monitor.control([event], 0, 0)

    def _bind_ancestor_chain(self, path: Path) -> None:
        current = path.resolve().parent
        while True:
            descriptor = os.open(current, _directory_open_flags())
            identity = _directory_identity(os.fstat(descriptor))
            key = (identity["device"], identity["inode"])
            if key in self.claimed_directory_identities:
                os.close(descriptor)
            else:
                self.claimed_directory_identities.add(key)
                claim = {
                    "path": current,
                    "descriptor": descriptor,
                    "identity": identity,
                }
                self.ancestor_claims.append(claim)
                if self.monitor is not None:
                    self._register_monitor(descriptor, is_file=False)
            if current.parent == current:
                break
            current = current.parent

    def seal(self) -> None:
        if self.monitor is not None:
            raise ValueError(f"{self.label} claim is already sealed")
        self.monitor = select.kqueue()
        self._register_monitor(self.descriptor, is_file=False)
        for claim in self.ancestor_claims:
            self._register_monitor(claim["descriptor"], is_file=False)
        for claim in self.file_claims:
            self._register_monitor(claim["descriptor"], is_file=True)
        self._assert_no_mutation_events()

    def _assert_no_mutation_events(self) -> None:
        if self.monitor is not None:
            events = self.monitor.control(None, 64, 0)
            if events:
                raise ValueError(
                    f"{self.label} or a retained file had a mutation event"
                )

    def bind_file(
        self, path: Path, label: str, expected_sha256: str | None = None
    ) -> str:
        self._bind_ancestor_chain(path)
        descriptor = os.open(
            path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
        )
        claim: dict[str, Any] | None = None
        try:
            metadata = os.fstat(descriptor)
            named = os.stat(path, follow_symlinks=False)
            identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
            if (
                not stat_module.S_ISREG(metadata.st_mode)
                or {"device": named.st_dev, "inode": named.st_ino} != identity
            ):
                raise ValueError(f"{label} does not match its exact file claim")
            claim = {
                "path": path,
                "descriptor": descriptor,
                "identity": identity,
                "sha256": "",
                "bytes": metadata.st_size,
                "label": label,
            }
            self.file_claims.append(claim)
            if self.monitor is not None:
                self._register_monitor(descriptor, is_file=True)
                self._assert_no_mutation_events()
            actual_sha256 = sha256_descriptor(descriptor)
            if expected_sha256 is not None and actual_sha256 != expected_sha256:
                raise ValueError(f"{label} does not match its exact file claim")
            claim["sha256"] = actual_sha256
            named = os.stat(path, follow_symlinks=False)
            if {"device": named.st_dev, "inode": named.st_ino} != identity:
                raise ValueError(f"{label} does not match its exact file claim")
            self._assert_no_mutation_events()
        except BaseException:
            if claim is not None:
                self.file_claims.remove(claim)
            os.close(descriptor)
            raise
        return actual_sha256

    def assert_current(self) -> None:
        if self.closed:
            raise ValueError(f"{self.label} claim closed before proof completion")
        self._assert_no_mutation_events()
        _assert_directory_path_identity(
            self.path, self.descriptor, self.identity, self.label
        )
        for claim in self.ancestor_claims:
            _assert_directory_path_identity(
                claim["path"], claim["descriptor"], claim["identity"],
                "path-ancestor",
            )
        for claim in self.file_claims:
            metadata = os.fstat(claim["descriptor"])
            named = os.stat(claim["path"], follow_symlinks=False)
            identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
            if (
                not stat_module.S_ISREG(metadata.st_mode)
                or identity != claim["identity"]
                or {"device": named.st_dev, "inode": named.st_ino} != identity
                or sha256_descriptor(claim["descriptor"]) != claim["sha256"]
            ):
                raise ValueError(
                    f"{claim['label']} no longer matches its retained file claim"
                )
        self._assert_no_mutation_events()

    def close(self) -> None:
        if not self.closed:
            if self.monitor is not None:
                self.monitor.close()
            for claim in self.file_claims:
                os.close(claim["descriptor"])
            for claim in self.ancestor_claims:
                os.close(claim["descriptor"])
            os.close(self.descriptor)
            self.closed = True

    def __del__(self) -> None:
        self.close()


def _renameat_no_replace(
    source_parent: int, source_name: str, destination_parent: int,
    destination_name: str,
) -> None:
    """Publish one relative path with macOS RENAME_EXCL semantics."""
    libc = ctypes.CDLL(None, use_errno=True)
    renameatx = getattr(libc, "renameatx_np", None)
    if renameatx is None:
        raise ValueError("fresh directory claims require macOS renameatx_np")
    renameatx.argtypes = (
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
        ctypes.c_uint,
    )
    renameatx.restype = ctypes.c_int
    if renameatx(
        source_parent, os.fsencode(source_name), destination_parent,
        os.fsencode(destination_name), 0x00000004,
    ) != 0:
        value = ctypes.get_errno()
        raise OSError(value, os.strerror(value), destination_name)


def claim_fresh_directory(path: Path, label: str) -> RetainedDirectoryClaim:
    """Create, retain, and no-replace publish the exact directory inode."""
    if path.name in {"", ".", ".."}:
        raise ValueError(f"{label} must have a safe final path component")
    parent_descriptor = os.open(path.parent, _directory_open_flags())
    temporary_name = f".{path.name}.{secrets.token_hex(16)}.claim"
    temporary_descriptor: int | None = None
    temporary_identity: dict[str, int] | None = None
    temporary_exists = False
    try:
        parent_identity = _directory_identity(os.fstat(parent_descriptor))
        _assert_directory_path_identity(
            path.parent, parent_descriptor, parent_identity, f"{label}-parent"
        )
        os.mkdir(temporary_name, mode=0o700, dir_fd=parent_descriptor)
        temporary_exists = True
        temporary_descriptor = os.open(
            temporary_name, _directory_open_flags(), dir_fd=parent_descriptor
        )
        temporary_identity = _directory_identity(os.fstat(temporary_descriptor))
        named = _directory_identity(os.stat(
            temporary_name, dir_fd=parent_descriptor, follow_symlinks=False
        ))
        if named != temporary_identity:
            raise ValueError(f"{label} staging inode changed before publication")
        _renameat_no_replace(
            parent_descriptor, temporary_name,
            parent_descriptor, path.name,
        )
        temporary_exists = False
        _assert_directory_path_identity(
            path.parent, parent_descriptor, parent_identity, f"{label}-parent"
        )
        published = _directory_identity(os.stat(
            path.name, dir_fd=parent_descriptor, follow_symlinks=False
        ))
        if published != temporary_identity:
            raise ValueError(f"{label} publication did not preserve its created inode")
        os.fsync(parent_descriptor)
        claim = RetainedDirectoryClaim(
            path, temporary_descriptor, temporary_identity, label
        )
        temporary_descriptor = None
        try:
            claim.seal()
            claim.assert_current()
            return claim
        except BaseException:
            claim.close()
            raise
    except OSError as error:
        if error.errno == errno.EEXIST:
            raise ValueError(f"{label} was not fresh at no-replace publication") from error
        raise ValueError(f"{label} could not be atomically claimed: {error}") from error
    finally:
        if temporary_descriptor is not None:
            os.close(temporary_descriptor)
        if temporary_exists and temporary_identity is not None:
            try:
                named = _directory_identity(os.stat(
                    temporary_name,
                    dir_fd=parent_descriptor,
                    follow_symlinks=False,
                ))
                if named == temporary_identity:
                    os.rmdir(temporary_name, dir_fd=parent_descriptor)
                    os.fsync(parent_descriptor)
            except OSError:
                pass
        os.close(parent_descriptor)


def retain_existing_directory(path: Path, label: str) -> RetainedDirectoryClaim:
    descriptor = os.open(path, _directory_open_flags())
    claim: RetainedDirectoryClaim | None = None
    try:
        claim = RetainedDirectoryClaim(
            path, descriptor, _directory_identity(os.fstat(descriptor)), label
        )
        descriptor = -1
        claim.seal()
        claim.assert_current()
        return claim
    except BaseException:
        if claim is not None:
            claim.close()
        elif descriptor >= 0:
            os.close(descriptor)
        raise


def _git_blob_digest_descriptor(descriptor: int) -> str:
    metadata = os.fstat(descriptor)
    digest = hashlib.sha1()
    digest.update(f"blob {metadata.st_size}\0".encode())
    offset = 0
    while offset < metadata.st_size:
        chunk = os.pread(
            descriptor, min(1024 * 1024, metadata.st_size - offset), offset
        )
        if not chunk:
            raise ValueError("retained Git source ended before its claimed size")
        digest.update(chunk)
        offset += len(chunk)
    if os.fstat(descriptor).st_size != metadata.st_size:
        raise ValueError("retained Git source size changed while hashing")
    return digest.hexdigest()


def _git_blob_digest_bytes(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def _raise_descriptor_limit(required: int) -> None:
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft >= required:
        return
    requested = required if hard == resource.RLIM_INFINITY else min(required, hard)
    if requested < required:
        raise ValueError("exact source sealing exceeds the process descriptor limit")
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (requested, hard))
    except (OSError, ValueError) as error:
        raise ValueError(
            "cannot raise the descriptor limit for exact source sealing"
        ) from error


class RetainedTreeClaim:
    """Retain every bounded tree input and fail on transient path/content writes."""

    def __init__(
        self, root: Path, label: str, expected: dict[str, tuple[str, str]],
        *, watch_directory_writes: bool = True,
    ):
        self.root = root
        self.label = label
        self.expected = expected
        self.watch_directory_writes = watch_directory_writes
        self.closed = False
        self.anchor: RetainedDirectoryClaim | None = None
        self.root_descriptor = -1
        self.monitor: Any | None = None
        self.directory_claims: list[dict[str, Any]] = []
        self.file_claims: list[dict[str, Any]] = []
        try:
            directory_names = {"."}
            for relative in expected:
                parent = Path(relative).parent
                while str(parent) not in {"", "."}:
                    directory_names.add(parent.as_posix())
                    parent = parent.parent
            _raise_descriptor_limit(len(expected) + len(directory_names) + 512)
            self.anchor = retain_existing_directory(root, f"{label}-root")
            self.root_descriptor = os.dup(self.anchor.descriptor)
            self.monitor = select.kqueue()
            for relative in sorted(
                directory_names, key=lambda value: (len(Path(value).parts), value)
            ):
                descriptor = (
                    os.dup(self.root_descriptor)
                    if relative == "."
                    else os.open(
                        relative, _directory_open_flags(), dir_fd=self.root_descriptor
                    )
                )
                metadata = os.fstat(descriptor)
                named = os.fstat(self.root_descriptor) if relative == "." else os.stat(
                    relative, dir_fd=self.root_descriptor, follow_symlinks=False
                )
                identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
                if (
                    not stat_module.S_ISDIR(metadata.st_mode)
                    or {"device": named.st_dev, "inode": named.st_ino} != identity
                ):
                    os.close(descriptor)
                    raise ValueError(f"{label} directory claim is incoherent: {relative}")
                self.directory_claims.append({
                    "relative": relative, "descriptor": descriptor,
                    "identity": identity,
                })
                self._register(descriptor, is_file=False)
            for relative, (mode, expected_blob) in sorted(expected.items()):
                self._bind(relative, mode, expected_blob)
            self.assert_full()
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _file_flags() -> int:
        return sum(getattr(select, name) for name in (
            "KQ_NOTE_DELETE", "KQ_NOTE_WRITE", "KQ_NOTE_EXTEND",
            "KQ_NOTE_ATTRIB", "KQ_NOTE_LINK", "KQ_NOTE_RENAME",
            "KQ_NOTE_REVOKE",
        ))

    def _directory_flags(self) -> int:
        names = ["KQ_NOTE_DELETE", "KQ_NOTE_RENAME", "KQ_NOTE_REVOKE"]
        if self.watch_directory_writes:
            names.append("KQ_NOTE_WRITE")
        return sum(getattr(select, name) for name in names)

    def _register(self, descriptor: int, *, is_file: bool) -> None:
        assert self.monitor is not None
        self.monitor.control([select.kevent(
            descriptor,
            filter=select.KQ_FILTER_VNODE,
            flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR,
            fflags=self._file_flags() if is_file else self._directory_flags(),
        )], 0, 0)

    def _bind(self, relative: str, mode: str, expected_blob: str) -> None:
        symlink = mode == "120000"
        flags = os.O_RDONLY | (
            getattr(os, "O_SYMLINK", 0)
            if symlink else getattr(os, "O_NOFOLLOW", 0)
        )
        descriptor = os.open(relative, flags, dir_fd=self.root_descriptor)
        try:
            metadata = os.fstat(descriptor)
            named = os.stat(
                relative, dir_fd=self.root_descriptor, follow_symlinks=False
            )
            identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
            expected_kind = stat_module.S_ISLNK if symlink else stat_module.S_ISREG
            if (
                not expected_kind(metadata.st_mode)
                or {"device": named.st_dev, "inode": named.st_ino} != identity
            ):
                raise ValueError(f"{self.label} source identity drift: {relative}")
            if symlink:
                target = os.readlink(relative, dir_fd=self.root_descriptor)
                actual_blob = _git_blob_digest_bytes(os.fsencode(target))
            else:
                executable = bool(metadata.st_mode & 0o111)
                if executable != (mode == "100755"):
                    raise ValueError(f"{self.label} source mode drift: {relative}")
                actual_blob = _git_blob_digest_descriptor(descriptor)
            if expected_blob and actual_blob != expected_blob:
                raise ValueError(f"{self.label} source blob drift: {relative}")
            self.file_claims.append({
                "relative": relative, "descriptor": descriptor,
                "identity": identity, "mode": mode, "blob": actual_blob,
            })
            descriptor = -1
            self._register(self.file_claims[-1]["descriptor"], is_file=True)
            self._assert_no_events()
        finally:
            if descriptor >= 0:
                os.close(descriptor)

    def _assert_no_events(self) -> None:
        assert self.monitor is not None
        events = self.monitor.control(None, 64, 0)
        if events:
            raise ValueError(f"{self.label} had a retained tree mutation event")

    def assert_current(self) -> None:
        if self.closed:
            raise ValueError(f"{self.label} claim closed before proof completion")
        self._assert_no_events()
        assert self.anchor is not None
        self.anchor.assert_current()
        self._assert_no_events()

    def assert_full(self) -> None:
        self.assert_current()
        for claim in self.directory_claims:
            metadata = os.fstat(claim["descriptor"])
            named = (
                os.fstat(self.root_descriptor)
                if claim["relative"] == "."
                else os.stat(
                    claim["relative"], dir_fd=self.root_descriptor,
                    follow_symlinks=False,
                )
            )
            identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
            if (
                not stat_module.S_ISDIR(metadata.st_mode)
                or identity != claim["identity"]
                or {"device": named.st_dev, "inode": named.st_ino} != identity
            ):
                raise ValueError(
                    f"{self.label} directory identity drift: {claim['relative']}"
                )
        for claim in self.file_claims:
            metadata = os.fstat(claim["descriptor"])
            named = os.stat(
                claim["relative"], dir_fd=self.root_descriptor,
                follow_symlinks=False,
            )
            identity = {"device": metadata.st_dev, "inode": metadata.st_ino}
            symlink = claim["mode"] == "120000"
            actual_blob = (
                _git_blob_digest_bytes(os.fsencode(os.readlink(
                    claim["relative"], dir_fd=self.root_descriptor
                )))
                if symlink else _git_blob_digest_descriptor(claim["descriptor"])
            )
            if (
                identity != claim["identity"]
                or {"device": named.st_dev, "inode": named.st_ino} != identity
                or actual_blob != claim["blob"]
            ):
                raise ValueError(
                    f"{self.label} source changed during proof: {claim['relative']}"
                )
        self._assert_no_events()
        assert self.anchor is not None
        self.anchor.assert_current()

    def evidence(self) -> dict[str, Any]:
        return {
            "method": "retained-git-tree-vnode-and-descriptor-v1",
            "regular_or_symlink_files": len(self.file_claims),
            "retained_directories": len(self.directory_claims),
        }

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self.monitor is not None:
            try:
                self.monitor.close()
            except BaseException:
                pass
        for claim in self.file_claims:
            os.close(claim["descriptor"])
        for claim in self.directory_claims:
            os.close(claim["descriptor"])
        if self.root_descriptor >= 0:
            os.close(self.root_descriptor)
        if self.anchor is not None:
            self.anchor.close()

    def __del__(self) -> None:
        self.close()


def retain_git_source_tree(
    repository: Path, revision: str, label: str,
    *, overlays: dict[str, bytes] | None = None,
) -> tuple[RetainedTreeClaim, dict[str, Any]]:
    overlays = overlays or {}
    if _git_text(repository, "rev-parse", "--show-object-format") != "sha1":
        raise ValueError("exact source sealing currently requires SHA-1 Git objects")
    completed = subprocess.run(
        ["git", "ls-tree", "-rz", "-r", revision],
        cwd=repository, check=False, capture_output=True,
    )
    if completed.returncode != 0 or len(completed.stdout) > 32 * 1024 * 1024:
        raise ValueError(f"cannot enumerate bounded exact source for {label}")
    expected: dict[str, tuple[str, str]] = {}
    gitlinks: dict[str, str] = {}
    for raw in completed.stdout.split(b"\0"):
        if not raw:
            continue
        try:
            metadata, raw_path = raw.split(b"\t", 1)
            mode, kind, blob = metadata.decode("ascii").split()
            relative = os.fsdecode(raw_path)
        except (ValueError, UnicodeDecodeError) as error:
            raise ValueError(f"cannot parse exact source inventory for {label}") from error
        path = Path(relative)
        if (
            path.is_absolute() or ".." in path.parts or path.as_posix() != relative
            or not re.fullmatch(r"[0-9a-f]{40}", blob)
        ):
            raise ValueError(f"unsafe exact source inventory for {label}")
        if mode == "160000" and kind == "commit":
            gitlinks[relative] = blob
            continue
        if mode not in {"100644", "100755", "120000"} or kind != "blob":
            raise ValueError(f"unsupported exact source row for {label}: {relative}")
        if relative in overlays:
            blob = _git_blob_digest_bytes(overlays[relative])
        expected[relative] = (mode, blob)
    if set(overlays) - set(expected):
        raise ValueError(f"{label} overlay does not name a tracked regular file")
    claim = RetainedTreeClaim(repository, label, expected)
    evidence = claim.evidence()
    evidence.update({
        "revision": revision,
        "root_tree": _git_text(repository, "rev-parse", f"{revision}^{{tree}}"),
        "overlay_paths": sorted(overlays),
        "excluded_gitlinks": gitlinks,
    })
    return claim, evidence


class CombinedClaims:
    def __init__(self, *claims: Any):
        self.claims = claims

    def assert_current(self) -> None:
        for claim in self.claims:
            claim.assert_current()

    def bind_file(
        self, path: Path, label: str, expected_sha256: str | None = None
    ) -> str:
        binder = getattr(self.claims[0], "bind_file", None)
        if binder is None:
            raise ValueError("combined claim has no retained file binder")
        return binder(path, label, expected_sha256)


def retain_generated_build_inputs(
    build_dir: Path, label: str, *, targets: tuple[str, ...] = BUILD_TARGETS,
    forced_clean_before_build: bool = True,
) -> tuple[RetainedTreeClaim, dict[str, Any]]:
    """Retain regenerated CMake/Ninja/configure-time inputs through compilation."""
    expected: dict[str, tuple[str, str]] = {}
    total_bytes = 0
    for path in build_dir.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        relative = path.relative_to(build_dir).as_posix()
        if not (
            relative == "CMakeCache.txt"
            or path.name in {"build.ninja", "rules.ninja"}
            or path.suffix in {".cmake", ".h", ".hpp", ".inc"}
        ):
            continue
        metadata = path.stat()
        total_bytes += metadata.st_size
        if len(expected) >= 4096 or total_bytes > 64 * 1024 * 1024:
            raise ValueError("regenerated build-input inventory is unsafe or unbounded")
        mode = "100755" if metadata.st_mode & 0o111 else "100644"
        expected[relative] = (mode, "")
    required = {
        "CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja",
    }
    if not required.issubset(expected):
        raise ValueError("regenerated build lacks the exact CMake/Ninja input graph")
    claim = RetainedTreeClaim(
        build_dir, label, expected, watch_directory_writes=False
    )
    manifest = [
        {"path": row["relative"], "blob": row["blob"]}
        for row in claim.file_claims
    ]
    digest = hashlib.sha256(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return claim, {
        "method": "regenerated-cmake-ninja-input-descriptor-v1",
        "file_count": len(manifest),
        "manifest_sha256": digest,
        "build_targets": list(targets),
        "forced_clean_before_build": forced_clean_before_build,
    }


def retain_current_regular_tree(
    root: Path, label: str, *, maximum_files: int = 4096,
    maximum_bytes: int = 256 * 1024 * 1024,
) -> tuple[RetainedTreeClaim, dict[str, Any]]:
    expected: dict[str, tuple[str, str]] = {}
    symlink_targets: dict[str, str] = {}
    total_bytes = 0
    for path in root.rglob("*"):
        metadata = path.lstat()
        if stat_module.S_ISDIR(metadata.st_mode):
            continue
        relative = path.relative_to(root).as_posix()
        if stat_module.S_ISLNK(metadata.st_mode):
            mode = "120000"
            target = os.readlink(path)
            symlink_targets[relative] = target
            expected_blob = _git_blob_digest_bytes(os.fsencode(target))
            total_bytes += len(os.fsencode(target))
        elif stat_module.S_ISREG(metadata.st_mode):
            mode = "100755" if metadata.st_mode & 0o111 else "100644"
            expected_blob = ""
            total_bytes += metadata.st_size
        else:
            raise ValueError(f"{label} contains a non-regular filesystem entry")
        if len(expected) >= maximum_files or total_bytes > maximum_bytes:
            raise ValueError(f"{label} inventory is unsafe or unbounded")
        expected[relative] = (mode, expected_blob)
    if not expected:
        raise ValueError(f"{label} contains no retained files")
    try:
        resolved_root = root.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise ValueError(f"{label} root cannot be resolved safely") from error
    for relative, target in symlink_targets.items():
        target_path = Path(target)
        if target_path.is_absolute():
            raise ValueError(f"{label} contains an absolute symlink: {relative}")
        try:
            resolved_target = (
                root / Path(relative).parent / target_path
            ).resolve(strict=True)
            target_relative = resolved_target.relative_to(resolved_root).as_posix()
        except (OSError, RuntimeError, ValueError) as error:
            raise ValueError(f"{label} contains a symlink escape: {relative}") from error
        if resolved_target == resolved_root:
            raise ValueError(f"{label} contains a root-cycle symlink: {relative}")
        if resolved_target.is_file():
            target_row = expected.get(target_relative)
            if target_row is None or target_row[0] == "120000":
                raise ValueError(
                    f"{label} symlink target is not retained: {relative}"
                )
        elif resolved_target.is_dir():
            prefix = target_relative.rstrip("/") + "/"
            if not any(path.startswith(prefix) for path in expected):
                raise ValueError(
                    f"{label} symlink directory is not retained: {relative}"
                )
        else:
            raise ValueError(f"{label} has an unsupported symlink target: {relative}")
    claim = RetainedTreeClaim(root, label, expected)
    manifest = [
        {"path": row["relative"], "blob": row["blob"], "mode": row["mode"]}
        for row in claim.file_claims
    ]
    return claim, {
        "method": "retained-complete-tree-vnode-and-descriptor-v1",
        "file_count": len(manifest),
        "bytes": total_bytes,
        "manifest_sha256": hashlib.sha256(
            json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest(),
    }


class RetainedClaimSet:
    """Own a bounded set of retained provider trees as one lifetime claim."""

    def __init__(self, claims: dict[str, RetainedTreeClaim]):
        self.claims = claims

    def assert_current(self) -> None:
        for claim in self.claims.values():
            claim.assert_current()

    def assert_full(self) -> None:
        for claim in self.claims.values():
            claim.assert_full()

    def close(self) -> None:
        for claim in self.claims.values():
            claim.close()


def _absolute_lexical_path(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def _pinned_provider_asset_digests(dependency_name: str) -> tuple[str, set[str]]:
    manifest_path = SCRIPT_DIR.parents[1] / PROVIDER_MANIFEST_PATH
    try:
        data = manifest_path.read_bytes()
        manifest = json.loads(data)
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError("cannot read the pinned render-provider manifest") from error
    if len(data) > 4 * 1024 * 1024:
        raise ValueError("render-provider manifest exceeds 4 MiB")
    dependency = next(
        (
            row for row in manifest.get("dependencies", [])
            if isinstance(row, dict) and row.get("name") == dependency_name
        ),
        None,
    )
    assets = (
        dependency.get("determinism", {}).get("release_assets", {})
        if isinstance(dependency, dict) else {}
    )
    digests = {
        row.get("sha256") for row in assets.values()
        if isinstance(row, dict)
        and isinstance(row.get("sha256"), str)
        and valid_lower_hex(row["sha256"], 64)
        and row.get("library", True) is not False
    }
    if not digests:
        raise ValueError(f"pinned {dependency_name} release assets are unavailable")
    return hashlib.sha256(data).hexdigest(), digests


def _provider_generation_authority(
    root: Path,
    *,
    dependency_name: str,
    stamp_name: str,
    allowed_top_level: frozenset[str],
    additional_allowed_top_level: frozenset[str] = frozenset(),
) -> dict[str, Any]:
    """Bind a provider to one pinned release generation and package layout."""
    try:
        entries = sorted(root.iterdir(), key=lambda path: path.name)
    except OSError as error:
        raise ValueError(f"cannot inventory {dependency_name} provider root") from error
    exact_allowed = allowed_top_level | additional_allowed_top_level
    unexpected = [entry.name for entry in entries if entry.name not in exact_allowed]
    if unexpected:
        raise ValueError(
            f"{dependency_name} provider root contains unrelated top-level data: "
            + ", ".join(unexpected[:5])
        )
    stamp = root / stamp_name
    if not stamp.is_file() or stamp.is_symlink():
        raise ValueError(
            f"{dependency_name} provider lacks its pinned generation stamp"
        )
    try:
        stamp_bytes = stamp.read_bytes()
        asset_sha256 = stamp_bytes.decode("ascii").strip()
    except (OSError, UnicodeDecodeError) as error:
        raise ValueError(
            f"{dependency_name} provider generation stamp is unreadable"
        ) from error
    manifest_sha256, pinned_digests = _pinned_provider_asset_digests(
        dependency_name
    )
    if len(stamp_bytes) > 128 or asset_sha256 not in pinned_digests:
        raise ValueError(
            f"{dependency_name} provider generation is not pinned by the manifest"
        )
    return {
        "method": "pinned-release-asset-stamp-and-exact-layout-v1",
        "dependency": dependency_name,
        "dependency_manifest_path": PROVIDER_MANIFEST_PATH,
        "dependency_manifest_sha256": manifest_sha256,
        "generation_stamp_path": stamp_name,
        "generation_asset_sha256": asset_sha256,
        "generation_stamp_sha256": hashlib.sha256(stamp_bytes).hexdigest(),
        "top_level_entries": [entry.name for entry in entries],
    }


def _skia_archive_layout(library: Path) -> tuple[Path, str] | None:
    if library.name not in {"libskia.a", "skia.lib"}:
        return None
    parents = library.parents
    if (
        len(parents) >= 5
        and parents[0].name == "Release"
        and parents[1].name == "lib"
        and SKIA_GPU_PLATFORM_DIRECTORY.fullmatch(parents[2].name)
        and parents[3].name == "build"
    ):
        return parents[4], "build/<platform>-gpu/lib/Release"
    if (
        len(parents) >= 4
        and parents[0].name == "Release"
        and parents[1].name == "lib"
        and SKIA_GPU_PLATFORM_DIRECTORY.fullmatch(parents[2].name)
    ):
        return parents[3], "<platform>-gpu/lib/Release"
    if (
        len(parents) >= 3
        and parents[0].name == "lib"
        and re.fullmatch(
            r"(?:mac|win|linux|android|ios|wasm)(?:[-_][A-Za-z0-9_+-]+)?",
            parents[1].name,
        )
    ):
        return parents[2], "<platform>/lib"
    if len(parents) >= 2 and parents[0].name == "lib":
        return parents[1], "lib"
    return None


def skia_consumed_top_level_entries(authority: dict[str, Any]) -> frozenset[str]:
    """Derive the only provider-root additions authorized by consumed archives."""
    layout = authority.get("provider_layout")
    patterns = {
        "build/<platform>-gpu/lib/Release": re.compile(
            r"build/(?:mac|win|linux|android(?:-x86_64)?|ios|wasm)-gpu/"
            r"lib/Release/(?:libskia\.a|skia\.lib)"
        ),
        "<platform>-gpu/lib/Release": re.compile(
            r"(?:mac|win|linux|android(?:-x86_64)?|ios|wasm)-gpu/"
            r"lib/Release/(?:libskia\.a|skia\.lib)"
        ),
        "<platform>/lib": re.compile(
            r"(?:mac|win|linux|android|ios|wasm)(?:[-_][A-Za-z0-9_+-]+)?/"
            r"lib/(?:libskia\.a|skia\.lib)"
        ),
        "lib": re.compile(r"lib/(?:libskia\.a|skia\.lib)"),
    }
    pattern = patterns.get(layout)
    archives = authority.get("consumed_skia_archives")
    if pattern is None or not isinstance(archives, list) or not archives:
        raise ValueError("Skia provider lacks an exact consumed archive layout")
    if any(not isinstance(value, str) for value in archives):
        raise ValueError("Skia provider has an invalid consumed archive path")
    if len(archives) != len(set(archives)):
        raise ValueError("Skia provider repeats a consumed archive path")
    top_level: set[str] = set()
    for value in archives:
        if len(value) > 512:
            raise ValueError("Skia provider has an invalid consumed archive path")
        path = PurePosixPath(value)
        if (
            path.is_absolute()
            or not path.parts
            or any(part in {"", ".", ".."} for part in path.parts)
            or pattern.fullmatch(value) is None
        ):
            raise ValueError("Skia provider consumed archive disagrees with its layout")
        top_level.add(path.parts[0])
    return frozenset(top_level)


def _resolved_skia_root(
    build_dir: Path, cache: dict[str, str]
) -> tuple[Path, dict[str, Any]]:
    graph_libraries: list[Path] = []
    configured = cache.get("SKIA_DIR", "")
    for graph in (build_dir / "build.ninja", build_dir / "CMakeFiles/rules.ninja"):
        try:
            data = graph.read_bytes()
        except OSError:
            continue
        if len(data) > 64 * 1024 * 1024:
            raise ValueError("generated build graph is too large for provider discovery")
        for raw in re.findall(
            rb"/[A-Za-z0-9_./+@%$:-]+/(?:libskia\.a|skia\.lib)", data
        ):
            graph_libraries.append(
                _absolute_lexical_path(Path(os.fsdecode(raw).replace("$ ", " ")))
            )
    configured_root: Path | None = None
    if configured:
        configured_path = Path(configured)
        if not configured_path.is_absolute():
            raise ValueError("CMake SKIA_DIR must be an exact absolute provider root")
        configured_root = _absolute_lexical_path(configured_path)
        if configured_root.is_symlink():
            raise ValueError("CMake SKIA_DIR provider root must not be a symlink")
        try:
            configured_root.resolve(strict=True)
        except OSError as error:
            raise ValueError("CMake SKIA_DIR provider root does not exist") from error
    candidates = [
        library for library in graph_libraries
        if library.is_file() and not library.is_symlink()
    ]
    if not candidates and configured_root is not None:
        candidates = (
            list(configured_root.glob("build/*-gpu/lib/Release/libskia.a"))
            + list(configured_root.glob("build/*-gpu/lib/Release/skia.lib"))
            + list(configured_root.glob("*-gpu/lib/Release/libskia.a"))
            + list(configured_root.glob("*-gpu/lib/Release/skia.lib"))
            + list(configured_root.glob("*/lib/libskia.a"))
            + list(configured_root.glob("*/lib/skia.lib"))
            + [configured_root / "lib/libskia.a", configured_root / "lib/skia.lib"]
        )
        candidates = [path for path in candidates if path.is_file() and not path.is_symlink()]
    derived: list[tuple[Path, str, Path]] = []
    for library in candidates:
        layout = _skia_archive_layout(library)
        if layout is None:
            continue
        root, layout_name = layout
        if configured_root is not None and root != configured_root:
            continue
        derived.append((root, layout_name, library))
    roots = {root for root, _layout, _library in derived}
    if len(roots) != 1:
        raise ValueError(
            "cannot resolve one exact-layout Skia/Dawn provider root from CMake/Ninja"
        )
    lexical_root = roots.pop()
    layouts = {layout for root, layout, _library in derived if root == lexical_root}
    if len(layouts) != 1:
        raise ValueError("Skia/Dawn provider has mixed consumed archive layouts")
    layout_name = layouts.pop()
    consumed = sorted(
        {library for root, _layout, library in derived if root == lexical_root}
    )
    for library in consumed:
        dawn_name = (
            "dawn_combined.lib" if library.name == "skia.lib"
            else "libdawn_combined.a"
        )
        dawn = library.with_name(dawn_name)
        if not dawn.is_file() or dawn.is_symlink():
            raise ValueError("Skia provider layout lacks the matching Dawn archive")
    header_roots = (
        (lexical_root / "build/include/include/core/SkCanvas.h",)
        if layout_name.startswith("build/")
        else (
            lexical_root / "include/include/core/SkCanvas.h",
            lexical_root / "include/core/SkCanvas.h",
        )
    )
    if not any(path.is_file() and not path.is_symlink() for path in header_roots):
        raise ValueError("Skia provider layout lacks its exact include root")
    consumed_relative = [
        library.relative_to(lexical_root).as_posix() for library in consumed
    ]
    layout_authority = {
        "provider_layout": layout_name,
        "consumed_skia_archives": consumed_relative,
    }
    consumed_top_level = skia_consumed_top_level_entries(layout_authority)
    authority = _provider_generation_authority(
        lexical_root,
        dependency_name="Skia",
        stamp_name=".skia-asset-sha256",
        allowed_top_level=SKIA_PROVIDER_TOP_LEVEL,
        additional_allowed_top_level=consumed_top_level,
    )
    authority.update({
        "cache_authority": "SKIA_DIR" if configured_root is not None else "generated-ninja",
        "provider_layout": layout_name,
        "consumed_skia_archives": consumed_relative,
        "consumed_top_level_entries": sorted(consumed_top_level),
    })
    return lexical_root, authority


def _resolved_v8_root(cache: dict[str, str]) -> tuple[Path, Path, dict[str, Any]]:
    runtime_value = cache.get("V8_RUNTIME_LIBRARY", "")
    if not runtime_value or not Path(runtime_value).is_absolute():
        raise ValueError("CMake cache lacks one exact absolute V8 runtime library")
    runtime = _absolute_lexical_path(Path(runtime_value))
    if not runtime.is_absolute() or not runtime.is_file() or runtime.is_symlink():
        raise ValueError("CMake cache lacks one resolved regular V8 runtime library")
    if runtime.name not in {"libv8.dylib", "libv8.so", "v8.dll"}:
        raise ValueError("resolved V8 runtime is not the pinned sealed V8 library")
    if runtime.parent.name == "lib":
        root = runtime.parent.parent
        layout = "lib/<runtime>"
    elif (
        runtime.parent.name == "arm64-v8a"
        and runtime.parent.parent.name == "jniLibs"
    ):
        root = runtime.parent.parent.parent
        layout = "jniLibs/arm64-v8a/<runtime>"
    else:
        raise ValueError("resolved V8 runtime does not match an exact provider layout")
    if root.is_symlink():
        raise ValueError("resolved V8 provider root is substituted")
    root.resolve(strict=True)
    header = root / "include/v8.h"
    if not header.is_file() or header.is_symlink():
        raise ValueError("resolved V8 provider lacks direct include/v8.h")
    configured_include = cache.get("V8_INCLUDE_DIR", "")
    if configured_include and _absolute_lexical_path(Path(configured_include)) != root / "include":
        raise ValueError("CMake V8_INCLUDE_DIR does not name the resolved provider include root")
    authority = _provider_generation_authority(
        root,
        dependency_name="V8",
        stamp_name=".v8-asset-sha256",
        allowed_top_level=V8_PROVIDER_TOP_LEVEL,
    )
    authority.update({
        "cache_authority": "V8_RUNTIME_LIBRARY",
        "provider_layout": layout,
        "consumed_runtime": runtime.relative_to(root).as_posix(),
    })
    return root, runtime, authority


def _resolved_skia_source_root(cache: dict[str, str], skia_root: Path) -> Path | None:
    lexical_root = _absolute_lexical_path(skia_root.parent / "skia-src")
    if not (lexical_root / "src/core").is_dir():
        return None
    try:
        if lexical_root.is_symlink():
            raise ValueError("adjacent Skia source provider root is substituted")
        lexical_root.resolve(strict=True)
    except OSError as error:
        raise ValueError("adjacent Skia source provider cannot be resolved") from error
    return lexical_root


def retain_resolved_render_provider_inputs(
    build_dir: Path, label: str,
) -> tuple[RetainedClaimSet, dict[str, Any]]:
    """Retain the actual Skia/Dawn and V8 provider trees through every launch."""
    cache = _cmake_cache(build_dir)
    if cache.get("PULP_HAS_SKIA") != "TRUE" or cache.get("PULP_JS_ENGINE") != "v8":
        raise ValueError("render provider sealing requires the exact GPU + V8 build")
    skia_root, skia_authority = _resolved_skia_root(build_dir, cache)
    v8_root, v8_runtime, v8_authority = _resolved_v8_root(cache)
    skia_source_root = _resolved_skia_source_root(cache, skia_root)
    provider_specs: list[tuple[str, Path, int, int, dict[str, Any]]] = [
        (
            "skia_dawn", skia_root, 32768, 8 * 1024 * 1024 * 1024,
            skia_authority,
        ),
        ("v8", v8_root, 32768, 2 * 1024 * 1024 * 1024, v8_authority),
    ]
    if skia_source_root is not None:
        provider_specs.append(
            (
                "skia_source", skia_source_root, 65536,
                16 * 1024 * 1024 * 1024,
                {
                    "method": "exact-adjacent-skia-source-layout-v1",
                    "cache_authority": "SKIA_DIR/../skia-src",
                    "provider_layout": "src/core",
                },
            )
        )
    resolved_roots = {
        role: root.resolve(strict=True)
        for role, root, _, _, _authority in provider_specs
    }
    for index, (left_role, left_root) in enumerate(resolved_roots.items()):
        for right_role, right_root in list(resolved_roots.items())[index + 1:]:
            if (
                left_root == right_root
                or left_root in right_root.parents
                or right_root in left_root.parents
            ):
                raise ValueError(
                    f"resolved {left_role} and {right_role} provider roots overlap"
                )
    _raise_descriptor_limit(196608)
    claims: dict[str, RetainedTreeClaim] = {}
    rows: dict[str, Any] = {}
    try:
        for role, root, maximum_files, maximum_bytes, root_authority in provider_specs:
            claim, tree_evidence = retain_current_regular_tree(
                root,
                f"{label} {role}",
                maximum_files=maximum_files,
                maximum_bytes=maximum_bytes,
            )
            claims[role] = claim
            by_relative = {row["relative"]: row for row in claim.file_claims}
            if role == "skia_dawn":
                required_paths = sorted(
                    relative for relative in by_relative
                    if by_relative[relative]["mode"] != "120000"
                    and Path(relative).name in {
                        "libskia.a", "skia.lib",
                        "libdawn_combined.a", "dawn_combined.lib",
                    }
                )
                if not any(Path(path).name in {"libskia.a", "skia.lib"} for path in required_paths):
                    raise ValueError("retained Skia/Dawn provider lacks the Skia archive")
                if not any(
                    Path(path).name in {"libdawn_combined.a", "dawn_combined.lib"}
                    for path in required_paths
                ):
                    raise ValueError("retained Skia/Dawn provider lacks the Dawn archive")
                required_paths.append(".skia-asset-sha256")
                required_paths = sorted(set(required_paths))
                resolution = "exact CMake/Ninja Skia layout plus pinned asset generation"
            elif role == "v8":
                runtime_relative = v8_runtime.relative_to(v8_root).as_posix()
                required_paths = [
                    ".v8-asset-sha256", "include/v8.h", runtime_relative
                ]
                if any(
                    path not in by_relative
                    or by_relative[path]["mode"] == "120000"
                    for path in required_paths
                ):
                    raise ValueError("retained V8 provider lacks its resolved headers/runtime")
                resolution = "exact CMake V8 runtime layout plus pinned asset generation"
            else:
                required_paths = sorted(
                    relative for relative, member in by_relative.items()
                    if member["mode"] != "120000"
                    and relative.startswith("src/core/")
                )[:1]
                if not required_paths:
                    raise ValueError(
                        "retained adjacent Skia source provider lacks src/core inputs"
                    )
                resolution = "FindSkia adjacent SKIA_DIR/../skia-src include root"
            rows[role] = {
                "root_role": f"resolved-{role.replace('_', '-')}-provider-root",
                "resolution": resolution,
                "root_authority": root_authority,
                "tree_claim": tree_evidence,
                "required_members": [
                    {
                        "path": path,
                        "git_blob_sha1": by_relative[path]["blob"],
                        "sha256": sha256_descriptor(
                            by_relative[path]["descriptor"]
                        ),
                        "bytes": os.fstat(by_relative[path]["descriptor"]).st_size,
                    }
                    for path in required_paths
                ],
            }
        evidence = {
            "method": "retained-resolved-render-provider-trees-v3",
            "skia_source_disposition": (
                "retained-complete-adjacent-source-tree"
                if skia_source_root is not None
                else "not-present-at-configured-or-resolved-sibling"
            ),
            "providers": rows,
        }
        evidence["manifest_sha256"] = hashlib.sha256(
            json.dumps(
                {
                    "providers": rows,
                    "skia_source_disposition": evidence["skia_source_disposition"],
                },
                sort_keys=True,
                separators=(",", ":"),
            ).encode()
        ).hexdigest()
        retained = RetainedClaimSet(claims)
        retained.assert_full()
        return retained, evidence
    except BaseException:
        for claim in claims.values():
            claim.close()
        raise


def validate_output_path(output: Path, protected: tuple[Path, ...]) -> None:
    _require_external_path(output, protected, "output")
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ValueError("output must be a new path under an existing external directory")


def atomic_write_json(
    output: Path, payload: dict[str, Any],
    output_parent_claim: RetainedDirectoryClaim | None = None,
) -> None:
    data = (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")
    expected_digest = hashlib.sha256(data).hexdigest()
    if output.name in {"", ".", ".."}:
        raise ValueError("output must have a safe final path component")
    owned_parent_claim: RetainedDirectoryClaim | None = None
    if output_parent_claim is None:
        owned_parent_claim = retain_existing_directory(
            output.parent, "output-parent"
        )
        output_parent_claim = owned_parent_claim
    if output_parent_claim.path != output.parent:
        raise ValueError("output publication requires its exact retained parent")
    output_parent_claim.assert_current()
    parent_descriptor = output_parent_claim.descriptor
    parent_claim = output_parent_claim.identity
    temporary_name = f".{output.name}.{secrets.token_hex(16)}.tmp"
    temporary_descriptor: int | None = None
    output_linked = False
    try:
        _assert_directory_path_identity(
            output.parent, parent_descriptor, parent_claim, "output-parent"
        )
        output_parent_claim.assert_current()
        temporary_descriptor = os.open(
            temporary_name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
            dir_fd=parent_descriptor,
        )
        remaining = memoryview(data)
        while remaining:
            written = os.write(temporary_descriptor, remaining)
            if written <= 0:
                raise OSError("short write while staging A2T receipt")
            remaining = remaining[written:]
        os.fsync(temporary_descriptor)
        staged = os.fstat(temporary_descriptor)
        staged_identity = {"device": staged.st_dev, "inode": staged.st_ino}
        named_staged = os.stat(
            temporary_name, dir_fd=parent_descriptor, follow_symlinks=False
        )
        if {
            "device": named_staged.st_dev,
            "inode": named_staged.st_ino,
        } != staged_identity:
            raise ValueError("A2T staged receipt identity changed before publication")
        _assert_directory_path_identity(
            output.parent, parent_descriptor, parent_claim, "output-parent"
        )
        output_parent_claim.assert_current()
        os.link(
            temporary_name,
            output.name,
            src_dir_fd=parent_descriptor,
            dst_dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
        output_linked = True
        named_staged = os.stat(
            temporary_name, dir_fd=parent_descriptor, follow_symlinks=False
        )
        published = os.stat(
            output.name, dir_fd=parent_descriptor, follow_symlinks=False
        )
        identities = (
            {"device": named_staged.st_dev, "inode": named_staged.st_ino},
            {"device": published.st_dev, "inode": published.st_ino},
        )
        if any(identity != staged_identity for identity in identities):
            os.unlink(output.name, dir_fd=parent_descriptor)
            output_linked = False
            os.fsync(parent_descriptor)
            raise ValueError("A2T staged receipt identity changed during publication")
        os.fsync(parent_descriptor)
        _assert_directory_path_identity(
            output.parent, parent_descriptor, parent_claim, "output-parent"
        )
        output_parent_claim.assert_current()
        published = os.stat(
            output.name, dir_fd=parent_descriptor, follow_symlinks=False
        )
        if {"device": published.st_dev, "inode": published.st_ino} != staged_identity:
            os.unlink(output.name, dir_fd=parent_descriptor)
            output_linked = False
            os.fsync(parent_descriptor)
            raise ValueError("A2T output no longer names the published receipt")
        published_descriptor = os.open(
            output.name,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=parent_descriptor,
        )
        try:
            metadata = os.fstat(published_descriptor)
            named = os.stat(
                output.name, dir_fd=parent_descriptor, follow_symlinks=False
            )
            if (
                {"device": metadata.st_dev, "inode": metadata.st_ino}
                != staged_identity
                or {"device": named.st_dev, "inode": named.st_ino}
                != staged_identity
                or sha256_descriptor(published_descriptor) != expected_digest
            ):
                raise ValueError("A2T published receipt bytes differ from payload")
        finally:
            os.close(published_descriptor)
        os.fsync(parent_descriptor)
        output_parent_claim.assert_current()
    except BaseException:
        if output_linked:
            try:
                os.unlink(output.name, dir_fd=parent_descriptor)
                os.fsync(parent_descriptor)
            except OSError:
                pass
        raise
    finally:
        if temporary_descriptor is not None:
            os.close(temporary_descriptor)
        try:
            os.unlink(temporary_name, dir_fd=parent_descriptor)
        except FileNotFoundError:
            pass
        finally:
            os.fsync(parent_descriptor)
            if owned_parent_claim is not None:
                owned_parent_claim.close()


def _release_build_contract(
    repository: Path, build_dir: Path
) -> tuple[dict[str, str], dict[str, str]]:
    values = _cmake_cache(build_dir)
    try:
        home = Path(values["CMAKE_HOME_DIRECTORY"]).resolve()
    except KeyError as error:
        raise ValueError("A2T CMake cache lacks CMAKE_HOME_DIRECTORY") from error
    if home != repository.resolve():
        raise ValueError("A2T build is not configured from the exact source checkout")
    for key, expected in REQUIRED_BUILD_SETTINGS.items():
        if values.get(key) != expected:
            raise ValueError(f"A2T build requires {key}={expected}")
    return values, {key: values[key] for key in REQUIRED_BUILD_SETTINGS}


def _run_bounded_build(
    command: list[str], *, repository: Path, environment: dict[str, str], timeout: int,
    directory_claim: Any | None = None,
) -> None:
    if directory_claim is not None:
        directory_claim.assert_current()
    try:
        completed = subprocess.run(
            command, cwd=repository, env=environment, check=False,
            capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"A2T build/install timed out: {command[0]}") from error
    finally:
        if directory_claim is not None:
            directory_claim.assert_current()
    if len(completed.stdout.encode()) + len(completed.stderr.encode()) > BUILD_OUTPUT_LIMIT:
        raise ValueError("A2T build/install output exceeds 4 MiB")
    if completed.returncode != 0:
        detail = completed.stderr[-2000:] or completed.stdout[-2000:]
        raise ValueError(f"A2T build/install failed: {detail}")


def _binary_paths(build_dir: Path, prefix: Path) -> dict[str, tuple[Path, Path]]:
    return {
        "pulp": (build_dir / "pulp", prefix / "bin/pulp"),
        "pulp-cpp": (build_dir / "tools/cli/pulp-cpp", prefix / "bin/pulp-cpp"),
        "pulp-mcp": (build_dir / "tools/mcp/pulp-mcp", prefix / "bin/pulp-mcp"),
    }


def _bind_build_outputs(
    build_dir: Path, prefix: Path, claim: RetainedDirectoryClaim
) -> dict[str, dict[str, Any]]:
    outputs: dict[str, dict[str, Any]] = {}
    for role, (built, _installed) in _binary_paths(build_dir, prefix).items():
        if not built.is_file() or built.is_symlink() or not os.access(built, os.X_OK):
            raise ValueError(f"{role} build output is not a regular executable")
        digest = claim.bind_file(built, f"{role} build output")
        retained = claim.file_claims[-1]
        outputs[role] = {
            "sha256": digest,
            "bytes": retained["bytes"],
            "identity": retained["identity"],
        }
    claim.assert_current()
    return outputs


def _build_install_binary_identity(
    build_dir: Path, prefix: Path, build_outputs: dict[str, dict[str, Any]],
    claim: RetainedDirectoryClaim,
) -> dict[str, Any]:
    paths = _binary_paths(build_dir, prefix)
    if set(build_outputs) != set(paths):
        raise ValueError("retained build outputs do not bind the exact executable set")
    rows: dict[str, Any] = {}
    for role, (_built, installed) in paths.items():
        if (
            not installed.is_file() or installed.is_symlink()
            or not os.access(installed, os.X_OK)
        ):
            raise ValueError(f"{role} is not a regular executable build/install pair")
        built_digest = build_outputs[role]["sha256"]
        try:
            installed_digest = claim.bind_file(
                installed, f"installed {role}", built_digest
            )
        except ValueError as error:
            raise ValueError(
                f"installed {role} bytes differ from the exact build output"
            ) from error
        retained = claim.file_claims[-1]
        rows[role] = {
            "installed_role": f"installed-prefix/bin/{role}",
            "build_output_role": {
                "pulp": "external-build/pulp",
                "pulp-cpp": "external-build/tools/cli/pulp-cpp",
                "pulp-mcp": "external-build/tools/mcp/pulp-mcp",
            }[role],
            "sha256": installed_digest,
            "build_output_sha256": built_digest,
            "bytes": retained["bytes"],
            "build_output_bytes": build_outputs[role]["bytes"],
        }
    if rows["pulp"]["sha256"] == rows["pulp-cpp"]["sha256"]:
        raise ValueError("installed pulp is a C++ copy rather than the required Rust front")
    claim.assert_current()
    return rows


def exact_build_install_identity(
    repository: Path, revision: str, build_dir: Path, prefix: Path,
    cli: Path, mcp: Path, binaries: dict[str, Any], cmake_cache_sha256: str,
    source_tree_evidence: dict[str, Any], build_input_evidence: dict[str, Any],
    provider_input_evidence: dict[str, Any],
) -> dict[str, Any]:
    _values, settings = _release_build_contract(repository, build_dir)
    identity = installed_source_identity(repository, revision, cli, mcp)
    try:
        prefix_claim = _directory_identity(os.stat(prefix, follow_symlinks=False))
    except OSError as error:
        raise ValueError("installed prefix claim is no longer reachable") from error
    identity["build_provenance"] = {
        "method": "fresh-external-cmake-build-install-byte-identity-v1",
        "install_prefix_initial_state": "absent-and-atomically-claimed",
        "install_prefix_claim_method": (
            "unpredictable-staging-directory-renameatx-noreplace-retained-fd-v1"
        ),
        "install_prefix_claim": prefix_claim,
        "cmake_cache_sha256": cmake_cache_sha256,
        "cmake_home_revision": revision,
        "build_targets": list(BUILD_TARGETS),
        "build_settings": settings,
        "source_tree_claim": source_tree_evidence,
        "build_input_claim": build_input_evidence,
        "render_provider_input_claim": provider_input_evidence,
        "binaries": binaries,
    }
    return identity


def refresh_exact_build_install(
    repository: Path, revision: str, build_dir: Path, prefix: Path,
    planning_repository: Path, environment: dict[str, str],
) -> tuple[
    Path, Path, dict[str, Any], RetainedDirectoryClaim,
    RetainedTreeClaim, RetainedTreeClaim, RetainedClaimSet,
]:
    _require_external_path(build_dir, (repository, planning_repository), "build-dir")
    _require_external_path(prefix, (repository, planning_repository), "install-prefix")
    if not build_dir.is_dir() or build_dir.is_symlink():
        raise ValueError("build-dir must be an existing regular external directory")
    if prefix.exists() or prefix.is_symlink() or not prefix.parent.is_dir():
        raise ValueError("install-prefix must be a new path under an existing directory")
    _release_build_contract(repository, build_dir)
    retained_claim = claim_fresh_directory(prefix, "install-prefix")
    prefix_descriptor = retained_claim.descriptor
    prefix_claim = retained_claim.identity
    source_claim: RetainedTreeClaim | None = None
    build_input_claim: RetainedTreeClaim | None = None
    provider_input_claim: RetainedClaimSet | None = None
    try:
        source_claim, source_tree_evidence = retain_git_source_tree(
            repository, revision, "Pulp exact source"
        )
        prebuild_claim = CombinedClaims(retained_claim, source_claim)
        _run_bounded_build(
            ["cmake", "-S", str(repository), "-B", str(build_dir)],
            repository=repository, environment=environment, timeout=1800,
            directory_claim=prebuild_claim,
        )
        _release_build_contract(repository, build_dir)
        build_input_claim, build_input_evidence = retain_generated_build_inputs(
            build_dir, "Pulp regenerated build inputs"
        )
        provider_input_claim, provider_input_evidence = (
            retain_resolved_render_provider_inputs(
                build_dir, "Pulp resolved render providers"
            )
        )
        compilation_claim = CombinedClaims(
            retained_claim, source_claim, build_input_claim, provider_input_claim
        )
        _run_bounded_build(
            ["cmake", "--build", str(build_dir), "--target", "clean"],
            repository=repository, environment=environment, timeout=1800,
            directory_claim=compilation_claim,
        )
        cargo_target = build_dir / "experimental/pulp-rs/cargo-target"
        if cargo_target.is_symlink():
            raise ValueError("Pulp Cargo target cache must not be a symlink")
        if cargo_target.exists():
            _run_bounded_build(
                ["cmake", "-E", "remove_directory", str(cargo_target)],
                repository=repository, environment=environment, timeout=1800,
                directory_claim=compilation_claim,
            )
        if cargo_target.exists() or any(
            built.exists() for built, _installed in _binary_paths(build_dir, prefix).values()
        ):
            raise ValueError("forced-clean Pulp build retained a measured target output")
        _run_bounded_build(
            ["cmake", "--build", str(build_dir), "--target", *BUILD_TARGETS, "--parallel"],
            repository=repository, environment=environment, timeout=3600,
            directory_claim=compilation_claim,
        )
        source_claim.assert_full()
        build_input_claim.assert_full()
        provider_input_claim.assert_full()
        cmake_cache_sha256 = retained_claim.bind_file(
            build_dir / "CMakeCache.txt", "refreshed CMake cache",
        )
        _release_build_contract(repository, build_dir)
        build_outputs = _bind_build_outputs(build_dir, prefix, retained_claim)
        _assert_directory_path_identity(
            prefix, prefix_descriptor, prefix_claim, "install-prefix"
        )
        _run_bounded_build(
            ["cmake", "--install", str(build_dir), "--prefix", str(prefix)],
            repository=repository, environment=environment, timeout=1800,
            directory_claim=compilation_claim,
        )
        _assert_directory_path_identity(
            prefix, prefix_descriptor, prefix_claim, "install-prefix"
        )
        cli = prefix / "bin/pulp"
        mcp = prefix / "bin/pulp-mcp"
        build_info_sha256 = retained_claim.bind_file(
            prefix / "include/pulp/runtime/build_info.hpp",
            "installed build_info.hpp",
        )
        binaries = _build_install_binary_identity(
            build_dir, prefix, build_outputs, retained_claim
        )
        retained_claim.assert_current()
        identity = exact_build_install_identity(
            repository, revision, build_dir, prefix, cli, mcp,
            binaries, cmake_cache_sha256,
            source_tree_evidence, build_input_evidence, provider_input_evidence,
        )
        retained_claim.assert_current()
        if identity["build_info_sha256"] != build_info_sha256:
            raise ValueError("installed build_info differs from its retained claim")
        if identity["build_provenance"]["install_prefix_claim"] != prefix_claim:
            raise ValueError("installed provenance differs from the claimed prefix identity")
        return (
            cli, mcp, identity, retained_claim, source_claim, build_input_claim,
            provider_input_claim,
        )
    except BaseException:
        if provider_input_claim is not None:
            provider_input_claim.close()
        if build_input_claim is not None:
            build_input_claim.close()
        if source_claim is not None:
            source_claim.close()
        retained_claim.close()
        raise


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
    cli: Path, mcp: Path, repository: Path, environment: dict[str, str],
    directory_claim: Any | None = None,
) -> list[dict[str, Any]]:
    """Replay the required A2T fixture matrix through both installed fronts."""
    fixture_root = repository / "test/fixtures/perfetto-gpu"
    session = McpSession(mcp, environment, directory_claim)
    session.initialize()
    rows: list[dict[str, Any]] = []
    try:
        for case, filename, question, verdict, dominant, action in FIXTURE_REPLAY:
            trace = fixture_root / filename
            _, cli_first = run_cli(cli, question, trace, environment, directory_claim)
            _, cli_second = run_cli(cli, question, trace, environment, directory_claim)
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


def has_a2t_semantic_identifier(text: str) -> bool:
    """Recognize only stable exact A2T contract identifiers, not symptom prose."""
    return any(identifier in text for identifier in A2T_SEMANTIC_IDENTIFIERS)


def _tracked_paths(repository: Path, revision: str) -> set[str]:
    completed = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", revision],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    if completed.returncode != 0 or len(completed.stdout.encode()) > 4 * 1024 * 1024:
        raise ValueError(f"cannot enumerate bounded Git tree at {revision}")
    paths = completed.stdout.splitlines()
    if len(paths) > 20_000 or any(
        not path or path.startswith("/") or ".." in Path(path).parts for path in paths
    ):
        raise ValueError(f"Git tree at {revision} has an unsafe or unbounded path inventory")
    return set(paths)


def _semantic_discovery_paths(
    repository: Path, revision: str, tracked: set[str]
) -> set[str]:
    command = ["git", "grep", "-l", "-I", "-F"]
    for identifier in A2T_SEMANTIC_IDENTIFIERS:
        command.extend(("-e", identifier))
    command.extend((revision, "--", *A2T_SEMANTIC_DISCOVERY_PATHS))
    completed = subprocess.run(
        command, cwd=repository, check=False, capture_output=True, text=True,
    )
    if completed.returncode not in (0, 1) or len(completed.stdout.encode()) > 1024 * 1024:
        raise ValueError(f"cannot discover bounded A2T identifiers at {revision}")
    prefix = f"{revision}:"
    paths = {
        line.removeprefix(prefix) for line in completed.stdout.splitlines()
        if line.startswith(prefix)
    }
    if len(paths) > 256 or any(path not in tracked for path in paths):
        raise ValueError(f"A2T semantic discovery at {revision} is unsafe or unbounded")
    for path in paths:
        blob = subprocess.run(
            ["git", "show", f"{revision}:{path}"],
            cwd=repository, check=False, capture_output=True,
        )
        if (
            blob.returncode != 0
            or len(blob.stdout) > 2 * 1024 * 1024
            or not has_a2t_semantic_identifier(blob.stdout.decode(errors="replace"))
        ):
            raise ValueError(f"A2T semantic discovery returned an invalid blob at {revision}")
    return paths


def _skip_cxx_quoted(text: str, index: int, quote: str) -> int:
    index += 1
    while index < len(text):
        if text[index] == "\\":
            index += 2
        elif text[index] == quote:
            return index + 1
        else:
            index += 1
    return len(text)


def _is_cxx_quote_start(text: str, index: int) -> bool:
    if text[index] != "'":
        return text[index] == '"'
    return not (
        index > 0
        and index + 1 < len(text)
        and text[index - 1].isalnum()
        and text[index + 1].isalnum()
    )


def _bounded_product_trace_calls(text: str) -> tuple[str, ...]:
    """Lex bounded macro calls outside comments and string/character literals."""
    calls: list[str] = []
    index = 0
    while index < len(text):
        if text[index] == "#" and not text[text.rfind("\n", 0, index) + 1:index].strip():
            directive_start = index
            while True:
                newline = text.find("\n", index + 1)
                if newline < 0:
                    index = len(text)
                    break
                continued = text[text.rfind("\n", 0, newline) + 1:newline].rstrip(
                    " \t\r"
                ).endswith("\\")
                index = newline + 1
                if not continued:
                    break
            directive = re.sub(
                r"\\\r?\n", " ", text[directive_start:index]
            )
            define = re.match(
                r"\s*#\s*define\s+[A-Za-z_][A-Za-z0-9_]*"
                r"(?:\s*\([^\r\n)]*\))?",
                directive,
            )
            body = directive[define.end():] if define else directive.lstrip()[1:]
            if any(macro in body for macro in PRODUCT_TRACE_MACROS):
                # Wrapper definitions are product producer call sites too. Scan
                # their body conservatively; the synthetic semicolon terminates
                # the common final macro call omitted by #define syntax.
                calls.extend(_bounded_product_trace_calls(body.rstrip() + ";"))
            continue
        if text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = len(text) if newline < 0 else newline + 1
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = len(text) if end < 0 else end + 2
            continue
        if text[index] in {'"', "'"} and _is_cxx_quote_start(text, index):
            index = _skip_cxx_quoted(text, index, text[index])
            continue
        if not (text[index].isalpha() or text[index] == "_"):
            index += 1
            continue
        token_start = index
        index += 1
        while index < len(text) and (text[index].isalnum() or text[index] == "_"):
            index += 1
        token = text[token_start:index]
        if token not in PRODUCT_TRACE_MACROS:
            continue
        cursor = index
        while cursor < len(text) and text[cursor].isspace():
            cursor += 1
        if cursor >= len(text) or text[cursor] != "(":
            continue
        depth = 0
        call_end: int | None = None
        while cursor < len(text) and cursor - token_start <= MAX_PRODUCT_TRACE_CALL_BYTES:
            if text.startswith("//", cursor):
                newline = text.find("\n", cursor + 2)
                cursor = len(text) if newline < 0 else newline + 1
                continue
            if text.startswith("/*", cursor):
                end = text.find("*/", cursor + 2)
                cursor = len(text) if end < 0 else end + 2
                continue
            if text[cursor] in {'"', "'"} and _is_cxx_quote_start(text, cursor):
                cursor = _skip_cxx_quoted(text, cursor, text[cursor])
                continue
            if text[cursor] == "(":
                depth += 1
            elif text[cursor] == ")":
                depth -= 1
                if depth == 0:
                    call_end = cursor + 1
                    break
            cursor += 1
        if call_end is None:
            raise ValueError(
                "recognized product trace call is unterminated or exceeds the bounded scan"
            )
        cursor = call_end
        while cursor < len(text):
            if text[cursor].isspace():
                cursor += 1
                continue
            if text.startswith("//", cursor):
                newline = text.find("\n", cursor + 2)
                if newline < 0:
                    raise ValueError(
                        "recognized product trace call has no terminating semicolon"
                    )
                cursor = newline + 1
                continue
            if text.startswith("/*", cursor):
                end = text.find("*/", cursor + 2)
                if end < 0:
                    raise ValueError(
                        "recognized product trace call has an unterminated trailing comment"
                    )
                cursor = end + 2
                continue
            break
        if cursor >= len(text) or text[cursor] != ";":
            raise ValueError(
                "recognized product trace call has no terminating semicolon"
            )
        calls.append(text[token_start:cursor + 1])
        index = cursor + 1
    return tuple(calls)


def _cxx_string_literals(text: str) -> tuple[str, ...]:
    literals: list[str] = []
    previous_end: int | None = None
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = len(text) if newline < 0 else newline + 1
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = len(text) if end < 0 else end + 2
        elif text[index] == "'" and _is_cxx_quote_start(text, index):
            index = _skip_cxx_quoted(text, index, "'")
        elif text[index] == '"':
            raw_prefix = text[max(0, index - 3):index]
            if re.search(r"(?:u8|u|U|L)?R$", raw_prefix):
                raise ValueError(
                    "recognized product trace call uses an unsupported raw string literal"
                )
            if previous_end is not None:
                separator = re.sub(
                    r"//[^\n]*(?:\n|$)|/\*.*?\*/", "", text[previous_end:index],
                    flags=re.DOTALL,
                )
                if re.sub(r"\s+", "", separator) in {"", "u8", "u", "U", "L"}:
                    raise ValueError(
                        "recognized product trace call uses unsupported adjacent string literals"
                    )
            end = _skip_cxx_quoted(text, index, '"')
            literal = text[index + 1:max(index + 1, end - 1)]
            if "\\" in literal:
                raise ValueError(
                    "recognized product trace call uses an unsupported escaped string literal"
                )
            literals.append(literal)
            previous_end = end
            index = end
        else:
            index += 1
    return tuple(literals)


def _product_trace_call_identity(call: str) -> tuple[str, str]:
    """Return literal category/event names or reject an ambiguous source form."""
    macro = call[:call.find("(")].strip()
    normalized = _normalize_product_trace_call(call)
    literal = r'(?:u8|u|U|L)?"([^"\\]*)"'
    if macro == "PULP_TRACE_SCOPE":
        match = re.match(
            rf"^{re.escape(macro)}\(\s*{literal}\s*\)", normalized
        )
        if match is None:
            raise ValueError(
                "recognized product trace call has an unsupported literal category"
            )
        return match.group(1), ""
    category_match = re.match(
        rf"^{re.escape(macro)}\(\s*{literal}\s*,", normalized
    )
    if category_match is None:
        raise ValueError(
            "recognized product trace call has an unsupported literal category"
        )
    category = category_match.group(1)
    if macro == "PULP_TRACE_SCOPE_DYNAMIC":
        if category != "gpu":
            raise ValueError(
                "recognized dynamic trace call has an unsupported non-GPU category"
            )
        return category, ""
    named_match = re.match(
        rf"^{re.escape(macro)}\(\s*{literal}\s*,\s*{literal}\s*(?:,|\))",
        normalized,
    )
    if named_match is None:
        raise ValueError(
            "recognized product trace call has an unsupported literal event name"
        )
    return named_match.group(1), named_match.group(2)


def _has_cxx_identifier(text: str, identifiers: set[str]) -> bool:
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = len(text) if newline < 0 else newline + 1
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = len(text) if end < 0 else end + 2
        elif text[index] in {'"', "'"} and _is_cxx_quote_start(text, index):
            index = _skip_cxx_quoted(text, index, text[index])
        elif text[index].isalpha() or text[index] == "_":
            start = index
            index += 1
            while index < len(text) and (text[index].isalnum() or text[index] == "_"):
                index += 1
            if text[start:index].lower() in identifiers:
                return True
        else:
            index += 1
    return False


def _normalize_product_trace_call(text: str) -> str:
    pieces: list[str] = []
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = len(text) if newline < 0 else newline + 1
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = len(text) if end < 0 else end + 2
        elif text[index] in {'"', "'"} and _is_cxx_quote_start(text, index):
            end = _skip_cxx_quoted(text, index, text[index])
            pieces.append(text[index:end])
            index = end
        else:
            pieces.append(text[index])
            index += 1
    normalized = re.sub(r"\s+", " ", "".join(pieces)).strip()
    return re.sub(r"\s+;", ";", normalized)


def product_producer_signatures(text: str) -> tuple[str, ...]:
    """Extract bounded GPU-relevant trace call sites independently of evidence keys."""
    signatures = []
    for call in _bounded_product_trace_calls(text):
        string_literals = _cxx_string_literals(call)
        category, event_name = _product_trace_call_identity(call)
        gpu_relevant = (
            PRODUCER_SOURCE_ANNOTATION.strip('"') in string_literals
            or category == "gpu"
            or event_name.startswith("gpu_")
            or _has_cxx_identifier(call, {"gpu_evidence", "gpu_evidence_id"})
        )
        if gpu_relevant:
            signatures.append(_normalize_product_trace_call(call))
    return tuple(signatures)


def is_product_producer_source(text: str) -> bool:
    """Recognize bounded runtime GPU trace call sites, not prose or key references."""
    return bool(product_producer_signatures(text))


def has_added_product_producer(
    base_signatures: tuple[str, ...], source_signatures: tuple[str, ...]
) -> bool:
    return bool(Counter(source_signatures) - Counter(base_signatures))


def _product_producer_paths(
    repository: Path, revision: str, tracked: set[str]
) -> dict[str, tuple[str, ...]]:
    completed = subprocess.run(
        [
            "git", "grep", "-l", "-I", "-F", "-e", "PULP_TRACE_",
            revision, "--", *PRODUCT_PRODUCER_ROOTS,
        ],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    if completed.returncode not in (0, 1) or len(completed.stdout.encode()) > 1024 * 1024:
        raise ValueError(f"cannot discover product GPU producers at {revision}")
    prefix = f"{revision}:"
    candidates = {
        line.removeprefix(prefix) for line in completed.stdout.splitlines()
        if line.startswith(prefix)
    }
    if len(candidates) > 128 or any(path not in tracked for path in candidates):
        raise ValueError(f"product GPU producer discovery at {revision} is unsafe or unbounded")
    producers: dict[str, tuple[str, ...]] = {}
    for path in candidates:
        blob = subprocess.run(
            ["git", "show", f"{revision}:{path}"],
            cwd=repository, check=False, capture_output=True,
        )
        if blob.returncode != 0 or len(blob.stdout) > 2 * 1024 * 1024:
            raise ValueError(f"cannot inspect bounded product producer blob {path}")
        signatures = product_producer_signatures(blob.stdout.decode(errors="replace"))
        if signatures:
            producers[path] = signatures
    return producers


def _a3_authority_paths(repository: Path, source_revision: str) -> tuple[set[str], str]:
    blob = subprocess.run(
        ["git", "show", f"{source_revision}:{A3_SCOPE_AUTHORITY_PATH}"],
        cwd=repository, check=False, capture_output=True,
    )
    if blob.returncode != 0 or len(blob.stdout) > 2 * 1024 * 1024:
        raise ValueError("cannot read bounded A3 scope authority from source revision")
    try:
        tree = ast.parse(blob.stdout.decode("utf-8"), filename=A3_SCOPE_AUTHORITY_PATH)
    except (UnicodeDecodeError, SyntaxError) as error:
        raise ValueError(f"cannot parse A3 scope authority: {error}") from error
    values: list[Any] = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "A3_IMPLEMENTATION_SOURCE_PATHS"
            for target in node.targets
        ):
            try:
                values.append(ast.literal_eval(node.value))
            except (ValueError, TypeError, SyntaxError) as error:
                raise ValueError("A3 scope authority is not a literal path set") from error
    if len(values) != 1 or not isinstance(values[0], set):
        raise ValueError("A3 scope authority lacks one literal implementation path set")
    paths = values[0]
    if len(paths) > 64 or any(
        not isinstance(path, str)
        or not path
        or path.startswith("/")
        or ".." in Path(path).parts
        for path in paths
    ):
        raise ValueError("A3 scope authority has an unsafe or unbounded path set")
    authority_blob = git_blobs(
        repository, source_revision, {A3_SCOPE_AUTHORITY_PATH}
    ).get(A3_SCOPE_AUTHORITY_PATH)
    if authority_blob is None:
        raise ValueError("A3 scope authority has no exact source blob")
    return paths, authority_blob


def _producer_introduction(
    repository: Path, source_revision: str, path: str
) -> tuple[str, str]:
    completed = subprocess.run(
        [
            "git", "log", "--reverse", "--format=%H%x00%s",
            f"{A2T_SCOPE_BASE}..{source_revision}", "--", path,
        ],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    if completed.returncode != 0 or len(completed.stdout.encode()) > 1024 * 1024:
        raise ValueError(f"cannot derive bounded product producer introduction for {path}")
    for line in completed.stdout.splitlines():
        try:
            revision, subject = line.split("\x00", 1)
        except ValueError:
            continue
        parent = subprocess.run(
            ["git", "rev-parse", f"{revision}^"],
            cwd=repository, check=False, capture_output=True, text=True,
        )
        if parent.returncode != 0 or not valid_lower_hex(parent.stdout.strip(), 40):
            continue
        before = subprocess.run(
            ["git", "show", f"{parent.stdout.strip()}:{path}"],
            cwd=repository, check=False, capture_output=True,
        )
        after = subprocess.run(
            ["git", "show", f"{revision}:{path}"],
            cwd=repository, check=False, capture_output=True,
        )
        if len(before.stdout) > 2 * 1024 * 1024 or len(after.stdout) > 2 * 1024 * 1024:
            raise ValueError(f"product producer source is unbounded for {path}")
        before_signatures = product_producer_signatures(
            before.stdout.decode(errors="replace")
        ) if before.returncode == 0 else ()
        after_signatures = product_producer_signatures(
            after.stdout.decode(errors="replace")
        ) if after.returncode == 0 else ()
        if has_added_product_producer(before_signatures, after_signatures):
            if not valid_lower_hex(revision, 40):
                break
            return revision, subject
    raise ValueError(f"cannot identify product producer introduction for {path}")


def _revision_changed_paths(repository: Path, revision: str) -> set[str]:
    completed = subprocess.run(
        [
            "git", "diff-tree", "--no-commit-id", "--name-only", "-r",
            revision,
        ],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    paths = set(completed.stdout.splitlines()) if completed.returncode == 0 else set()
    if len(paths) > 256 or any(
        not path or path.startswith("/") or ".." in Path(path).parts for path in paths
    ):
        raise ValueError(f"cannot derive bounded producer-introduction paths for {revision}")
    return paths


def _non_a2t_producer_authorities(
    repository: Path, source_revision: str
) -> dict[str, dict[str, Any]]:
    """Load bounded, immutable-commit package authorities for external producers."""
    blob = subprocess.run(
        ["git", "show", f"{source_revision}:{A2T_SCOPE_MANIFEST_PATH}"],
        cwd=repository, check=False, capture_output=True,
    )
    if blob.returncode != 0 or len(blob.stdout) > 64 * 1024:
        raise ValueError("cannot read bounded non-A2T producer authorities")
    try:
        manifest = json.loads(blob.stdout.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot parse non-A2T producer authorities: {error}") from error
    rows = manifest.get("non_a2t_product_producer_authorities")
    if not isinstance(rows, list) or len(rows) > 16:
        raise ValueError("non-A2T producer authorities must be a bounded list")
    expected_keys = {
        "owner_package", "introducing_revision", "introducing_parent",
        "introducing_subject", "changed_paths", "added_producer_signatures",
    }
    authorities: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict) or set(row) != expected_keys:
            raise ValueError("non-A2T producer authority has an invalid schema")
        revision = row["introducing_revision"]
        parent = row["introducing_parent"]
        subject = row["introducing_subject"]
        owner = row["owner_package"]
        changed = row["changed_paths"]
        signatures = row["added_producer_signatures"]
        if (
            not valid_lower_hex(revision, 40)
            or not valid_lower_hex(parent, 40)
            or not isinstance(subject, str)
            or not subject
            or not isinstance(owner, str)
            or not owner
            or not isinstance(changed, list)
            or len(changed) > 64
            or any(not isinstance(path, str) for path in changed)
            or len(set(changed)) != len(changed)
            or not isinstance(signatures, dict)
            or not signatures
            or len(signatures) > 16
        ):
            raise ValueError("non-A2T producer authority is unsafe or unbounded")
        all_paths = changed + list(signatures)
        if any(
            not isinstance(path, str)
            or not path
            or path.startswith("/")
            or ".." in Path(path).parts
            for path in all_paths
        ) or not set(signatures).issubset(changed):
            raise ValueError("non-A2T producer authority has an unsafe path contract")
        if any(
            not isinstance(values, list)
            or not values
            or len(values) > 32
            or any(not isinstance(value, str) or not value for value in values)
            for values in signatures.values()
        ):
            raise ValueError("non-A2T producer authority has invalid signatures")
        identity = subprocess.run(
            ["git", "show", "-s", "--format=%H%x00%P%x00%s", revision],
            cwd=repository, check=False, capture_output=True, text=True,
        )
        if (
            identity.returncode != 0
            or len(identity.stdout.encode()) > 4096
            or identity.stdout.rstrip("\n") != f"{revision}\x00{parent}\x00{subject}"
            or _revision_changed_paths(repository, revision) != set(changed)
        ):
            raise ValueError("non-A2T producer authority does not match its immutable commit")
        ancestor = subprocess.run(
            ["git", "merge-base", "--is-ancestor", revision, source_revision],
            cwd=repository, check=False, capture_output=True,
        )
        if ancestor.returncode != 0:
            raise ValueError("non-A2T producer authority is not in source history")
        for path, declared in signatures.items():
            before = subprocess.run(
                ["git", "show", f"{parent}:{path}"], cwd=repository,
                check=False, capture_output=True,
            )
            after = subprocess.run(
                ["git", "show", f"{revision}:{path}"], cwd=repository,
                check=False, capture_output=True,
            )
            if len(before.stdout) > 2 * 1024 * 1024 or len(after.stdout) > 2 * 1024 * 1024:
                raise ValueError("non-A2T producer authority source is unbounded")
            before_signatures = product_producer_signatures(
                before.stdout.decode(errors="replace")
            ) if before.returncode == 0 else ()
            after_signatures = product_producer_signatures(
                after.stdout.decode(errors="replace")
            ) if after.returncode == 0 else ()
            added = Counter(after_signatures) - Counter(before_signatures)
            if added != Counter(declared):
                raise ValueError("non-A2T producer authority signature delta does not match")
            if path in authorities:
                raise ValueError("non-A2T producer authority path is duplicated")
            authorities[path] = row
    return authorities


def classify_non_a2t_product_producers(
    repository: Path, source_revision: str, producer_paths: set[str]
) -> list[dict[str, Any]]:
    """Expose later package-owned producers without attributing them to A2T."""
    a3_paths, authority_blob = _a3_authority_paths(repository, source_revision)
    commit_authorities = _non_a2t_producer_authorities(repository, source_revision)
    if not set(commit_authorities).issubset(producer_paths):
        raise ValueError("non-A2T producer authority does not name a discovered delta")
    source_blobs = git_blobs(repository, source_revision, producer_paths)
    base_blobs = git_blobs(repository, A2T_SCOPE_BASE, producer_paths)
    rows: list[dict[str, Any]] = []
    for path in sorted(producer_paths):
        revision, subject = _producer_introduction(repository, source_revision, path)
        introduction_paths = _revision_changed_paths(repository, revision)
        is_a3_producer = (
            path in a3_paths
            and path in introduction_paths
            and A3_SCOPE_AUTHORITY_PATH in introduction_paths
        )
        commit_authority = commit_authorities.get(path)
        if is_a3_producer and commit_authority is not None:
            raise ValueError(f"product producer has overlapping package authorities: {path}")
        if not is_a3_producer and commit_authority is None:
            raise ValueError(f"unclassified product producer path cannot use A2T disposition: {path}")
        if commit_authority is not None and (
            revision != commit_authority["introducing_revision"]
            or subject != commit_authority["introducing_subject"]
        ):
            raise ValueError(f"product producer introduction differs from authority: {path}")
        if is_a3_producer:
            owner_package = "A3-first-visible-product-evidence"
            scope_authority = {
                "kind": "immutable-package-path-set",
                "path": A3_SCOPE_AUTHORITY_PATH,
                "source_blob": authority_blob,
                "symbol": "A3_IMPLEMENTATION_SOURCE_PATHS",
                "producer_introduction_touched_authority_path": True,
            }
        else:
            owner_package = commit_authority["owner_package"]
            scope_authority = {
                "kind": "immutable-git-commit-boundary",
                "manifest_path": A2T_SCOPE_MANIFEST_PATH,
                "introducing_revision": commit_authority["introducing_revision"],
                "introducing_parent": commit_authority["introducing_parent"],
                "changed_paths": commit_authority["changed_paths"],
                "added_producer_signatures": commit_authority[
                    "added_producer_signatures"
                ][path],
                "producer_introduction_touched_authority_path": False,
            }
        rows.append({
            "path": path,
            "base_blob": base_blobs.get(path),
            "source_blob": source_blobs.get(path),
            "owner_package": owner_package,
            "scope_authority": scope_authority,
            "introducing_revision": revision,
            "introducing_subject": subject,
            "overhead_evidence_required_from_owner": True,
            "owner_evidence_status": "external-not-evaluated-by-a2t",
        })
    return rows


def _rule_discovery_paths(tracked: set[str]) -> set[str]:
    """Discover exact bounded behavior families that may be binary or identifier-free."""
    return {
        path for path in tracked
        if (
            path.startswith("test/fixtures/perfetto-gpu/")
            or (
                path.startswith("experimental/pulp-rs/src/cmd/trace")
                and path.endswith(".rs")
            )
            or (
                path.startswith("experimental/pulp-rs/tests/")
                and "trace_gpu_analysis" in Path(path).name
            )
            or (
                path.startswith("tools/scripts/")
                and "gpu_trace_overhead" in Path(path).name
            )
        )
    }


def authoritative_a2t_scope_paths(
    repository: Path, source_revision: str
) -> tuple[set[str], set[str], list[dict[str, Any]]]:
    """Derive current scope from immutable Git objects and bounded contract rules."""
    accepted_run = subprocess.run(
        [
            "git", "diff-tree", "--no-commit-id", "--name-only", "-r",
            A2T_INTEGRATED_PATCH_EQUIVALENT,
        ],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    accepted_paths = set(accepted_run.stdout.splitlines())
    if accepted_run.returncode != 0 or len(accepted_paths) != 29:
        raise ValueError("cannot derive the canonical-plan 29-path A2T implementation")
    base_tracked = _tracked_paths(repository, A2T_SCOPE_BASE)
    source_tracked = _tracked_paths(repository, source_revision)
    discovered = (
        _rule_discovery_paths(base_tracked)
        | _rule_discovery_paths(source_tracked)
        | _semantic_discovery_paths(repository, A2T_SCOPE_BASE, base_tracked)
        | _semantic_discovery_paths(repository, source_revision, source_tracked)
    )
    base_producers = _product_producer_paths(repository, A2T_SCOPE_BASE, base_tracked)
    source_producers = _product_producer_paths(
        repository, source_revision, source_tracked
    )
    producer_paths = {
        path for path, signatures in source_producers.items()
        if has_added_product_producer(base_producers.get(path, ()), signatures)
    }
    external_producers = classify_non_a2t_product_producers(
        repository, source_revision, producer_paths
    )
    paths = (accepted_paths | A2T_FIXED_SCOPE_PATHS | discovered) - producer_paths
    if len(paths) > 128 or any(
        path not in base_tracked and path not in source_tracked for path in paths
    ):
        raise ValueError("derived A2T scope is missing, unsafe, or unbounded")
    return paths, accepted_paths, external_producers


def _load_a2t_scope_manifest(
    repository: Path, source_revision: str
) -> dict[str, Any]:
    """Load and cross-check the independent exact-path A2T scope contract."""
    path = repository / A2T_SCOPE_MANIFEST_PATH
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 64 * 1024:
        raise ValueError("A2T scope manifest must be a bounded regular file")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read A2T scope manifest: {error}") from error
    if not isinstance(manifest, dict):
        raise ValueError("A2T scope manifest must be an object")
    accepted = manifest.get("accepted_plan_implementation")
    expected_accepted = {
        "plan_revision": "641649b7e7fece6baae34380b6e719904506af22",
        "original_revision": "69059fa0bf8f8878a735909115bb2dc2831c2907",
        "replay_revision": "b7c118d0c98aa4e3d1c7b874ee704c8053a01bf5",
        "integrated_patch_equivalent": A2T_INTEGRATED_PATCH_EQUIVALENT,
        "stable_patch_id": "a5d5850162385c1cbcb2cf34344fea2511636353",
        "path_count": 29,
    }
    producer_prefixes = [
        "core/runtime/", "core/render/", "core/view/", "core/format/", "inspect/",
    ]
    paths = manifest.get("scope_paths")
    if (
        manifest.get("schema") != "pulp.gpu-trace-overhead-scope.v1"
        or manifest.get("base_revision") != A2T_SCOPE_BASE
        or accepted != expected_accepted
        or manifest.get("producer_prefixes") != producer_prefixes
        or not isinstance(paths, list)
        or paths != sorted(paths)
        or len(paths) != len(set(paths))
    ):
        raise ValueError("A2T scope manifest differs from the authoritative current path contract")
    if any(
        not isinstance(path_value, str)
        or not path_value
        or path_value.startswith("/")
        or ".." in Path(path_value).parts
        for path_value in paths
    ):
        raise ValueError("A2T scope manifest contains an unsafe path")
    if any(path_value.startswith(tuple(producer_prefixes)) for path_value in paths):
        raise ValueError("A2T path-scoped contract includes a product producer path")
    authoritative_paths, accepted_paths, _external_producers = authoritative_a2t_scope_paths(
        repository, source_revision
    )
    if set(paths) != authoritative_paths or len(accepted_paths) != expected_accepted["path_count"]:
        raise ValueError(
            "A2T scope manifest differs from independently discovered Git-object paths"
        )
    return manifest


def _stable_patch_id(repository: Path, revision: str) -> str:
    patch = subprocess.run(
        ["git", "-C", str(repository), "show", revision],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    patch_id_run = subprocess.run(
        ["git", "patch-id", "--stable"], input=patch.stdout,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if patch.returncode != 0 or patch_id_run.returncode != 0 or not patch_id_run.stdout:
        raise ValueError("cannot calculate stable patch id for accepted A2T implementation")
    output = (
        patch_id_run.stdout.decode() if isinstance(patch_id_run.stdout, bytes)
        else patch_id_run.stdout
    )
    patch_id = output.split()[0]
    if not valid_lower_hex(patch_id, 40):
        raise ValueError("accepted A2T implementation has an invalid stable patch id")
    return patch_id


def path_limited_changed_paths(
    changed_paths: list[str], scope_paths: set[str]
) -> list[str]:
    """Keep only exact manifest paths from a possibly mixed commit inventory."""
    return sorted({path for path in changed_paths if path in scope_paths})


def a2t_scope_inventory(repository: Path, source_revision: str) -> dict[str, Any]:
    """Recompute the immutable-base-to-source, path-scoped A2T tree delta."""
    if not valid_lower_hex(source_revision, 40):
        raise ValueError("A2T scope source revision must be exact lowercase 40-hex")
    manifest = _load_a2t_scope_manifest(repository, source_revision)
    authoritative_paths, _accepted_paths, external_producers = (
        authoritative_a2t_scope_paths(repository, source_revision)
    )
    if set(manifest["scope_paths"]) != authoritative_paths:
        raise ValueError("A2T manifest changed after independent scope discovery")
    base_revision = manifest["base_revision"]
    paths = manifest["scope_paths"]
    ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", base_revision, source_revision],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    if ancestor.returncode != 0:
        raise ValueError("immutable pre-A2T scope base is not in source history")
    accepted = manifest["accepted_plan_implementation"]
    equivalent = accepted["integrated_patch_equivalent"]
    equivalent_ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", equivalent, source_revision],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    if equivalent_ancestor.returncode != 0:
        raise ValueError("accepted A2T patch-equivalent revision is not in source history")
    if _stable_patch_id(repository, equivalent) != accepted["stable_patch_id"]:
        raise ValueError("integrated A2T implementation does not match the accepted stable patch")
    accepted_paths_run = subprocess.run(
        ["git", "diff-tree", "--no-commit-id", "--name-only", "-r", equivalent],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    if (
        accepted_paths_run.returncode != 0
        or len(set(accepted_paths_run.stdout.splitlines())) != accepted["path_count"]
    ):
        raise ValueError("integrated A2T implementation differs from the accepted 29-path scope")

    base_blobs = git_blobs(repository, base_revision, set(paths))
    source_blobs = git_blobs(repository, source_revision, set(paths))
    deltas = [
        {"path": path, "base_blob": base_blobs.get(path), "source_blob": source_blobs.get(path)}
        for path in paths
        if base_blobs.get(path) != source_blobs.get(path)
    ]
    if not deltas:
        raise ValueError("A2T path-scoped tree delta is empty")

    history = subprocess.run(
        [
            "git", "rev-list", "--first-parent", "--reverse",
            f"{base_revision}..{source_revision}", "--", *paths,
        ],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    revisions = history.stdout.splitlines() if history.returncode == 0 else []
    if not revisions or len(revisions) > 128 or any(
        not valid_lower_hex(revision, 40) for revision in revisions
    ):
        raise ValueError("cannot derive bounded A2T scope-touching history")
    touching: list[dict[str, Any]] = []
    scope_set = set(paths)
    for revision in revisions:
        changed = subprocess.run(
            [
                "git", "diff-tree", "-m", "--first-parent", "--no-commit-id",
                "--name-only", "-r", revision, "--", *paths,
            ],
            cwd=repository, check=False, capture_output=True, text=True,
        )
        changed_paths = path_limited_changed_paths(
            changed.stdout.splitlines(), scope_set
        ) if changed.returncode == 0 else []
        if not changed_paths:
            raise ValueError(f"cannot derive path-limited A2T contribution for {revision}")
        touching.append({
            "revision": revision,
            "path_limited_changed_paths": changed_paths,
        })
    producer_prefixes = manifest["producer_prefixes"]
    producer_paths = sorted({
        row["path"] for row in deltas
        if row["path"].startswith(tuple(producer_prefixes))
    })
    return {
        "method": "immutable-base-to-source path-scoped tree delta",
        "manifest": {
            "path": A2T_SCOPE_MANIFEST_PATH,
            "sha256": sha256(repository / A2T_SCOPE_MANIFEST_PATH),
            "schema": manifest["schema"],
            "scope_path_count": len(paths),
        },
        "accepted_plan_implementation": accepted,
        "base_revision": base_revision,
        "source_revision": source_revision,
        "path_deltas": deltas,
        "scope_touching_revisions": touching,
        "producer_prefixes_checked": producer_prefixes,
        "a2t_scoped_producer_paths": producer_paths,
        "no_a2t_scoped_producer_delta": not producer_paths,
        "non_a2t_product_producers": external_producers,
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


def executing_checkout_source_identity(
    repository: Path, revision: str
) -> dict[str, Any]:
    """Bind executed A2T Python sources to their one exact live checkout."""
    repository = repository.resolve()
    script_repository = SCRIPT_DIR.parents[1].resolve()
    if repository != script_repository:
        raise ValueError(
            "A2T repository must be the exact checkout containing the executing "
            "recorder, verifier, and support modules"
        )
    assert_exact_live_head(repository, revision)
    for relative in A2T_EXECUTING_SOURCE_PATHS:
        path = repository / relative
        if path.is_symlink() or not path.is_file() or path.parent != SCRIPT_DIR:
            raise ValueError(
                f"executing A2T source is not an exact sibling regular file: {relative}"
            )
    paths = set(A2T_EXECUTING_SOURCE_PATHS)
    historical = git_blobs(repository, revision, paths)
    head = git_blobs(repository, "HEAD", paths)
    checkout = checkout_blobs(repository, paths)
    if set(historical) != paths or historical != head or historical != checkout:
        raise ValueError(
            "executing A2T recorder/verifier/support sources are not exact-head Git blobs"
        )
    return {
        "method": "same-executing-checkout-exact-git-blobs-v1",
        "repository": "Generous-Corp/pulp",
        "revision": revision,
        "source_blobs": historical,
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
    return (
        A2T_ANALYZER_SOURCE_PATHS
        | FIXTURE_SOURCE_PATHS
        | A2T_PRODUCER_AUTHORITY_SOURCE_PATHS
    )


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
    artifacts = receipt.get("artifacts")
    trace_artifact = artifacts.get("trace") if isinstance(artifacts, dict) else None
    role = trace_artifact.get("role") if isinstance(trace_artifact, dict) else None
    if not isinstance(role, str) or not role.startswith("repository/"):
        errors.append("trace role does not identify a repository fixture")
        return errors
    trace_path = role.removeprefix("repository/")
    if trace_path not in FIXTURE_SOURCE_PATHS:
        errors.append("trace role does not identify a required replay fixture")
    expected = (
        A2T_ANALYZER_SOURCE_PATHS
        | FIXTURE_SOURCE_PATHS
        | A2T_PRODUCER_AUTHORITY_SOURCE_PATHS
    )
    declared = receipt.get("source_blobs")
    if not isinstance(declared, dict) or set(declared) != expected:
        errors.append(
            "source_blobs does not bind the exact A2T behavior/build/fixture/producer-authority set"
        )
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


def _identity_names_automated_system(value: str) -> bool:
    """Reject normalized agent/model identities, including numeric versions."""
    normalized = unicodedata.normalize("NFKC", value).casefold()
    tokens = re.findall(r"[a-z0-9]+", normalized)
    for token in tokens:
        base = re.sub(r"[0-9]+$", "", token)
        if base in NON_HUMAN_IDENTITY_TOKENS:
            return True
    compact = "".join(tokens)
    return any(phrase in compact for phrase in NON_HUMAN_IDENTITY_PHRASES)


def validate_independent_human_review_document(
    review_document: Any,
    *,
    question: str,
    trace_sha256: str,
    semantic_result: dict[str, Any],
) -> dict[str, Any]:
    """Validate a dedicated same-artifact review document structurally.

    This function deliberately does not certify terminal acceptance.  The
    caller must additionally bind the document to its independently published
    Git blob; final authority lives outside this recorder and verifier.
    """
    if question != "gpu-startup":
        raise ValueError("human Perfetto UI correlation applies only to gpu-startup")
    if not isinstance(review_document, dict):
        raise ValueError("independent human-review document must be an object")
    if review_document.get("schema") != HUMAN_REVIEW_SCHEMA:
        raise ValueError(
            f"independent human-review document schema must be {HUMAN_REVIEW_SCHEMA}"
        )
    expected_document_fields = {
        "schema", "question", "artifact_sha256", "review_authority",
        "reviewed_utc", "ui_revision", "delivery", "observed_spans",
    }
    missing_document_fields = expected_document_fields - set(review_document)
    if missing_document_fields:
        raise ValueError(
            "independent human-review document lacks required fields: "
            + ", ".join(sorted(missing_document_fields))
        )
    if set(review_document) != expected_document_fields:
        raise ValueError(
            "independent human-review document must contain only the exact review fields"
        )
    if review_document.get("question") != "gpu-startup":
        raise ValueError("independent human-review document must be for gpu-startup")
    human_digest = review_document.get("artifact_sha256")
    if not isinstance(human_digest, str) or not valid_lower_hex(human_digest, 64):
        raise ValueError("independent human-review document has an invalid artifact SHA-256")
    if human_digest != trace_sha256:
        raise ValueError("independent human-review document is not bound to the measured trace")
    authority = review_document.get("review_authority")
    if not isinstance(authority, dict):
        raise ValueError("independent human-review document lacks review_authority")
    expected_authority_fields = {
        "kind", "reviewer_kind", "reviewer", "git_commit_author", "attestation"
    }
    missing_authority_fields = expected_authority_fields - set(authority)
    if missing_authority_fields:
        raise ValueError(
            "independent human-review authority lacks required identity fields: "
            + ", ".join(sorted(missing_authority_fields))
        )
    if set(authority) != expected_authority_fields:
        raise ValueError(
            "independent human-review authority must contain only the exact identity fields"
        )
    if (
        authority.get("kind") != "independent-human-same-artifact-review"
        or authority.get("reviewer_kind") != "human"
    ):
        raise ValueError(
            "independent review must declare human same-artifact authority"
        )
    for field in ("reviewer", "git_commit_author", "attestation"):
        if not isinstance(authority.get(field), str) or not authority[field].strip():
            raise ValueError(
                f"independent human-review document lacks nonempty review_authority.{field}"
            )
    reviewer = authority["reviewer"].strip()
    git_author = authority["git_commit_author"].strip()
    if (
        len(reviewer) > 200
        or _identity_names_automated_system(reviewer)
        or _identity_names_automated_system(git_author)
    ):
        raise ValueError(
            "independent human-review identity names an automated agent or model"
        )
    if len(git_author) > 320:
        raise ValueError("independent human-review Git author identity is too long")
    attestation = authority["attestation"].strip()
    if len(attestation) < 32 or len(attestation) > 2000:
        raise ValueError(
            "independent human-review attestation must be a bounded substantive statement"
        )
    for field in ("reviewed_utc", "ui_revision", "delivery"):
        if (
            not isinstance(review_document.get(field), str)
            or not review_document[field].strip()
        ):
            raise ValueError(f"independent human-review document lacks nonempty {field}")
        if len(review_document[field].strip()) > 500:
            raise ValueError(f"independent human-review document {field} is too long")
    if re.fullmatch(
        r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z",
        review_document["reviewed_utc"],
    ) is None:
        raise ValueError("independent human-review reviewed_utc is not exact UTC")
    if (
        not isinstance(review_document.get("observed_spans"), list)
        or not review_document["observed_spans"]
        or len(review_document["observed_spans"]) > 64
    ):
        raise ValueError("independent human-review document lacks observed span details")
    contributors = semantic_result.get("contributors")
    if not isinstance(contributors, list) or len(contributors) < 2:
        raise ValueError("current semantic result lacks two ranked contributors")
    observed_by_stage: dict[str, dict[str, Any]] = {}
    for observed in review_document["observed_spans"]:
        if not isinstance(observed, dict):
            raise ValueError("independent human-review observed span must be an object")
        expected_observed_fields = {
            "name", "duration_ns", "gpu_evidence_id", "frame_index",
            "sequence", "health_state",
        }
        missing_observed_fields = expected_observed_fields - set(observed)
        if missing_observed_fields:
            raise ValueError(
                "independent human-review observed span lacks typed fields: "
                + ", ".join(sorted(missing_observed_fields))
            )
        if set(observed) != expected_observed_fields:
            raise ValueError(
                "independent human-review observed span has noncanonical fields"
            )
        name = observed.get("name")
        if not isinstance(name, str) or not name.startswith("gpu_"):
            raise ValueError("independent human-review observed span has no canonical GPU name")
        stage = name.removeprefix("gpu_").replace("_", "-")
        if stage in observed_by_stage:
            raise ValueError("independent human-review observed spans contain a duplicate stage")
        observed_by_stage[stage] = observed
    for contributor in contributors[:2]:
        if not isinstance(contributor, dict) or not isinstance(contributor.get("stage"), str):
            raise ValueError("current semantic contributor is malformed")
        observed = observed_by_stage.get(contributor["stage"])
        if observed is None:
            raise ValueError(
                f"independent human review did not observe current contributor {contributor['stage']}"
            )
        compared = {
            "duration_ns": "duration_ns",
            "evidence_id": "gpu_evidence_id",
            "frame_index": "frame_index",
            "sequence": "sequence",
            "health_state": "health_state",
        }
        for semantic_field, observed_field in compared.items():
            semantic_value = contributor.get(semantic_field)
            observed_value = observed.get(observed_field)
            if semantic_field in {"duration_ns", "frame_index", "sequence"}:
                minimum = 1 if semantic_field == "duration_ns" else 0
                if (
                    not isinstance(semantic_value, int)
                    or isinstance(semantic_value, bool)
                    or semantic_value < minimum
                    or not isinstance(observed_value, int)
                    or isinstance(observed_value, bool)
                    or observed_value < minimum
                ):
                    raise ValueError(
                        f"independent human review lacks typed {contributor['stage']} "
                        f"{semantic_field}"
                    )
            elif semantic_field == "evidence_id":
                if (
                    not isinstance(semantic_value, str)
                    or re.fullmatch(r"[0-9a-f]{32}", semantic_value) is None
                    or not isinstance(observed_value, str)
                    or re.fullmatch(r"[0-9a-f]{32}", observed_value) is None
                ):
                    raise ValueError(
                        f"independent human review lacks typed {contributor['stage']} "
                        "evidence_id"
                    )
            elif (
                not isinstance(semantic_value, str)
                or semantic_value not in {
                    "healthy", "failed", "unavailable", "unverified", "lost"
                }
                or not isinstance(observed_value, str)
                or observed_value not in {
                    "healthy", "failed", "unavailable", "unverified", "lost"
                }
            ):
                raise ValueError(
                    f"independent human review lacks typed {contributor['stage']} "
                    "health_state"
                )
            if observed_value != semantic_value:
                raise ValueError(
                    f"independent human review disagrees with current {contributor['stage']} "
                    f"{semantic_field}"
                )
    return review_document


def bind_independent_human_review_document(
    repository: Path,
    source_revision: str,
    prior_path: Path,
    *,
    question: str,
    trace_sha256: str,
    semantic_result: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Bind a dedicated independently published review as an immutable Git blob."""
    repository = repository.resolve()
    resolved = prior_path.resolve()
    try:
        relative = resolved.relative_to(repository).as_posix()
    except ValueError as error:
        raise ValueError("independent human-review document must be tracked in this repository") from error
    if (
        prior_path.is_symlink()
        or not relative.startswith(HUMAN_REVIEW_PATH_PREFIX)
        or not relative.endswith(".json")
        or ".." in Path(relative).parts
    ):
        raise ValueError("independent human-review document has an unsafe repository path")
    if Path(relative).name != f"{trace_sha256}.json":
        raise ValueError(
            "independent human-review filename must equal the reviewed trace SHA-256"
        )
    try:
        data = resolved.read_bytes()
    except OSError as error:
        raise ValueError(f"cannot read independent human-review document: {error}") from error
    if len(data) > HUMAN_REVIEW_MAX_BYTES:
        raise ValueError("independent human-review document exceeds 256 KiB")
    historical = subprocess.run(
        ["git", "show", f"{source_revision}:{relative}"],
        cwd=repository, check=False, capture_output=True,
    )
    if historical.returncode != 0 or historical.stdout != data:
        raise ValueError(
            "independent human-review document is not the exact source-revision Git blob"
        )
    blob = _git_blob_digest_bytes(data)
    if _git_text(repository, "rev-parse", f"{source_revision}:{relative}") != blob:
        raise ValueError("independent human-review Git blob identity disagrees")
    if _git_text(repository, "rev-parse", f"HEAD:{relative}") != blob:
        raise ValueError("independent human-review document differs at current HEAD")
    parents = _git_text(repository, "rev-list", "--parents", "-n", "1", source_revision)
    parent_revisions = parents.split()[1:]
    if not parent_revisions or not any(
        subprocess.run(
            ["git", "rev-parse", f"{parent}:{relative}"],
            cwd=repository, check=False, capture_output=True, text=True,
        ).stdout.strip() == blob
        for parent in parent_revisions
    ):
        raise ValueError(
            "independent human-review document was not published before source revision"
        )
    review_revision = _git_text(
        repository, "log", "-1", "--format=%H", source_revision, "--", relative
    )
    review_parents = _git_text(
        repository, "rev-list", "--parents", "-n", "1", review_revision
    ).split()
    if len(review_parents) != 2:
        raise ValueError(
            "independent human-review publication must be a single-parent commit"
        )
    changed = subprocess.run(
        [
            "git", "diff", "--name-only", "--no-renames",
            review_parents[1], review_revision,
        ],
        cwd=repository, check=False, capture_output=True, text=True,
    )
    changed_paths = [line for line in changed.stdout.splitlines() if line]
    if changed.returncode != 0 or changed_paths != [relative]:
        raise ValueError(
            "independent human-review publication commit must change exactly its review file"
        )
    commit_author = _git_text(
        repository, "show", "-s", "--format=%an <%ae>", review_revision
    )
    try:
        review_document = json.loads(data)
    except json.JSONDecodeError as error:
        raise ValueError("independent human-review document is malformed JSON") from error
    human = validate_independent_human_review_document(
        review_document,
        question=question,
        trace_sha256=trace_sha256,
        semantic_result=semantic_result,
    )
    authority = human["review_authority"]
    if authority["git_commit_author"] != commit_author:
        raise ValueError(
            "independent human-review Git author does not match its publication commit"
        )
    return human, {
        "method": "independently-committed-single-file-git-blob-v1",
        "repository_path": relative,
        "source_revision": source_revision,
        "review_revision": review_revision,
        "review_commit_parent": review_parents[1],
        "review_commit_author": commit_author,
        "git_blob_sha1": blob,
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
        "existed_unchanged_in_direct_parent": True,
        "review_commit_changed_exactly_this_file": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--install-prefix", type=Path, required=True)
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
    parser.add_argument("--plan-revision", required=True)
    parser.add_argument("--plan-sha256", required=True)
    parser.add_argument("--planning-repository", type=Path, required=True)
    parser.add_argument("--routing-inventory", type=Path)
    parser.add_argument(
        "--human-review-document", type=Path,
        help=(
            "Dedicated docs/validation/gpu-trace-overhead/human-reviews/<trace-sha>.json "
            "whose prior single-file Git commit and same-artifact observations are bound"
        ),
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    prefix = args.install_prefix.resolve()
    output = args.output.absolute()
    trace = args.trace.resolve()
    processor = args.trace_processor.resolve()
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
    planning_repository = args.planning_repository.resolve()
    build_environment = os.environ.copy()
    for inherited_override in (
        "PULP_SKIP_RUST_CLI",
        "PULP_USE_CPP",
        "PULP_RS_CPP_BINARY",
        "PULP_RS_FALLTHROUGH",
        "PULP_RS_NO_FALLTHROUGH",
    ):
        build_environment.pop(inherited_override, None)
    try:
        executing_source_identity = executing_checkout_source_identity(
            repository, args.source_revision
        )
        validate_output_path(
            output, (repository, planning_repository, build_dir, prefix)
        )
        output_parent_claim = retain_existing_directory(
            output.parent, "output-parent"
        )
        source_identity = clean_source_identity(repository, args.source_revision)
        accepted_plan = plan_identity(
            planning_repository, args.plan_revision, args.plan_sha256
        )
        (
            cli, mcp, installed_identity, install_claim,
            source_tree_claim, build_input_claim, provider_input_claim,
        ) = refresh_exact_build_install(
            repository, args.source_revision, build_dir, prefix,
            planning_repository, build_environment,
        )
        execution_claim = CombinedClaims(
            install_claim, source_tree_claim, build_input_claim,
            provider_input_claim,
        )
        validate_paths(cli, mcp, trace, processor)
        binding = source_binding(repository, args.source_revision, trace)
        processor_identity = trace_processor_identity(repository, processor)
        install_claim.bind_file(trace, "measured Perfetto trace", sha256(trace))
        for fixture_path in sorted(FIXTURE_SOURCE_PATHS):
            fixture = repository / fixture_path
            if fixture != trace:
                install_claim.bind_file(
                    fixture, f"replayed fixture {fixture_path}", sha256(fixture)
                )
        install_claim.bind_file(
            processor,
            "SDK-matched trace processor",
            processor_identity["sha256"],
        )
        execution_claim.assert_current()
        producer_inventory = a2t_scope_inventory(repository, args.source_revision)
        if producer_inventory["no_a2t_scoped_producer_delta"] is not True:
            raise ValueError(
                "the A2T path-scoped tree delta contains product producer paths; "
                "run the product overhead gate"
            )
    except ValueError as error:
        parser.error(str(error))

    trace_sha256 = sha256(trace)
    environment = build_environment.copy()
    environment["PULP_TRACE_PROCESSOR"] = str(processor)
    environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    load_average_start = list(os.getloadavg()) if hasattr(os, "getloadavg") else None

    fixture_replay = run_fixture_replay(
        cli, mcp, repository, environment, execution_claim
    )

    persistent = McpSession(mcp, environment, execution_claim)
    persistent.initialize()
    reference_projection: dict[str, Any] | None = None
    try:
        for _ in range(args.warmups):
            _, cli_payload = run_cli(
                cli, args.question, trace, environment, execution_claim
            )
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
                cli_ns, cli_payload = run_cli(
                    cli, args.question, trace, environment, execution_claim
                )
                mcp_ns, mcp_payload = persistent.call(args.question, trace)
                order = "cli-first"
            else:
                mcp_ns, mcp_payload = persistent.call(args.question, trace)
                cli_ns, cli_payload = run_cli(
                    cli, args.question, trace, environment, execution_claim
                )
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
            cli_ns, cli_payload = run_cli(
                cli, args.question, trace, environment, execution_claim
            )
            start = time.perf_counter_ns()
            session = McpSession(mcp, environment, execution_claim)
            try:
                session.initialize()
                _, mcp_payload = session.call(args.question, trace)
            finally:
                session.close()
            mcp_ns = time.perf_counter_ns() - start
            order = "cli-first"
        else:
            start = time.perf_counter_ns()
            session = McpSession(mcp, environment, execution_claim)
            try:
                session.initialize()
                _, mcp_payload = session.call(args.question, trace)
            finally:
                session.close()
            mcp_ns = time.perf_counter_ns() - start
            cli_ns, cli_payload = run_cli(
                cli, args.question, trace, environment, execution_claim
            )
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
    confidence = paired_delta_confidence(cli_samples, mcp_samples)

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
    human_perfetto_ui_review_source = None
    if args.human_review_document is not None:
        try:
            (
                human_perfetto_ui_correlation,
                human_perfetto_ui_review_source,
            ) = bind_independent_human_review_document(
                repository,
                args.source_revision,
                args.human_review_document,
                question=args.question,
                trace_sha256=trace_sha256,
                semantic_result=reference_projection,
            )
        except ValueError as error:
            parser.error(f"invalid human-review-document: {error}")

    try:
        execution_claim.assert_current()
        source_tree_claim.assert_full()
        build_input_claim.assert_full()
        provider_input_claim.assert_full()
        if clean_source_identity(repository, args.source_revision) != source_identity:
            raise ValueError("source identity changed during A2T recording")
        if executing_checkout_source_identity(
            repository, args.source_revision
        ) != executing_source_identity:
            raise ValueError("executing A2T source identity changed during recording")
        if source_binding(repository, args.source_revision, trace) != binding:
            raise ValueError("source binding changed during A2T recording")
        if exact_build_install_identity(
            repository, args.source_revision, build_dir, prefix, cli, mcp,
            installed_identity["build_provenance"]["binaries"],
            installed_identity["build_provenance"]["cmake_cache_sha256"],
            installed_identity["build_provenance"]["source_tree_claim"],
            installed_identity["build_provenance"]["build_input_claim"],
            installed_identity["build_provenance"]["render_provider_input_claim"],
        ) != installed_identity:
            raise ValueError("installed CLI/MCP provenance changed during A2T recording")
        if trace_processor_identity(repository, processor) != processor_identity:
            raise ValueError("trace processor identity changed during A2T recording")
        if a2t_scope_inventory(repository, args.source_revision) != producer_inventory:
            raise ValueError("A2T path-scoped tree delta changed during recording")
        execution_claim.assert_current()
    except ValueError as error:
        parser.error(str(error))

    evidence = {
        "schema": "pulp.gpu-trace-overhead-acceptance.v3",
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source_revision": args.source_revision,
        "mcp_source_revision": args.mcp_source_revision,
        "integration_head": binding["integration_head"],
        "source_blobs": binding["source_blobs"],
        "source_identity": source_identity,
        "executing_source_identity": executing_source_identity,
        "installed_source_identity": installed_identity,
        "accepted_plan": accepted_plan,
        "scope": "offline-installed-cli-mcp-analysis",
        "producer_overhead_disposition": {
            "status": "not-applicable-no-a2t-scoped-producer-cost",
            "reason": "The exact A2T-scoped delta adds offline analysis and no product producer call site; input-to-present is already in the immutable base, while the post-base A3 producer is disclosed separately and is not graded here.",
            "evidence": producer_inventory,
            "routing_inventory": routing_inventory,
            "non_a2t_owner_followup": "The pre-base input-to-present latency tracing package and post-base A3 package must each provide or bind tracing-off, tracing-on/idle, and active-capture overhead/control evidence for their product producers; A2T does not evaluate that owner evidence.",
            "required_followup": "B6 must run the three-state 5-warmup/30-trial and 20 fresh-process protocol when Vellum producer instrumentation is added.",
            "formal_plan_status": "accepted-canonical-plan",
            "formal_plan_revision": args.plan_revision,
            "formal_plan_sha256": args.plan_sha256,
            "formal_plan_note": "The canonical plan accepts the no-added-producer disposition for the original A2T implementation and preserves the full product producer/xrun protocol for B6; pre-base input-to-present and post-base A3 producers remain separately accountable, and offline timing is analyzer evidence rather than product-capture evidence.",
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
        "human_perfetto_ui_review_source": human_perfetto_ui_review_source,
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
            "terminal_status": OFFLINE_STRUCTURAL_STATUS,
            "semantic_parity": "pass", "same_installed_prefix": "pass",
            "human_perfetto_ui_correlation": (
                "bound-independent-review-evidence"
                if human_perfetto_ui_correlation is not None
                else "unverified-no-human-perfetto-ui-correlation"
            ),
            "offline_latency_budget": "unverified-no-ratified-budget",
            "producer_overhead_budget": "not-applicable-no-a2t-scoped-producer-delta",
            "xrun_check": "not-applicable-offline-no-audio-thread",
        },
    }
    execution_claim.assert_current()
    atomic_write_json(output, evidence, output_parent_claim)
    output_parent_claim.bind_file(
        output,
        "published A2T structural receipt",
        hashlib.sha256(
            (json.dumps(evidence, indent=2, sort_keys=True) + "\n").encode("utf-8")
        ).hexdigest(),
    )
    execution_claim.assert_current()
    source_tree_claim.assert_full()
    build_input_claim.assert_full()
    provider_input_claim.assert_full()
    output_parent_claim.assert_current()
    structural_verifier = _load_local_source(
        "verify_gpu_trace_overhead_acceptance_recording",
        "verify_gpu_trace_overhead_acceptance.py",
    )
    problems = structural_verifier.verify(
        evidence, repository, require_terminal=False
    )
    if problems:
        parser.error("A2T structural self-verification failed: " + "; ".join(problems))
    assert_exact_live_head(repository, args.source_revision)
    if executing_checkout_source_identity(
        repository, args.source_revision
    ) != executing_source_identity:
        parser.error("executing A2T source identity changed after publication")
    execution_claim.assert_current()
    source_tree_claim.assert_full()
    build_input_claim.assert_full()
    provider_input_claim.assert_full()
    output_parent_claim.assert_current()
    install_claim.close()
    provider_input_claim.close()
    build_input_claim.close()
    source_tree_claim.close()
    output_parent_claim.close()
    print(json.dumps({
        "output": str(output),
        "result": "structural-evidence-written",
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
