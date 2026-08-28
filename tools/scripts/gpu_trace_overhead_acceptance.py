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
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


EXPECTED_EXIT = {"pass": 0, "fail": 1, "unavailable": 2, "unverified": 2}


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
        for key in (
            "schema",
            "question",
            "verdict",
            "capture_complete",
            "dominant_stage",
            "contributors",
            "evidence_ids",
            "next_actions",
            "ui_correlation",
            "unavailable_reason",
        )
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
        structured = response["result"].get("structuredContent")
        return elapsed, parse_analysis(structured, surface="MCP")

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
        if not path.is_file():
            raise ValueError(f"{label} path is not a file: {path}")
    if cli.parent.resolve() != mcp.parent.resolve():
        raise ValueError("CLI and MCP must be installed as siblings in one prefix/bin")
    if cli.name != "pulp" or mcp.name != "pulp-mcp":
        raise ValueError("installed sibling names must be pulp and pulp-mcp")


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


def source_revisions_match(source_revision: str, mcp_source_revision: str) -> bool:
    return bool(mcp_source_revision) and mcp_source_revision == source_revision


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
    parser.add_argument("--routing-inventory", type=Path)
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

    environment = os.environ.copy()
    environment["PULP_TRACE_PROCESSOR"] = str(processor)
    environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    load_average_start = list(os.getloadavg()) if hasattr(os, "getloadavg") else None

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

    evidence = {
        "schema": "pulp.gpu-trace-overhead-acceptance.v1",
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source_revision": args.source_revision,
        "mcp_source_revision": args.mcp_source_revision,
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
                      "sha256": sha256(trace), "bytes": trace.stat().st_size},
            "trace_processor": {"role": "pulp-home pinned v57.2 trace_processor_shell",
                                "sha256": sha256(processor),
                                "bytes": processor.stat().st_size},
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
            "semantic_parity": "pass", "same_installed_prefix": "pass",
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
