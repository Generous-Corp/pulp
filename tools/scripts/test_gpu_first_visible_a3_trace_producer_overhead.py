#!/usr/bin/env python3
"""Positive and planted-negative controls for A3 trace-producer overhead."""

from __future__ import annotations

import copy
import json
import tempfile
from pathlib import Path
from typing import Any, Callable

import gpu_first_visible_a3_trace_producer_overhead as overhead


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def make_fixture(
    root: Path, *, candidate_revision: str = "c" * 40,
    driver_path: str = "tools/testing/a3/trace-producer-overhead-driver.py",
    driver_sha256: str = "f" * 64,
) -> tuple[dict[str, Path], dict[str, dict[str, Any]]]:
    baseline_revision = overhead.BASELINE_REVISION
    machine = {
        "machine_id": "m5-overhead",
        "operating_system": "macOS",
        "architecture": "arm64",
    }
    workload = {
        "workload_id": "forge-first-visible-reference",
        "content_sha256": "d" * 64,
        "adapter_sha256": "e" * 64,
    }
    driver = {
        "revision": candidate_revision,
        "path": driver_path,
        "sha256": driver_sha256,
    }
    duration = {
        "pre-change-baseline": 10.0,
        "candidate-compile-out": 10.05,
        "candidate-compiled-in-idle": 10.1,
        "candidate-active": 10.4,
    }
    counter = 1
    documents: dict[str, dict[str, Any]] = {}
    paths: dict[str, Path] = {}
    for state in overhead.STATES:
        active = state == "candidate-active"

        def samples(section: str, count: int) -> list[dict[str, Any]]:
            nonlocal counter
            rows = []
            for sequence in range(count):
                evidence_id = f"{counter:032x}"
                counter += 1
                trace_path = None
                trace_digest = None
                trace_bytes = 0
                if active:
                    trace = root / "traces" / f"{state}-{section}-{sequence}.pftrace"
                    trace.parent.mkdir(parents=True, exist_ok=True)
                    trace.write_bytes(f"trace:{evidence_id}".encode())
                    trace_path = trace.relative_to(root).as_posix()
                    trace_digest = overhead.sha256_file(trace, "fixture trace")
                    trace_bytes = trace.stat().st_size
                rows.append({
                    "sequence": sequence,
                    "evidence_id": evidence_id,
                    "duration_ms": duration[state],
                    "xrun_count": 0,
                    "audio_thread_trace_events": 0,
                    "trace_path": trace_path,
                    "trace_sha256": trace_digest,
                    "trace_bytes": trace_bytes,
                })
            return rows

        compiled, session_active, ring_bytes = overhead.STATE_TRACING[state]
        document = {
            "schema": overhead.RAW_SCHEMA,
            "version": 1,
            "state": state,
            "producer_revision": overhead.PRODUCER_REVISION,
            "source_revision": (
                baseline_revision if state == "pre-change-baseline"
                else candidate_revision
            ),
            "baseline_revision": baseline_revision,
            "candidate_revision": candidate_revision,
            "build_family_id": "a3-overhead-build-family",
            "product_id": "dev.pulp.forge-overhead",
            "binary_sha256": (
                "3" * 64 if state == "candidate-active"
                else f"{overhead.STATES.index(state) + 1:x}" * 64
            ),
            "machine": copy.deepcopy(machine),
            "workload": copy.deepcopy(workload),
            "measurement_driver": copy.deepcopy(driver),
            "tracing": {
                "compiled_in": compiled,
                "session_active": session_active,
                "ring_bytes": ring_bytes,
            },
            "warmups": samples("warmup", 5),
            "measured": samples("measured", 30),
            "fresh_process": samples("fresh", 20),
        }
        path = root / f"{state}.json"
        write_json(path, document)
        documents[state] = document
        paths[state] = path
    return paths, documents


def expect_failure(
    root: Path, mutation: Callable[[dict[str, dict[str, Any]], dict[str, Path]], None],
    label: str,
) -> None:
    paths, documents = make_fixture(root)
    mutation(documents, paths)
    for state in overhead.STATES:
        write_json(paths[state], documents[state])
    try:
        receipt = overhead.build_receipt(
            paths, evidence_root=root, generated_utc="2026-08-29T12:00:00Z",
        )
    except overhead.OverheadError:
        return
    if receipt["verdict"] == "fail":
        return
    raise AssertionError(f"planted producer-overhead negative did not fire: {label}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pulp-a3-trace-overhead-") as temporary:
        root = Path(temporary)
        paths, _ = make_fixture(root / "positive")
        receipt = overhead.build_receipt(
            paths, evidence_root=root / "positive",
            generated_utc="2026-08-29T12:00:00Z",
        )
        assert receipt["verdict"] == "pass"
        receipt_path = root / "positive" / "receipt.json"
        write_json(receipt_path, receipt)
        overhead.validate_receipt(receipt, root / "positive")

        negatives: list[Callable[[dict[str, dict[str, Any]], dict[str, Path]], None]] = [
            lambda docs, _: docs["candidate-active"].__setitem__(
                "measured", [dict(row, duration_ms=20.0)
                             for row in docs["candidate-active"]["measured"]],
            ),
            lambda docs, _: docs["candidate-active"]["fresh_process"][0].__setitem__(
                "xrun_count", 1,
            ),
            lambda docs, _: docs["candidate-compiled-in-idle"]["measured"][0].__setitem__(
                "audio_thread_trace_events", 1,
            ),
            lambda docs, _: docs["candidate-active"]["measured"][0].__setitem__(
                "trace_sha256", "0" * 64,
            ),
            lambda docs, _: docs["candidate-compile-out"]["workload"].__setitem__(
                "content_sha256", "0" * 64,
            ),
            lambda docs, _: docs["candidate-active"]["measured"][1].__setitem__(
                "evidence_id", docs["candidate-active"]["measured"][0]["evidence_id"],
            ),
            lambda docs, _: docs["candidate-compile-out"].__setitem__(
                "source_revision", docs["candidate-compile-out"]["baseline_revision"],
            ),
            lambda docs, _: docs["pre-change-baseline"].__setitem__(
                "baseline_revision", "b" * 40,
            ),
            lambda docs, _: docs["candidate-active"].__setitem__(
                "binary_sha256", "4" * 64,
            ),
        ]
        for index, mutation in enumerate(negatives):
            case_root = root / f"negative-{index}"
            case_root.mkdir()
            expect_failure(case_root, mutation, str(index))

        mutated_receipt = copy.deepcopy(receipt)
        mutated_receipt["comparisons"]["candidate-active"]["pass"] = False
        try:
            overhead.validate_receipt(mutated_receipt, root / "positive")
        except overhead.OverheadError:
            pass
        else:
            raise AssertionError("derived receipt mutation did not fire")

        print("gpu-first-visible-a3-trace-producer-overhead: positive=1 planted_negatives=10")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
