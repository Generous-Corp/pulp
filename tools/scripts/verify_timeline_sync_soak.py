#!/usr/bin/env python3
"""Verify an opt-in physical MIDI clock/MTC loopback soak."""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import json
import math
import os
import sys
from pathlib import Path
from typing import Any

SPEC_SCHEMA = "pulp.timeline-sync-soak-spec.v1"
TRACE_SCHEMA = "pulp.timeline-sync-soak-trace.v1"
SKIP = 77
REQUIRED_STREAMS = ("midi_clock", "mtc")


def _positive_number(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
        and float(value) > 0
    )


def _timestamp(value: Any, field: str, errors: list[str]) -> dt.datetime | None:
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{field} must be a non-empty RFC 3339 timestamp")
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        errors.append(f"{field} must be an RFC 3339 timestamp")
        return None
    if parsed.tzinfo is None:
        errors.append(f"{field} must include a timezone")
        return None
    return parsed


def validate(spec: Any, trace: Any) -> list[str]:
    errors: list[str] = []
    if not isinstance(spec, dict) or spec.get("schema") != SPEC_SCHEMA:
        return [f"spec schema must be {SPEC_SCHEMA}"]
    if not isinstance(trace, dict) or trace.get("schema") != TRACE_SCHEMA:
        return [f"trace schema must be {TRACE_SCHEMA}"]

    fixed_at = _timestamp(spec.get("fixed_at"), "spec.fixed_at", errors)
    captured_at = _timestamp(trace.get("captured_at"), "trace.captured_at", errors)
    if fixed_at is not None and captured_at is not None and fixed_at > captured_at:
        errors.append("spec.fixed_at must not be later than trace.captured_at")
    for field in (
        "min_duration_seconds",
        "max_abs_offset_samples",
        "max_drift_ppm",
    ):
        if not _positive_number(spec.get(field)):
            errors.append(f"spec.{field} must be a finite positive number")
    min_points = spec.get("min_points_per_stream")
    if not isinstance(min_points, int) or isinstance(min_points, bool) or min_points < 2:
        errors.append("spec.min_points_per_stream must be an integer >= 2")

    sample_rate = trace.get("sample_rate")
    if not _positive_number(sample_rate):
        errors.append("trace.sample_rate must be a finite positive number")
    points = trace.get("points")
    if not isinstance(points, list):
        errors.append("trace.points must be an array")
        return errors

    parsed: dict[str, list[tuple[int, int]]] = {
        stream: [] for stream in REQUIRED_STREAMS
    }
    for index, point in enumerate(points):
        if not isinstance(point, dict):
            errors.append(f"trace.points[{index}] must be an object")
            continue
        expected = point.get("expected_sample")
        observed = point.get("observed_sample")
        stream = point.get("stream")
        if stream not in REQUIRED_STREAMS:
            errors.append(
                f"trace.points[{index}].stream must be midi_clock or mtc"
            )
            continue
        if not isinstance(expected, int) or isinstance(expected, bool):
            errors.append(f"trace.points[{index}].expected_sample must be an integer")
            continue
        if not isinstance(observed, int) or isinstance(observed, bool):
            errors.append(f"trace.points[{index}].observed_sample must be an integer")
            continue
        parsed[stream].append((expected, observed))

    if not _positive_number(sample_rate):
        return errors
    for stream, stream_points in parsed.items():
        if isinstance(min_points, int) and len(stream_points) < min_points:
            errors.append(
                f"{stream} has {len(stream_points)} points; spec requires "
                f"at least {min_points}"
            )
        if len(stream_points) < 2:
            continue
        for index in range(1, len(stream_points)):
            if stream_points[index][0] <= stream_points[index - 1][0]:
                errors.append(f"{stream} expected samples must be strictly increasing")
                break
            if stream_points[index][1] <= stream_points[index - 1][1]:
                errors.append(f"{stream} observed samples must be strictly increasing")
                break

        expected_span = stream_points[-1][0] - stream_points[0][0]
        observed_span = stream_points[-1][1] - stream_points[0][1]
        duration = expected_span / float(sample_rate)
        if _positive_number(spec.get("min_duration_seconds")) and duration < float(
            spec["min_duration_seconds"]
        ):
            errors.append(
                f"{stream} duration {duration:.6f}s is below "
                f"{float(spec['min_duration_seconds']):.6f}s"
            )

        maximum_offset = max(
            abs(observed - expected) for expected, observed in stream_points
        )
        if _positive_number(
            spec.get("max_abs_offset_samples")
        ) and maximum_offset > float(spec["max_abs_offset_samples"]):
            errors.append(
                f"{stream} maximum absolute offset {maximum_offset} samples exceeds "
                f"{float(spec['max_abs_offset_samples']):.3f}"
            )

        if expected_span <= 0:
            continue
        drift_ppm = abs(observed_span - expected_span) / expected_span * 1_000_000.0
        if _positive_number(spec.get("max_drift_ppm")) and drift_ppm > float(
            spec["max_drift_ppm"]
        ):
            errors.append(
                f"{stream} endpoint drift {drift_ppm:.3f} ppm exceeds "
                f"{float(spec['max_drift_ppm']):.3f} ppm"
            )
    return errors


def self_test() -> int:
    spec = {
        "schema": SPEC_SCHEMA,
        "fixed_at": "2030-01-01T00:00:00Z",
        "min_duration_seconds": 10,
        "max_abs_offset_samples": 16,
        "max_drift_ppm": 25,
        "min_points_per_stream": 3,
    }
    trace = {
        "schema": TRACE_SCHEMA,
        "captured_at": "2030-01-02T00:00:00Z",
        "sample_rate": 48_000,
        "points": [
            {"stream": "midi_clock", "expected_sample": 0, "observed_sample": 4},
            {
                "stream": "midi_clock",
                "expected_sample": 240_000,
                "observed_sample": 240_004,
            },
            {
                "stream": "midi_clock",
                "expected_sample": 480_000,
                "observed_sample": 480_008,
            },
            {"stream": "mtc", "expected_sample": 0, "observed_sample": 3},
            {
                "stream": "mtc",
                "expected_sample": 240_000,
                "observed_sample": 240_003,
            },
            {
                "stream": "mtc",
                "expected_sample": 480_000,
                "observed_sample": 480_007,
            },
        ],
    }
    if validate(spec, trace):
        print("self-test: valid fixture rejected", file=sys.stderr)
        return 1

    mutations: list[tuple[str, dict[str, Any], dict[str, Any]]] = []
    no_fixed_at = copy.deepcopy(spec)
    no_fixed_at["fixed_at"] = ""
    mutations.append(("unfixed tolerances", no_fixed_at, trace))
    fixed_after_capture = copy.deepcopy(spec)
    fixed_after_capture["fixed_at"] = "2030-01-03T00:00:00Z"
    mutations.append(("tolerances fixed after capture", fixed_after_capture, trace))
    too_short = copy.deepcopy(trace)
    for point in too_short["points"]:
        point["expected_sample"] //= 10
        point["observed_sample"] //= 10
    mutations.append(("short trace", spec, too_short))
    excessive_offset = copy.deepcopy(trace)
    excessive_offset["points"][1]["observed_sample"] += 100
    mutations.append(("excessive offset", spec, excessive_offset))
    excessive_drift = copy.deepcopy(trace)
    excessive_drift["points"][-1]["observed_sample"] += 100
    mutations.append(("excessive drift", spec, excessive_drift))
    unordered = copy.deepcopy(trace)
    unordered["points"][1]["observed_sample"] = -1
    mutations.append(("unordered observations", spec, unordered))
    missing_stream = copy.deepcopy(trace)
    missing_stream["points"] = [
        point for point in missing_stream["points"] if point["stream"] != "mtc"
    ]
    mutations.append(("missing MTC stream", spec, missing_stream))

    for name, mutated_spec, mutated_trace in mutations:
        if not validate(mutated_spec, mutated_trace):
            print(f"self-test: negative control passed: {name}", file=sys.stderr)
            return 1
    print(f"timeline sync soak verifier self-test passed ({len(mutations)} negative controls)")
    return 0


def _load(path: Path, label: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read {label} {path}: {exc}") from exc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    spec_path = args.spec or (
        Path(value) if (value := os.environ.get("PULP_TIMELINE_SYNC_SOAK_SPEC")) else None
    )
    trace_path = args.trace or (
        Path(value) if (value := os.environ.get("PULP_TIMELINE_SYNC_SOAK_TRACE")) else None
    )
    if spec_path is None and trace_path is None:
        print(
            "SKIP: physical timeline sync soak is opt-in; set "
            "PULP_TIMELINE_SYNC_SOAK_SPEC and PULP_TIMELINE_SYNC_SOAK_TRACE"
        )
        return SKIP
    if spec_path is None or trace_path is None:
        print("error: both soak spec and trace are required", file=sys.stderr)
        return 2
    try:
        spec = _load(spec_path, "spec")
        trace = _load(trace_path, "trace")
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    errors = validate(spec, trace)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print(
        f"timeline sync soak passed: {len(trace['points'])} points at "
        f"{trace['sample_rate']} Hz"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
