#!/usr/bin/env python3
"""Validate Pulp GPU-audio Perfetto positive and negative control captures.

This is deliberately an offline, exact-capture validator.  It appends a
constant SELECT to a private temporary copy of the checked-in SQL definitions,
then invokes the SDK-matched trace_processor once per capture.  The definitions
file is never modified, and trace paths are passed as subprocess arguments
rather than interpolated into SQL.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Mapping


METRIC_PATTERN = re.compile(r"pulp_gpu_audio_validation:([a-z_]+)=([0-9]+)")
PINNED_PERFETTO_VERSION = "v57.2"


class ValidationError(RuntimeError):
    """The capture did not establish the GPU-audio trace contract."""


def _require_regular_file(path: Path, label: str, *, executable: bool = False) -> None:
    if not path.is_file():
        raise ValidationError(f"{label} is not a regular file: {path}")
    if executable and not os.access(path, os.X_OK):
        raise ValidationError(f"{label} is not executable: {path}")


def _require_pinned_processor(processor: Path) -> None:
    _require_regular_file(processor, "trace_processor", executable=True)
    try:
        completed = subprocess.run(
            [str(processor), "--version"],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
    except OSError as error:
        raise ValidationError(f"could not start trace_processor: {error}") from error
    except subprocess.TimeoutExpired as error:
        raise ValidationError("trace_processor timed out while reporting its version") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise ValidationError(f"trace_processor could not report its version: {detail}")
    if not re.search(r"\bPerfetto " + re.escape(PINNED_PERFETTO_VERSION) + r"(?=[-\s]|$)",
                     completed.stdout):
        raise ValidationError(
            f"trace_processor is not the Pulp-pinned Perfetto {PINNED_PERFETTO_VERSION}: "
            f"{completed.stdout.strip()}"
        )


def _metrics_query(kind: str) -> str:
    if kind == "positive":
        return """
SELECT 'pulp_gpu_audio_validation:session_rows=' || COUNT(*)
  FROM pulp_gpu_audio_sessions
UNION ALL
SELECT 'pulp_gpu_audio_validation:block_rows=' || COUNT(*)
  FROM pulp_gpu_audio_blocks
UNION ALL
SELECT 'pulp_gpu_audio_validation:admission_rows=' || COUNT(*)
  FROM pulp_gpu_audio_admissions
UNION ALL
SELECT 'pulp_gpu_audio_validation:terminal_rows=' || COUNT(*)
  FROM pulp_gpu_audio_terminals
UNION ALL
SELECT 'pulp_gpu_audio_validation:eligible_rows=' || COUNT(*)
  FROM pulp_gpu_audio_eligible
UNION ALL
SELECT 'pulp_gpu_audio_validation:delivery_rows=' || COUNT(*)
  FROM pulp_gpu_audio_deliveries
UNION ALL
SELECT 'pulp_gpu_audio_validation:recovery_rows=' || COUNT(*)
  FROM pulp_gpu_audio_recoveries
UNION ALL
SELECT 'pulp_gpu_audio_validation:capture_issue_rows=' || COUNT(*)
  FROM pulp_gpu_audio_capture_issues
UNION ALL
SELECT 'pulp_gpu_audio_validation:full_lifecycle_issue_rows=' || COUNT(*)
  FROM pulp_gpu_audio_full_lifecycle_issues
UNION ALL
SELECT 'pulp_gpu_audio_validation:full_lifecycle_generation_rows=' || COUNT(*)
  FROM pulp_gpu_audio_full_lifecycle_generations
UNION ALL
SELECT 'pulp_gpu_audio_validation:lifecycle_violation_rows=' || COUNT(*)
  FROM pulp_gpu_audio_lifecycle_violations
UNION ALL
SELECT 'pulp_gpu_audio_validation:duplicate_terminal_rows=' || COUNT(*)
  FROM pulp_gpu_audio_duplicate_terminals
UNION ALL
SELECT 'pulp_gpu_audio_validation:duplicate_delivery_rows=' || COUNT(*)
  FROM pulp_gpu_audio_duplicate_deliveries
UNION ALL
SELECT 'pulp_gpu_audio_validation:admission_violation_rows=' || COUNT(*)
  FROM pulp_gpu_audio_admission_violations
UNION ALL
SELECT 'pulp_gpu_audio_validation:duplicate_admission_rows=' || COUNT(*)
  FROM pulp_gpu_audio_duplicate_admissions
UNION ALL
SELECT 'pulp_gpu_audio_validation:unavailable_gpu_null_rows=' || COUNT(*)
  FROM pulp_gpu_audio_blocks
 WHERE gpu_elapsed_available = 0 AND gpu_elapsed_ns IS NULL
UNION ALL
SELECT 'pulp_gpu_audio_validation:unavailable_gpu_nonnull_rows=' || COUNT(*)
  FROM pulp_gpu_audio_blocks
 WHERE gpu_elapsed_available = 0 AND gpu_elapsed_ns IS NOT NULL;
"""
    if kind == "negative":
        return """
SELECT 'pulp_gpu_audio_validation:queue_dropped_issue_rows=' || COUNT(*)
  FROM pulp_gpu_audio_capture_issues
 WHERE issue = 'queue_dropped'
UNION ALL
SELECT 'pulp_gpu_audio_validation:full_lifecycle_issue_rows=' || COUNT(*)
  FROM pulp_gpu_audio_full_lifecycle_issues
UNION ALL
SELECT 'pulp_gpu_audio_validation:lifecycle_violation_rows=' || COUNT(*)
  FROM pulp_gpu_audio_lifecycle_violations
UNION ALL
SELECT 'pulp_gpu_audio_validation:missing_gpu_terminal_rows=' || COUNT(*)
  FROM pulp_gpu_audio_lifecycle_violations
 WHERE violation = 'missing_gpu_terminal'
UNION ALL
SELECT 'pulp_gpu_audio_validation:missing_delivery_rows=' || COUNT(*)
  FROM pulp_gpu_audio_lifecycle_violations
 WHERE violation = 'missing_delivery'
UNION ALL
SELECT 'pulp_gpu_audio_validation:orphan_terminal_rows=' || COUNT(*)
  FROM pulp_gpu_audio_lifecycle_violations
 WHERE violation = 'orphan_terminal'
UNION ALL
SELECT 'pulp_gpu_audio_validation:delivery_without_eligibility_rows=' || COUNT(*)
  FROM pulp_gpu_audio_lifecycle_violations
 WHERE violation = 'delivery_without_eligibility'
UNION ALL
SELECT 'pulp_gpu_audio_validation:duplicate_terminal_rows=' || COUNT(*)
  FROM pulp_gpu_audio_duplicate_terminals
UNION ALL
SELECT 'pulp_gpu_audio_validation:duplicate_delivery_rows=' || COUNT(*)
  FROM pulp_gpu_audio_duplicate_deliveries
UNION ALL
SELECT 'pulp_gpu_audio_validation:missing_terminal_for_admission_rows=' || COUNT(*)
  FROM pulp_gpu_audio_admission_violations
 WHERE violation = 'missing_terminal_for_admission'
UNION ALL
SELECT 'pulp_gpu_audio_validation:terminal_without_admission_rows=' || COUNT(*)
  FROM pulp_gpu_audio_admission_violations
 WHERE violation = 'terminal_without_admission'
UNION ALL
SELECT 'pulp_gpu_audio_validation:duplicate_admission_rows=' || COUNT(*)
  FROM pulp_gpu_audio_duplicate_admissions;
"""
    raise ValueError(f"unknown validation kind: {kind}")


def _parse_metrics(output: str, required: set[str]) -> dict[str, int]:
    metrics: dict[str, int] = {}
    for name, raw_value in METRIC_PATTERN.findall(output):
        if name in metrics:
            raise ValidationError(f"trace_processor reported duplicate metric {name}")
        metrics[name] = int(raw_value)
    missing = sorted(required - metrics.keys())
    if missing:
        raise ValidationError(
            "trace_processor did not return required metric(s): " + ", ".join(missing)
        )
    return metrics


def _query_metrics(
    processor: Path,
    definitions: Path,
    trace: Path,
    *,
    kind: str,
    timeout_seconds: int = 30,
) -> dict[str, int]:
    """Run one constant query against a private definitions copy."""
    definitions_text = definitions.read_text(encoding="utf-8")
    if not definitions_text.strip():
        raise ValidationError(f"definitions SQL is empty: {definitions}")
    query = _metrics_query(kind)
    # Keep the metrics closed. The command output must carry every one; a
    # changed query cannot accidentally turn a partial result into success.
    if kind == "positive":
        required = {
            "session_rows", "block_rows", "admission_rows", "terminal_rows",
            "eligible_rows", "delivery_rows", "recovery_rows", "capture_issue_rows",
            "full_lifecycle_issue_rows", "full_lifecycle_generation_rows",
            "lifecycle_violation_rows",
            "duplicate_terminal_rows", "duplicate_delivery_rows", "unavailable_gpu_null_rows",
            "unavailable_gpu_nonnull_rows", "admission_violation_rows",
            "duplicate_admission_rows",
        }
    else:
        required = {
            "queue_dropped_issue_rows", "full_lifecycle_issue_rows",
            "lifecycle_violation_rows",
            "missing_gpu_terminal_rows", "missing_delivery_rows", "orphan_terminal_rows",
            "delivery_without_eligibility_rows",
            "duplicate_terminal_rows", "duplicate_delivery_rows",
            "missing_terminal_for_admission_rows", "terminal_without_admission_rows",
            "duplicate_admission_rows",
        }

    with tempfile.TemporaryDirectory(prefix="pulp-gpu-audio-trace-") as temporary:
        query_file = Path(temporary) / "query.sql"
        query_file.write_text(f"{definitions_text}\n\n{query}", encoding="utf-8")
        try:
            completed = subprocess.run(
                [str(processor), "query", "-f", str(query_file), str(trace)],
                check=False,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=timeout_seconds,
            )
        except OSError as error:
            raise ValidationError(f"could not start trace_processor: {error}") from error
        except subprocess.TimeoutExpired as error:
            raise ValidationError(
                f"trace_processor timed out after {timeout_seconds}s for {trace}"
            ) from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise ValidationError(f"trace_processor failed for {trace}: {detail}")
    return _parse_metrics(completed.stdout, required)


def _expect_exact(metrics: Mapping[str, int], expected: Mapping[str, int], capture: str) -> None:
    failures = [
        f"{name}={metrics[name]} (expected {value})"
        for name, value in expected.items()
        if metrics[name] != value
    ]
    if failures:
        raise ValidationError(f"{capture} capture failed: " + "; ".join(failures))


def _expect_minimum(metrics: Mapping[str, int], expected: Mapping[str, int], capture: str) -> None:
    failures = [
        f"{name}={metrics[name]} (expected at least {value})"
        for name, value in expected.items()
        if metrics[name] < value
    ]
    if failures:
        raise ValidationError(f"{capture} capture failed: " + "; ".join(failures))


def validate_captures(
    processor: Path,
    definitions: Path,
    positive_trace: Path,
    negative_trace: Path,
) -> tuple[dict[str, int], dict[str, int]]:
    """Validate the bounded positive trace and planted-negative trace."""
    _require_pinned_processor(processor)
    _require_regular_file(definitions, "definitions SQL")
    _require_regular_file(positive_trace, "positive trace")
    _require_regular_file(negative_trace, "negative trace")

    positive = _query_metrics(processor, definitions, positive_trace, kind="positive")
    _expect_exact(
        positive,
        {
            "session_rows": 1,
            "block_rows": 2,
            "admission_rows": 2,
            "terminal_rows": 2,
            "eligible_rows": 2,
            "delivery_rows": 2,
            "recovery_rows": 0,
            "capture_issue_rows": 0,
            "full_lifecycle_issue_rows": 0,
            "full_lifecycle_generation_rows": 1,
            "lifecycle_violation_rows": 0,
            "duplicate_terminal_rows": 0,
            "duplicate_delivery_rows": 0,
            "admission_violation_rows": 0,
            "duplicate_admission_rows": 0,
            # The control intentionally has no authentic GPU timestamps. A
            # missing timestamp must remain SQL NULL, never become zero.
            "unavailable_gpu_null_rows": 2,
            "unavailable_gpu_nonnull_rows": 0,
        },
        "positive",
    )

    negative = _query_metrics(processor, definitions, negative_trace, kind="negative")
    _expect_minimum(
        negative,
        {
            "queue_dropped_issue_rows": 1,
            "full_lifecycle_issue_rows": 1,
            "lifecycle_violation_rows": 1,
            "missing_gpu_terminal_rows": 1,
            "missing_delivery_rows": 1,
            "orphan_terminal_rows": 1,
            "delivery_without_eligibility_rows": 1,
            "duplicate_terminal_rows": 1,
            "duplicate_delivery_rows": 1,
            "missing_terminal_for_admission_rows": 1,
            "terminal_without_admission_rows": 1,
            "duplicate_admission_rows": 1,
        },
        "negative",
    )
    return positive, negative


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--processor", type=Path, required=True,
                        help="SDK-matched trace_processor_shell executable")
    parser.add_argument("--definitions", type=Path, required=True,
                        help="checked-in gpu-audio Perfetto SQL definitions")
    parser.add_argument("--positive-trace", type=Path, required=True,
                        help="lossless GPU-audio positive control capture")
    parser.add_argument("--negative-trace", type=Path, required=True,
                        help="planted-invalid GPU-audio capture")
    args = parser.parse_args(argv)
    try:
        positive, negative = validate_captures(
            args.processor, args.definitions, args.positive_trace, args.negative_trace,
        )
    except ValidationError as error:
        print(f"GPU audio trace validation failed: {error}", file=sys.stderr)
        return 1
    print(
        "GPU audio trace validation passed: "
        f"positive blocks={positive['block_rows']} sessions={positive['session_rows']}; "
        f"negative queue_drops={negative['queue_dropped_issue_rows']} "
        f"lifecycle={negative['lifecycle_violation_rows']} "
        f"duplicate_terminals={negative['duplicate_terminal_rows']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
