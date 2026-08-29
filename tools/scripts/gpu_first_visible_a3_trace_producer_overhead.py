#!/usr/bin/env python3
"""Ratify and verify the A3 product trace-producer overhead control."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import stat
import statistics
import tempfile
from pathlib import Path
from typing import Any

import gpu_first_visible_a3_trace_producer_overhead_analyzer as trace_replay

RAW_SCHEMA = "pulp.gpu-first-visible-trace-producer-overhead-raw.v1"
RECEIPT_SCHEMA = "pulp.gpu-first-visible-trace-producer-overhead.v1"
METRICS_SCHEMA = "pulp.gpu-first-visible-trace-producer-runtime-metrics.v1"
SESSION_CONFIG_SCHEMA = "pulp.gpu-first-visible-trace-session-config.v1"
PRODUCER_REVISION = "8175bd483f5e4ca66989c9ad91a4d9ed5a864bb0"
BASELINE_REVISION = "5048ce72dd28d87974550a3feb526de0f44af32c"
RING_BYTES = 128 * 1024 * 1024
STATES = (
    "pre-change-baseline",
    "candidate-compile-out",
    "candidate-compiled-in-idle",
    "candidate-active",
)
CAMPAIGN_ROLES = {"standalone", "headless-constrained", "daw", "forge"}
STATE_TRACING = {
    "pre-change-baseline": (False, False, 0),
    "candidate-compile-out": (False, False, 0),
    "candidate-compiled-in-idle": (True, False, 0),
    "candidate-active": (True, True, RING_BYTES),
}
LIMITS = {
    "candidate-compile-out": {"median_percent": 1.0, "p95_percent": 2.0},
    "candidate-compiled-in-idle": {"median_percent": 2.0, "p95_percent": 5.0},
    "candidate-active": {"median_percent": 5.0, "p95_percent": 10.0},
}
RAW_KEYS = {
    "schema", "version", "state", "producer_revision", "source_revision",
    "baseline_revision", "candidate_revision", "campaign_role", "campaign_id",
    "build_family_id", "product_id", "product_name", "plugin_format",
    "binary_sha256", "machine", "workload", "measurement_driver",
    "trace_session_config", "tracing", "warmups", "measured", "fresh_process",
}
MACHINE_KEYS = {"machine_id", "operating_system", "architecture"}
WORKLOAD_KEYS = {"workload_id", "content_sha256", "adapter_sha256"}
DRIVER_KEYS = {"revision", "path", "sha256"}
TRACING_KEYS = {"compiled_in", "session_active", "ring_bytes"}
ARTIFACT_KEYS = {"path", "sha256"}
TRACE_KEYS = {"path", "sha256", "bytes", "format"}
SAMPLE_KEYS = {"sequence", "evidence_id", "duration_ms", "runtime_metrics", "trace"}
METRICS_KEYS = {
    "schema", "version", "state", "sequence", "evidence_id", "host_pid",
    "process_start_identity", "executable_sha256", "audio_thread_tids",
    "started_monotonic_ns", "finished_monotonic_ns", "xrun_count",
}
SESSION_CONFIG_KEYS = {
    "schema", "version", "ring_bytes", "fill_policy", "categories",
}
RECEIPT_KEYS = {
    "schema", "version", "generated_utc", "producer_revision",
    "baseline_revision", "candidate_revision", "campaign_role", "campaign_id",
    "build_family_id", "product_id", "product_name", "plugin_format", "binaries",
    "machine", "workload", "measurement_driver", "trace_session_config",
    "trace_processor", "protocol", "raw_artifacts", "summaries",
    "comparisons", "replay_summary", "verdict",
}
SHA256 = re.compile(r"^[0-9a-f]{64}$")
EVIDENCE_ID = re.compile(r"^[0-9a-f]{32}$")
GIT_REVISION = re.compile(r"^[0-9a-f]{40}$")
UTC_TIMESTAMP = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[^ ]+Z$")


class OverheadError(ValueError):
    """The producer-overhead evidence is incomplete or inconsistent."""


def exact_keys(value: Any, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise OverheadError(f"{label} has the wrong fields")


def regular_file_bytes(path: Path, label: str) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise OverheadError(f"{label} is not a readable regular file: {path}") from error
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise OverheadError(f"{label} is not a regular file: {path}")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)
    finally:
        os.close(descriptor)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path, label: str) -> str:
    return sha256_bytes(regular_file_bytes(path, label))


def regular_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(regular_file_bytes(path, label))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise OverheadError(f"{label} is not valid JSON") from error
    if not isinstance(value, dict):
        raise OverheadError(f"{label} must contain an object")
    return value


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    temporary.chmod(0o600)
    os.replace(temporary, path)


def safe_artifact(root: Path, relative: Any, label: str) -> Path:
    if not isinstance(relative, str) or not relative:
        raise OverheadError(f"{label} path is missing")
    lexical = root / relative
    if Path(relative).is_absolute() or ".." in Path(relative).parts or lexical.is_symlink():
        raise OverheadError(f"{label} path is unsafe")
    resolved = lexical.resolve()
    root_resolved = root.resolve()
    if root_resolved not in resolved.parents or not resolved.is_file():
        raise OverheadError(f"{label} artifact is unavailable")
    return resolved


def artifact_path(ref: Any, root: Path, label: str) -> Path:
    exact_keys(ref, ARTIFACT_KEYS, label)
    path = safe_artifact(root, ref["path"], label)
    if not isinstance(ref["sha256"], str) or SHA256.fullmatch(ref["sha256"]) is None:
        raise OverheadError(f"{label} digest is invalid")
    if sha256_file(path, label) != ref["sha256"]:
        raise OverheadError(f"{label} digest differs")
    return path


def artifact_ref(path: Path, evidence_root: Path) -> dict[str, str]:
    resolved = path.resolve()
    root = evidence_root.resolve()
    if root not in resolved.parents:
        raise OverheadError(f"artifact escapes the evidence root: {path}")
    return {
        "path": resolved.relative_to(root).as_posix(),
        "sha256": sha256_file(resolved, "artifact"),
    }


def finite_positive(value: Any) -> bool:
    return (
        isinstance(value, (int, float)) and not isinstance(value, bool)
        and math.isfinite(value) and value > 0
    )


def validate_session_config(ref: Any, evidence_root: Path) -> tuple[Path, str]:
    path = artifact_path(ref, evidence_root, "trace_session_config")
    payload = regular_json(path, "trace_session_config")
    exact_keys(payload, SESSION_CONFIG_KEYS, "trace_session_config")
    if payload != {
        "schema": SESSION_CONFIG_SCHEMA,
        "version": 1,
        "ring_bytes": RING_BYTES,
        "fill_policy": "ring-buffer",
        "categories": ["dsp", "gpu", "metadata", "render"],
    }:
        raise OverheadError("trace_session_config is not the closed 128 MiB session")
    return path, ref["sha256"]


def validate_metrics(
    ref: Any, *, state: str, sequence: int, evidence_id: str,
    binary_sha256: str, evidence_root: Path, duration_ms: float, label: str,
) -> dict[str, Any]:
    path = artifact_path(ref, evidence_root, f"{label}.runtime_metrics")
    metrics = regular_json(path, f"{label}.runtime_metrics")
    exact_keys(metrics, METRICS_KEYS, f"{label}.runtime_metrics")
    tids = metrics["audio_thread_tids"]
    if (
        metrics["schema"] != METRICS_SCHEMA
        or metrics["version"] != 1
        or metrics["state"] != state
        or metrics["sequence"] != sequence
        or metrics["evidence_id"] != evidence_id
        or not isinstance(metrics["host_pid"], int)
        or isinstance(metrics["host_pid"], bool)
        or metrics["host_pid"] <= 0
        or not isinstance(metrics["process_start_identity"], str)
        or not metrics["process_start_identity"]
        or metrics["executable_sha256"] != binary_sha256
        or not isinstance(tids, list)
        or not tids
        or tids != sorted(set(tids))
        or any(not isinstance(item, int) or isinstance(item, bool) or item <= 0 for item in tids)
        or not isinstance(metrics["started_monotonic_ns"], int)
        or isinstance(metrics["started_monotonic_ns"], bool)
        or not isinstance(metrics["finished_monotonic_ns"], int)
        or isinstance(metrics["finished_monotonic_ns"], bool)
        or metrics["finished_monotonic_ns"] <= metrics["started_monotonic_ns"]
        or metrics["xrun_count"] != 0
    ):
        raise OverheadError(f"{label}.runtime_metrics lacks authentic process/xrun facts")
    derived_duration = round(
        (metrics["finished_monotonic_ns"] - metrics["started_monotonic_ns"]) / 1_000_000,
        6,
    )
    if round(float(duration_ms), 6) != derived_duration:
        raise OverheadError(f"{label}.duration_ms is not derived from runtime metrics")
    return metrics


def validate_trace(
    ref: Any, *, metrics: dict[str, Any], evidence_id: str,
    session_config_sha256: str, evidence_root: Path,
    trace_processor: Path | None, label: str,
) -> dict[str, Any]:
    exact_keys(ref, TRACE_KEYS, f"{label}.trace")
    path = safe_artifact(evidence_root, ref["path"], f"{label}.trace")
    data = regular_file_bytes(path, f"{label}.trace")
    if (
        not isinstance(ref["sha256"], str)
        or SHA256.fullmatch(ref["sha256"]) is None
        or sha256_bytes(data) != ref["sha256"]
        or not isinstance(ref["bytes"], int)
        or isinstance(ref["bytes"], bool)
        or ref["bytes"] != len(data)
        or not 0 < len(data) <= RING_BYTES
        or ref["format"] not in {"chrome-json", "perfetto-proto"}
    ):
        raise OverheadError(f"{label}.trace lacks bounded immutable bytes")
    request = {
        "evidence_id": evidence_id,
        "host_pid": metrics["host_pid"],
        "process_start_identity": metrics["process_start_identity"],
        "executable_sha256": metrics["executable_sha256"],
        "audio_thread_tids": metrics["audio_thread_tids"],
        "session_config_sha256": session_config_sha256,
        "ring_bytes": RING_BYTES,
    }
    try:
        analysis = trace_replay.analyze_trace(path, request, trace_processor)
    except (OSError, trace_replay.TraceReplayError) as error:
        raise OverheadError(f"{label}.trace replay failed: {error}") from error
    if analysis["trace_format"] != ref["format"]:
        raise OverheadError(f"{label}.trace format differs from replay")
    return analysis


def validate_samples(
    rows: Any, *, count: int, state: str, evidence_root: Path,
    binary_sha256: str, session_config_sha256: str,
    trace_processor: Path | None, label: str,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    if not isinstance(rows, list) or len(rows) != count:
        raise OverheadError(f"{label} must contain exactly {count} samples")
    active = state == "candidate-active"
    metrics_rows: list[dict[str, Any]] = []
    analyses: list[dict[str, Any]] = []
    evidence_ids: set[str] = set()
    metric_paths: set[str] = set()
    for index, row in enumerate(rows):
        exact_keys(row, SAMPLE_KEYS, f"{label}[{index}]")
        if (
            row["sequence"] != index
            or not isinstance(row["evidence_id"], str)
            or EVIDENCE_ID.fullmatch(row["evidence_id"]) is None
            or row["evidence_id"] in evidence_ids
            or not finite_positive(row["duration_ms"])
        ):
            raise OverheadError(f"{label}[{index}] lacks bounded timing/identity")
        evidence_ids.add(row["evidence_id"])
        metrics_ref = row["runtime_metrics"]
        exact_keys(metrics_ref, ARTIFACT_KEYS, f"{label}[{index}].runtime_metrics")
        if metrics_ref["path"] in metric_paths:
            raise OverheadError(f"{label} reuses a runtime metrics artifact")
        metric_paths.add(metrics_ref["path"])
        metrics = validate_metrics(
            metrics_ref, state=state, sequence=index, evidence_id=row["evidence_id"],
            binary_sha256=binary_sha256, evidence_root=evidence_root,
            duration_ms=float(row["duration_ms"]), label=f"{label}[{index}]",
        )
        metrics_rows.append(metrics)
        if active:
            if row["trace"] is None:
                raise OverheadError(f"{label}[{index}] lacks its active trace")
            analyses.append(validate_trace(
                row["trace"], metrics=metrics, evidence_id=row["evidence_id"],
                session_config_sha256=session_config_sha256,
                evidence_root=evidence_root, trace_processor=trace_processor,
                label=f"{label}[{index}]",
            ))
        elif row["trace"] is not None:
            raise OverheadError(f"{label}[{index}] inactive state claims a trace")
    if label.endswith("fresh_process"):
        processes = {
            (item["host_pid"], item["process_start_identity"])
            for item in metrics_rows
        }
        if len(processes) != count:
            raise OverheadError(f"{label} does not contain {count} unique process starts")
    return metrics_rows, analyses


def validate_raw(
    payload: dict[str, Any], *, state: str, evidence_root: Path,
    trace_processor: Path | None, label: str,
) -> dict[str, Any]:
    exact_keys(payload, RAW_KEYS, label)
    exact_keys(payload["machine"], MACHINE_KEYS, f"{label}.machine")
    exact_keys(payload["workload"], WORKLOAD_KEYS, f"{label}.workload")
    exact_keys(payload["measurement_driver"], DRIVER_KEYS, f"{label}.measurement_driver")
    exact_keys(payload["tracing"], TRACING_KEYS, f"{label}.tracing")
    compiled, active, ring = STATE_TRACING[state]
    if (
        payload["schema"] != RAW_SCHEMA
        or payload["version"] != 1
        or payload["state"] != state
        or payload["producer_revision"] != PRODUCER_REVISION
        or not isinstance(payload["source_revision"], str)
        or GIT_REVISION.fullmatch(payload["source_revision"]) is None
        or payload["baseline_revision"] != BASELINE_REVISION
        or not isinstance(payload["candidate_revision"], str)
        or GIT_REVISION.fullmatch(payload["candidate_revision"]) is None
        or payload["baseline_revision"] == payload["candidate_revision"]
        or payload["source_revision"] != (
            payload["baseline_revision"] if state == "pre-change-baseline"
            else payload["candidate_revision"]
        )
        or payload["campaign_role"] not in CAMPAIGN_ROLES
        or any(
            not isinstance(payload[field], str) or not payload[field]
            for field in (
                "campaign_id", "build_family_id", "product_id", "product_name",
                "plugin_format",
            )
        )
        or not isinstance(payload["binary_sha256"], str)
        or SHA256.fullmatch(payload["binary_sha256"]) is None
        or payload["tracing"] != {
            "compiled_in": compiled, "session_active": active, "ring_bytes": ring,
        }
    ):
        raise OverheadError(f"{label} has an invalid state/source/build contract")
    machine = payload["machine"]
    if any(not isinstance(machine[key], str) or not machine[key] for key in MACHINE_KEYS):
        raise OverheadError(f"{label}.machine is incomplete")
    workload = payload["workload"]
    if (
        not isinstance(workload["workload_id"], str) or not workload["workload_id"]
        or any(
            not isinstance(workload[key], str) or SHA256.fullmatch(workload[key]) is None
            for key in ("content_sha256", "adapter_sha256")
        )
    ):
        raise OverheadError(f"{label}.workload is invalid")
    driver = payload["measurement_driver"]
    if (
        driver["revision"] != payload["candidate_revision"]
        or not isinstance(driver["path"], str) or not driver["path"]
        or Path(driver["path"]).is_absolute() or ".." in Path(driver["path"]).parts
        or not isinstance(driver["sha256"], str) or SHA256.fullmatch(driver["sha256"]) is None
    ):
        raise OverheadError(f"{label}.measurement_driver is not source-bound")
    _, session_digest = validate_session_config(payload["trace_session_config"], evidence_root)
    metrics: dict[str, list[dict[str, Any]]] = {}
    analyses: dict[str, list[dict[str, Any]]] = {}
    for section, count in (("warmups", 5), ("measured", 30), ("fresh_process", 20)):
        metrics[section], analyses[section] = validate_samples(
            payload[section], count=count, state=state, evidence_root=evidence_root,
            binary_sha256=payload["binary_sha256"],
            session_config_sha256=session_digest,
            trace_processor=trace_processor, label=f"{label}.{section}",
        )
    return {"payload": payload, "metrics": metrics, "analyses": analyses}


def percentile95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[math.ceil(0.95 * len(ordered)) - 1]


def summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    values = [float(row["duration_ms"]) for row in rows]
    return {
        "count": len(values),
        "median_ms": round(statistics.median(values), 6),
        "p95_ms": round(percentile95(values), 6),
    }


def delta_percent(candidate: float, baseline: float) -> float:
    return round((candidate - baseline) * 100.0 / baseline, 6)


def comparison(
    baseline: dict[str, Any], candidate: dict[str, Any], limits: dict[str, float],
) -> dict[str, Any]:
    median_delta = delta_percent(candidate["median_ms"], baseline["median_ms"])
    p95_delta = delta_percent(candidate["p95_ms"], baseline["p95_ms"])
    return {
        "median_delta_percent": median_delta,
        "p95_delta_percent": p95_delta,
        "median_limit_percent": limits["median_percent"],
        "p95_limit_percent": limits["p95_percent"],
        "pass": (
            median_delta <= limits["median_percent"]
            and p95_delta <= limits["p95_percent"]
        ),
    }


def build_receipt(
    raw_paths: dict[str, Path], *, evidence_root: Path, generated_utc: str,
    trace_processor_path: Path | None = None,
) -> dict[str, Any]:
    if set(raw_paths) != set(STATES):
        raise OverheadError("all four producer-overhead states are required")
    if UTC_TIMESTAMP.fullmatch(generated_utc) is None:
        raise OverheadError("generated_utc is not a UTC timestamp")
    if trace_processor_path is not None:
        try:
            relative_processor = trace_processor_path.resolve().relative_to(
                evidence_root.resolve()
            ).as_posix()
        except ValueError as error:
            raise OverheadError("trace_processor escapes the evidence root") from error
        trace_processor_path = safe_artifact(
            evidence_root, relative_processor, "trace_processor",
        )
        if not os.access(trace_processor_path, os.X_OK):
            raise OverheadError("trace_processor is not executable")
    raw = {
        state: validate_raw(
            regular_json(raw_paths[state], f"{state} raw evidence"),
            state=state, evidence_root=evidence_root,
            trace_processor=trace_processor_path, label=state,
        )
        for state in STATES
    }
    payloads = {state: raw[state]["payload"] for state in STATES}
    common_fields = (
        "producer_revision", "baseline_revision", "candidate_revision",
        "campaign_role", "campaign_id", "build_family_id", "product_id",
        "product_name", "plugin_format", "machine", "workload",
        "measurement_driver", "trace_session_config",
    )
    baseline = payloads["pre-change-baseline"]
    for state in STATES[1:]:
        if any(payloads[state][field] != baseline[field] for field in common_fields):
            raise OverheadError(f"{state} does not use the same product/host/workload family")
    if (
        payloads["candidate-compiled-in-idle"]["binary_sha256"]
        != payloads["candidate-active"]["binary_sha256"]
    ):
        raise OverheadError("compiled-in idle and active states must use the same product binary")
    if (
        payloads["candidate-compile-out"]["binary_sha256"]
        == payloads["candidate-compiled-in-idle"]["binary_sha256"]
    ):
        raise OverheadError("compile-out and compiled-in states do not identify distinct builds")
    all_ids = [
        row["evidence_id"] for state in STATES for section in (
            "warmups", "measured", "fresh_process",
        ) for row in payloads[state][section]
    ]
    if len(set(all_ids)) != len(all_ids):
        raise OverheadError("producer-overhead evidence IDs are not globally unique")
    fresh_processes = [
        (metrics["host_pid"], metrics["process_start_identity"])
        for state in STATES for metrics in raw[state]["metrics"]["fresh_process"]
    ]
    if len(set(fresh_processes)) != len(fresh_processes):
        raise OverheadError("fresh-process identities are reused across overhead states")
    summaries = {
        state: {
            "measured": summary(payloads[state]["measured"]),
            "fresh_process": summary(payloads[state]["fresh_process"]),
        }
        for state in STATES
    }
    comparisons: dict[str, Any] = {}
    passed = True
    for state in STATES[1:]:
        state_comparison = {
            section: comparison(
                summaries["pre-change-baseline"][section],
                summaries[state][section], LIMITS[state],
            )
            for section in ("measured", "fresh_process")
        }
        state_comparison["pass"] = all(
            state_comparison[section]["pass"]
            for section in ("measured", "fresh_process")
        )
        comparisons[state] = state_comparison
        passed = passed and state_comparison["pass"]
    active_analyses = [
        analysis for section in ("warmups", "measured", "fresh_process")
        for analysis in raw["candidate-active"]["analyses"][section]
    ]
    return {
        "schema": RECEIPT_SCHEMA,
        "version": 1,
        "generated_utc": generated_utc,
        "producer_revision": PRODUCER_REVISION,
        "baseline_revision": baseline["baseline_revision"],
        "candidate_revision": baseline["candidate_revision"],
        "campaign_role": baseline["campaign_role"],
        "campaign_id": baseline["campaign_id"],
        "build_family_id": baseline["build_family_id"],
        "product_id": baseline["product_id"],
        "product_name": baseline["product_name"],
        "plugin_format": baseline["plugin_format"],
        "binaries": {state: payloads[state]["binary_sha256"] for state in STATES},
        "machine": baseline["machine"],
        "workload": baseline["workload"],
        "measurement_driver": baseline["measurement_driver"],
        "trace_session_config": baseline["trace_session_config"],
        "trace_processor": (
            artifact_ref(trace_processor_path, evidence_root)
            if trace_processor_path is not None else None
        ),
        "protocol": {
            "warmups": 5,
            "measured_trials": 30,
            "fresh_process_trials": 20,
            "ring_bytes": RING_BYTES,
            "xrun_limit": 0,
            "audio_thread_trace_event_limit": 0,
            "comparison_baseline": "pre-change-baseline",
        },
        "raw_artifacts": {
            state: artifact_ref(raw_paths[state], evidence_root) for state in STATES
        },
        "summaries": summaries,
        "comparisons": comparisons,
        "replay_summary": {
            "active_trace_count": len(active_analyses),
            "trace_formats": sorted({item["trace_format"] for item in active_analyses}),
            "producer_events": sum(item["producer_events"] for item in active_analyses),
            "foreign_producer_events": sum(
                item["foreign_producer_events"] for item in active_analyses
            ),
            "xrun_events": sum(item["xrun_events"] for item in active_analyses),
            "audio_thread_producer_events": sum(
                item["audio_thread_producer_events"] for item in active_analyses
            ),
        },
        "verdict": "pass" if passed else "fail",
    }


def validate_receipt(
    receipt: dict[str, Any], evidence_root: Path, *, require_pass: bool = True,
) -> None:
    exact_keys(receipt, RECEIPT_KEYS, "producer-overhead receipt")
    if receipt["schema"] != RECEIPT_SCHEMA or receipt["version"] != 1:
        raise OverheadError("producer-overhead receipt has the wrong protocol")
    refs = receipt["raw_artifacts"]
    exact_keys(refs, set(STATES), "producer-overhead raw artifacts")
    raw_paths: dict[str, Path] = {}
    for state in STATES:
        raw_paths[state] = artifact_path(
            refs[state], evidence_root, f"raw_artifacts.{state}",
        )
    processor_ref = receipt["trace_processor"]
    processor = (
        artifact_path(processor_ref, evidence_root, "trace_processor")
        if processor_ref is not None else None
    )
    if processor is not None and not os.access(processor, os.X_OK):
        raise OverheadError("trace_processor is not executable")
    expected = build_receipt(
        raw_paths, evidence_root=evidence_root,
        generated_utc=receipt["generated_utc"], trace_processor_path=processor,
    )
    if receipt != expected:
        raise OverheadError("producer-overhead receipt is not derived from its raw evidence")
    if require_pass and receipt["verdict"] != "pass":
        raise OverheadError("producer-overhead ceilings failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    ratify = subparsers.add_parser("ratify")
    for state in STATES:
        ratify.add_argument(f"--{state}", required=True, type=Path)
    ratify.add_argument("--evidence-root", required=True, type=Path)
    ratify.add_argument("--trace-processor", type=Path)
    ratify.add_argument("--generated-utc", required=True)
    ratify.add_argument("--output", required=True, type=Path)
    verify = subparsers.add_parser("verify")
    verify.add_argument("receipt", type=Path)
    verify.add_argument("--evidence-root", required=True, type=Path)
    args = parser.parse_args()
    try:
        evidence_root = args.evidence_root.resolve()
        if args.command == "ratify":
            paths = {
                state: getattr(args, state.replace("-", "_")).resolve()
                for state in STATES
            }
            receipt = build_receipt(
                paths, evidence_root=evidence_root,
                generated_utc=args.generated_utc,
                trace_processor_path=(
                    args.trace_processor.resolve() if args.trace_processor else None
                ),
            )
            atomic_json(args.output.resolve(), receipt)
            print(f"A3 trace producer overhead: {receipt['verdict']}")
            return 0 if receipt["verdict"] == "pass" else 1
        validate_receipt(
            regular_json(args.receipt, "producer-overhead receipt"), evidence_root,
        )
        print("A3 trace producer overhead: pass")
        return 0
    except (OverheadError, ValueError) as error:
        print(f"A3 trace producer overhead: FAIL: {error}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
