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
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8",
    )


def make_fixture(
    root: Path, *, candidate_revision: str = "c" * 40,
    driver_path: str = "tools/testing/a3/trace-producer-overhead-driver.py",
    driver_sha256: str = "f" * 64,
    campaign_role: str = "standalone", campaign_id: str = "campaign-1",
    build_family_id: str = "pulp-build-1",
    product_id: str = "dev.pulp.product-1",
    product_name: str = "GPU Product 1", plugin_format: str = "standalone",
    machine_id: str = "m5-blackbook",
    candidate_binary_sha256: str = "3" * 64,
) -> tuple[dict[str, Path], dict[str, dict[str, Any]]]:
    baseline_revision = overhead.BASELINE_REVISION
    machine = {
        "machine_id": machine_id,
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
    session_config_path = root / "trace-session-config.json"
    write_json(session_config_path, {
        "schema": overhead.SESSION_CONFIG_SCHEMA,
        "version": 1,
        "ring_bytes": overhead.RING_BYTES,
        "fill_policy": "ring-buffer",
        "categories": ["dsp", "gpu", "metadata", "render"],
    })
    session_config_ref = overhead.artifact_ref(session_config_path, root)
    counter = 1
    documents: dict[str, dict[str, Any]] = {}
    paths: dict[str, Path] = {}
    for state in overhead.STATES:
        active = state == "candidate-active"
        binary_sha256 = (
            candidate_binary_sha256 if state in {
                "candidate-compiled-in-idle", "candidate-active",
            } else f"{overhead.STATES.index(state) + 1:x}" * 64
        )

        def samples(section: str, count: int) -> list[dict[str, Any]]:
            nonlocal counter
            rows = []
            for sequence in range(count):
                evidence_id = f"{counter:032x}"
                host_pid = 10_000 + counter
                process_start = f"fixture-process-start-{counter}"
                audio_tids = [90_001, 90_002]
                started_ns = counter * 1_000_000_000
                finished_ns = started_ns + round(duration[state] * 1_000_000)
                metrics_path = (
                    root / "metrics" / f"{state}-{section}-{sequence}.json"
                )
                write_json(metrics_path, {
                    "schema": overhead.METRICS_SCHEMA,
                    "version": 1,
                    "state": state,
                    "sequence": sequence,
                    "evidence_id": evidence_id,
                    "host_pid": host_pid,
                    "process_start_identity": process_start,
                    "executable_sha256": binary_sha256,
                    "audio_thread_tids": audio_tids,
                    "started_monotonic_ns": started_ns,
                    "finished_monotonic_ns": finished_ns,
                    "xrun_count": 0,
                })
                trace_ref = None
                if active:
                    trace = root / "traces" / f"{state}-{section}-{sequence}.pftrace"
                    write_json(trace, {"traceEvents": [
                        {
                            "name": "pulp_a3_trace_session",
                            "cat": "metadata",
                            "ph": "i",
                            "pid": host_pid,
                            "tid": host_pid,
                            "args": {
                                "gpu_evidence_id": evidence_id,
                                "process_start_identity": process_start,
                                "executable_sha256": binary_sha256,
                                "session_config_sha256": session_config_ref["sha256"],
                                "audio_thread_tids_sha256": (
                                    overhead.trace_replay.tids_digest(audio_tids)
                                ),
                                "ring_bytes": overhead.RING_BYTES,
                                "session_active": True,
                            },
                        },
                        {
                            "name": "gpu_health_transition_first_visible",
                            "cat": "gpu",
                            "ph": "X",
                            "pid": host_pid,
                            "tid": host_pid,
                            "ts": started_ns / 1000,
                            "dur": max(1, (finished_ns - started_ns) / 1000),
                            "args": {"gpu_evidence_id": evidence_id},
                        },
                    ]})
                    trace_ref = {
                        **overhead.artifact_ref(trace, root),
                        "bytes": trace.stat().st_size,
                        "format": "chrome-json",
                    }
                rows.append({
                    "sequence": sequence,
                    "evidence_id": evidence_id,
                    "duration_ms": duration[state],
                    "runtime_metrics": overhead.artifact_ref(metrics_path, root),
                    "trace": trace_ref,
                })
                counter += 1
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
            "campaign_role": campaign_role,
            "campaign_id": campaign_id,
            "build_family_id": build_family_id,
            "product_id": product_id,
            "product_name": product_name,
            "plugin_format": plugin_format,
            "binary_sha256": binary_sha256,
            "machine": copy.deepcopy(machine),
            "workload": copy.deepcopy(workload),
            "measurement_driver": copy.deepcopy(driver),
            "trace_session_config": copy.deepcopy(session_config_ref),
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


def rewrite_artifact(
    root: Path, ref: dict[str, Any], mutation: Callable[[dict[str, Any]], None],
) -> None:
    path = root / ref["path"]
    payload = json.loads(path.read_text(encoding="utf-8"))
    mutation(payload)
    write_json(path, payload)
    ref["sha256"] = overhead.sha256_file(path, "mutated fixture artifact")
    if "bytes" in ref:
        ref["bytes"] = path.stat().st_size


def duplicate_process(root: Path, documents: dict[str, dict[str, Any]]) -> None:
    rows = documents["candidate-active"]["fresh_process"]
    first = json.loads((root / rows[0]["runtime_metrics"]["path"]).read_text())

    def mutate(second: dict[str, Any]) -> None:
        second["host_pid"] = first["host_pid"]
        second["process_start_identity"] = first["process_start_identity"]

    rewrite_artifact(root, rows[1]["runtime_metrics"], mutate)


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
        temp_root = Path(temporary)
        paths, _ = make_fixture(temp_root / "positive")
        receipt = overhead.build_receipt(
            paths, evidence_root=temp_root / "positive",
            generated_utc="2026-08-29T12:00:00Z",
        )
        assert receipt["verdict"] == "pass"
        receipt_path = temp_root / "positive" / "receipt.json"
        write_json(receipt_path, receipt)
        overhead.validate_receipt(receipt, temp_root / "positive")

        def mutation_table(case_root: Path) -> list[
            Callable[[dict[str, dict[str, Any]], dict[str, Path]], None]
        ]:
            return [
                lambda docs, _: docs["candidate-active"]["measured"][0].__setitem__(
                    "duration_ms", 20.0,
                ),
                lambda docs, _: rewrite_artifact(
                    case_root,
                    docs["candidate-active"]["fresh_process"][0]["runtime_metrics"],
                    lambda value: value.__setitem__("xrun_count", 1),
                ),
                lambda docs, _: rewrite_artifact(
                    case_root, docs["candidate-active"]["measured"][0]["trace"],
                    lambda value: value["traceEvents"][1].__setitem__("tid", 90_001),
                ),
                lambda docs, _: docs["candidate-active"]["measured"][0]["trace"].__setitem__(
                    "sha256", "0" * 64,
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
                lambda docs, _: duplicate_process(case_root, docs),
                lambda docs, _: rewrite_artifact(
                    case_root, docs["candidate-active"]["measured"][1]["trace"],
                    lambda value: value["traceEvents"][1]["args"].__setitem__(
                        "gpu_evidence_id", "f" * 32,
                    ),
                ),
                lambda docs, _: rewrite_artifact(
                    case_root,
                    docs["candidate-active"]["measured"][2]["runtime_metrics"],
                    lambda value: value.__setitem__("executable_sha256", "a" * 64),
                ),
            ]

        for index in range(12):
            case_root = temp_root / f"negative-{index}"
            case_root.mkdir()
            expect_failure(case_root, mutation_table(case_root)[index], str(index))

        mutated_receipt = copy.deepcopy(receipt)
        mutated_receipt["comparisons"]["candidate-active"]["pass"] = False
        try:
            overhead.validate_receipt(mutated_receipt, temp_root / "positive")
        except overhead.OverheadError:
            pass
        else:
            raise AssertionError("derived receipt mutation did not fire")

        print(
            "gpu-first-visible-a3-trace-producer-overhead: "
            "positive=1 planted_negatives=13"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
