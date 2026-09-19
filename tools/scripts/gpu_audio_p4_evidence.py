#!/usr/bin/env python3
"""Validate and summarize raw P4 GPU-audio campaign JSONL.

The file is deliberately streaming-friendly: one manifest, followed by complete
trial_begin/block/trial_end groups.  A truncated run is invalid rather than a
smaller-looking successful sample.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import random
import sys
import tempfile
from collections.abc import Sequence
from typing import Any, Callable, Iterable, TextIO

from gpu_audio_p4_evidence_timing import (
    Diagnostics, OBSERVATION_FIELDS, ProvenanceTracker,
    check_timing as _check_timing, finite_number as _finite_number,
    provenance as _provenance, verdict_observation as _verdict_observation,
)

from gpu_audio_p4_evidence_storage import (
    IdentityStore as _IdentityStore,
    MetricStore as _MetricStore,
    RecordStore as _RecordStore,
    sha256_file as _sha256_file,
)


SCHEMA = "pulp.gpu-audio.p4.raw.v1"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
OPTIMIZATION_FLAG_RE = re.compile(r"^-O(?:[0-9]+|fast|g|s|z)?$")
PATHS = {"staged_sync", "staged_async", "shared_async"}
CAMPAIGNS = {"screening", "confirmation", "default", "overload"}
GPU_TERMINALS = {
    "completed",
    "stale_rejected",
    "late_rejected",
    "provider_failure",
    "device_lost",
    "cancelled_teardown",
}
DELIVERIES = {"gpu", "cpu_fallback", "silence", "passthrough", "priming"}
TIMINGS = (
    "callback_cpu",
    "worker_pack_copy",
    "encode_cpu",
    "submit_cpu",
    "event_processing_cpu",
    "retirement_cpu",
    "worker_other_cpu",
    "worker_end_to_end",
    "submit_to_completion",
    "gpu_elapsed",
    "completion_to_retirement_observed",
    "retirement_to_result_visible",
    "publish_to_consumable",
)
CPU_TIMINGS = ("callback_cpu", "worker_pack_copy", "encode_cpu", "submit_cpu",
               "event_processing_cpu", "retirement_cpu", "worker_other_cpu")
VERDICT_TIMINGS = CPU_TIMINGS + ("worker_end_to_end", "submit_to_completion",
                                "publish_to_consumable")
TRANSFER_COUNTERS = (
    "write_buffer_calls",
    "write_buffer_bytes",
    "output_copy_calls",
    "output_copy_bytes",
    "map_async_calls",
    "mapped_readback_memcpy_calls",
    "mapped_readback_memcpy_bytes",
)
UINT64_MAX = (1 << 64) - 1


def _nonempty(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _integer_at_least(value: Any, minimum: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= minimum


def _uint64_integer(value: Any) -> bool:
    return _integer_at_least(value, 0) and value <= UINT64_MAX


def _effective_release_flags(flags: Any) -> bool:
    """Interpret ordered Clang/GCC optimization and NDEBUG flag overrides."""
    if not isinstance(flags, list) or not flags or not all(_nonempty(flag) for flag in flags):
        return False
    optimization: str | None = None
    ndebug_defined: bool | None = None
    for index, flag in enumerate(flags):
        if OPTIMIZATION_FLAG_RE.fullmatch(flag):
            optimization = flag

        action: bool | None = None
        macro = ""
        if flag in {"-D", "-U"} and index + 1 < len(flags):
            action = flag == "-D"
            macro = flags[index + 1]
        elif flag.startswith("-D") and len(flag) > 2:
            action = True
            macro = flag[2:]
        elif flag.startswith("-U") and len(flag) > 2:
            action = False
            macro = flag[2:]
        if action is not None and macro.split("=", 1)[0] == "NDEBUG":
            ndebug_defined = action
    return optimization == "-O3" and ndebug_defined is True


def _canonical(record: dict[str, Any]) -> bytes:
    return json.dumps(
        record, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
        allow_nan=False,
    ).encode()


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate object key {key!r}")
        value[key] = item
    return value


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON constant {value!r}")


def _strict_json_loads(line: str) -> Any:
    value = json.loads(
        line,
        object_pairs_hook=_unique_json_object,
        parse_constant=_reject_json_constant,
    )
    stack = [value]
    while stack:
        item = stack.pop()
        if isinstance(item, str):
            if any(0xD800 <= ord(character) <= 0xDFFF for character in item):
                raise ValueError("unpaired Unicode surrogate in JSON string")
        elif isinstance(item, float) and not math.isfinite(item):
            raise ValueError("non-finite JSON number")
        elif isinstance(item, dict):
            stack.extend(item.keys())
            stack.extend(item.values())
        elif isinstance(item, list):
            stack.extend(item)
    return value


def _inspect_distinct_paths(paths: list[tuple[str, Path]]) -> str | None:
    resolved: dict[Path, str] = {}
    identities: dict[tuple[int, int], str] = {}
    for label, path in paths:
        try:
            canonical = path.resolve()
        except (OSError, RuntimeError) as exc:
            return f"cannot resolve {label} {path}: {exc}"
        if canonical in resolved:
            return f"{label} aliases {resolved[canonical]}"
        resolved[canonical] = label
        try:
            stat = path.stat()
        except FileNotFoundError:
            continue
        except OSError as exc:
            return f"cannot inspect {label} {path}: {exc}"
        identity = (stat.st_dev, stat.st_ino)
        if identity in identities:
            return f"{label} aliases {identities[identity]}"
        identities[identity] = label
    return None


def _distinct_paths(paths: list[tuple[str, Path]], *,
                    probe_absent: Iterable[Path] = ()) -> str | None:
    """Reject aliases, probing absent outputs through the destination filesystem."""
    labels = {path: label for label, path in paths}
    created: list[tuple[Path, tuple[int, int]]] = []
    error: str | None = None
    for path in probe_absent:
        if os.path.lexists(path):
            continue
        try:
            descriptor = os.open(
                path,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0),
                0o600,
            )
        except FileExistsError:
            # An earlier probe may have created this same filesystem entry under
            # a case-folded or normalization-equivalent spelling.
            continue
        except OSError as exc:
            error = f"cannot probe {labels.get(path, 'output')} {path}: {exc}"
            break
        try:
            stat = os.fstat(descriptor)
        finally:
            os.close(descriptor)
        created.append((path, (stat.st_dev, stat.st_ino)))

    if error is None:
        error = _inspect_distinct_paths(paths)

    cleanup_errors: list[str] = []
    for path, identity in reversed(created):
        try:
            stat = path.lstat()
            if (stat.st_dev, stat.st_ino) != identity:
                cleanup_errors.append(f"probe path changed while checking {path}")
                continue
            path.unlink()
        except FileNotFoundError:
            continue
        except OSError as exc:
            cleanup_errors.append(f"cannot remove path probe {path}: {exc}")
    if cleanup_errors:
        return "; ".join(cleanup_errors)
    return error


def _stage_output(path: Path, writer: Callable[[TextIO], None], *,
                  newline: str | None = None) -> Path:
    """Write and sync a sibling temporary file without touching the destination."""
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp",
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline=newline) as handle:
            writer(handle)
            handle.flush()
            os.fsync(handle.fileno())
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        temporary.unlink(missing_ok=True)
        raise
    return temporary


def _publish_staged_outputs(staged: list[tuple[Path, Path]]) -> None:
    """Publish every staged output, restoring the complete old set on failure."""
    backups: list[tuple[Path, Path | None]] = []
    published: set[Path] = set()
    try:
        for _, destination in staged:
            backup: Path | None = None
            if os.path.lexists(destination):
                descriptor, backup_name = tempfile.mkstemp(
                    dir=destination.parent,
                    prefix=f".{destination.name}.",
                    suffix=".rollback",
                )
                os.close(descriptor)
                backup = Path(backup_name)
                try:
                    os.replace(destination, backup)
                except BaseException:
                    backup.unlink(missing_ok=True)
                    raise
            backups.append((destination, backup))

        for temporary, destination in staged:
            os.replace(temporary, destination)
            published.add(destination)
    except BaseException as publish_error:
        rollback_errors: list[str] = []
        for destination, backup in reversed(backups):
            try:
                if destination in published:
                    destination.unlink(missing_ok=True)
                if backup is not None:
                    os.replace(backup, destination)
            except OSError as exc:
                rollback_errors.append(f"{destination}: {exc}")
        if rollback_errors:
            raise OSError(
                f"{publish_error}; output rollback also failed: "
                + "; ".join(rollback_errors)
            ) from publish_error
        raise
    else:
        # Publication has committed. A stale backup is safer than reporting a
        # failure after the requested outputs have already changed.
        for _, backup in backups:
            if backup is not None:
                try:
                    backup.unlink(missing_ok=True)
                except OSError:
                    pass
    finally:
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)


def _percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * pct / 100.0
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    weight = position - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def _bootstrap_mean_ci(values: list[float], seed: int, resamples: int) -> dict[str, float | int | None]:
    if not values:
        return {"pairs": 0, "mean": None, "ci95_low": None, "ci95_high": None}
    rng = random.Random(seed)
    means = []
    count = len(values)
    for _ in range(resamples):
        means.append(sum(values[rng.randrange(count)] for _ in range(count)) / count)
    return {
        "pairs": count,
        "mean": sum(values) / count,
        "ci95_low": _percentile(means, 2.5),
        "ci95_high": _percentile(means, 97.5),
    }


def _improvement_percent(staged: float | None, shared: float | None) -> float | None:
    if staged is None or shared is None or staged <= 0:
        return None
    return (staged - shared) * 100.0 / staged


def _row_gate(summary: dict[str, Any], expected_pairs: int) -> dict[str, Any]:
    """Evaluate the gates one evidence row can prove; never infer a program verdict."""
    paths = summary["paths"]
    staged = paths.get("staged_async")
    shared = paths.get("shared_async")
    confidence = summary["matched_improvement"]
    checks: dict[str, dict[str, Any]] = {}

    def add(name: str, passed: bool | None, evidence: Any) -> None:
        checks[name] = {"passed": passed, "evidence": evidence}

    if not staged or not shared:
        add("paired_paths_present", False, sorted(paths))
        return {"status": "incomplete", "checks": checks,
                "program_verdict": "unassigned"}

    shared_transfers = sum(shared["transfers"].values())
    staged_transfers = sum(staged["transfers"].values())
    add("payload_transfer_elimination", shared_transfers == 0 and staged_transfers > 0,
        {"shared_named_transfers": shared_transfers,
         "staged_named_transfers": staged_transfers})

    cpu = confidence["total_cpu_per_block_percent"]
    latency = confidence["submit_to_completion_p99_percent"]
    latency_ns = confidence["submit_to_completion_p99_ns"]
    pair_complete = all(metric["pairs"] == expected_pairs
                        for metric in (cpu, latency, latency_ns))
    add("complete_metric_pairs", pair_complete,
        {"expected": expected_pairs,
         "observed": {"cpu": cpu["pairs"], "latency": latency["pairs"],
                      "latency_absolute": latency_ns["pairs"]}})
    cpu_wins = pair_complete and cpu["ci95_low"] is not None and cpu["ci95_low"] >= 20.0
    latency_wins = (pair_complete and latency["ci95_low"] is not None
                    and latency["ci95_low"] >= 20.0
                    and latency_ns["ci95_low"] is not None
                    and latency_ns["ci95_low"] >= 25_000.0)
    nonwinner_ok = ((cpu_wins and latency["ci95_low"] is not None
                     and latency["ci95_low"] >= -5.0)
                    or (latency_wins and cpu["ci95_low"] is not None
                        and cpu["ci95_low"] >= -5.0))
    add("useful_performance", cpu_wins or latency_wins,
        {"cpu_wins": cpu_wins, "latency_wins": latency_wins,
         "cpu": cpu, "latency": latency, "latency_absolute_ns": latency_ns})
    add("nonwinning_metric_regression", nonwinner_ok,
        {"required_ci95_low_percent": -5.0})

    normal = summary["campaign"] in {"confirmation", "default"}
    normal_health = (
        all(path[name] == 0 for path in (staged, shared)
            for name in ("deadline_misses", "watchdog_expiries", "device_losses", "audio_xruns"))
        and shared["driver_stalls"] <= staged["driver_stalls"]
    )
    add("normal_load_health", normal_health if normal else None,
        {"campaign": summary["campaign"],
         "shared": {name: shared[name] for name in
                    ("deadline_misses", "watchdog_expiries", "device_losses",
                     "audio_xruns", "driver_stalls")},
         "staged": {name: staged[name] for name in
                    ("deadline_misses", "watchdog_expiries", "device_losses",
                     "audio_xruns", "driver_stalls")}})

    failure_fields = ("deadline_misses", "watchdog_expiries", "late_completions",
                      "resync_drops")
    fallback_kinds = ("cpu_fallback", "silence", "passthrough")
    failure_ok = all(shared[name] <= staged[name] for name in failure_fields)
    fallback_ok = sum(shared["deliveries"][name] for name in fallback_kinds) <= sum(
        staged["deliveries"][name] for name in fallback_kinds)
    add("failure_counts_nonincreasing", failure_ok and fallback_ok,
        {"shared": {name: shared[name] for name in failure_fields},
         "staged": {name: staged[name] for name in failure_fields},
         "shared_fallback_deliveries": sum(shared["deliveries"][name]
                                             for name in fallback_kinds),
         "staged_fallback_deliveries": sum(staged["deliveries"][name]
                                             for name in fallback_kinds)})

    staged_callback = staged["timings_ns"]["callback_cpu"]["p99"]
    shared_callback = shared["timings_ns"]["callback_cpu"]["p99"]
    callback_allowance = (max(staged_callback * 0.05, 2_000.0)
                          if staged_callback is not None else None)
    callback_ok = (staged_callback is not None and shared_callback is not None
                   and shared_callback - staged_callback <= callback_allowance)
    add("callback_p99", callback_ok,
        {"staged_ns": staged_callback, "shared_ns": shared_callback,
         "allowance_ns": callback_allowance})

    ui_pairs = summary["matched_ui_frame_p99"]
    ui_ok = (ui_pairs["pairs"] == expected_pairs
             and ui_pairs["max_regression_percent"] is not None
             and ui_pairs["max_regression_percent"] <= 10.0)
    add("ui_frame_p99", ui_ok,
        {"matched_trials": ui_pairs, "expected_pairs": expected_pairs,
         "maximum_regression_percent_per_pair": 10.0})

    default_gate = None
    if summary["campaign"] == "default":
        default_gate = (shared["blocks"] >= 100_000
                        and shared["deadline_misses"] == 0
                        and shared["watchdog_expiries"] == 0)
    add("default_long_tail", default_gate,
        {"campaign": summary["campaign"], "shared_blocks": shared["blocks"],
         "minimum_blocks": 100_000})

    applicable = [item["passed"] for item in checks.values()
                  if item["passed"] is not None]
    status = "pass" if applicable and all(applicable) else "fail"
    return {"status": status, "checks": checks,
            "program_verdict": "unassigned"}


def read_jsonl(path: Path) -> tuple[Sequence[dict[str, Any]], list[str]]:
    records = _RecordStore()
    errors: list[str] = Diagnostics()
    try:
        handle = path.open("rb")
    except OSError as exc:
        records.close()
        return [], [f"cannot read {path}: {exc}"]
    line_count = 0
    with handle:
        for line_number, encoded_line in enumerate(handle, 1):
            line_count = line_number
            try:
                line = encoded_line.decode("utf-8")
            except UnicodeDecodeError as exc:
                errors.append(
                    f"line {line_number}: invalid UTF-8 at byte {exc.start}: {exc.reason}"
                )
                continue
            if not line.strip():
                errors.append(f"line {line_number}: blank lines are not allowed")
                continue
            try:
                value = _strict_json_loads(line)
            except (json.JSONDecodeError, ValueError, RecursionError) as exc:
                detail = exc.msg if isinstance(exc, json.JSONDecodeError) else str(exc)
                errors.append(f"line {line_number}: invalid JSON: {detail}")
                continue
            if not isinstance(value, dict):
                errors.append(f"line {line_number}: record must be an object")
                continue
            value["__line__"] = line_number
            records.append(value)
    records.finish()
    if line_count == 0:
        errors.append("evidence file is empty")
    return records, errors


def validate_binary(manifest: dict[str, Any], binary: Path) -> list[str]:
    try:
        actual = _sha256_file(binary)
    except OSError as exc:
        return [f"cannot hash benchmark binary {binary}: {exc}"]
    if actual != manifest.get("binary_sha256"):
        return ["manifest binary_sha256 does not match the supplied benchmark binary"]
    return []


def validate_records(records: Sequence[dict[str, Any]]) -> list[str]:
    errors: list[str] = Diagnostics()
    if not records:
        return ["no records"]
    manifest = records[0]
    if manifest.get("record_kind") != "manifest":
        errors.append("line 1: first record must be manifest")
        return errors
    if manifest.get("schema") != SCHEMA:
        errors.append(f"line 1: schema must be {SCHEMA}")
    campaign = manifest.get("campaign")
    if not isinstance(campaign, str) or campaign not in CAMPAIGNS:
        errors.append("line 1: campaign is invalid")
    revision = manifest.get("source_revision")
    if not isinstance(revision, str) or not SHA_RE.fullmatch(revision):
        errors.append("line 1: source_revision must be a full git SHA")
    binary = manifest.get("binary_sha256")
    if not isinstance(binary, str) or not SHA256_RE.fullmatch(binary):
        errors.append("line 1: binary_sha256 must be SHA-256")
    for field in ("campaign_id", "machine_id", "machine_model", "os_version", "adapter_name",
                  "adapter_backend", "provider", "generated_utc"):
        if not _nonempty(manifest.get(field)):
            errors.append(f"line 1: {field} must be non-empty")
    if not isinstance(manifest.get("provider_revision"), str) or not SHA_RE.fullmatch(
        manifest["provider_revision"]
    ):
        errors.append("line 1: provider_revision must be a full git SHA")
    if not isinstance(manifest.get("provider_asset_sha256"), str) or not SHA256_RE.fullmatch(
        manifest["provider_asset_sha256"]
    ):
        errors.append("line 1: provider_asset_sha256 must be SHA-256")
    for field in ("adapter_registry_id", "adapter_vendor_id", "adapter_device_id"):
        if (not isinstance(manifest.get(field), int) or isinstance(manifest.get(field), bool)
                or manifest[field] < 0 or manifest[field] > UINT64_MAX):
            errors.append(f"line 1: {field} must be a non-negative integer")
    if manifest.get("build_type") != "Release":
        errors.append("line 1: build_type must be Release")
    flags = manifest.get("build_flags")
    if not _effective_release_flags(flags):
        errors.append(
            "line 1: build_flags must be non-empty strings with effective -O3 and NDEBUG defined"
        )
    if manifest.get("paced") is not True:
        errors.append("line 1: paced must be true")
    if not _integer_at_least(manifest.get("warmup_blocks"), 1):
        errors.append("line 1: warmup_blocks must be a positive integer")

    expected_trials = manifest.get("expected_trials")
    expected_pairs = manifest.get("expected_matched_pairs")
    expected_sync_trials = manifest.get("expected_staged_sync_trials")
    expected_blocks = manifest.get("expected_blocks_per_trial")
    row = manifest.get("row")
    if not _integer_at_least(expected_trials, 2):
        errors.append("line 1: expected_trials must be at least 2")
    if not _integer_at_least(expected_blocks, 1):
        errors.append("line 1: expected_blocks_per_trial must be positive")
    if not _integer_at_least(expected_pairs, 1):
        errors.append("line 1: expected_matched_pairs must be positive")
    if not _integer_at_least(expected_sync_trials, 0):
        errors.append("line 1: expected_staged_sync_trials must be non-negative")
    if not _integer_at_least(manifest.get("bootstrap_seed"), 0):
        errors.append("line 1: bootstrap_seed must be a non-negative integer")
    resamples = manifest.get("bootstrap_resamples")
    if not _integer_at_least(resamples, 100):
        errors.append("line 1: bootstrap_resamples must be at least 100")
    if (isinstance(expected_trials, int) and isinstance(expected_pairs, int)
            and isinstance(expected_sync_trials, int)
            and expected_trials != expected_pairs * 2 + expected_sync_trials):
        errors.append("line 1: expected_trials must exactly cover matched pairs and staged_sync trials")
    if campaign == "confirmation" and isinstance(expected_pairs, int) and expected_pairs < 30:
        errors.append("line 1: confirmation campaigns require at least 30 matched pairs")
    if campaign in ("screening", "confirmation") and expected_sync_trials != 1:
        errors.append("line 1: screening/confirmation campaigns require exactly one staged_sync reference trial")
    if campaign == "confirmation" and isinstance(row, dict) and row.get("load") == "overload":
        errors.append("line 1: overloaded rows must use campaign=overload")
    if isinstance(row, dict) and (campaign == "overload") != (row.get("load") == "overload"):
        errors.append("line 1: campaign=overload and row.load=overload must agree")
    if campaign == "confirmation" and isinstance(resamples, int) and resamples < 10_000:
        errors.append("line 1: confirmation campaigns require at least 10000 bootstrap resamples")
    if campaign == "default" and isinstance(expected_blocks, int) and expected_blocks < 100_000:
        errors.append("line 1: default campaigns require at least 100000 blocks per trial")
    if (campaign == "default" and isinstance(row, dict)
            and row.get("load") not in ("graphite_ui", "gpu_contention")):
        errors.append("line 1: default campaigns require concurrent UI/GPU load")
    if campaign == "default":
        if not _nonempty(manifest.get("confirmation_campaign_id")):
            errors.append("line 1: default campaigns require confirmation_campaign_id")
        confirmation_digest = manifest.get("confirmation_summary_sha256")
        if not isinstance(confirmation_digest, str) or not SHA256_RE.fullmatch(
            confirmation_digest
        ):
            errors.append("line 1: default campaigns require confirmation_summary_sha256")

    if not isinstance(row, dict):
        errors.append("line 1: row must be an object")
    else:
        for field in ("block_frames", "sample_rate_hz", "channels", "ir_frames", "inflight_depth",
                      "lead_blocks", "deadline_ns", "watchdog_ns"):
            if not _integer_at_least(row.get(field), 1):
                errors.append(f"line 1: row.{field} must be a positive integer")
        if not isinstance(row.get("load"), str) or row.get("load") not in {
            "quiet", "graphite_ui", "gpu_contention", "overload"
        }:
            errors.append("line 1: row.load is invalid")
        if (isinstance(row.get("deadline_ns"), int) and isinstance(row.get("watchdog_ns"), int)
                and row["watchdog_ns"] <= row["deadline_ns"]):
            errors.append("line 1: row.watchdog_ns must be greater than row.deadline_ns")

    current: dict[str, Any] | None = None
    completed: list[dict[str, Any]] = []
    seen_trial_ids: set[int] = set()
    identities = _IdentityStore()
    pair_paths: dict[int, list[str]] = {}
    previous_async_path: str | None = None
    overload_failure_observed = False
    timing_provenance = ProvenanceTracker()
    for record_index, record in enumerate(records):
        if record_index == 0:
            continue
        line = record.get("__line__", "?")
        kind = record.get("record_kind")
        if record.get("schema") != SCHEMA:
            errors.append(f"line {line}: schema mismatch")
        if kind == "trial_begin":
            if current is not None:
                errors.append(f"line {line}: trial begins before prior trial_end")
                continue
            trial_id = record.get("trial_id")
            path = record.get("path")
            pair_id = record.get("pair_id")
            trial_id_valid = _uint64_integer(trial_id)
            if not trial_id_valid or trial_id in seen_trial_ids:
                errors.append(f"line {line}: trial_id must be a unique uint64 integer")
            path_valid = isinstance(path, str) and path in PATHS
            if not path_valid:
                errors.append(f"line {line}: path is invalid")
            if path_valid and path in {"staged_async", "shared_async"}:
                if not _uint64_integer(pair_id):
                    errors.append(f"line {line}: async trial pair_id must be a uint64 integer")
                else:
                    pair_paths.setdefault(pair_id, []).append(path)
                if previous_async_path == path:
                    errors.append(f"line {line}: matched trials must alternate async paths")
                previous_async_path = path
            elif path_valid and pair_id is not None:
                errors.append(f"line {line}: staged_sync pair_id must be null")
            current = {"id": trial_id, "path": path, "pair_id": pair_id,
                       "block_count": 0, "digest": hashlib.sha256(),
                       "first_sequence": None, "submit_samples": 0,
                       "cpu_samples": 0, "submit_positive": False,
                       "cpu_positive": False, "engine_generation": None,
                       "terminals": {name: 0 for name in GPU_TERMINALS},
                       "deliveries": {name: 0 for name in DELIVERIES}}
            if trial_id_valid:
                seen_trial_ids.add(trial_id)
        elif kind == "block":
            if current is None:
                errors.append(f"line {line}: block outside a trial")
                continue
            if (type(record.get("trial_id")) is not type(current["id"])
                    or type(record.get("pair_id")) is not type(current["pair_id"])
                    or record.get("trial_id") != current["id"]
                    or record.get("path") != current["path"]
                    or record.get("pair_id") != current["pair_id"]):
                errors.append(f"line {line}: block identity does not match trial")
            ordinal = record.get("block_ordinal")
            if not _integer_at_least(ordinal, 0) or ordinal != current["block_count"]:
                errors.append(f"line {line}: block_ordinal must be contiguous from zero")
            for field in ("engine_id", "generation", "sequence"):
                if (not isinstance(record.get(field), int) or isinstance(record.get(field), bool)
                        or record[field] < 0 or record[field] > UINT64_MAX):
                    errors.append(f"line {line}: {field} must be a uint64 integer")
            if all(isinstance(record.get(field), int) and record[field] >= 0
                   and not isinstance(record.get(field), bool) and record[field] <= UINT64_MAX
                   for field in ("engine_id", "generation", "sequence")):
                identity = (record["engine_id"], record["generation"], record["sequence"])
                if current["engine_generation"] is None:
                    current["engine_generation"] = identity[:2]
                elif current["engine_generation"] != identity[:2]:
                    errors.append(f"line {line}: engine_id and generation must be constant within a trial")
                if not identities.add(identity):
                    errors.append(f"line {line}: duplicate block identity {identity}")
                if current["first_sequence"] is None:
                    current["first_sequence"] = record["sequence"]
                expected_sequence = current["first_sequence"] + current["block_count"]
                if record["sequence"] != expected_sequence:
                    errors.append(f"line {line}: sequence must advance by one within a trial")
            where = f"line {line}.timings"
            timings = record.get("timings")
            if not isinstance(timings, dict):
                errors.append(f"line {line}: timings must be an object")
            else:
                for name in TIMINGS:
                    _check_timing(name, timings.get(name), where, errors)
                    if isinstance(current["path"], str) and current["path"] in PATHS:
                        timing_provenance.check(current["path"], name, timings.get(name), where, errors)
            transfers = record.get("transfers")
            if not isinstance(transfers, dict):
                errors.append(f"line {line}: transfers must be an object")
            else:
                unknown = set(transfers) - set(TRANSFER_COUNTERS)
                missing = set(TRANSFER_COUNTERS) - set(transfers)
                if unknown or missing:
                    errors.append(f"line {line}: transfers must contain exactly the declared counter keys")
                for name in TRANSFER_COUNTERS:
                    value = transfers.get(name)
                    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                        errors.append(f"line {line}: transfers.{name} must be a non-negative integer")
                if current["path"] == "shared_async" and any(transfers.get(name, 0) != 0 for name in TRANSFER_COUNTERS):
                    errors.append(f"line {line}: shared_async contains a named payload transfer")
            if (not isinstance(record.get("gpu_terminal"), str)
                    or record.get("gpu_terminal") not in GPU_TERMINALS):
                errors.append(f"line {line}: gpu_terminal is invalid")
            if (not isinstance(record.get("delivery"), str)
                    or record.get("delivery") not in DELIVERIES):
                errors.append(f"line {line}: delivery is invalid")
            for field, bucket in (("gpu_terminal", "terminals"), ("delivery", "deliveries")):
                value = record.get(field)
                if isinstance(value, str) and value in current[bucket]:
                    current[bucket][value] += 1
            for field in ("deadline_miss", "watchdog_expiry", "late_completion", "resync_drop"):
                if not isinstance(record.get(field), bool):
                    errors.append(f"line {line}: {field} must be boolean")
            if record.get("delivery") == "gpu" and record.get("gpu_terminal") != "completed":
                errors.append(f"line {line}: GPU delivery requires a completed GPU terminal")
            if record.get("delivery") == "gpu" and any(
                record.get(name) is True for name in ("deadline_miss", "watchdog_expiry", "resync_drop")
            ):
                errors.append(f"line {line}: missed, expired, or resync-dropped block cannot be delivered by GPU")
            if ((record.get("gpu_terminal") == "late_rejected")
                    != (record.get("late_completion") is True)):
                errors.append(f"line {line}: late_completion must agree with late_rejected terminal")
            if isinstance(timings, dict) and isinstance(row, dict):
                publish = timings.get("publish_to_consumable")
                worker = timings.get("worker_end_to_end")
                if (isinstance(publish, dict) and publish.get("availability") == "available"
                        and _finite_number(publish.get("value_ns"))
                        and _integer_at_least(row.get("deadline_ns"), 1)):
                    derived = publish.get("value_ns", 0) >= row.get("deadline_ns", 0)
                    if record.get("deadline_miss") is not derived:
                        errors.append(f"line {line}: deadline_miss disagrees with publish_to_consumable")
                if (isinstance(worker, dict) and worker.get("availability") == "available"
                        and _finite_number(worker.get("value_ns"))
                        and _integer_at_least(row.get("watchdog_ns"), 1)):
                    derived = worker.get("value_ns", 0) >= row.get("watchdog_ns", 0)
                    if record.get("watchdog_expiry") is not derived:
                        errors.append(f"line {line}: watchdog_expiry disagrees with worker_end_to_end")
            if isinstance(timings, dict):
                submit = timings.get("submit_to_completion", {})
                if (_verdict_observation(submit)
                        and _finite_number(submit.get("value_ns"))):
                    current["submit_samples"] += 1
                    current["submit_positive"] |= submit["value_ns"] > 0
                cpu = [timings.get(name, {}) for name in CPU_TIMINGS]
                if all(_verdict_observation(item)
                       and _finite_number(item.get("value_ns")) for item in cpu):
                    current["cpu_samples"] += 1
                    current["cpu_positive"] |= sum(item["value_ns"] for item in cpu) > 0
            digest_record = {key: value for key, value in record.items() if key != "__line__"}
            current["digest"].update(_canonical(digest_record) + b"\n")
            if current["path"] == "staged_async" and record.get("gpu_terminal") == "completed":
                if not any(isinstance(record.get("transfers"), dict)
                           and isinstance(record["transfers"].get(name), int)
                           and record["transfers"][name] > 0 for name in TRANSFER_COUNTERS):
                    errors.append(f"line {line}: completed staged_async block has no named payload transfer")
            if campaign in ("confirmation", "default", "overload"):
                unavailable = sorted(name for name in VERDICT_TIMINGS
                                     if not isinstance(timings, dict)
                                     or not _verdict_observation(timings.get(name)))
                if unavailable:
                    errors.append(f"line {line}: verdict timing unavailable or inferred: {','.join(unavailable)}")
            if (campaign in ("confirmation", "default") and isinstance(row, dict)
                    and row.get("load") != "overload"
                    and (record.get("deadline_miss") is True
                         or record.get("watchdog_expiry") is True)):
                errors.append(f"line {line}: normal-load confirmation/default block missed its gate")
            if campaign == "overload" and (
                record.get("deadline_miss") is True
                or record.get("watchdog_expiry") is True
                or record.get("late_completion") is True
                or record.get("resync_drop") is True
                or record.get("gpu_terminal") != "completed"
                or record.get("delivery") in ("cpu_fallback", "silence", "passthrough")
            ):
                overload_failure_observed = True
            current["block_count"] += 1
        elif kind == "trial_end":
            if current is None:
                errors.append(f"line {line}: trial_end outside a trial")
                continue
            if (type(record.get("trial_id")) is not type(current["id"])
                    or type(record.get("pair_id")) is not type(current["pair_id"])
                    or record.get("trial_id") != current["id"]
                    or record.get("path") != current["path"]
                    or record.get("pair_id") != current["pair_id"]):
                errors.append(f"line {line}: trial_end identity does not match trial")
            if (not _integer_at_least(record.get("block_count"), 0)
                    or record.get("block_count") != current["block_count"]):
                errors.append(f"line {line}: trial_end block_count mismatch")
            if isinstance(expected_blocks, int) and current["block_count"] != expected_blocks:
                errors.append(f"line {line}: incomplete trial; expected {expected_blocks} blocks")
            if record.get("blocks_sha256") != current["digest"].hexdigest():
                errors.append(f"line {line}: blocks_sha256 mismatch")
            _check_timing("ui_frame_p99", record.get("ui_frame_p99"), f"line {line}", errors)
            _check_timing("duration", record.get("duration"), f"line {line}", errors)
            for field in ("ui_frame_p99", "duration"):
                if not _verdict_observation(record.get(field)):
                    errors.append(f"line {line}: trial_end {field} must be available and direct/correlated")
                if isinstance(current["path"], str) and current["path"] in PATHS:
                    timing_provenance.check(current["path"], field, record.get(field), f"line {line}", errors)
            for field, bucket in (("gpu_terminal_counts", "terminals"), ("delivery_counts", "deliveries")):
                counts = record.get(field)
                if (not isinstance(counts, dict) or set(counts) != set(current[bucket])
                        or any(not _integer_at_least(value, 0) for value in counts.values())
                        or counts != current[bucket]):
                    errors.append(f"line {line}: trial_end {field} must exactly match block dispositions")
            if current["terminals"]["device_lost"] and record.get("device_loss") is not True:
                errors.append(f"line {line}: device_lost block requires trial_end device_loss")
            for field in ("device_loss", "audio_xrun", "driver_stall"):
                if not isinstance(record.get(field), bool):
                    errors.append(f"line {line}: {field} must be boolean")
            if (campaign in ("screening", "confirmation", "default")
                    and current["path"] in ("staged_async", "shared_async")):
                if current["deliveries"]["gpu"] == 0:
                    errors.append(f"line {line}: normal-load async trial requires eligible GPU delivery")
                if current["submit_samples"] != current["block_count"]:
                    errors.append(f"line {line}: submit-to-completion metric is incomplete")
                if current["cpu_samples"] != current["block_count"]:
                    errors.append(f"line {line}: total-CPU metric is incomplete")
                if current["path"] == "staged_async" and not current["submit_positive"]:
                    errors.append(f"line {line}: staged submit-to-completion baseline must be positive")
                if current["path"] == "staged_async" and not current["cpu_positive"]:
                    errors.append(f"line {line}: staged total-CPU baseline must be positive")
            if (campaign in ("confirmation", "default") and isinstance(row, dict)
                    and row.get("load") != "overload"
                    and any(record.get(field) is True for field in ("device_loss", "audio_xrun"))):
                errors.append(f"trial {current['id']}: normal-load confirmation/default terminal health failure")
            completed.append({"id": current["id"], "path": current["path"],
                              "pair_id": current["pair_id"], "end": record})
            current = None
        else:
            errors.append(f"line {line}: unknown record_kind {kind!r}")
    if current is not None:
        errors.append("evidence ends with an open trial")
    if isinstance(expected_trials, int) and len(completed) != expected_trials:
        errors.append(f"completed trial count {len(completed)} != expected {expected_trials}")
    if campaign == "overload" and not overload_failure_observed:
        errors.append("overload campaign did not exercise a typed failure or recovery disposition")
    for pair_id, paths in pair_paths.items():
        if sorted(paths) != ["shared_async", "staged_async"]:
            errors.append(f"matched pair {pair_id} must contain exactly one staged_async and one shared_async trial")
    if isinstance(expected_pairs, int) and len(pair_paths) != expected_pairs:
        errors.append(f"completed matched pair count {len(pair_paths)} != expected {expected_pairs}")
    identities.close()
    sync_count = sum(trial["path"] == "staged_sync" for trial in completed)
    if isinstance(expected_sync_trials, int) and sync_count != expected_sync_trials:
        errors.append(f"completed staged_sync trial count {sync_count} != expected {expected_sync_trials}")
    completed_paths = {trial["path"] for trial in completed if isinstance(trial["path"], str)}
    if completed and not {"staged_async", "shared_async"}.issubset(completed_paths):
        errors.append("campaign must contain staged_async and shared_async trials")
    return errors


def summarize(records: Sequence[dict[str, Any]], *,
              evidence_sha256: str | None = None) -> dict[str, Any]:
    manifest = records[0]
    paths: dict[str, dict[str, Any]] = {}
    trial_ends: dict[int, dict[str, Any]] = {}
    trial_paths: dict[int, str] = {}
    trial_pairs: dict[int, int | None] = {}
    metrics = _MetricStore()
    for record in records:
        if record.get("record_kind") == "trial_begin":
            trial_paths[record["trial_id"]] = record["path"]
            trial_pairs[record["trial_id"]] = record["pair_id"]
        if record.get("record_kind") == "trial_end":
            trial_ends[record["trial_id"]] = record
        if record.get("record_kind") != "block":
            continue
        path = record["path"]
        bucket = paths.setdefault(path, {
            "blocks": 0, "deadline_misses": 0, "watchdog_expiries": 0,
            "late_completions": 0, "resync_drops": 0,
            "gpu_terminals": {name: 0 for name in sorted(GPU_TERMINALS)},
            "deliveries": {name: 0 for name in sorted(DELIVERIES)},
            "transfers": {name: 0 for name in TRANSFER_COUNTERS},
            "timing_provenance": {},
        })
        bucket["blocks"] += 1
        bucket["deadline_misses"] += int(record["deadline_miss"])
        bucket["watchdog_expiries"] += int(record["watchdog_expiry"])
        bucket["late_completions"] += int(record["late_completion"])
        bucket["resync_drops"] += int(record["resync_drop"])
        bucket["gpu_terminals"][record["gpu_terminal"]] += 1
        bucket["deliveries"][record["delivery"]] += 1
        for name in TRANSFER_COUNTERS:
            bucket["transfers"][name] += record["transfers"][name]
        for name in TIMINGS:
            observation = record["timings"][name]
            bucket["timing_provenance"].setdefault(name, [_provenance(observation)])
            if _verdict_observation(observation):
                metrics.add(path, record["trial_id"], name, float(observation["value_ns"]))
        cpu_observations = [record["timings"][name]
                            for name in CPU_TIMINGS]
        if all(_verdict_observation(item) for item in cpu_observations):
            metrics.add(path, record["trial_id"], "total_cpu_per_block",
                        sum(float(item["value_ns"]) for item in cpu_observations))
    metrics.finish()
    for path, bucket in paths.items():
        bucket["timings_ns"] = {}
        for name in TIMINGS:
            bucket["timings_ns"][name] = {
                "available_samples": metrics.count(path, name),
                "p50": metrics.percentile(path, name, 50),
                "p95": metrics.percentile(path, name, 95),
                "p99": metrics.percentile(path, name, 99),
                "p99_9": metrics.percentile(path, name, 99.9),
                "max": metrics.maximum(path, name),
            }
        ends = [end for trial_id, end in trial_ends.items() if trial_paths.get(trial_id) == path]
        bucket["trials"] = len(ends)
        bucket["device_losses"] = sum(int(end["device_loss"]) for end in ends)
        bucket["audio_xruns"] = sum(int(end["audio_xrun"]) for end in ends)
        bucket["driver_stalls"] = sum(int(end["driver_stall"]) for end in ends)
        bucket["ui_frame_p99_ns"] = {
            "p50": _percentile([float(end["ui_frame_p99"]["value_ns"]) for end in ends], 50),
            "p95": _percentile([float(end["ui_frame_p99"]["value_ns"]) for end in ends], 95),
            "p99": _percentile([float(end["ui_frame_p99"]["value_ns"]) for end in ends], 99),
            "p99_9": _percentile([float(end["ui_frame_p99"]["value_ns"]) for end in ends], 99.9),
            "max": max((float(end["ui_frame_p99"]["value_ns"]) for end in ends), default=None),
        }
        bucket["duration_ns"] = sum(float(end["duration"]["value_ns"]) for end in ends)
        bucket["trial_observation_provenance"] = {
            field: [_provenance(ends[0][field])] if ends else []
            for field in ("ui_frame_p99", "duration")
        }
    paired_metrics: dict[int, dict[str, float]] = {}
    for trial_id, path in trial_paths.items():
        pair_id = trial_pairs[trial_id]
        if pair_id is None or path not in {"staged_async", "shared_async"}:
            continue
        paired_metrics.setdefault(pair_id, {})[f"{path}_submit_p99"] = metrics.percentile(
            path, "submit_to_completion", 99, trial_id)
        paired_metrics[pair_id][f"{path}_cpu_mean"] = metrics.mean(
            path, "total_cpu_per_block", trial_id)
        paired_metrics[pair_id][f"{path}_ui_p99"] = float(
            trial_ends[trial_id]["ui_frame_p99"]["value_ns"])

    ui_regressions: list[float] = []
    improvements: dict[str, list[float]] = {"submit_to_completion_p99_percent": [],
                                           "submit_to_completion_p99_ns": [],
                                           "total_cpu_per_block_percent": []}
    for pair in paired_metrics.values():
        staged_ui = pair.get("staged_async_ui_p99")
        shared_ui = pair.get("shared_async_ui_p99")
        if staged_ui is not None and shared_ui is not None:
            if staged_ui > 0:
                ui_regressions.append((shared_ui - staged_ui) * 100.0 / staged_ui)
            elif shared_ui == 0:
                ui_regressions.append(0.0)
        for output, stem in (("submit_to_completion_p99_percent", "submit_p99"),
                             ("total_cpu_per_block_percent", "cpu_mean")):
            staged = pair.get(f"staged_async_{stem}")
            shared = pair.get(f"shared_async_{stem}")
            if isinstance(staged, (int, float)) and isinstance(shared, (int, float)) and staged > 0:
                improvements[output].append((staged - shared) * 100.0 / staged)
                if stem == "submit_p99":
                    improvements["submit_to_completion_p99_ns"].append(staged - shared)
    seed = manifest["bootstrap_seed"]
    resamples = manifest["bootstrap_resamples"]
    confidence = {name: _bootstrap_mean_ci(values, seed + offset, resamples)
                  for offset, (name, values) in enumerate(improvements.items())}

    result = {
        "schema": "pulp.gpu-audio.p4.summary.v1",
        "campaign_id": manifest["campaign_id"],
        "campaign": manifest["campaign"],
        "source_revision": manifest["source_revision"],
        "binary_sha256": manifest["binary_sha256"],
        "machine": {name: manifest[name] for name in ("machine_id", "machine_model", "os_version")},
        "adapter": {"name": manifest["adapter_name"], "backend": manifest["adapter_backend"],
                    "registry_id": manifest["adapter_registry_id"],
                    "vendor_id": manifest["adapter_vendor_id"],
                    "device_id": manifest["adapter_device_id"]},
        "provider": {"name": manifest["provider"], "revision": manifest["provider_revision"],
                     "asset_sha256": manifest["provider_asset_sha256"]},
        "build": {"type": manifest["build_type"], "flags": manifest["build_flags"]},
        "generated_utc": manifest["generated_utc"],
        "row": manifest["row"],
        "paths": paths,
        "trial_count": len(trial_ends),
        "duration_ns": sum(float(item["duration"]["value_ns"]) for item in trial_ends.values()),
        "matched_improvement": confidence,
        "matched_ui_frame_p99": {
            "pairs": len(ui_regressions),
            "max_regression_percent": max(ui_regressions, default=None),
        },
        "bootstrap_seed": seed,
        "bootstrap_resamples": resamples,
        "cpu_total_components": list(CPU_TIMINGS),
        "verdict": "unassigned",
    }
    if evidence_sha256 is not None:
        result["evidence_sha256"] = evidence_sha256
    if manifest["campaign"] == "default":
        result["confirmation"] = {
            "campaign_id": manifest["confirmation_campaign_id"],
            "summary_sha256": manifest["confirmation_summary_sha256"],
        }
    result["row_gate"] = _row_gate(result, manifest["expected_matched_pairs"])
    metrics.close()
    return result


def _write_csv_handle(records: Iterable[dict[str, Any]], handle: TextIO, *,
                      evidence_sha256: str | None = None) -> None:
    manifest: dict[str, Any] | None = None
    identity_columns = ["campaign_id", "campaign", "source_revision", "binary_sha256",
                        "machine_id", "machine_model", "os_version", "adapter_name",
                        "adapter_backend", "adapter_registry_id", "adapter_vendor_id",
                        "adapter_device_id", "provider", "provider_revision", "provider_asset_sha256"]
    columns = ["evidence_sha256"] + identity_columns + ["record_kind", "trial_id", "pair_id",
               "path", "block_ordinal", "engine_id", "generation", "sequence",
               "gpu_terminal", "delivery", "deadline_miss", "watchdog_expiry",
               "late_completion", "resync_drop"]
    for name in TIMINGS:
        columns.extend(f"{name}_{suffix}" for suffix in
                       OBSERVATION_FIELDS)
    columns.extend(TRANSFER_COUNTERS)
    for name in ("ui_frame_p99", "duration"):
        columns.extend(f"{name}_{suffix}" for suffix in
                       OBSERVATION_FIELDS)
    columns.extend(("device_loss", "audio_xrun", "driver_stall"))
    columns.extend(f"gpu_terminal_count_{name}" for name in sorted(GPU_TERMINALS))
    columns.extend(f"delivery_count_{name}" for name in sorted(DELIVERIES))
    writer = csv.DictWriter(handle, fieldnames=columns)
    writer.writeheader()
    for record in records:
        if record.get("record_kind") == "manifest":
            manifest = record
            continue
        if record.get("record_kind") not in {"block", "trial_end"}:
            continue
        if manifest is None:
            raise ValueError("manifest must precede block records")
        row = {key: manifest[key] for key in identity_columns}
        row["evidence_sha256"] = evidence_sha256 or ""
        row["record_kind"] = record["record_kind"]
        for key in ("trial_id", "pair_id", "path", "block_ordinal", "engine_id",
                    "generation", "sequence", "gpu_terminal", "delivery", "deadline_miss",
                    "watchdog_expiry", "late_completion", "resync_drop"):
            row[key] = record.get(key, "")
        if record["record_kind"] == "block":
            for name in TIMINGS:
                observation = record["timings"][name]
                for suffix in OBSERVATION_FIELDS:
                    value = observation[suffix]
                    row[f"{name}_{suffix}"] = "" if value is None else value
            row.update(record["transfers"])
        else:
            for name in ("ui_frame_p99", "duration"):
                observation = record[name]
                for suffix in OBSERVATION_FIELDS:
                    value = observation[suffix]
                    row[f"{name}_{suffix}"] = "" if value is None else value
            for name in ("device_loss", "audio_xrun", "driver_stall"):
                row[name] = record[name]
            for name, count in record["gpu_terminal_counts"].items():
                row[f"gpu_terminal_count_{name}"] = count
            for name, count in record["delivery_counts"].items():
                row[f"delivery_count_{name}"] = count
        writer.writerow(row)


def write_csv(records: Iterable[dict[str, Any]], path: Path, *,
              evidence_sha256: str | None = None) -> None:
    temporary = _stage_output(
        path,
        lambda handle: _write_csv_handle(
            records, handle, evidence_sha256=evidence_sha256,
        ),
        newline="",
    )
    _publish_staged_outputs([(temporary, path)])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--benchmark-binary", required=True, type=Path)
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()
    paths = [("evidence input", args.evidence),
             ("benchmark input", args.benchmark_binary)]
    output_paths = [(label, path) for label, path in (
        ("summary output", args.summary), ("CSV output", args.csv),
    ) if path is not None]
    paths.extend(output_paths)
    alias_error = _distinct_paths(paths, probe_absent=(path for _, path in output_paths))
    if alias_error:
        print(f"gpu-audio-p4-evidence: paths must refer to distinct files: {alias_error}",
              file=sys.stderr)
        return 1
    try:
        evidence_sha256 = _sha256_file(args.evidence)
    except OSError as exc:
        print(f"gpu-audio-p4-evidence: cannot hash {args.evidence}: {exc}", file=sys.stderr)
        return 1
    records, errors = read_jsonl(args.evidence)
    try:
        try:
            if _sha256_file(args.evidence) != evidence_sha256:
                errors.append("evidence file changed while it was being read")
        except OSError as exc:
            errors.append(f"cannot re-hash {args.evidence}: {exc}")
        errors.extend(validate_records(records))
        if records and records[0].get("record_kind") == "manifest":
            errors.extend(validate_binary(records[0], args.benchmark_binary))
        if errors:
            for error in errors:
                print(f"gpu-audio-p4-evidence: {error}", file=sys.stderr)
            return 1
        result = summarize(records, evidence_sha256=evidence_sha256)
        try:
            encoded = json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
        except (TypeError, ValueError) as exc:
            print(f"gpu-audio-p4-evidence: cannot serialize summary: {exc}", file=sys.stderr)
            return 1
        staged: list[tuple[Path, Path]] = []
        try:
            if args.summary:
                staged.append((_stage_output(args.summary, lambda handle: handle.write(encoded)),
                               args.summary))
            if args.csv:
                staged.append((_stage_output(
                    args.csv,
                    lambda handle: _write_csv_handle(
                        records, handle, evidence_sha256=evidence_sha256,
                    ),
                    newline="",
                ), args.csv))
            if _sha256_file(args.evidence) != evidence_sha256:
                raise ValueError("evidence file changed during analysis")
            if _sha256_file(args.benchmark_binary) != records[0]["binary_sha256"]:
                raise ValueError("benchmark binary changed during analysis")
            alias_error = _distinct_paths(paths, probe_absent=(path for _, path in output_paths))
            if alias_error:
                raise ValueError(f"path identity changed during analysis: {alias_error}")
            _publish_staged_outputs(staged)
        except (OSError, ValueError) as exc:
            for temporary, _ in staged:
                temporary.unlink(missing_ok=True)
            print(f"gpu-audio-p4-evidence: cannot publish outputs: {exc}", file=sys.stderr)
            return 1
        if not args.summary:
            print(encoded, end="")
        return 0
    finally:
        if isinstance(records, _RecordStore):
            records.close()


if __name__ == "__main__":
    raise SystemExit(main())
