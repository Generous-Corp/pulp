#!/usr/bin/env python3
"""Fail-closed verifier for the private GPU-audio P5 unified receipt.

This is deliberately a standalone, opt-in validation tool.  It verifies the
lossless admission-to-terminal row join and provenance identity binding; it
does not claim that a receipt proves a physical provider or product support.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import math
from collections import Counter
from pathlib import Path
from typing import Any


SCHEMA = "pulp.gpu-audio.p5.unified-receipt.v1"
TERMINAL_DISPOSITIONS = {
    "completed",
    "cpu_fallback",
    "silence",
    "passthrough",
    "cancelled",
    "provider_failure",
    "device_lost",
    "dropped",
}
HEX = re.compile(r"^[0-9a-fA-F]+$")
GPU_TERMINALS = {"completed", "stale_rejected", "late_rejected", "provider_failure", "device_lost", "cancelled_teardown"}
OUTCOMES = {"success", "submission_rejected", "completion_failed", "deadline_exceeded", "stale_rejected", "late_rejected", "device_lost", "cancelled"}


class ReceiptError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ReceiptError(message)


def _sha256(path: Path) -> str:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError as exc:
        raise ReceiptError(f"cannot read {path}: {exc}") from exc


def _load_json(path: Path) -> Any:
    try:
        def reject(value: str) -> None:
            raise ValueError(f"non-finite JSON constant {value}")
        def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
            result: dict[str, Any] = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError(f"duplicate JSON key {key}")
                result[key] = value
            return result
        value = json.loads(path.read_text(encoding="utf-8"), parse_constant=reject,
                          object_pairs_hook=unique)
        def walk(item: Any) -> None:
            if isinstance(item, float) and not math.isfinite(item):
                raise ValueError("non-finite JSON number")
            if isinstance(item, dict):
                for key, child in item.items():
                    walk(key); walk(child)
            elif isinstance(item, list):
                for child in item: walk(child)
        walk(value)
        return value
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        raise ReceiptError(f"cannot parse {path}: {exc}") from exc


def verify_artifacts(raw_jsonl: Path, perfetto_json: Path, run_manifest: Path, *, expected_source_revision: str | None = None) -> dict[str, Any]:
    """Verify the actual P4 JSONL + flushed Perfetto query output + run manifest.

    The P4 validator remains the authority for its strict writer contract.  This
    layer binds those artifacts together and checks that the lossless Perfetto
    rows retain exactly one admission and terminal for every raw block.
    """
    try:
        scripts_dir = Path(__file__).resolve().parents[1] / "scripts"
        if str(scripts_dir) not in sys.path:
            sys.path.insert(0, str(scripts_dir))
        from gpu_audio_p4_evidence import read_jsonl, validate_records
    except ImportError as exc:  # pragma: no cover - only happens outside the tree
        raise ReceiptError(f"cannot load P4 validator: {exc}") from exc
    records, parse_errors = read_jsonl(raw_jsonl)
    errors = list(parse_errors) + list(validate_records(records))
    _require(not errors, "strict p4.raw.v1 validation failed: " + "; ".join(errors[:4]))
    _require(records and records[0].get("record_kind") == "manifest", "raw JSONL lacks manifest")
    raw_manifest = records[0]
    raw_blocks = [record for record in records if record.get("record_kind") == "block"]
    manifest = _load_json(run_manifest)
    _require(isinstance(manifest, dict), "run manifest must be an object")
    _require(manifest.get("schema") == "pulp.gpu-audio.p5.run-manifest.v1",
             "run manifest schema is invalid")
    required = ("source_revision", "binary_sha256", "provider_revision",
                "provider_asset_sha256", "model_sha256", "resident_plan_sha256", "upid", "session_id",
                "raw_sha256", "trace_sha256", "expected_admissions")
    _require(all(isinstance(manifest.get(key), str) and manifest[key] for key in required[:6]),
             "run manifest is missing required identity or artifact digest")
    for key in ("source_revision", "provider_revision"):
        _require(len(manifest[key]) == 40 and HEX.fullmatch(manifest[key]) is not None,
                 f"run manifest {key} is not a git revision")
    for key in ("binary_sha256", "provider_asset_sha256", "model_sha256", "resident_plan_sha256", "raw_sha256", "trace_sha256"):
        _require(len(manifest[key]) == 64 and HEX.fullmatch(manifest[key]) is not None,
                 f"run manifest {key} is not a SHA-256 digest")
    _require(type(manifest.get("expected_admissions")) is int and manifest["expected_admissions"] > 0,
             "run manifest expected_admissions must be positive")
    _require(type(manifest["upid"]) is int and manifest["upid"] >= 0 and isinstance(manifest["session_id"], str) and manifest["session_id"],
             "run manifest process/session identity is required")
    _require(_sha256(raw_jsonl) == manifest["raw_sha256"], "raw JSONL digest does not match run manifest")
    _require(_sha256(perfetto_json) == manifest["trace_sha256"], "Perfetto output digest does not match run manifest")
    _require(expected_source_revision is not None and manifest["source_revision"] == expected_source_revision,
             "source revision does not match caller-provided expected revision")
    resolved = [path.resolve() for path in (raw_jsonl, perfetto_json, run_manifest)]
    _require(len(set(resolved)) == 3, "artifact paths must be distinct and cannot alias")
    stats = [path.stat() for path in (raw_jsonl, perfetto_json, run_manifest)]
    _require(len({(stat.st_dev, stat.st_ino) for stat in stats}) == 3,
             "artifact paths must not hardlink the same inode")
    for key in ("source_revision", "binary_sha256", "provider_revision", "provider_asset_sha256"):
        _require(raw_manifest.get(key) == manifest[key], f"raw manifest {key} disagrees with run manifest")

    trace = _load_json(perfetto_json)
    _require(isinstance(trace, dict), "Perfetto SQL output must be an object")
    _require(trace.get("schema") == "pulp.gpu-audio.p5.perfetto-sql.v1",
             "Perfetto SQL output schema is invalid")
    _require(trace.get("flushed") is True, "Perfetto capture is not marked flushed")
    rows = trace.get("rows")
    _require(isinstance(rows, list) and rows, "Perfetto output must contain non-empty rows")
    admissions: set[tuple[int, int, int]] = set()
    terminals: set[tuple[int, int, int]] = set()
    last_sequence: dict[tuple[str, int, int], int] = {}
    admission_index: dict[tuple[int, int, int], int] = {}
    process_identity: dict[tuple[int, int, int], tuple[Any, Any]] = {}
    for index, row in enumerate(rows):
        _require(isinstance(row, dict), f"Perfetto row {index} is not an object")
        key = tuple(row.get(name) for name in ("engine_id", "generation", "sequence"))
        _require(all(isinstance(value, int) and not isinstance(value, bool) and value >= 0 for value in key),
                 f"Perfetto row {index} has invalid lifecycle identity")
        _require(isinstance(row.get("upid"), int) and not isinstance(row.get("upid"), bool)
                 and row["upid"] >= 0 and isinstance(row.get("session_id"), str)
                 and row["session_id"], f"Perfetto row {index} process/session identity is invalid")
        _require((row["upid"], row["session_id"]) == (manifest["upid"], manifest["session_id"]),
                 f"Perfetto row {index} process/session identity disagrees with manifest")
        process = (row["upid"], row["session_id"])
        _require(process_identity.setdefault(key, process) == process,
                 f"Perfetto row {index} process/session identity mismatch")
        event_fields = [row[name] for name in ("event", "lifecycle_event", "name") if name in row]
        _require(all(isinstance(field, str) for field in event_fields),
                 f"Perfetto row {index} event fields must be strings")
        _require(len(event_fields) == 1 or len(set(event_fields)) == 1,
                 f"Perfetto row {index} has conflicting event fields")
        event = event_fields[0] if event_fields else None
        _require(event in {"admission", "terminal", "gpu.audio.admission", "gpu.audio.terminal"},
                 f"Perfetto row {index} has invalid lifecycle event")
        _require(row.get("model_hash") == manifest["model_sha256"],
                 f"Perfetto row {index} has forged model_hash")
        _require(row.get("resident_plan_hash") == manifest["resident_plan_sha256"],
                 f"Perfetto row {index} has forged resident_plan_hash")
        _require(isinstance(row.get("provider_source_hash"), str) and row.get("provider_source_hash") in {
            manifest["provider_asset_sha256"]},
                 f"Perfetto row {index} has forged provider_source_hash")
        _require(row.get("provider_executable_hash") == manifest["binary_sha256"],
                 f"Perfetto row {index} has forged provider_executable_hash")
        target = admissions if "admission" in event else terminals
        _require(key not in target, f"Perfetto duplicate {event} row for {key}")
        order_key = ("admission" if "admission" in event else "terminal", key[0], key[1])
        _require(key[2] > last_sequence.get(order_key, -1),
                 f"Perfetto {event} rows are reordered for engine/generation {key[:2]}")
        last_sequence[order_key] = key[2]
        target.add(key)
        if "admission" in event:
            admission_index[key] = index
        else:
            _require(key in admission_index and index > admission_index[key],
                     f"Perfetto terminal precedes admission for {key}")
        if event.endswith("terminal"):
            _require(type(row.get("admission_ns")) is int and row["admission_ns"] >= 0,
                     f"Perfetto row {index} lacks admission timestamp")
            _require(type(row.get("terminal_ns")) is int and row["terminal_ns"] >= 0,
                     f"Perfetto row {index} lacks flushed terminal timing")
            _require(row["terminal_ns"] >= row["admission_ns"],
                     f"Perfetto row {index} terminal precedes admission timestamp")
            _require(isinstance(row.get("gpu_terminal"), str) and row.get("gpu_terminal") in GPU_TERMINALS,
                     f"Perfetto row {index} lacks gpu_terminal")
            _require(isinstance(row.get("outcome"), str) and row["outcome"] in OUTCOMES,
                     f"Perfetto row {index} lacks outcome")
            _require(isinstance(row.get("delivery"), str) and row["delivery"],
                     f"Perfetto row {index} lacks delivery")
            consistency = {"completed": {"success"}, "provider_failure": {"submission_rejected", "completion_failed"}, "device_lost": {"device_lost"},
                           "cancelled_teardown": {"cancelled"}, "stale_rejected": {"stale_rejected"},
                           "late_rejected": {"late_rejected"}}
            _require(row["gpu_terminal"] in consistency and row["outcome"] in consistency[row["gpu_terminal"]],
                     f"Perfetto outcome disagrees with gpu_terminal for row {index}")
            available = row.get("gpu_elapsed_available")
            _require(isinstance(available, bool),
                     f"Perfetto row {index} gpu_elapsed_available must be boolean")
            elapsed = row.get("gpu_elapsed_ns")
            _require((isinstance(elapsed, int) and not isinstance(elapsed, bool) and elapsed >= 0)
                     if available else elapsed is None,
                     f"Perfetto row {index} GPU timing availability/null mismatch")
    _require(admissions == terminals, "Perfetto admission-to-terminal join is incomplete")
    raw_keys = {(row.get("engine_id"), row.get("generation"), row.get("sequence"))
                for row in raw_blocks}
    raw_by_key = {(row.get("engine_id"), row.get("generation"), row.get("sequence")): row
                  for row in raw_blocks}
    _require(len(raw_blocks) == len(raw_keys), "raw P4 block identities are duplicated")
    _require(raw_keys == {(key[0], key[1], key[2]) for key in admissions},
             "Perfetto lifecycle identities do not exactly match raw P4 blocks")
    for key in terminals:
        terminal = next(row for row in rows if (row.get("engine_id"), row.get("generation"), row.get("sequence")) == key and (row.get("event") or row.get("lifecycle_event") or row.get("name", "")).endswith("terminal"))
        raw_row = raw_by_key[key]
        _require(terminal["gpu_terminal"] == raw_row.get("gpu_terminal") and terminal["delivery"] == raw_row.get("delivery"),
                 f"Perfetto terminal disposition disagrees with raw P4 block {key}")
        raw_gpu = raw_row.get("timings", {}).get("gpu_elapsed", {})
        raw_available = isinstance(raw_gpu, dict) and raw_gpu.get("availability") == "available"
        _require(terminal.get("gpu_elapsed_available") is raw_available,
                 f"Perfetto GPU timing availability disagrees with raw P4 block {key}")
        if raw_available:
            _require(type(raw_gpu.get("value_ns")) is int and terminal.get("gpu_elapsed_ns") == raw_gpu["value_ns"],
                     f"Perfetto GPU elapsed timing disagrees with raw P4 block {key}")
        else:
            _require(terminal.get("gpu_elapsed_ns") is None,
                     f"Perfetto unavailable GPU timing must be null for raw P4 block {key}")
    _require(len(admissions) == manifest["expected_admissions"] == len(raw_keys),
             "Perfetto lifecycle count disagrees with run manifest")
    return {"verdict": "PASS", "schema": SCHEMA, "admissions": len(admissions),
            "artifacts": {"raw_sha256": manifest["raw_sha256"], "trace_sha256": manifest["trace_sha256"]}}


def _identity(receipt: dict[str, Any]) -> dict[str, str]:
    value = receipt.get("identity")
    _require(isinstance(value, dict), "identity must be an object")
    required = (
        "source_revision",
        "provider_identity",
        "executable_sha256",
        "model_sha256",
        "resident_plan_sha256",
    )
    _require(all(isinstance(value.get(key), str) and value[key] for key in required),
             "identity is missing a required non-empty field")
    for key in ("executable_sha256", "model_sha256", "resident_plan_sha256"):
        _require(len(value[key]) == 64 and HEX.fullmatch(value[key]) is not None,
                 f"identity.{key} must be a 64-character hex digest")
    _require(len(value["source_revision"]) == 40 and HEX.fullmatch(value["source_revision"]) is not None,
             "identity.source_revision must be a 40-character hex revision")
    return {key: value[key] for key in required}


def verify_receipt(receipt: Any) -> dict[str, Any]:
    _require(isinstance(receipt, dict), "receipt must be an object")
    _require(receipt.get("schema") == SCHEMA, f"schema must be {SCHEMA}")
    expected = receipt.get("expected_admissions")
    _require(type(expected) is int and expected > 0, "expected_admissions must be positive")
    identity = _identity(receipt)
    rows = receipt.get("rows")
    _require(isinstance(rows, list), "rows must be an array")
    _require(len(rows) == expected * 2, "rows must contain exactly one admission and terminal per admission")

    admissions: dict[str, dict[str, Any]] = {}
    terminals: dict[str, dict[str, Any]] = {}
    seen_order: list[tuple[str, str]] = []
    for index, row in enumerate(rows):
        _require(isinstance(row, dict), f"rows[{index}] must be an object")
        admission_id = row.get("admission_id")
        phase = row.get("phase")
        _require(isinstance(admission_id, str) and admission_id, f"rows[{index}] has no admission_id")
        _require(phase in {"admitted", "terminal"}, f"rows[{index}] has invalid phase")
        _require(row.get("identity") == identity, f"rows[{index}] provenance identity mismatch")
        sequence = row.get("sequence")
        _require(type(sequence) is int and sequence > 0, f"rows[{index}] sequence must be positive")
        timing = row.get("timing")
        _require(isinstance(timing, dict), f"rows[{index}] timing must be an object")
        for key in ("admission_ns", "terminal_ns", "duration_ns"):
            _require(type(timing.get(key)) is int and timing[key] >= 0,
                     f"rows[{index}] timing.{key} must be non-negative")
        _require(timing["terminal_ns"] >= timing["admission_ns"],
                 f"rows[{index}] terminal precedes admission")
        _require(timing["duration_ns"] == timing["terminal_ns"] - timing["admission_ns"],
                 f"rows[{index}] duration does not match timestamps")
        target = admissions if phase == "admitted" else terminals
        _require(admission_id not in target, f"duplicate {phase} row for {admission_id}")
        if phase == "terminal":
            disposition = row.get("disposition")
            _require(disposition in TERMINAL_DISPOSITIONS,
                     f"rows[{index}] has invalid terminal disposition")
        target[admission_id] = row
        seen_order.append((admission_id, phase))

    _require(len(admissions) == expected and len(terminals) == expected,
             "admission and terminal row counts do not match expected_admissions")
    _require(set(admissions) == set(terminals), "admission-to-terminal join is incomplete")
    sequences = sorted(row["sequence"] for row in admissions.values())
    _require(sequences == list(range(1, expected + 1)), "admission sequences are not contiguous")
    for admission_id, admission in admissions.items():
        terminal = terminals[admission_id]
        _require(terminal["sequence"] == admission["sequence"],
                 f"terminal sequence mismatch for {admission_id}")
        _require(seen_order.index((admission_id, "admitted")) < seen_order.index((admission_id, "terminal")),
                 f"terminal row precedes admission for {admission_id}")

    census = receipt.get("disposition_census")
    _require(isinstance(census, dict), "disposition_census must be an object")
    observed = Counter(row["disposition"] for row in terminals.values())
    normalized = {key: value for key, value in census.items() if value}
    _require(all(type(value) is int and value >= 0 for value in census.values()),
             "disposition_census values must be non-negative integers")
    _require(dict(observed) == normalized, "disposition census does not match terminal rows")
    return {"verdict": "PASS", "schema": SCHEMA, "admissions": expected,
            "disposition_census": dict(sorted(observed.items()))}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", type=Path, nargs="?")
    parser.add_argument("--raw-jsonl", type=Path)
    parser.add_argument("--perfetto-json", type=Path)
    parser.add_argument("--run-manifest", type=Path)
    parser.add_argument("--expected-source-revision")
    args = parser.parse_args(argv)
    try:
        if args.raw_jsonl or args.perfetto_json or args.run_manifest:
            _require(args.raw_jsonl and args.perfetto_json and args.run_manifest,
                     "--raw-jsonl, --perfetto-json, and --run-manifest are required together")
            result = verify_artifacts(args.raw_jsonl, args.perfetto_json, args.run_manifest,
                                      expected_source_revision=args.expected_source_revision)
        else:
            _require(args.receipt is not None, "receipt path is required")
            result = verify_receipt(json.loads(args.receipt.read_text(encoding="utf-8")))
    except (OSError, json.JSONDecodeError, ReceiptError) as exc:
        print(json.dumps({"verdict": "FAIL", "reason": str(exc)}, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
