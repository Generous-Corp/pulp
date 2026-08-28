#!/usr/bin/env python3
"""Fail a Perfetto trace when a continuous interaction exceeds its budget."""

from __future__ import annotations

import argparse
import dataclasses
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


MARKER = "PULP_TRANSIENT_INTERACTION_BUDGET"


@dataclasses.dataclass(frozen=True)
class Metrics:
    input_count: int
    input_p95_ms: float
    work_count: int
    work_per_input: float


def _sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def build_query(input_slice: str, work_slice: str) -> str:
    input_name = _sql_string(input_slice)
    work_name = _sql_string(work_slice)
    return f"""
WITH input AS (
  SELECT ts, dur,
         ROW_NUMBER() OVER (ORDER BY dur) AS rn,
         COUNT(*) OVER () AS n
  FROM slice
  WHERE name = {input_name} AND dur >= 0
), bounds AS (
  SELECT MIN(ts) AS first_ts, MAX(ts + dur) AS last_ts, COUNT(*) AS input_count
  FROM input
), p95 AS (
  SELECT COALESCE(MAX(dur) / 1000000.0, 0.0) AS input_p95_ms
  FROM input
  WHERE rn = CAST((95 * n + 99) / 100 AS INT)
), work AS (
  SELECT COUNT(*) AS work_count
  FROM slice, bounds
  WHERE name = {work_name} AND dur >= 0
    AND ts >= bounds.first_ts AND ts <= bounds.last_ts
)
SELECT '{MARKER}|' || bounds.input_count || '|' ||
       printf('%.6f', p95.input_p95_ms) || '|' || work.work_count || '|' ||
       printf('%.6f', CASE WHEN bounds.input_count = 0 THEN 0.0
                           ELSE 1.0 * work.work_count / bounds.input_count END)
FROM bounds, p95, work;
"""


def parse_metrics(output: str) -> Metrics:
    match = re.search(
        rf"{MARKER}\|(\d+)\|([0-9.]+)\|(\d+)\|([0-9.]+)", output)
    if not match:
        raise ValueError("trace_processor output did not contain the budget marker")
    return Metrics(
        input_count=int(match.group(1)),
        input_p95_ms=float(match.group(2)),
        work_count=int(match.group(3)),
        work_per_input=float(match.group(4)),
    )


def budget_failures(metrics: Metrics, max_input_p95_ms: float,
                    max_work_per_input: float) -> list[str]:
    failures = []
    if metrics.input_count == 0:
        failures.append("trace contains no matching input slices")
    if metrics.input_p95_ms > max_input_p95_ms:
        failures.append(
            f"input p95 {metrics.input_p95_ms:.3f} ms exceeds {max_input_p95_ms:.3f} ms")
    if metrics.work_per_input > max_work_per_input:
        failures.append(
            f"work/input {metrics.work_per_input:.3f} exceeds {max_work_per_input:.3f}")
    return failures


def resolve_processor(explicit: str) -> str | None:
    if explicit:
        return explicit
    configured = os.environ.get("PULP_TRACE_PROCESSOR", "")
    if configured:
        return configured
    return shutil.which("trace_processor_shell") or shutil.which("trace_processor")


def run_processor(processor: str, trace: Path, query: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as handle:
        handle.write(query)
        query_path = Path(handle.name)
    try:
        result = subprocess.run(
            [processor, "-q", str(query_path), str(trace)],
            check=False,
            capture_output=True,
            text=True,
        )
    finally:
        query_path.unlink(missing_ok=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"trace_processor exited {result.returncode}: {result.stderr.strip()}")
    return result.stdout


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--input-slice", default="native_drag_dispatch")
    parser.add_argument("--work-slice", default="layout_root")
    parser.add_argument("--max-input-p95-ms", type=float, required=True)
    parser.add_argument("--max-work-per-input", type=float, required=True)
    parser.add_argument("--processor", default="")
    args = parser.parse_args(argv)

    if not args.trace.is_file():
        parser.error(f"trace does not exist: {args.trace}")
    processor = resolve_processor(args.processor)
    if not processor:
        parser.error(
            "trace_processor not found; set PULP_TRACE_PROCESSOR or pass --processor")

    try:
        output = run_processor(
            processor, args.trace, build_query(args.input_slice, args.work_slice))
        metrics = parse_metrics(output)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"trace budget error: {error}", file=sys.stderr)
        return 2

    print(
        f"input_count={metrics.input_count} input_p95_ms={metrics.input_p95_ms:.3f} "
        f"work_count={metrics.work_count} work_per_input={metrics.work_per_input:.3f}")
    failures = budget_failures(
        metrics, args.max_input_p95_ms, args.max_work_per_input)
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
