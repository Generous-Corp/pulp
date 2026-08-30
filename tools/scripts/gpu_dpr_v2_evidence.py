#!/usr/bin/env python3
"""Fail-closed file, cell, and runner-state evidence for A4 DPR v2.

This module deliberately has no collection policy.  It integrity-checks the
runner journal, confines retained artifacts to one owned root, and proves that
the eight files named by every v2 cell contain the evidence that the cell
projects.  The experiment module remains the authority for matrix and policy
analysis.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import math
import os
import secrets
import stat
import statistics
import struct
import tempfile
import zlib
from pathlib import Path, PurePosixPath
from typing import Any


RUN_STATE_SCHEMA = "pulp.gpu-dpr-run-state.v2"
RUN_INTEGRITY_SCHEMA = "pulp.gpu-dpr-run-state-integrity.v1"
RUN_ENVELOPE_SCHEMA = "pulp.gpu-dpr-run-state-envelope.v1"
CELL_RECEIPT_SCHEMA = "pulp.gpu-dpr-cell-receipt.v2"
ARTIFACT_KINDS = frozenset({
    "raw_trials",
    "frame_sequences",
    "capture",
    "reference_capture",
    "trace",
    "input_receipt",
    "identity_receipt",
    "product",
})
JSON_ARTIFACT_KINDS = ARTIFACT_KINDS - {
    "capture", "reference_capture", "trace", "product",
}
ARTIFACT_FIELDS = {"kind", "path", "sha256", "bytes", "binding_sha256"}
RUN_KEY = ".a4-v2-run-key"
RUN_STATE = "run-state-v2.json"
RUN_INTEGRITY = "run-state-v2.integrity.json"
RUN_ENVELOPE = "run-state-v2.envelope.json"
TRACE_ANALYZER_DESCRIPTOR = "trace-analyzer-v2.json"
MAX_JSON_BYTES = 64 * 1024 * 1024
MAX_CAPTURE_BYTES = 256 * 1024 * 1024
MAX_TRACE_BYTES = 2 * 1024 * 1024 * 1024
MAX_PRODUCT_BYTES = 512 * 1024 * 1024
MAX_CAPTURE_PIXELS = 100_000_000
_PNG_CACHE: dict[tuple[str, int, int], None] = {}


class V2EvidenceError(ValueError):
    """A v2 receipt or retained byte cannot support terminal evidence."""


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def _is_lower_hex(value: Any, length: int) -> bool:
    return (
        isinstance(value, str)
        and len(value) == length
        and all(character in "0123456789abcdef" for character in value)
    )


def _owned(metadata: os.stat_result) -> bool:
    return not hasattr(os, "geteuid") or metadata.st_uid == os.geteuid()


def _checked_root(root: Path, label: str) -> Path:
    if not root.is_absolute():
        raise V2EvidenceError(f"{label} root must be absolute")
    try:
        metadata = os.lstat(root)
    except OSError as error:
        raise V2EvidenceError(f"{label} root is unavailable: {root}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise V2EvidenceError(f"{label} root must be a non-symlink directory")
    if not _owned(metadata):
        raise V2EvidenceError(f"{label} root is not owned by the runner user")
    return root.resolve()


def safe_relative(value: Any) -> PurePosixPath:
    if not isinstance(value, str) or not value or "\\" in value or "\x00" in value:
        raise V2EvidenceError("artifact path must be a safe repository/runner-relative path")
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or str(path) != value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise V2EvidenceError("artifact path must be a safe repository/runner-relative path")
    return path


def checked_regular_path(root: Path, relative: Any, label: str) -> Path:
    resolved_root = _checked_root(root, label)
    parts = safe_relative(relative).parts
    current = root
    for index, part in enumerate(parts):
        current = current / part
        try:
            metadata = os.lstat(current)
        except OSError as error:
            raise V2EvidenceError(f"{label} file is missing: {relative}") from error
        if stat.S_ISLNK(metadata.st_mode):
            raise V2EvidenceError(f"{label} path contains a symlink: {relative}")
        if index + 1 < len(parts) and not stat.S_ISDIR(metadata.st_mode):
            raise V2EvidenceError(f"{label} parent is not a directory: {relative}")
    resolved = current.resolve()
    if resolved_root not in resolved.parents:
        raise V2EvidenceError(f"{label} path escapes its owned root: {relative}")
    metadata = os.lstat(current)
    if not stat.S_ISREG(metadata.st_mode) or not _owned(metadata):
        raise V2EvidenceError(f"{label} must be an owned regular file: {relative}")
    return current


def _open_regular(path: Path, label: str) -> tuple[int, os.stat_result]:
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError as error:
        raise V2EvidenceError(f"{label} is not a readable regular file: {path}") from error
    metadata = os.fstat(descriptor)
    if not stat.S_ISREG(metadata.st_mode) or not _owned(metadata):
        os.close(descriptor)
        raise V2EvidenceError(f"{label} is not an owned regular file: {path}")
    return descriptor, metadata


def file_identity(
    path: Path, label: str, *, max_bytes: int, retain: bool = False,
    prefix_bytes: int = 0,
) -> tuple[str, int, bytes | None]:
    descriptor, before = _open_regular(path, label)
    digest = hashlib.sha256()
    payload = bytearray() if retain or prefix_bytes else None
    total = 0
    try:
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            total += len(block)
            if total > max_bytes:
                raise V2EvidenceError(f"{label} exceeds its {max_bytes}-byte bound")
            digest.update(block)
            if retain and payload is not None:
                payload.extend(block)
            elif payload is not None and len(payload) < prefix_bytes:
                payload.extend(block[:prefix_bytes - len(payload)])
        after = os.fstat(descriptor)
        stable = (
            before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns
        ) == (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns
        )
        if not stable or total != after.st_size:
            raise V2EvidenceError(f"{label} changed while it was being read")
    finally:
        os.close(descriptor)
    return digest.hexdigest(), total, bytes(payload) if payload is not None else None


def regular_json(root: Path, relative: Any, label: str) -> tuple[dict[str, Any], str, int]:
    path = checked_regular_path(root, relative, label)
    digest, byte_count, payload = file_identity(
        path, label, max_bytes=MAX_JSON_BYTES, retain=True
    )
    try:
        value = json.loads((payload or b"").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise V2EvidenceError(f"{label} is not valid UTF-8 JSON") from error
    if not isinstance(value, dict):
        raise V2EvidenceError(f"{label} must contain one JSON object")
    return value, digest, byte_count


def trace_analyzer_identity(
    evidence_root: Path, expected_sha256: Any,
) -> dict[str, str]:
    descriptor, _, _ = regular_json(
        evidence_root, TRACE_ANALYZER_DESCRIPTOR, "runner trace-analyzer descriptor"
    )
    if set(descriptor) != {"path", "sha256", "bytes"}:
        raise V2EvidenceError("runner trace-analyzer descriptor is malformed")
    if descriptor.get("sha256") != expected_sha256:
        raise V2EvidenceError("runner trace analyzer differs from terminal A2T authority")
    path = checked_regular_path(
        evidence_root, descriptor["path"], "runner trace analyzer"
    )
    digest, byte_count, _ = file_identity(
        path, "runner trace analyzer", max_bytes=128 * 1024 * 1024
    )
    if (
        digest != descriptor["sha256"]
        or byte_count != descriptor["bytes"]
        or not os.lstat(path).st_mode & 0o111
    ):
        raise V2EvidenceError("runner trace-analyzer bytes/mode changed")
    return {"path": str(path.resolve()), "sha256": digest}


def snapshot_regular(
    source_root: Path, relative: Any, destination: Path, label: str, *,
    max_bytes: int, expected_sha256: str, expected_bytes: int,
    executable: bool = False,
) -> tuple[str, int]:
    """Snapshot one held, bounded regular file into a new runner-owned file."""
    source = checked_regular_path(source_root, relative, label)
    descriptor, before = _open_regular(source, label)
    if executable and not before.st_mode & 0o111:
        os.close(descriptor)
        raise V2EvidenceError(f"{label} is not executable")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        os.close(descriptor)
        raise V2EvidenceError(f"runner snapshot already exists: {destination}")
    digest = hashlib.sha256()
    total = 0
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile("wb", dir=destination.parent, delete=False) as handle:
            temporary = Path(handle.name)
            while True:
                block = os.read(descriptor, 1024 * 1024)
                if not block:
                    break
                total += len(block)
                if total > max_bytes:
                    raise V2EvidenceError(f"{label} exceeds its {max_bytes}-byte bound")
                digest.update(block)
                handle.write(block)
            handle.flush()
            os.fsync(handle.fileno())
        after = os.fstat(descriptor)
        if (
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
            or total != after.st_size
        ):
            raise V2EvidenceError(f"{label} changed while it was snapshotted")
        actual = digest.hexdigest()
        if actual != expected_sha256 or total != expected_bytes:
            raise V2EvidenceError(f"{label} digest/length differs from its receipt")
        temporary.chmod(0o555 if executable else 0o444)
        os.replace(temporary, destination)
        temporary = None
        return actual, total
    finally:
        os.close(descriptor)
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def cell_key(cell: dict[str, Any]) -> str:
    dpr = float(cell.get("requested_dpr", 0))
    dpr_text = str(int(dpr)) if dpr.is_integer() else format(dpr, ".15g")
    return f"{cell.get('scenario_id', '')}__{cell.get('mode', '')}__dpr-{dpr_text}"


def binding_document(cell: dict[str, Any]) -> dict[str, Any]:
    identity = cell.get("identity")
    fresh = cell.get("fresh_process_trials")
    if not isinstance(identity, dict) or not isinstance(fresh, list):
        raise V2EvidenceError("cell identity/fresh-process ledger is missing")
    process_id = identity.get("process_id")
    fresh_pids = [trial.get("pid") for trial in fresh if isinstance(trial, dict)]
    return {
        "campaign": cell.get("campaign"),
        "scenario_id": cell.get("scenario_id"),
        "mode": cell.get("mode"),
        "requested_dpr": cell.get("requested_dpr"),
        "observed_dpr": cell.get("observed_dpr"),
        "logical_size": cell.get("logical_size"),
        "physical_size": cell.get("physical_size"),
        "attempt_nonce": cell.get("attempt_nonce"),
        "machine_id": identity.get("machine_id"),
        "provider": identity.get("provider"),
        "adapter": identity.get("adapter"),
        "build_sha": identity.get("build_sha"),
        "host": identity.get("host"),
        "app": identity.get("app"),
        "format": identity.get("format"),
        "instance_id": identity.get("instance_id"),
        "product_sha256": identity.get("product_sha256"),
        "process_ids": [identity.get("producer_process_id"), process_id, *fresh_pids],
        "measured_trial_indices": [
            trial.get("trial_index")
            for trial in cell.get("measured_trials", [])
            if isinstance(trial, dict)
        ],
        "fresh_trial_indices": [
            trial.get("trial_index") for trial in fresh if isinstance(trial, dict)
        ],
    }


def binding_sha256(cell: dict[str, Any]) -> str:
    return canonical_sha256(binding_document(cell))


def cell_payload(cell: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in cell.items() if key != "artifacts"}


def _png_dimensions_and_integrity(payload: bytes, expected: tuple[int, int]) -> None:
    if not payload.startswith(b"\x89PNG\r\n\x1a\n"):
        raise V2EvidenceError("capture artifact is not PNG")
    offset = 8
    dimensions: tuple[int, int] | None = None
    depth = color = interlace = None
    compressed = bytearray()
    saw_iend = False
    while offset + 12 <= len(payload):
        length = struct.unpack(">I", payload[offset:offset + 4])[0]
        end = offset + 12 + length
        if end > len(payload):
            raise V2EvidenceError("capture PNG chunk escapes the file")
        kind = payload[offset + 4:offset + 8]
        body = payload[offset + 8:offset + 8 + length]
        crc = struct.unpack(">I", payload[offset + 8 + length:end])[0]
        if zlib.crc32(kind + body) & 0xffffffff != crc:
            raise V2EvidenceError("capture PNG chunk checksum is invalid")
        if kind == b"IHDR":
            if dimensions is not None or length != 13:
                raise V2EvidenceError("capture PNG IHDR is malformed")
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            dimensions = (width, height)
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            saw_iend = True
            if end != len(payload):
                raise V2EvidenceError("capture PNG carries bytes after IEND")
            break
        offset = end
    if (
        dimensions != expected or not compressed or not saw_iend
        or depth != 8 or color not in {2, 6} or interlace != 0
    ):
        raise V2EvidenceError("capture PNG format/dimensions differ from the cell")
    if dimensions[0] * dimensions[1] > MAX_CAPTURE_PIXELS:
        raise V2EvidenceError("capture PNG exceeds the bounded pixel count")
    channels = 4 if color == 6 else 3
    expected_raw = (dimensions[0] * channels + 1) * dimensions[1]
    decoder = zlib.decompressobj()
    produced = 0
    for index in range(0, len(compressed), 1024 * 1024):
        produced += len(decoder.decompress(compressed[index:index + 1024 * 1024]))
        if produced > expected_raw:
            raise V2EvidenceError("capture PNG expands beyond its declared dimensions")
    produced += len(decoder.flush())
    if produced != expected_raw or not decoder.eof or decoder.unused_data:
        raise V2EvidenceError("capture PNG pixels do not match its declared dimensions")


def _file_format(payload: bytes) -> str | None:
    if payload.startswith(b"\x7fELF"):
        if (
            len(payload) < 64
            or payload[4] not in {1, 2}
            or payload[5] not in {1, 2}
        ):
            return None
        byte_order = "little" if payload[5] == 1 else "big"
        executable_type = int.from_bytes(payload[16:18], byte_order)
        machine = int.from_bytes(payload[18:20], byte_order)
        program_headers = int.from_bytes(
            payload[44:46] if payload[4] == 1 else payload[56:58], byte_order
        )
        return "elf" if executable_type in {2, 3} and machine and program_headers else None
    if payload.startswith(b"MZ"):
        if len(payload) < 0x40:
            return None
        offset = int.from_bytes(payload[0x3c:0x40], "little")
        if offset + 26 > len(payload) or payload[offset:offset + 4] != b"PE\0\0":
            return None
        machine = int.from_bytes(payload[offset + 4:offset + 6], "little")
        sections = int.from_bytes(payload[offset + 6:offset + 8], "little")
        optional_size = int.from_bytes(payload[offset + 20:offset + 22], "little")
        optional_magic = int.from_bytes(payload[offset + 24:offset + 26], "little")
        return (
            "pe" if machine and sections and optional_size and optional_magic in {0x10B, 0x20B}
            else None
        )
    if payload.startswith(b"\x00asm"):
        return "wasm" if len(payload) >= 8 and payload[4:8] == b"\x01\0\0\0" else None
    if payload[:4] in {
        b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
    }:
        if len(payload) < 28:
            return None
        magic = payload[:4]
        if magic in {b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}:
            byte_order = "big" if magic == b"\xca\xfe\xba\xbe" else "little"
            architectures = int.from_bytes(payload[4:8], byte_order)
            return "mach-o" if architectures and 8 + architectures * 20 <= len(payload) else None
        byte_order = "big" if magic in {b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf"} else "little"
        commands = int.from_bytes(payload[16:20], byte_order)
        command_bytes = int.from_bytes(payload[20:24], byte_order)
        header_bytes = 32 if magic in {b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe"} else 28
        return (
            "mach-o" if commands and command_bytes and header_bytes + command_bytes <= len(payload)
            else None
        )
    return None


def _artifact_map(cell: dict[str, Any]) -> dict[str, dict[str, Any]]:
    artifacts = cell.get("artifacts")
    if not isinstance(artifacts, list):
        raise V2EvidenceError("cell artifact inventory is missing")
    result: dict[str, dict[str, Any]] = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict) or set(artifact) != ARTIFACT_FIELDS:
            raise V2EvidenceError("cell artifact fields differ from the closed v2 contract")
        kind = artifact.get("kind")
        if kind not in ARTIFACT_KINDS or kind in result:
            raise V2EvidenceError("cell artifact kinds are missing, duplicated, or unsupported")
        if not _is_lower_hex(artifact.get("sha256"), 64):
            raise V2EvidenceError(f"{kind} artifact lacks a lowercase SHA-256")
        if not _is_lower_hex(artifact.get("binding_sha256"), 64):
            raise V2EvidenceError(f"{kind} artifact lacks a binding SHA-256")
        if (
            isinstance(artifact.get("bytes"), bool)
            or not isinstance(artifact.get("bytes"), int)
            or artifact["bytes"] <= 0
        ):
            raise V2EvidenceError(f"{kind} artifact has an invalid byte count")
        result[kind] = artifact
    if set(result) != ARTIFACT_KINDS:
        raise V2EvidenceError("cell must retain all eight v2 artifact kinds")
    return result


def _expected_identity(cell: dict[str, Any]) -> tuple[dict[str, Any], list[int]]:
    identity = cell.get("identity")
    required = {
        "machine_id", "provider", "adapter", "adapter_sha256", "build_sha",
        "host", "format", "app", "producer_process_id", "process_id",
        "instance_id", "product_sha256",
    }
    if not isinstance(identity, dict) or set(identity) != required:
        raise V2EvidenceError("cell identity differs from the closed product identity")
    textual = required - {"producer_process_id", "process_id"}
    if any(not isinstance(identity[field], str) or not identity[field] for field in textual):
        raise V2EvidenceError("cell product identity contains an empty field")
    if not _is_lower_hex(identity["build_sha"], 40):
        raise V2EvidenceError("cell build identity is not an exact Git revision")
    if not _is_lower_hex(identity["product_sha256"], 64):
        raise V2EvidenceError("cell product identity lacks exact bytes")
    if not _is_lower_hex(identity["adapter_sha256"], 64):
        raise V2EvidenceError("cell producer identity lacks exact adapter bytes")
    producer_process_id = identity["producer_process_id"]
    process_id = identity["process_id"]
    if any(
        isinstance(value, bool) or not isinstance(value, int) or value <= 0
        for value in (producer_process_id, process_id)
    ):
        raise V2EvidenceError("cell producer/product process id is invalid")
    fresh = cell.get("fresh_process_trials")
    if not isinstance(fresh, list):
        raise V2EvidenceError("cell fresh-process ledger is missing")
    fresh_pids = [trial.get("pid") for trial in fresh if isinstance(trial, dict)]
    if (
        len(fresh_pids) != len(fresh)
        or any(isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0 for pid in fresh_pids)
        or len({producer_process_id, process_id, *fresh_pids}) != len(fresh_pids) + 2
    ):
        raise V2EvidenceError("cell process identities are missing or reused")
    return identity, [producer_process_id, process_id, *fresh_pids]


def _numeric_samples(
    value: Any, count: int, label: str, *, integer: bool = True,
) -> list[int | float]:
    if not isinstance(value, list) or len(value) != count:
        raise V2EvidenceError(f"{label} does not contain exactly {count} frame samples")
    expected_type = int if integer else (int, float)
    if any(
        isinstance(sample, bool)
        or not isinstance(sample, expected_type)
        or not math.isfinite(float(sample))
        or sample < 0
        for sample in value
    ):
        raise V2EvidenceError(f"{label} contains an invalid frame sample")
    return value


def _nearest_rank(values: list[int | float], quantile: float) -> int | float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(quantile * len(ordered)) - 1)]


def _validate_gpu_calibration(
    value: Any, manifest: dict[str, Any], scenario: dict[str, Any],
) -> None:
    required = {
        "schema", "version", "clock", "resolution_ns", "baseline_samples_ns",
        "extra_work_samples_ns", "extra_work_multiplier", "control_detected",
    }
    if not isinstance(value, dict) or set(value) != required:
        raise V2EvidenceError("retained GPU timer calibration is missing or malformed")
    contract = manifest.get("trial_contract", {})
    count = contract.get("gpu_timer_calibration_trials")
    multiplier = contract.get("gpu_timer_extra_work_multiplier")
    expected_clock = {
        "Dawn/WebGPU": "dawn-gpu-timestamp",
        "WebGL2": "webgl2-timer-query",
    }.get(scenario.get("required_provider"))
    if expected_clock is None:
        raise V2EvidenceError("GPU timer calibration provider authority is invalid")
    if isinstance(count, bool) or not isinstance(count, int) or count < 3:
        raise V2EvidenceError("GPU timer calibration trial authority is invalid")
    baseline = _numeric_samples(
        value.get("baseline_samples_ns"), count, "GPU calibration baseline"
    )
    extra = _numeric_samples(
        value.get("extra_work_samples_ns"), count, "GPU calibration extra-work control"
    )
    resolution = value.get("resolution_ns")
    if value.get("clock") != expected_clock:
        raise V2EvidenceError(
            "retained GPU timer calibration clock does not match scenario provider"
        )
    if (
        value.get("schema") != "pulp.gpu-dpr-timer-calibration.v2"
        or value.get("version") != 2
        or value.get("extra_work_multiplier") != multiplier
        or value.get("control_detected") is not True
        or isinstance(resolution, bool)
        or not isinstance(resolution, int)
        or resolution <= 0
    ):
        raise V2EvidenceError("retained GPU timer calibration contract is invalid")
    detectable = max(float(resolution) * 2.0, statistics.median(baseline) * 0.10)
    if statistics.median(extra) < statistics.median(baseline) + detectable:
        raise V2EvidenceError("retained GPU timer known-extra-work control was not detected")


def _rederived_trial(document: Any) -> dict[str, Any]:
    required = {"trial_index", "mode_order", "frame_samples", "sequence_sha256"}
    if not isinstance(document, dict) or set(document) != required:
        raise V2EvidenceError("retained measured trial has an invalid contract")
    samples = document.get("frame_samples")
    sample_fields = {
        "gpu_ns", "cpu_ns", "first_frame_ns", "interaction_ns",
        "render_target_bytes", "resident_bytes", "upload_bytes",
        "frame_missed", "xruns",
    }
    if not isinstance(samples, dict) or set(samples) != sample_fields:
        raise V2EvidenceError("retained measured trial lacks exact frame-level samples")
    frame_count = len(samples.get("gpu_ns", [])) if isinstance(samples.get("gpu_ns"), list) else 0
    if frame_count < 240:
        raise V2EvidenceError("retained measured trial has fewer than 240 frames")
    numeric = {
        field: _numeric_samples(samples.get(field), frame_count, f"retained {field}")
        for field in sample_fields - {"frame_missed"}
    }
    misses = samples.get("frame_missed")
    if (
        not isinstance(misses, list)
        or len(misses) != frame_count
        or any(not isinstance(value, bool) for value in misses)
    ):
        raise V2EvidenceError("retained frame-miss samples are malformed")
    if document.get("sequence_sha256") != canonical_sha256(samples):
        raise V2EvidenceError("retained frame sequence digest differs from its samples")
    return {
        "trial_index": document.get("trial_index"),
        "mode_order": document.get("mode_order"),
        "frame_count": frame_count,
        "gpu_p95_ns": _nearest_rank(numeric["gpu_ns"], 0.95),
        "cpu_median_ns": statistics.median(numeric["cpu_ns"]),
        "cpu_p95_ns": _nearest_rank(numeric["cpu_ns"], 0.95),
        "first_frame_median_ns": statistics.median(numeric["first_frame_ns"]),
        "first_frame_p95_ns": _nearest_rank(numeric["first_frame_ns"], 0.95),
        "interaction_median_ns": statistics.median(numeric["interaction_ns"]),
        "interaction_p95_ns": _nearest_rank(numeric["interaction_ns"], 0.95),
        "render_target_p95_bytes": _nearest_rank(numeric["render_target_bytes"], 0.95),
        "resident_p95_bytes": _nearest_rank(numeric["resident_bytes"], 0.95),
        "upload_p95_bytes": _nearest_rank(numeric["upload_bytes"], 0.95),
        "frame_misses": sum(misses),
        "xruns": sum(numeric["xruns"]),
        "sequence_sha256": document.get("sequence_sha256"),
    }


def _validate_input_receipt(
    cell: dict[str, Any], receipt: Any, scenario: dict[str, Any], binding: str,
    process_id: int,
) -> None:
    required = {
        "schema", "version", "binding_sha256", "logical_size", "physical_size",
        "physical_dimensions_verified", "logical_input_exact", "event",
    }
    if not isinstance(receipt, dict) or set(receipt) != required:
        raise V2EvidenceError("input receipt differs from the closed runtime observation contract")
    event = receipt.get("event")
    event_fields = {
        "requested_logical_point", "requested_physical_point",
        "observed_logical_point", "observed_physical_point", "event_received",
        "expected_target", "observed_target", "process_id",
    }
    oracle = scenario.get("logical_input_oracle", {})
    expected_logical = oracle.get("point")
    expected_target = oracle.get("target")
    observed_dpr = float(cell.get("observed_dpr", 0))
    if (
        not isinstance(expected_logical, list)
        or len(expected_logical) != 2
        or not math.isfinite(observed_dpr)
        or observed_dpr <= 0
    ):
        raise V2EvidenceError("input receipt has invalid oracle/DPR authority")
    expected_requested_physical = [
        float(value) * observed_dpr for value in expected_logical
    ]
    expected_observed_physical = [
        round(float(value) * observed_dpr) for value in expected_logical
    ]
    expected_observed_logical = [
        value / observed_dpr for value in expected_observed_physical
    ]
    if not isinstance(event, dict) or set(event) != event_fields:
        raise V2EvidenceError("input receipt lacks exact runtime event observations")
    coordinate_fields = (
        "requested_logical_point", "requested_physical_point",
        "observed_logical_point", "observed_physical_point",
    )
    if any(
        not isinstance(event.get(field), list)
        or len(event[field]) != 2
        or any(
            isinstance(value, bool) or not isinstance(value, (int, float))
            or not math.isfinite(float(value))
            for value in event[field]
        )
        for field in coordinate_fields
    ):
        raise V2EvidenceError("input receipt contains malformed runtime coordinates")
    exact = bool(
        event["requested_logical_point"] == expected_logical
        and event["requested_physical_point"] == expected_requested_physical
        and event["observed_physical_point"] == expected_observed_physical
        and event["observed_logical_point"] == expected_observed_logical
        and event["event_received"] is True
        and event["expected_target"] == expected_target
        and event["observed_target"] == expected_target
        and event["process_id"] == process_id
    )
    if (
        receipt["schema"] != "pulp.gpu-dpr-input-receipt.v2"
        or receipt["version"] != 2
        or receipt["binding_sha256"] != binding
        or receipt["logical_size"] != cell.get("logical_size")
        or receipt["physical_size"] != cell.get("physical_size")
        or receipt["physical_dimensions_verified"] is not cell.get("physical_dimensions_verified")
        or receipt["logical_input_exact"] is not exact
        or cell.get("fidelity", {}).get("logical_input_exact") is not exact
    ):
        raise V2EvidenceError("input receipt result is not derived from runtime coordinates/event/target")


def _validate_json_artifacts(
    cell: dict[str, Any], documents: dict[str, dict[str, Any]],
    artifacts: dict[str, dict[str, Any]], manifest: dict[str, Any], binding: str,
) -> None:
    payload = cell_payload(cell)
    raw = documents["raw_trials"]
    if raw != {
        "schema": "pulp.gpu-dpr-raw-trials.v2",
        "version": 2,
        "binding_sha256": binding,
        "cell": payload,
    }:
        raise V2EvidenceError("raw-trials artifact is not the exact bound cell evidence")

    measured = cell.get("measured_trials", [])
    fresh = cell.get("fresh_process_trials", [])
    sequences = documents["frame_sequences"]
    if not isinstance(sequences, dict) or set(sequences) != {
        "schema", "version", "binding_sha256", "gpu_timer_calibration",
        "measured", "fresh",
    }:
        raise V2EvidenceError("frame-sequences artifact differs from the closed sample contract")
    scenario = next(
        item for item in manifest["scenarios"]
        if item["id"] == cell.get("scenario_id")
    )
    _validate_gpu_calibration(
        sequences["gpu_timer_calibration"], manifest, scenario
    )
    retained_trials = sequences.get("measured")
    if not isinstance(retained_trials, list) or len(retained_trials) != len(measured):
        raise V2EvidenceError("frame-sequences artifact lacks all measured trials")
    for summary, retained in zip(measured, retained_trials):
        rederived = _rederived_trial(retained)
        for field, value in rederived.items():
            if not isinstance(summary, dict) or summary.get(field) != value:
                raise V2EvidenceError(
                    f"measured trial {rederived.get('trial_index')} {field} differs from retained frames"
                )
    expected_fresh = [
            {
                "trial_index": trial.get("trial_index"),
                "pid": trial.get("pid"),
                "sequence_sha256": trial.get("sequence_sha256"),
            }
            for trial in fresh if isinstance(trial, dict)
        ]
    if (
        sequences.get("schema") != "pulp.gpu-dpr-frame-sequences.v2"
        or sequences.get("version") != 2
        or sequences.get("binding_sha256") != binding
        or sequences.get("fresh") != expected_fresh
    ):
        raise V2EvidenceError("frame-sequences artifact differs from the cell trials")

    identity, process_ids = _expected_identity(cell)
    _validate_input_receipt(
        cell, documents["input_receipt"], scenario, binding, identity["process_id"]
    )

    identity_receipt = documents["identity_receipt"]
    product = identity_receipt.get("product") if isinstance(identity_receipt, dict) else None
    daw_documents = (
        identity_receipt.get("daw_receipt_documents")
        if isinstance(identity_receipt, dict) else None
    )
    if identity_receipt != {
        "schema": "pulp.gpu-dpr-identity-receipt.v2",
        "version": 2,
        "binding_sha256": binding,
        "identity": identity,
        "process_ids": process_ids,
        "product": product,
        "daw_subreceipts": cell.get("daw_subreceipts"),
        "daw_receipt_documents": daw_documents,
    }:
        raise V2EvidenceError("identity receipt is not bound to cell/process/DAW identity")
    if not isinstance(product, dict) or set(product) != {
        "sha256", "bytes", "file_format", "app", "format", "host", "provider",
    }:
        raise V2EvidenceError("identity receipt lacks the exact product artifact contract")
    product_artifact = artifacts["product"]
    if (
        product.get("sha256") != product_artifact["sha256"]
        or identity.get("product_sha256") != product_artifact["sha256"]
        or product.get("bytes") != product_artifact["bytes"]
        or product.get("app") != identity["app"]
        or product.get("format") != identity["format"]
        or product.get("host") != identity["host"]
        or product.get("provider") != identity["provider"]
        or product.get("file_format") not in {"mach-o", "elf", "pe", "wasm"}
    ):
        raise V2EvidenceError("product artifact identity differs from measured app/format/host")
    daw = cell.get("daw_subreceipts")
    if not isinstance(daw, list) or not isinstance(daw_documents, list) or len(daw_documents) != len(daw):
        raise V2EvidenceError("identity receipt lacks exact DAW subreceipt documents")


def _retained_reference(
    root: Path, value: Any, label: str, *, max_bytes: int, executable: bool = False,
) -> tuple[Path, str, int, bytes | None]:
    if not isinstance(value, dict) or set(value) != {"path", "sha256", "bytes"}:
        raise V2EvidenceError(f"{label} reference is malformed")
    if (
        not _is_lower_hex(value.get("sha256"), 64)
        or isinstance(value.get("bytes"), bool)
        or not isinstance(value.get("bytes"), int)
        or value["bytes"] <= 0
    ):
        raise V2EvidenceError(f"{label} reference lacks an exact digest/length")
    path = checked_regular_path(root, value["path"], label)
    digest, byte_count, payload = file_identity(
        path, label, max_bytes=max_bytes, retain=True,
    )
    if digest != value["sha256"] or byte_count != value["bytes"]:
        raise V2EvidenceError(f"{label} reference differs from retained bytes")
    if executable and (not os.lstat(path).st_mode & 0o111 or _file_format(payload or b"") is None):
        raise V2EvidenceError(f"{label} is not a retained executable product")
    return path.resolve(), digest, byte_count, payload


def _validate_daw_subreceipts(
    cell: dict[str, Any], identity_receipt: dict[str, Any],
    artifacts: dict[str, dict[str, Any]], manifest: dict[str, Any], binding: str,
    evidence_root: Path,
) -> list[Path]:
    projections = cell.get("daw_subreceipts")
    documents = identity_receipt.get("daw_receipt_documents")
    if not isinstance(projections, list) or not isinstance(documents, list):
        raise V2EvidenceError("identity receipt lacks exact DAW subreceipt documents")
    if len(projections) != len(documents):
        raise V2EvidenceError("identity receipt lacks exact DAW subreceipt documents")
    authority = manifest.get("v2_protocol", {}).get("product_policy", {}).get(
        "a3_receipt_sha256"
    )
    if projections and not _is_lower_hex(authority, 64):
        raise V2EvidenceError("DAW subreceipts lack fixed A3 authority")
    retained: list[Path] = []
    for projection, document in zip(projections, documents):
        if not isinstance(projection, dict) or not isinstance(document, dict):
            raise V2EvidenceError("DAW subreceipt projection/document is malformed")
        if projection.get("outcome") != "pass" or projection.get("gates_passed") is not True:
            raise V2EvidenceError("required DAW subreceipt did not pass")
        expected_fields = {
            "schema", "version", "binding_sha256", "format", "host", "a3_role",
            "outcome", "gates_passed", "a3_evidence", "product", "host_product",
            "lifecycle",
        }
        if set(document) != expected_fields:
            raise V2EvidenceError("DAW subreceipt differs from the retained-evidence contract")
        a3_path, _, _, a3_payload = _retained_reference(
            evidence_root, document["a3_evidence"], "DAW fixed A3 evidence",
            max_bytes=MAX_JSON_BYTES,
        )
        try:
            a3_document = json.loads((a3_payload or b"").decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise V2EvidenceError("DAW fixed A3 evidence is not UTF-8 JSON") from error
        if not isinstance(a3_document, dict) or canonical_sha256(a3_document) != authority:
            raise V2EvidenceError("DAW evidence differs from the fixed A3 authority")
        product_path, product_sha, _, _ = _retained_reference(
            evidence_root, document["product"], "DAW measured product",
            max_bytes=MAX_PRODUCT_BYTES, executable=True,
        )
        top_product = artifacts["product"]
        if document["product"] != {
            "path": top_product["path"], "sha256": top_product["sha256"],
            "bytes": top_product["bytes"],
        }:
            raise V2EvidenceError("DAW subreceipt product differs from the measured cell product")
        host_path, host_sha, _, _ = _retained_reference(
            evidence_root, document["host_product"], "DAW host product",
            max_bytes=MAX_PRODUCT_BYTES, executable=True,
        )
        lifecycle_path, _, _, lifecycle_payload = _retained_reference(
            evidence_root, document["lifecycle"], "DAW lifecycle evidence",
            max_bytes=MAX_JSON_BYTES,
        )
        try:
            lifecycle = json.loads((lifecycle_payload or b"").decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise V2EvidenceError("DAW lifecycle evidence is not UTF-8 JSON") from error
        lifecycle_id = lifecycle.get("lifecycle_id") if isinstance(lifecycle, dict) else None
        process_start = (
            lifecycle.get("process_start_identity") if isinstance(lifecycle, dict) else None
        )
        expected_lifecycle = {
            "schema": "pulp.gpu-dpr-daw-lifecycle.v2", "version": 2,
            "binding_sha256": binding, "a3_receipt_sha256": authority,
            "product_sha256": product_sha, "host_executable_sha256": host_sha,
            "format": projection.get("format"), "host": projection.get("host"),
            "a3_role": projection.get("a3_role"), "lifecycle_id": lifecycle_id,
            "process_start_identity": process_start,
        }
        retained_identity = {
            "a3_receipt_sha256": authority, "product_sha256": product_sha,
            "host_executable_sha256": host_sha,
            "process_start_identity": process_start, "lifecycle_id": lifecycle_id,
            "format": projection.get("format"), "host": projection.get("host"),
            "a3_role": projection.get("a3_role"),
        }
        expected_document = {
            "schema": "pulp.gpu-dpr-daw-subreceipt.v2", "version": 2,
            "binding_sha256": binding, "format": projection.get("format"),
            "host": projection.get("host"), "a3_role": projection.get("a3_role"),
            "outcome": projection.get("outcome"),
            "gates_passed": projection.get("gates_passed"),
            "a3_evidence": document["a3_evidence"], "product": document["product"],
            "host_product": document["host_product"], "lifecycle": document["lifecycle"],
        }
        if (
            not isinstance(lifecycle_id, str) or not lifecycle_id
            or not isinstance(process_start, str) or not process_start
            or lifecycle != expected_lifecycle
            or document != expected_document
            or canonical_sha256(retained_identity) != projection.get("identity_sha256")
            or canonical_sha256(document) != projection.get("receipt_sha256")
        ):
            raise V2EvidenceError(
                "DAW subreceipt is not bound to fixed A3/product/host/lifecycle evidence"
            )
        retained.extend((a3_path, host_path, lifecycle_path))
    return retained

def validate_cell_artifacts(
    cell: dict[str, Any], manifest: dict[str, Any], evidence_root: Path,
    trace_analyzer: dict[str, str],
) -> list[Path]:
    artifacts = _artifact_map(cell)
    expected_binding = binding_sha256(cell)
    documents: dict[str, dict[str, Any]] = {}
    paths: list[Path] = []
    for kind in sorted(ARTIFACT_KINDS):
        artifact = artifacts[kind]
        if artifact["binding_sha256"] != expected_binding:
            raise V2EvidenceError(f"{kind} artifact is bound to a different cell")
        path = checked_regular_path(evidence_root, artifact["path"], f"{kind} artifact")
        paths.append(path.resolve())
        max_bytes = (
            MAX_CAPTURE_BYTES if kind in {"capture", "reference_capture"}
            else MAX_TRACE_BYTES if kind == "trace"
            else MAX_PRODUCT_BYTES if kind == "product"
            else MAX_JSON_BYTES
        )
        digest, byte_count, payload = file_identity(
            path, f"{kind} artifact", max_bytes=max_bytes,
            retain=kind in JSON_ARTIFACT_KINDS or kind in {"capture", "reference_capture"},
            prefix_bytes=65536 if kind == "product" else 0,
        )
        if digest != artifact["sha256"] or byte_count != artifact["bytes"]:
            raise V2EvidenceError(f"{kind} artifact digest/length differs from its bytes")
        if kind in JSON_ARTIFACT_KINDS:
            try:
                document = json.loads((payload or b"").decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise V2EvidenceError(f"{kind} artifact is not valid UTF-8 JSON") from error
            if not isinstance(document, dict):
                raise V2EvidenceError(f"{kind} artifact must contain one JSON object")
            documents[kind] = document
        elif kind in {"capture", "reference_capture"}:
            physical = cell.get("physical_size")
            if (
                not isinstance(physical, dict)
                or isinstance(physical.get("width"), bool)
                or isinstance(physical.get("height"), bool)
                or not isinstance(physical.get("width"), int)
                or not isinstance(physical.get("height"), int)
            ):
                raise V2EvidenceError("cell physical capture dimensions are missing")
            cache_key = (digest, physical["width"], physical["height"])
            if cache_key not in _PNG_CACHE:
                _png_dimensions_and_integrity(
                    payload or b"", (physical["width"], physical["height"])
                )
                if len(_PNG_CACHE) >= 64:
                    _PNG_CACHE.pop(next(iter(_PNG_CACHE)))
                _PNG_CACHE[cache_key] = None
        elif kind == "product":
            if not os.lstat(path).st_mode & 0o111:
                raise V2EvidenceError("product artifact is not executable")
            file_format = _file_format(payload or b"")
            identity_receipt = documents.get("identity_receipt")
            # identity_receipt sorts before product, so this is always available.
            declared = (
                identity_receipt.get("product", {}).get("file_format")
                if isinstance(identity_receipt, dict) else None
            )
            if file_format is None or file_format != declared:
                raise V2EvidenceError("product artifact bytes differ from the declared file format")
    _validate_json_artifacts(cell, documents, artifacts, manifest, expected_binding)
    paths.extend(_validate_daw_subreceipts(
        cell, documents["identity_receipt"], artifacts, manifest, expected_binding,
        evidence_root,
    ))
    try:
        import gpu_dpr_evidence as v1_evidence

        trace_path = checked_regular_path(
            evidence_root, artifacts["trace"]["path"], "trace artifact"
        )
        scenario = next(
            item for item in manifest["scenarios"]
            if item["id"] == cell.get("scenario_id")
        )
        evidence_ids = v1_evidence.validate_trace(
            {"trace": cell.get("trace")}, manifest, trace_path, trace_analyzer,
            cell["attempt_nonce"], scenario["kind"],
        )
        if evidence_ids != [cell["attempt_nonce"]]:
            raise V2EvidenceError("trace analyzer returned a foreign evidence identity")
        capture = checked_regular_path(
            evidence_root, artifacts["capture"]["path"], "capture artifact"
        )
        reference = checked_regular_path(
            evidence_root, artifacts["reference_capture"]["path"],
            "reference-capture artifact",
        )
        width, height, content, similarity, text_stddev, stroke_coverage = (
            v1_evidence.recompute_fidelity(
                capture, reference, scenario, float(cell["observed_dpr"])
            )
        )
        if {"width": width, "height": height} != cell.get("physical_size"):
            raise V2EvidenceError("capture pixels differ from the cell physical dimensions")
        fidelity = cell.get("fidelity", {})
        requirements = set(scenario.get("required_oracles", []))
        if (
            fidelity.get("content_floor") is not content
            or not math.isclose(
                float(fidelity.get("capture_similarity", -1)), similarity,
                rel_tol=0, abs_tol=1e-12,
            )
            or (
                "small_text" in requirements
                and not math.isclose(
                    float(fidelity.get("small_text_luminance_stddev", -1)),
                    text_stddev, rel_tol=0, abs_tol=1e-9,
                )
            )
            or (
                "thin_strokes" in requirements
                and not math.isclose(
                    float(fidelity.get("thin_stroke_coverage", -1)),
                    stroke_coverage, rel_tol=0, abs_tol=1e-12,
                )
            )
        ):
            raise V2EvidenceError("cell fidelity differs from retained capture pixels")
    except (StopIteration, KeyError, TypeError, ValueError) as error:
        if isinstance(error, V2EvidenceError):
            raise
        raise V2EvidenceError(f"retained trace/capture validation failed: {error}") from error
    return paths


def result_artifact_errors(
    result: dict[str, Any], manifest: dict[str, Any], evidence_root: Path,
    trace_analyzer: dict[str, str],
) -> list[str]:
    errors: list[str] = []
    seen_paths: dict[Path, str] = {}
    seen_files: dict[tuple[int, int], str] = {}
    cells = [*result.get("cells", []), *result.get("repeat_cells", [])]
    for cell in cells:
        label = f"{cell.get('campaign')}/{cell_key(cell)}"
        try:
            for path in validate_cell_artifacts(
                cell, manifest, evidence_root, trace_analyzer
            ):
                predecessor = seen_paths.get(path)
                if predecessor is not None:
                    raise V2EvidenceError(
                        f"artifact path is reused across cells: {predecessor} and {label}"
                    )
                seen_paths[path] = label
                metadata = os.stat(path, follow_symlinks=False)
                file_identity_key = (metadata.st_dev, metadata.st_ino)
                predecessor = seen_files.get(file_identity_key)
                if predecessor is not None:
                    raise V2EvidenceError(
                        f"artifact file is hard-linked across cells: {predecessor} and {label}"
                    )
                seen_files[file_identity_key] = label
        except (OSError, TypeError, ValueError, V2EvidenceError) as error:
            errors.append(f"{label}: {error}")
    expected_paths = sum(
        len(ARTIFACT_KINDS) + 3 * len(cell.get("daw_subreceipts", []))
        for cell in cells
    )
    if len(cells) == 168 and len(seen_paths) != expected_paths:
        errors.append(
            f"v2 retained artifact inventory is not exactly {expected_paths} unique files"
        )
    return errors


def _atomic_bytes(path: Path, payload: bytes, mode: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise V2EvidenceError(f"runner state target is not a regular file: {path}")
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile("wb", dir=path.parent, delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        temporary.chmod(mode)
        os.replace(temporary, path)
        temporary = None
        if os.name != "nt":
            directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def initialize_run_key(run_dir: Path) -> None:
    """Create a local corruption-detection key, never a terminal authority."""
    root = _checked_root(run_dir, "runner")
    path = root / RUN_KEY
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as error:
        raise V2EvidenceError("runner integrity key already exists or is unsafe") from error
    try:
        payload = secrets.token_bytes(32)
        if os.write(descriptor, payload) != len(payload):
            raise V2EvidenceError("short write while creating runner integrity key")
        os.fsync(descriptor)
        os.fchmod(descriptor, 0o400)
    finally:
        os.close(descriptor)


def _run_key(run_dir: Path) -> bytes:
    path = checked_regular_path(run_dir, RUN_KEY, "runner integrity key")
    descriptor, metadata = _open_regular(path, "runner integrity key")
    try:
        if metadata.st_mode & 0o077:
            raise V2EvidenceError("runner integrity key is accessible outside its owner")
        payload = os.read(descriptor, 64)
        if os.read(descriptor, 1):
            raise V2EvidenceError("runner integrity key has an invalid length")
    finally:
        os.close(descriptor)
    if len(payload) != 32:
        raise V2EvidenceError("runner integrity key has an invalid length")
    return payload


def write_integrity_state(run_dir: Path, state: dict[str, Any]) -> str:
    """Persist state with local integrity detection; receipts remain authoritative."""
    if state.get("schema") != RUN_STATE_SCHEMA or state.get("version") != 2:
        raise V2EvidenceError("refusing to integrity-seal an unsupported runner state")
    key = _run_key(run_dir)
    state_digest = canonical_sha256(state)
    tag = hmac.new(key, canonical_bytes(state), hashlib.sha256).hexdigest()
    integrity = {
        "schema": RUN_INTEGRITY_SCHEMA,
        "version": 1,
        "state_sha256": state_digest,
        "hmac_sha256": tag,
    }
    envelope = {
        "schema": RUN_ENVELOPE_SCHEMA,
        "version": 1,
        "state": state,
        "integrity": integrity,
    }
    _atomic_bytes(
        run_dir / RUN_ENVELOPE,
        json.dumps(envelope, sort_keys=True, indent=2).encode("utf-8") + b"\n",
        0o400,
    )
    return state_digest


def read_integrity_state(run_dir: Path) -> tuple[dict[str, Any], str]:
    """Read integrity-checked state, which finalize must still fully rederive."""
    envelope_path = run_dir / RUN_ENVELOPE
    if envelope_path.exists() or envelope_path.is_symlink():
        envelope, _, _ = regular_json(
            run_dir, RUN_ENVELOPE, "A4 v2 runner state envelope"
        )
        if (
            not isinstance(envelope, dict)
            or set(envelope) != {"schema", "version", "state", "integrity"}
            or envelope.get("schema") != RUN_ENVELOPE_SCHEMA
            or envelope.get("version") != 1
            or not isinstance(envelope.get("state"), dict)
            or not isinstance(envelope.get("integrity"), dict)
        ):
            raise V2EvidenceError("runner state envelope is malformed")
        state = envelope["state"]
        integrity = envelope["integrity"]
    else:
        # Read-only compatibility for runs created before envelope publication.
        # All new writes use RUN_ENVELOPE, so no current writer can expose a
        # mixed state/integrity pair through this migration path.
        state, _, _ = regular_json(run_dir, RUN_STATE, "legacy A4 v2 runner state")
        integrity, _, _ = regular_json(
            run_dir, RUN_INTEGRITY, "legacy A4 v2 runner state integrity receipt"
        )
    if state.get("schema") != RUN_STATE_SCHEMA or state.get("version") != 2:
        raise V2EvidenceError("unsupported A4 v2 runner state")
    if (
        integrity.get("schema") != RUN_INTEGRITY_SCHEMA
        or integrity.get("version") != 1
        or set(integrity) != {"schema", "version", "state_sha256", "hmac_sha256"}
    ):
        raise V2EvidenceError("runner state integrity receipt is malformed")
    digest = canonical_sha256(state)
    expected = hmac.new(_run_key(run_dir), canonical_bytes(state), hashlib.sha256).hexdigest()
    if integrity.get("state_sha256") != digest or not hmac.compare_digest(
        str(integrity.get("hmac_sha256", "")), expected
    ):
        raise V2EvidenceError("runner state integrity check failed")
    return state, digest
