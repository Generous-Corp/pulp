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

RAW_SCHEMA = "pulp.gpu-first-visible-trace-producer-overhead-raw.v1"
RECEIPT_SCHEMA = "pulp.gpu-first-visible-trace-producer-overhead.v1"
PRODUCER_REVISION = "8175bd483f5e4ca66989c9ad91a4d9ed5a864bb0"
BASELINE_REVISION = "5048ce72dd28d87974550a3feb526de0f44af32c"
RING_BYTES = 128 * 1024 * 1024
STATES = (
    "pre-change-baseline",
    "candidate-compile-out",
    "candidate-compiled-in-idle",
    "candidate-active",
)
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
    "baseline_revision", "candidate_revision", "build_family_id", "product_id",
    "binary_sha256", "machine", "workload", "measurement_driver", "tracing",
    "warmups", "measured", "fresh_process",
}
MACHINE_KEYS = {"machine_id", "operating_system", "architecture"}
WORKLOAD_KEYS = {"workload_id", "content_sha256", "adapter_sha256"}
DRIVER_KEYS = {"revision", "path", "sha256"}
TRACING_KEYS = {"compiled_in", "session_active", "ring_bytes"}
SAMPLE_KEYS = {
    "sequence", "evidence_id", "duration_ms", "xrun_count",
    "audio_thread_trace_events", "trace_path", "trace_sha256", "trace_bytes",
}
RECEIPT_KEYS = {
    "schema", "version", "generated_utc", "producer_revision",
    "baseline_revision", "candidate_revision", "build_family_id", "product_id",
    "machine", "workload", "measurement_driver", "protocol", "raw_artifacts", "summaries",
    "comparisons", "verdict",
}
SHA256 = re.compile(r"^[0-9a-f]{64}$")
EVIDENCE_ID = re.compile(r"^[0-9a-f]{32}$")
GIT_REVISION = re.compile(r"^[0-9a-f]{40}$")


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


def finite_positive(value: Any) -> bool:
    return (
        isinstance(value, (int, float)) and not isinstance(value, bool)
        and math.isfinite(value) and value > 0
    )


def validate_samples(
    rows: Any, *, count: int, state: str, evidence_root: Path, label: str,
) -> list[dict[str, Any]]:
    if not isinstance(rows, list) or len(rows) != count:
        raise OverheadError(f"{label} must contain exactly {count} samples")
    active = state == "candidate-active"
    evidence_ids: set[str] = set()
    for index, row in enumerate(rows):
        exact_keys(row, SAMPLE_KEYS, f"{label}[{index}]")
        if (
            row["sequence"] != index
            or not isinstance(row["evidence_id"], str)
            or EVIDENCE_ID.fullmatch(row["evidence_id"]) is None
            or row["evidence_id"] in evidence_ids
            or not finite_positive(row["duration_ms"])
            or row["xrun_count"] != 0
            or row["audio_thread_trace_events"] != 0
        ):
            raise OverheadError(f"{label}[{index}] lacks bounded timing/xrun identity")
        evidence_ids.add(row["evidence_id"])
        if active:
            if (
                not isinstance(row["trace_sha256"], str)
                or SHA256.fullmatch(row["trace_sha256"]) is None
                or not isinstance(row["trace_bytes"], int)
                or isinstance(row["trace_bytes"], bool)
                or not 0 < row["trace_bytes"] <= RING_BYTES
            ):
                raise OverheadError(f"{label}[{index}] lacks a bounded active trace")
            trace = safe_artifact(
                evidence_root, row["trace_path"], f"{label}[{index}].trace",
            )
            data = regular_file_bytes(trace, f"{label}[{index}].trace")
            if len(data) != row["trace_bytes"] or sha256_bytes(data) != row["trace_sha256"]:
                raise OverheadError(f"{label}[{index}] trace digest/size differs")
        elif (
            row["trace_path"] is not None
            or row["trace_sha256"] is not None
            or row["trace_bytes"] != 0
        ):
            raise OverheadError(f"{label}[{index}] inactive state claims a trace")
    return rows


def validate_raw(
    payload: dict[str, Any], *, state: str, evidence_root: Path, label: str,
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
        or not isinstance(payload["build_family_id"], str)
        or not payload["build_family_id"]
        or not isinstance(payload["product_id"], str)
        or not payload["product_id"]
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
    validate_samples(
        payload["warmups"], count=5, state=state,
        evidence_root=evidence_root, label=f"{label}.warmups",
    )
    validate_samples(
        payload["measured"], count=30, state=state,
        evidence_root=evidence_root, label=f"{label}.measured",
    )
    validate_samples(
        payload["fresh_process"], count=20, state=state,
        evidence_root=evidence_root, label=f"{label}.fresh_process",
    )
    return payload


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


def artifact_ref(path: Path, evidence_root: Path) -> dict[str, str]:
    resolved = path.resolve()
    root = evidence_root.resolve()
    if root not in resolved.parents:
        raise OverheadError(f"raw artifact escapes the evidence root: {path}")
    return {"path": resolved.relative_to(root).as_posix(), "sha256": sha256_file(resolved, "raw artifact")}


def build_receipt(
    raw_paths: dict[str, Path], *, evidence_root: Path, generated_utc: str,
) -> dict[str, Any]:
    if set(raw_paths) != set(STATES):
        raise OverheadError("all four producer-overhead states are required")
    raw = {
        state: validate_raw(
            regular_json(raw_paths[state], f"{state} raw evidence"),
            state=state, evidence_root=evidence_root, label=state,
        )
        for state in STATES
    }
    common_fields = (
        "producer_revision", "baseline_revision", "candidate_revision",
        "build_family_id", "product_id", "machine", "workload", "measurement_driver",
    )
    baseline = raw["pre-change-baseline"]
    for state in STATES[1:]:
        if any(raw[state][field] != baseline[field] for field in common_fields):
            raise OverheadError(f"{state} does not use the same source/host/workload family")
    if (
        raw["candidate-compiled-in-idle"]["binary_sha256"]
        != raw["candidate-active"]["binary_sha256"]
    ):
        raise OverheadError("compiled-in idle and active states must use the same product binary")
    if (
        raw["candidate-compile-out"]["binary_sha256"]
        == raw["candidate-compiled-in-idle"]["binary_sha256"]
    ):
        raise OverheadError("compile-out and compiled-in states do not identify distinct builds")
    all_ids = [
        row["evidence_id"] for state in STATES for section in (
            "warmups", "measured", "fresh_process",
        ) for row in raw[state][section]
    ]
    if len(set(all_ids)) != len(all_ids):
        raise OverheadError("producer-overhead evidence IDs are not globally unique")
    summaries = {
        state: {
            "measured": summary(raw[state]["measured"]),
            "fresh_process": summary(raw[state]["fresh_process"]),
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
    return {
        "schema": RECEIPT_SCHEMA,
        "version": 1,
        "generated_utc": generated_utc,
        "producer_revision": PRODUCER_REVISION,
        "baseline_revision": baseline["baseline_revision"],
        "candidate_revision": baseline["candidate_revision"],
        "build_family_id": baseline["build_family_id"],
        "product_id": baseline["product_id"],
        "machine": baseline["machine"],
        "workload": baseline["workload"],
        "measurement_driver": baseline["measurement_driver"],
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
        exact_keys(refs[state], {"path", "sha256"}, f"raw_artifacts.{state}")
        path = safe_artifact(evidence_root, refs[state]["path"], f"raw_artifacts.{state}")
        if sha256_file(path, f"raw_artifacts.{state}") != refs[state]["sha256"]:
            raise OverheadError(f"raw_artifacts.{state} digest differs")
        raw_paths[state] = path
    expected = build_receipt(
        raw_paths, evidence_root=evidence_root,
        generated_utc=receipt["generated_utc"],
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
            )
            atomic_json(args.output.resolve(), receipt)
            print(f"A3 trace producer overhead: {receipt['verdict']}")
            return 0 if receipt["verdict"] == "pass" else 1
        validate_receipt(regular_json(args.receipt, "producer-overhead receipt"), evidence_root)
        print("A3 trace producer overhead: pass")
        return 0
    except OverheadError as error:
        print(f"A3 trace producer overhead: FAIL: {error}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
