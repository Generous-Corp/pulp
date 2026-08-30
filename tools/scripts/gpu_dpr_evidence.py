#!/usr/bin/env python3
"""Private secure-file and typed-evidence helpers for the A4 DPR runner."""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import stat
import statistics
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from typing import Any

import gpu_first_visible_a3_acceptance as a3_acceptance

ROOT = Path(__file__).resolve().parent.parent.parent
RECEIPT_SCHEMA = "pulp.gpu-dpr-cell-receipt.v1"
RAW_SCHEMA = "pulp.gpu-dpr-raw-samples.v1"
COMPLETE_OUTCOMES = {"pass", "fail"}
INCOMPLETE_OUTCOMES = {"skip", "inconclusive"}
ALL_OUTCOMES = COMPLETE_OUTCOMES | INCOMPLETE_OUTCOMES
ARTIFACT_KINDS = {
    "capture", "reference_capture", "trace", "raw_samples", "input_receipt"
}
FIDELITY_CACHE: dict[
    tuple[str, str, str, float, str],
    tuple[int, int, bool, float, float, float],
] = {}


def decode_png_rgba(path: Path) -> tuple[int, int, bytes]:
    data = regular_file_bytes(path, "PNG fidelity artifact")
    if len(data) > 128 * 1024 * 1024:
        raise EvidenceError("PNG fidelity artifact exceeds the 128 MiB cap")
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise EvidenceError("fidelity artifact is not PNG")
    try:
        from PIL import Image, UnidentifiedImageError
        import io
        try:
            with Image.open(io.BytesIO(data)) as image:
                if image.mode not in {"RGB", "RGBA"}:
                    raise EvidenceError("PNG fidelity format is unsupported")
                image.load()
                rgba_image = image.convert("RGBA")
                width, height = rgba_image.size
                if (
                    width <= 0 or height <= 0 or width > 16_384 or height > 16_384
                    or width * height > 40_000_000
                ):
                    raise EvidenceError("PNG fidelity dimensions are unsupported")
                return width, height, rgba_image.tobytes()
        except (UnidentifiedImageError, OSError, ValueError) as error:
            raise EvidenceError("PNG fidelity data is corrupt") from error
    except ImportError:
        pass
    offset = 8
    width = height = color = depth = interlace = None
    packed = bytearray()
    saw_iend = False
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        if offset + 12 + length > len(data):
            raise EvidenceError("PNG chunk escapes artifact")
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])[0]
        if zlib.crc32(kind + payload) & 0xffffffff != expected_crc:
            raise EvidenceError("PNG chunk checksum is invalid")
        if kind == b"IHDR":
            if width is not None or length != 13:
                raise EvidenceError("PNG IHDR is malformed")
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            packed.extend(payload)
            if len(packed) > 128 * 1024 * 1024:
                raise EvidenceError("PNG compressed pixels exceed the 128 MiB cap")
        elif kind == b"IEND":
            saw_iend = True
            break
        offset += 12 + length
    if (
        not saw_iend or not packed or not width or not height
        or width > 16_384 or height > 16_384 or width * height > 40_000_000
        or depth != 8 or color not in {2, 6} or interlace != 0
    ):
        raise EvidenceError("PNG fidelity format is unsupported")
    channels = 4 if color == 6 else 3
    stride = width * channels
    expected_size = (stride + 1) * height
    try:
        decompressor = zlib.decompressobj()
        raw = decompressor.decompress(bytes(packed), expected_size + 1)
        raw += decompressor.flush()
    except zlib.error as error:
        raise EvidenceError("PNG fidelity data is corrupt") from error
    if len(raw) != expected_size or not decompressor.eof or decompressor.unused_data:
        raise EvidenceError("PNG fidelity data has unexpected size")
    rows: list[bytearray] = []
    cursor = 0
    for _ in range(height):
        filter_type = raw[cursor]; cursor += 1
        encoded = raw[cursor:cursor + stride]; cursor += stride
        if filter_type == 0:
            rows.append(bytearray(encoded))
            continue
        row = bytearray(stride); prior = rows[-1] if rows else bytearray(stride)
        for index, value in enumerate(encoded):
            left = row[index - channels] if index >= channels else 0
            up = prior[index]
            upper_left = prior[index - channels] if index >= channels else 0
            if filter_type == 0: predictor = 0
            elif filter_type == 1: predictor = left
            elif filter_type == 2: predictor = up
            elif filter_type == 3: predictor = (left + up) // 2
            elif filter_type == 4:
                p = left + up - upper_left; pa=abs(p-left); pb=abs(p-up); pc=abs(p-upper_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
            else: raise EvidenceError("PNG fidelity filter is unsupported")
            row[index] = (value + predictor) & 0xff
        rows.append(row)
    # Color type 6 rows are already RGBA.  Joining them directly is both
    # byte-exact and important for the dependency-free path used by CI: the
    # per-pixel slice loop below can take minutes on large evidence captures.
    if channels == 4:
        return width, height, b"".join(rows)
    rgba = bytearray(width * height * 4)
    out = 0
    for row in rows:
        for index in range(0, stride, channels):
            rgba[out:out + 3] = row[index:index + 3]
            rgba[out + 3] = 255
            out += 4
    return width, height, bytes(rgba)


def recompute_fidelity(
    capture_path: Path, reference_path: Path, scenario: dict[str, Any], dpr: float,
) -> tuple[int, int, bool, float, float, float]:
    width, height, capture = decode_png_rgba(capture_path)
    rw, rh, reference = decode_png_rgba(reference_path)
    if (width, height) != (rw, rh):
        raise EvidenceError("fidelity images differ in size")
    cache_key = (
        hashlib.sha256(capture).hexdigest(), hashlib.sha256(reference).hexdigest(),
        json.dumps(scenario.get("fidelity_oracle") or {}, sort_keys=True), dpr,
        str(scenario.get("kind", "")),
    )
    if cache_key in FIDELITY_CACHE:
        return FIDELITY_CACHE[cache_key]
    pixels = width * height
    regions = scenario.get("fidelity_oracle") or {}
    def samples(region_name: str) -> list[tuple[int, int, int]]:
        region = regions.get(region_name)
        if not isinstance(region, dict):
            return []
        x0 = round(region["x"] * dpr)
        y0 = round(region["y"] * dpr)
        x1 = round((region["x"] + region["width"]) * dpr)
        y1 = round((region["y"] + region["height"]) * dpr)
        if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
            raise EvidenceError(f"{region_name} escapes the committed capture")
        return [
            tuple(capture[(y * width + x) * 4:(y * width + x) * 4 + 3])
            for y in range(y0, y1) for x in range(x0, x1)
        ]
    try:
        import numpy as np
        capture_pixels = np.frombuffer(capture, dtype=np.uint8).reshape((-1, 4))
        reference_pixels = np.frombuffer(reference, dtype=np.uint8).reshape((-1, 4))
        similarity = float(np.all(capture_pixels == reference_pixels, axis=1).mean())
        packed_rgb = (
            capture_pixels[:, 0].astype(np.uint32) << 16
            | capture_pixels[:, 1].astype(np.uint32) << 8
            | capture_pixels[:, 2].astype(np.uint32)
        )
        _, full_counts = np.unique(packed_rgb, return_counts=True)
        non_background = (pixels - int(full_counts.max())) / pixels
        if scenario.get("kind") == "maintained_web_canary":
            content_floor = len(full_counts) > 4 and non_background >= 0.001
        else:
            full_luminance = (
                0.2126 * capture_pixels[:, 0] + 0.7152 * capture_pixels[:, 1]
                + 0.0722 * capture_pixels[:, 2]
            )
            content_floor = (
                len(np.unique(capture_pixels.view(np.uint32).reshape(-1))) >= 16
                and float(full_luminance.std()) >= 1.0
                and non_background >= 0.05
                and float((capture_pixels[:, 3] >= 250).mean()) >= 0.95
            )
        def roi_array(region_name: str) -> Any:
            region = regions.get(region_name)
            if not isinstance(region, dict):
                return np.empty((0, 3), dtype=np.uint8)
            x0 = round(region["x"] * dpr)
            y0 = round(region["y"] * dpr)
            x1 = round((region["x"] + region["width"]) * dpr)
            y1 = round((region["y"] + region["height"]) * dpr)
            if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
                raise EvidenceError(f"{region_name} escapes the committed capture")
            return capture_pixels.reshape((height, width, 4))[y0:y1, x0:x1, :3].reshape((-1, 3))
        text_pixels = roi_array("small_text_roi")
        stroke_pixels = roi_array("thin_stroke_roi")
        if len(text_pixels):
            luminance = (
                0.2126 * text_pixels[:, 0] + 0.7152 * text_pixels[:, 1]
                + 0.0722 * text_pixels[:, 2]
            )
            text_stddev = float(luminance.std())
        else:
            text_stddev = 0.0
        if len(stroke_pixels):
            packed_strokes = (
                stroke_pixels[:, 0].astype(np.uint32) << 16
                | stroke_pixels[:, 1].astype(np.uint32) << 8
                | stroke_pixels[:, 2].astype(np.uint32)
            )
            _, stroke_counts = np.unique(packed_strokes, return_counts=True)
            stroke_coverage = (
                len(stroke_pixels) - int(stroke_counts.max())
            ) / len(stroke_pixels)
        else:
            stroke_coverage = 0.0
    except ImportError:
        capture_words = memoryview(capture).cast("I")
        reference_words = memoryview(reference).cast("I")
        similarity = sum(a == b for a, b in zip(capture_words, reference_words)) / pixels
        rgb_histogram: dict[int, int] = {}
        rgba_histogram: dict[int, int] = {}
        full_luminance: list[float] = []
        opaque = 0
        for index, value in enumerate(capture_words):
            rgb = int(value) & 0x00ffffff
            rgb_histogram[rgb] = rgb_histogram.get(rgb, 0) + 1
            rgba_histogram[int(value)] = rgba_histogram.get(int(value), 0) + 1
            offset = index * 4
            r, g, b, a = capture[offset:offset + 4]
            full_luminance.append(0.2126*r + 0.7152*g + 0.0722*b)
            opaque += a >= 250
        non_background = (pixels - max(rgb_histogram.values())) / pixels
        if scenario.get("kind") == "maintained_web_canary":
            content_floor = len(rgb_histogram) > 4 and non_background >= 0.001
        else:
            content_floor = (
                len(rgba_histogram) >= 16
                and statistics.pstdev(full_luminance) >= 1.0
                and non_background >= 0.05 and opaque / pixels >= 0.95
            )
        text = samples("small_text_roi")
        strokes = samples("thin_stroke_roi")
        luminance = [0.2126*r + 0.7152*g + 0.0722*b for r, g, b in text]
        text_stddev = statistics.pstdev(luminance) if luminance else 0.0
        stroke_coverage = 0.0
        if strokes:
            counts: dict[tuple[int, int, int], int] = {}
            for value in strokes:
                counts[value] = counts.get(value, 0) + 1
            stroke_coverage = (len(strokes) - max(counts.values())) / len(strokes)
    result = width, height, content_floor, similarity, text_stddev, stroke_coverage
    if len(FIDELITY_CACHE) >= 256:
        FIDELITY_CACHE.pop(next(iter(FIDELITY_CACHE)))
    FIDELITY_CACHE[cache_key] = result
    return result
TRACE_QUESTIONS = {"gpu-startup", "gpu-health", "gpu-probe"}
TRACE_ANALYSIS_SCHEMA = "pulp.trace-gpu-analysis.v1"
A2T_RECEIPT_SCHEMA = "pulp.gpu-trace-overhead-acceptance.v1"
A3_RECEIPT_SCHEMA = "dev.pulp.gpu-first-visible-a3-acceptance"
MEASUREMENT_ATTESTATION_SCHEMA = "pulp.gpu-dpr-native-measurement-attestation.v1"
BROWSER_MEASUREMENT_ATTESTATION_SCHEMA = "pulp.gpu-dpr-browser-measurement-attestation.v1"
SAME_PROCESS_FIELDS = {
    "adapter_identity", "capture", "frame_metrics", "memory_metrics",
    "logical_input", "trace_correlation",
}
NONCE_HEX_LENGTH = 32
METRIC_UNITS = {
    "cpu_frame_time": "ms",
    "gpu_frame_time": "ms",
    "first_frame_time": "ms",
    "interaction_latency": "ms",
    "render_target_bytes": "bytes",
    "resident_bytes": "bytes",
    "upload_bytes": "bytes",
}


class EvidenceError(ValueError):
    """A receipt exists but cannot support a measured A4 cell."""


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(value, sort_keys=True, indent=2) + "\n"
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False
    ) as handle:
        handle.write(payload)
        temporary = Path(handle.name)
    os.replace(temporary, path)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def regular_file_bytes(path: Path, label: str) -> bytes:
    """Read one non-symlink regular-file instance through a held descriptor."""
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise EvidenceError(f"{label} is not a readable regular file: {path}") from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise EvidenceError(f"{label} is not a regular file: {path}")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def regular_json(path: Path, label: str) -> Any:
    try:
        return json.loads(regular_file_bytes(path, label).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"{label} is not valid UTF-8 JSON: {path}") from error


def snapshot_file(
    source: Path, destination: Path, label: str, *, executable: bool = False,
    expected_sha256: str | None = None,
) -> str:
    """Copy the exact opened source bytes into a runner-owned immutable path."""
    data = regular_file_bytes(source, label)
    digest = hashlib.sha256(data).hexdigest()
    if expected_sha256 is not None and digest != expected_sha256:
        raise EvidenceError(f"{label} digest does not match its declared bytes")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        raise EvidenceError(f"runner snapshot already exists: {destination}")
    with tempfile.NamedTemporaryFile("wb", dir=destination.parent, delete=False) as handle:
        handle.write(data)
        temporary = Path(handle.name)
    temporary.chmod(0o555 if executable else 0o444)
    os.replace(temporary, destination)
    return digest


def valid_attempt_nonce(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == NONCE_HEX_LENGTH
        and all(character in "0123456789abcdef" for character in value)
    )


def cell_key(scenario_id: str, mode: str, dpr: float) -> str:
    rendered_dpr = str(int(dpr)) if float(dpr).is_integer() else str(dpr)
    return f"{scenario_id}__{mode}__dpr-{rendered_dpr}"


def parse_cell_key(key: str) -> tuple[str, str, float]:
    scenario, mode, dpr = key.rsplit("__", 2)
    return scenario, mode, float(dpr.removeprefix("dpr-"))


def scenario_map(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["id"]: item for item in manifest["scenarios"]}


def source_digest(
    scenario: dict[str, Any], manifest_path: Path, forge_sha: str | None
) -> str:
    if scenario.get("source_sha256"):
        return scenario["source_sha256"]
    source_value = scenario.get("source")
    if not isinstance(source_value, str) or not source_value:
        raise EvidenceError(f"{scenario.get('id', 'scenario')}: exact source is required")
    if scenario["kind"].startswith("maintained_"):
        fixture_override = manifest_path.parent / source_value
        source = fixture_override if fixture_override.is_file() else ROOT / source_value
        data = regular_file_bytes(source, f"{scenario['id']} maintained source")
        return hashlib.sha256(data).hexdigest()
    if scenario["kind"].startswith("external_forge"):
        if (
            not isinstance(forge_sha, str) or len(forge_sha) != 40
            or any(character not in "0123456789abcdef" for character in forge_sha)
        ):
            raise EvidenceError(f"{scenario['id']}: exact Forge SHA is required")
        return hashlib.sha256(f"{source_value}@{forge_sha}".encode("utf-8")).hexdigest()
    raise EvidenceError(f"{scenario['id']}: source_sha256 is required")


def cell_directory(run_dir: Path, key: str) -> Path:
    return run_dir / "cells" / key


def checked_cell_directory(run_dir: Path, key: str, *, create: bool = False) -> Path:
    cells_root = run_dir / "cells"
    if cells_root.is_symlink():
        raise EvidenceError("cells directory must not be a symlink")
    if create:
        cells_root.mkdir(parents=True, exist_ok=True)
    cell_dir = cells_root / key
    if cell_dir.is_symlink():
        raise EvidenceError(f"cell directory must not be a symlink: {key}")
    if create:
        cell_dir.mkdir(parents=False, exist_ok=True)
    root = run_dir.resolve()
    resolved = cell_dir.resolve()
    if root not in resolved.parents:
        raise EvidenceError(f"cell directory escapes the run: {key}")
    return cell_dir


def safe_artifact(root_dir: Path, relative: str) -> Path:
    if not isinstance(relative, str) or not relative:
        raise EvidenceError("artifact path is missing")
    lexical = root_dir / relative
    if lexical.is_symlink():
        raise EvidenceError(f"artifact must not be a symlink: {relative}")
    candidate = lexical.resolve()
    root = root_dir.resolve()
    if candidate != root and root not in candidate.parents:
        raise EvidenceError(f"artifact escapes its cell directory: {relative}")
    if not candidate.is_file() or not stat.S_ISREG(candidate.stat().st_mode):
        raise EvidenceError(f"artifact is missing: {relative}")
    return candidate


def new_frozen_evidence_directory(run_dir: Path, key: str, nonce: str) -> Path:
    frozen_root = run_dir / "frozen-evidence"
    if frozen_root.is_symlink():
        raise EvidenceError("frozen-evidence directory must not be a symlink")
    frozen_root.mkdir(parents=True, exist_ok=True)
    key_root = frozen_root / key
    if key_root.is_symlink():
        raise EvidenceError(f"frozen cell directory must not be a symlink: {key}")
    key_root.mkdir(exist_ok=True)
    evidence_dir = key_root / nonce
    if evidence_dir.exists() or evidence_dir.is_symlink():
        raise EvidenceError("attempt evidence snapshot already exists")
    evidence_dir.mkdir()
    if run_dir.resolve() not in evidence_dir.resolve().parents:
        raise EvidenceError("attempt evidence snapshot escapes the run")
    return evidence_dir


def nearest_rank_p95(samples: list[float]) -> float:
    ordered = sorted(samples)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


def metric_statistic(name: str, metric: Any, manifest: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(metric, dict) or set(metric) != {"provenance", "definition", "samples"}:
        raise EvidenceError(f"raw metric {name} lacks typed provenance")
    provenance = metric.get("provenance")
    definition = metric.get("definition")
    samples = metric.get("samples")
    if provenance not in {"measured", "derived", "unavailable"}:
        raise EvidenceError(f"raw metric {name} has invalid provenance")
    if not isinstance(definition, str) or not definition.strip():
        raise EvidenceError(f"raw metric {name} lacks a definition")
    if provenance == "unavailable":
        if samples != []:
            raise EvidenceError(f"unavailable raw metric {name} carries samples")
        return {
            "unit": METRIC_UNITS[name], "provenance": provenance,
            "definition": definition, "median": None, "p95": None,
            "sample_count": 0,
        }
    if not isinstance(samples, list) or not samples:
        raise EvidenceError(f"raw metric {name} has no samples")
    if any(isinstance(value, bool) or not isinstance(value, (int, float)) for value in samples):
        raise EvidenceError(f"raw metric {name} contains a non-number")
    values = [float(value) for value in samples]
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise EvidenceError(f"raw metric {name} contains an invalid value")
    if name == "gpu_frame_time" and any(value <= 0 for value in values):
        raise EvidenceError("raw metric gpu_frame_time contains a missing zero sample")
    trial = manifest["trial_contract"]
    minimum = (
        trial["fresh_process_first_frame_trials"]
        if name == "first_frame_time"
        else trial["measured_trials"]
    )
    if len(values) < minimum:
        raise EvidenceError(f"raw metric {name} has {len(values)} samples; needs {minimum}")
    median = statistics.median(values)
    return {
        "unit": METRIC_UNITS[name],
        "provenance": provenance,
        "definition": definition,
        "median": median,
        "p95": max(median, nearest_rank_p95(values)),
        "sample_count": len(values),
    }


def validate_fresh_process_ledger(
    raw: dict[str, Any], receipt: dict[str, Any], manifest: dict[str, Any]
) -> None:
    """Fail closed unless every first-frame sample has fresh bound provenance."""
    producer_pid = raw.get("producer_pid")
    trials = raw.get("fresh_process_trials")
    expected_count = manifest["trial_contract"]["fresh_process_first_frame_trials"]
    first_frame = raw.get("metrics", {}).get("first_frame_time", {})
    samples = first_frame.get("samples") if isinstance(first_frame, dict) else None
    attempt_number = receipt.get("attempt_number")
    if (
        isinstance(producer_pid, bool) or not isinstance(producer_pid, int)
        or producer_pid <= 0
        or isinstance(attempt_number, bool) or not isinstance(attempt_number, int)
        or attempt_number <= 0
        or not isinstance(trials, list) or len(trials) != expected_count
        or not isinstance(samples, list) or len(samples) != expected_count
    ):
        raise EvidenceError("fresh-process first-frame ledger shape is invalid")
    build = receipt.get("build_identity", {})
    producer = build.get("measurement_producer") or build.get("binary")
    if not isinstance(producer, dict) or not isinstance(producer.get("sha256"), str):
        raise EvidenceError("fresh-process ledger lacks exact producer identity")
    expected_adapter = receipt.get("adapter")
    expected_keys = {
        "schema", "version", "attempt_nonce", "attempt_number", "pid",
        "producer_sha256", "content_digest", "pulp_sha",
        "first_frame_time_ms", "adapter",
    }
    expected_build_sha = None
    if receipt.get("scenario_kind") == "maintained_web_canary":
        expected_keys.add("build_sha256")
        expected_build_sha = build.get("web_ui_bundle_sha256")
    pids: set[int] = set()
    for index, (trial, sample) in enumerate(zip(trials, samples)):
        if not isinstance(trial, dict) or set(trial) != expected_keys:
            raise EvidenceError(f"fresh-process trial {index} has an invalid contract")
        pid = trial.get("pid")
        measured = trial.get("first_frame_time_ms")
        if (
            trial.get("schema") != "pulp.gpu-dpr-first-frame-trial.v1"
            or trial.get("version") != 1
            or trial.get("attempt_nonce") != receipt.get("attempt_nonce")
            or trial.get("attempt_number") != attempt_number
            or isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0
            or pid == producer_pid or pid in pids
            or trial.get("producer_sha256") != producer.get("sha256")
            or trial.get("content_digest") != receipt.get("content_digest")
            or trial.get("pulp_sha") != build.get("pulp_sha")
            or trial.get("build_sha256") != expected_build_sha
            or trial.get("adapter") != expected_adapter
            or isinstance(measured, bool) or not isinstance(measured, (int, float))
            or not math.isfinite(float(measured)) or float(measured) < 0
            or isinstance(sample, bool) or not isinstance(sample, (int, float))
            or float(sample) != float(measured)
        ):
            raise EvidenceError(
                f"fresh-process trial {index} is not bound to this cell attempt"
            )
        pids.add(pid)


def validate_logical_input(
    raw: dict[str, Any], scenario: dict[str, Any], observed_dpr: float
) -> bool:
    trials = raw.get("logical_input_trials")
    if not isinstance(trials, list) or not trials:
        raise EvidenceError("logical-input oracle has no trials")
    oracle = scenario.get("logical_input_oracle")
    if not isinstance(oracle, dict):
        raise EvidenceError("scenario lacks an independent logical-input oracle")
    expected_point = oracle.get("point")
    expected_target = oracle.get("target")
    expected_physical = [round(float(value) * observed_dpr, 6) for value in expected_point]
    passed = True
    for trial in trials:
        expected = trial.get("expected_logical")
        observed = trial.get("observed_logical")
        requested_physical = trial.get("requested_physical")
        observed_physical = trial.get("observed_physical")
        if not (
            isinstance(expected, list) and isinstance(observed, list)
            and isinstance(requested_physical, list) and isinstance(observed_physical, list)
            and all(len(value) == 2 for value in (
                expected, observed, requested_physical, observed_physical
            ))
            and all(
                isinstance(value, (int, float)) and not isinstance(value, bool)
                for value in [*expected, *observed, *requested_physical, *observed_physical]
            )
        ):
            raise EvidenceError("logical-input trial is malformed")
        if expected != expected_point or trial.get("expected_target") != expected_target:
            raise EvidenceError("logical-input trial differs from the frozen scenario oracle")
        if not all(
            math.isclose(float(left), float(right), abs_tol=1e-6)
            for left, right in zip(requested_physical, expected_physical)
        ):
            raise EvidenceError("logical-input request does not apply the observed DPR")
        mapped = [float(value) / observed_dpr for value in observed_physical]
        coordinate_match = all(
            math.isclose(float(left), float(right), abs_tol=max(1e-6, 0.5 / observed_dpr))
            for left, right in zip(expected, mapped)
        ) and all(
            math.isclose(float(left), float(right), abs_tol=1e-6)
            for left, right in zip(observed, mapped)
        )
        passed = passed and coordinate_match and trial.get("event_received") is True and (
            expected_target == trial.get("observed_target")
        )
    return passed


def validate_gpu_timer_calibration(raw: dict[str, Any], manifest: dict[str, Any]) -> None:
    calibration = raw.get("gpu_timer_calibration")
    required = {
        "schema", "version", "clock", "resolution_ms", "baseline_samples_ms",
        "extra_work_samples_ms", "extra_work_multiplier", "control_detected",
    }
    if not isinstance(calibration, dict) or set(calibration) != required:
        raise EvidenceError("GPU timer calibration is missing or malformed")
    trials = manifest["trial_contract"]["gpu_timer_calibration_trials"]
    multiplier = manifest["trial_contract"]["gpu_timer_extra_work_multiplier"]
    baseline = calibration.get("baseline_samples_ms")
    extra = calibration.get("extra_work_samples_ms")
    resolution = calibration.get("resolution_ms")
    if (
        calibration.get("schema") != "pulp.gpu-dpr-timer-calibration.v1"
        or calibration.get("version") != 1
        or calibration.get("clock") not in {"dawn-gpu-timestamp", "webgl2-timer-query"}
        or calibration.get("extra_work_multiplier") != multiplier
        or calibration.get("control_detected") is not True
        or not isinstance(baseline, list) or len(baseline) != trials
        or not isinstance(extra, list) or len(extra) != trials
        or isinstance(resolution, bool) or not isinstance(resolution, (int, float))
        or not math.isfinite(float(resolution)) or float(resolution) <= 0
        or any(isinstance(value, bool) or not isinstance(value, (int, float))
               or not math.isfinite(float(value)) or float(value) <= 0
               for value in [*baseline, *extra])
    ):
        raise EvidenceError("GPU timer calibration contract is invalid")
    baseline_median = statistics.median(float(value) for value in baseline)
    extra_median = statistics.median(float(value) for value in extra)
    detectable = max(float(resolution) * 2.0, baseline_median * 0.10)
    if extra_median < baseline_median + detectable:
        raise EvidenceError("GPU timer known-extra-work control was not detected")


def validate_adaptive_evidence(
    raw: dict[str, Any], mode: str, manifest: dict[str, Any]
) -> None:
    evidence = raw.get("adaptive_policy_evidence")
    if mode != "adaptive_simulation":
        if evidence is not None:
            raise EvidenceError("non-adaptive cell carries adaptive policy evidence")
        return
    profile = manifest["adaptive_profile"]
    if not isinstance(evidence, dict) or set(evidence) != {
        "profile", "budget_ms", "initial_scale", "final_scale", "observations"
    }:
        raise EvidenceError("adaptive cell lacks exact typed transition evidence")
    if evidence["profile"] != profile:
        raise EvidenceError("adaptive evidence differs from the ratified manifest profile")
    downshift_frames = profile["downshift_after_over_budget_frames"]
    upshift_frames = profile["upshift_after_under_budget_frames"]
    budget = evidence.get("budget_ms")
    observations = evidence.get("observations")
    if (
        isinstance(budget, bool) or not isinstance(budget, (int, float)) or budget <= 0
        or not isinstance(observations, list)
        or len(observations) != downshift_frames + upshift_frames
    ):
        raise EvidenceError("adaptive driver observations are incomplete")
    down = observations[:downshift_frames]
    up = observations[downshift_frames:]
    expected_keys = {
        "phase", "frame_index", "sample_ms", "scale_before", "scale_after", "transition"
    }
    for index, observation in enumerate(observations):
        if not isinstance(observation, dict) or set(observation) != expected_keys:
            raise EvidenceError(f"adaptive observation {index} is malformed")
    if any(item["phase"] != "over-budget" or item["sample_ms"] <= budget for item in down):
        raise EvidenceError("adaptive downshift was not driven by over-budget measurements")
    if any(
        item["phase"] != "under-budget"
        or item["sample_ms"] > budget * profile["upshift_budget_fraction"]
        for item in up
    ):
        raise EvidenceError("adaptive upshift was not driven by under-budget measurements")
    if any(item["transition"] is not False or item["scale_after"] != item["scale_before"] for item in down[:-1]):
        raise EvidenceError("adaptive policy transitioned before its downshift boundary")
    down_action = down[-1]["transition"]
    if not (
        down_action == "downshift" and down[-1]["scale_after"] < down[-1]["scale_before"]
        or down_action == "floor-hold" and down[-1]["scale_after"] == down[-1]["scale_before"]
            == min(profile["scale_ladder"])
    ):
        raise EvidenceError("adaptive policy did not apply its downshift boundary")
    if any(item["transition"] is not False or item["scale_after"] != item["scale_before"] for item in up[:-1]):
        raise EvidenceError("adaptive policy transitioned before its upshift boundary")
    up_action = up[-1]["transition"]
    if not (
        up_action == "upshift" and up[-1]["scale_after"] > up[-1]["scale_before"]
        or up_action == "ceiling-hold" and up[-1]["scale_after"] == up[-1]["scale_before"]
            == max(profile["scale_ladder"])
    ):
        raise EvidenceError("adaptive policy did not apply its upshift boundary")
    if (
        evidence.get("initial_scale") != down[0]["scale_before"]
        or evidence.get("final_scale") != up[-1]["scale_after"]
        or up[0]["scale_before"] != down[-1]["scale_after"]
    ):
        raise EvidenceError("adaptive scale observations do not form one state-machine sequence")


def exact_executable(identity: Any, label: str) -> tuple[Path, str]:
    if not isinstance(identity, dict):
        raise EvidenceError(f"{label} identity is missing")
    path_value = identity.get("path")
    digest = identity.get("sha256")
    if not isinstance(path_value, str) or not path_value:
        raise EvidenceError(f"{label} path is missing")
    path = Path(path_value)
    if (
        not path.is_absolute() or path.is_symlink()
        or not path.is_file() or not os.access(path, os.X_OK)
    ):
        raise EvidenceError(f"{label} is not an executable absolute path")
    actual = hashlib.sha256(regular_file_bytes(path, label)).hexdigest()
    if digest != actual:
        raise EvidenceError(f"{label} digest does not match its executable bytes")
    return path, actual


def exact_regular_file(identity: Any, label: str) -> tuple[Path, str]:
    if not isinstance(identity, dict) or set(identity) != {"path", "sha256"}:
        raise EvidenceError(f"{label} identity is missing")
    path = Path(identity["path"])
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise EvidenceError(f"{label} identity is not an absolute regular file")
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if identity["sha256"] != actual:
        raise EvidenceError(f"{label} digest does not match the file bytes")
    return path, actual


def validate_browser_product(path: Path, product: Any) -> None:
    if not isinstance(product, dict) or set(product) != {
        "version", "codesign_identifier", "team_identifier",
    }:
        raise EvidenceError("browser product identity is missing")
    completed = subprocess.run(
        [str(path), "--version"], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=15,
    )
    version = completed.stdout.strip()
    if (
        completed.returncode != 0 or product.get("version") != version
        or not any(name in version for name in ("Google Chrome", "Chromium"))
    ):
        raise EvidenceError("browser product version is not bound to Chrome/Chromium")
    if sys.platform == "darwin":
        verified = subprocess.run(
            ["/usr/bin/codesign", "--verify", "--strict", str(path)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
        )
        details = subprocess.run(
            ["/usr/bin/codesign", "-dv", "--verbose=4", str(path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
        )
        expected = product.get("codesign_identifier")
        team = product.get("team_identifier")
        if (
            verified.returncode != 0 or details.returncode != 0
            or expected not in {"com.google.Chrome", "org.chromium.Chromium"}
            or f"Identifier={expected}" not in details.stdout
            or not isinstance(team, str) or not team
            or team == "not set" or f"TeamIdentifier={team}" not in details.stdout
            or (expected == "com.google.Chrome" and team != "EQHXZ8M8AV")
        ):
            raise EvidenceError("browser product signature is not accepted")


def analyze_trace(
    analyzer_identity: dict[str, str], trace_path: Path, expected_evidence_id: str,
) -> tuple[list[str], set[str], dict[str, Any]]:
    analyzer, _ = exact_executable(analyzer_identity, "runner-pinned trace analyzer")
    evidence_ids: list[str] = []
    observed_categories: set[str] = set()
    category_scope: dict[str, Any] | None = None
    for question in sorted(TRACE_QUESTIONS):
        completed = subprocess.run(
            [str(analyzer), "trace", question, "--trace", str(trace_path.resolve()), "--json"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120,
        )
        try:
            result = json.loads(completed.stdout)
        except (json.JSONDecodeError, TypeError) as error:
            raise EvidenceError(
                f"trace analyzer returned invalid JSON for {question}: {error}"
            ) from error
        if not isinstance(result, dict):
            raise EvidenceError(f"trace analyzer result for {question} is not an object")
        verdict = result.get("verdict")
        expected_exit = {
            "pass": 0, "fail": 1, "unavailable": 2, "unverified": 2
        }.get(verdict)
        if (
            result.get("schema") != TRACE_ANALYSIS_SCHEMA
            or result.get("question") != question
            or expected_exit is None or completed.returncode != expected_exit
        ):
            raise EvidenceError(f"trace analyzer result/exit contract failed for {question}")
        if question in {"gpu-health", "gpu-probe"} and verdict != "pass":
            raise EvidenceError(f"trace question {question} did not pass")
        if question == "gpu-startup" and verdict not in {"pass", "unverified"}:
            raise EvidenceError("trace question gpu-startup is unavailable or failed")
        question_ids = result.get("evidence_ids")
        if question_ids != [expected_evidence_id]:
            raise EvidenceError(
                f"trace analyzer must return the cell attempt nonce as the only evidence id "
                f"for {question}"
            )
        for evidence_id in question_ids:
            if evidence_id not in evidence_ids:
                evidence_ids.append(evidence_id)
        categories = result.get("observed_categories")
        if (
            not isinstance(categories, list)
            or any(not isinstance(item, str) or not item for item in categories)
            or categories != sorted(set(categories))
        ):
            raise EvidenceError(f"trace analyzer did not derive categories for {question}")
        scope = result.get("category_scope")
        if (
            not isinstance(scope, dict)
            or set(scope) != {"evidence_id", "process_upid", "process_pid"}
            or scope.get("evidence_id") != expected_evidence_id
            or isinstance(scope.get("process_upid"), bool)
            or not isinstance(scope.get("process_upid"), int)
            or scope["process_upid"] < 0
            or isinstance(scope.get("process_pid"), bool)
            or not isinstance(scope.get("process_pid"), int)
            or scope["process_pid"] < 0
        ):
            raise EvidenceError(
                f"trace categories are not scoped to the cell evidence/process for {question}"
            )
        if category_scope is not None and scope != category_scope:
            raise EvidenceError("trace questions resolved different category process scopes")
        category_scope = scope
        observed_categories.update(categories)
    if category_scope is None:
        raise EvidenceError("trace analyzer returned no correlated category scope")
    return evidence_ids, observed_categories, category_scope


def validate_trace(
    raw: dict[str, Any], manifest: dict[str, Any], trace_path: Path,
    analyzer_identity: dict[str, str], expected_evidence_id: str,
    scenario_kind: str,
) -> list[str]:
    trace = raw.get("trace")
    if not isinstance(trace, dict) or trace.get("complete") is not True:
        raise EvidenceError("trace receipt is absent or incomplete")
    if scenario_kind == "maintained_web_canary":
        expected_trace = {
            "complete", "kind", "process_pid",
        }
        if set(trace) != expected_trace or trace.get("kind") != "browser-devtools":
            raise EvidenceError("web trace metadata lacks the typed DevTools contract")
        try:
            document = json.loads(regular_file_bytes(trace_path, "browser trace"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise EvidenceError("browser trace is not valid Chrome trace JSON") from error
        events = document.get("traceEvents") if isinstance(document, dict) else None
        if not isinstance(events, list):
            raise EvidenceError("browser trace lacks traceEvents")
        marker_prefix = f"pulp.dpr.{expected_evidence_id}."
        categories: set[str] = set()
        pids: set[int] = set()
        foreign_ids: set[str] = set()
        marker_re = re.compile(r"^pulp\.dpr\.([0-9a-f]{32})\.([a-z]+)$")
        for event in events:
            if not isinstance(event, dict):
                continue
            name = event.get("name")
            if not isinstance(name, str):
                continue
            match = marker_re.fullmatch(name)
            if not match:
                continue
            evidence_id, category = match.groups()
            if evidence_id != expected_evidence_id:
                foreign_ids.add(evidence_id)
                continue
            pid = event.get("pid")
            duration = event.get("dur")
            if (
                event.get("ph") not in {"X", "b", "e", "R", "I"}
                or isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0
                or (event.get("ph") == "X" and (
                    isinstance(duration, bool)
                    or not isinstance(duration, (int, float)) or duration < 0
                ))
            ):
                raise EvidenceError("browser trace contains a malformed correlated span")
            categories.add(category)
            pids.add(pid)
        if foreign_ids:
            raise EvidenceError("browser trace contains a foreign DPR evidence id")
        if len(pids) != 1 or next(iter(pids), None) != trace.get("process_pid"):
            raise EvidenceError("browser trace categories are not scoped to one renderer process")
        missing = set(manifest["trial_contract"]["required_trace_categories"]) - categories
        if missing:
            raise EvidenceError(f"trace categories are missing: {sorted(missing)}")
        return [expected_evidence_id]
    if set(trace) != {"complete"}:
        raise EvidenceError("trace metadata must be derived by the pinned analyzer")
    evidence_ids, categories, _ = analyze_trace(
        analyzer_identity, trace_path, expected_evidence_id
    )
    missing = set(manifest["trial_contract"]["required_trace_categories"]) - categories
    if missing:
        raise EvidenceError(f"trace categories are missing: {sorted(missing)}")
    return evidence_ids


def validate_identity(
    receipt: dict[str, Any], scenario: dict[str, Any], plan: dict[str, Any]
) -> None:
    machine = receipt.get("machine")
    adapter = receipt.get("adapter")
    if not isinstance(machine, dict) or not machine.get("id"):
        raise EvidenceError("machine receipt lacks an id")
    if not machine.get("os") or not machine.get("architecture"):
        raise EvidenceError("machine receipt lacks OS/architecture identity")
    if not isinstance(adapter, dict) or adapter.get("class") not in {
        "hardware", "software", "unknown"
    }:
        raise EvidenceError("adapter receipt lacks a valid class")
    if not all(adapter.get(field) for field in ("name", "backend", "driver")):
        raise EvidenceError("adapter receipt lacks name/backend/driver identity")
    build = receipt.get("build_identity")
    if not isinstance(build, dict) or build.get("pulp_sha") != plan["pulp_sha"]:
        raise EvidenceError("receipt is not bound to the planned Pulp SHA")

    browser = scenario.get("kind") == "maintained_web_canary"
    attestation = receipt.get("measurement_attestation")
    if browser and attestation is None:
        raise EvidenceError("browser measurement attestation is missing")
    if attestation is not None:
        expected_schema = (
            BROWSER_MEASUREMENT_ATTESTATION_SCHEMA
            if browser else MEASUREMENT_ATTESTATION_SCHEMA
        )
        if (
            not isinstance(attestation, dict)
            or attestation.get("schema") != expected_schema
            or attestation.get("audio_device_opened") is not False
        ):
            raise EvidenceError("measurement attestation is malformed")
        same_process = attestation.get("same_process")
        if (
            not isinstance(same_process, dict)
            or set(same_process) != SAME_PROCESS_FIELDS
            or any(same_process[field] is not True for field in SAME_PROCESS_FIELDS)
        ):
            raise EvidenceError("native measurement attestation is not fully same-process")
        producer, producer_digest = exact_executable(
            build.get("measurement_producer"),
            "browser executable" if browser else "native measurement producer",
        )
        if attestation.get("producer_sha256") != producer_digest:
            raise EvidenceError("measurement attestation names different producer bytes")
        if browser:
            product_executable, product_digest = exact_executable(
                build.get("browser_product_executable"),
                "browser product executable",
            )
            if product_digest != producer_digest:
                raise EvidenceError("browser product and frozen producer bytes differ")
            validate_browser_product(
                product_executable, build.get("browser_product")
            )
            script, script_digest = exact_executable(
                build.get("measurement_script"), "browser measurement script"
            )
            del script
            if attestation.get("script_sha256") != script_digest:
                raise EvidenceError("browser attestation names different script bytes")
            web_artifacts = build.get("web_ui_artifacts")
            if not isinstance(web_artifacts, dict) or set(web_artifacts) != {
                "PulpSuperConvolverUi.js", "PulpSuperConvolverUi.wasm",
                "PulpSuperConvolverUi.data",
            }:
                raise EvidenceError("browser build artifact identity is missing")
            binding = ""
            for name in sorted(web_artifacts):
                path, digest = exact_regular_file(web_artifacts[name], f"browser build {name}")
                del path
                binding += f"{name}:{digest}\n"
            build_digest = hashlib.sha256(binding.encode()).hexdigest()
            if (
                build.get("web_ui_bundle_sha256") != build_digest
                or attestation.get("build_sha256") != build_digest
            ):
                raise EvidenceError("browser build identity differs from measured bytes")

    requirements = set(scenario["required_oracles"])
    if "authentic_gpu" in requirements:
        if adapter["class"] != "hardware" or adapter.get("authentic_identity") is not True:
            raise EvidenceError("scenario requires an authentic hardware GPU adapter")
    if "authentic_webgl" in requirements:
        identity = " ".join(str(adapter.get(key, "")) for key in ("name", "driver"))
        software = any(
            marker in identity.lower()
            for marker in ("swiftshader", "llvmpipe", "software", "lavapipe")
        )
        if (
            adapter["class"] != "hardware" or adapter.get("api") != "webgl2"
            or adapter.get("authentic_identity") is not True or software
        ):
            raise EvidenceError("web canary requires authentic WebGL2 hardware evidence")
    if "exact_binary_identity" in requirements:
        exact_executable(build.get("binary"), "Forge binary")
        if build.get("forge_sha") != plan["forge_sha"] or plan["forge_sha"] is None:
            raise EvidenceError("Forge receipt is not bound to the planned Forge SHA")
    if "exact_plugin_format" in requirements:
        if receipt.get("plugin_format") not in {"au", "vst3", "clap"}:
            raise EvidenceError("DAW receipt lacks an exact plugin format")
        if receipt.get("format_qualified") is not True or receipt.get("scan_cache_confirmed") is not True:
            raise EvidenceError("DAW receipt did not prove format-qualified scan/load")
    if "audio_thread_excluded" in requirements and receipt.get("audio_thread_excluded") is not True:
        raise EvidenceError("scenario did not prove audio-thread exclusion")


def receipt_observation(
    receipt_path: Path, state: dict[str, Any], manifest: dict[str, Any],
    manifest_path: Path, run_dir: Path,
) -> tuple[str, dict[str, Any] | None, list[str]]:
    receipt = regular_json(receipt_path, "cell receipt")
    if receipt.get("schema") != RECEIPT_SCHEMA or receipt.get("version") != 1:
        raise EvidenceError("unsupported DPR cell receipt schema")
    outcome = receipt.get("outcome")
    if outcome not in ALL_OUTCOMES:
        raise EvidenceError("receipt outcome must be pass/fail/skip/inconclusive")
    if not valid_attempt_nonce(receipt.get("attempt_nonce")):
        raise EvidenceError("receipt lacks a valid runner attempt nonce")
    requested = receipt.get("requested_dpr")
    if isinstance(requested, bool) or not isinstance(requested, (int, float)):
        raise EvidenceError("receipt requested DPR is not numeric")
    key = cell_key(receipt.get("scenario_id", ""), receipt.get("mode", ""), float(requested))
    if key not in state["cells"]:
        raise EvidenceError(f"receipt names an unexpected matrix cell: {key}")
    cell = state["cells"][key]
    scenario = scenario_map(manifest)[cell["scenario_id"]]
    if receipt.get("scenario_kind") != scenario["kind"]:
        raise EvidenceError("receipt scenario kind differs from the corpus")

    reason = receipt.get("reason")
    dependencies = receipt.get("dependencies", [])
    if outcome in INCOMPLETE_OUTCOMES:
        if not isinstance(reason, str) or not reason:
            raise EvidenceError(f"{outcome} receipt requires a reason")
        if (
            not isinstance(dependencies, list) or not dependencies
            or any(not isinstance(item, str) or not item for item in dependencies)
        ):
            raise EvidenceError(f"{outcome} receipt requires explicit dependencies")
        return key, None, dependencies

    validate_identity(receipt, scenario, state["plan"])
    cell_dir = checked_cell_directory(run_dir, key)
    frozen_root = (run_dir / "frozen-evidence").resolve()
    resolved_receipt = receipt_path.resolve()
    artifact_root = receipt_path.parent if frozen_root in resolved_receipt.parents else cell_dir
    artifacts = receipt.get("artifacts")
    if not isinstance(artifacts, list):
        raise EvidenceError("receipt artifact list is missing")
    by_kind: dict[str, dict[str, Any]] = {}
    artifact_paths: dict[str, Path] = {}
    artifact_payloads: dict[str, bytes] = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict):
            raise EvidenceError("receipt contains a malformed artifact")
        kind = artifact.get("kind")
        if kind not in ARTIFACT_KINDS or kind in by_kind:
            raise EvidenceError(
                "artifact kinds must be unique capture/reference_capture/trace/raw_samples/input_receipt"
            )
        path = safe_artifact(artifact_root, artifact.get("path", ""))
        digest = artifact.get("sha256")
        artifact_bytes = regular_file_bytes(path, f"{kind} artifact")
        if hashlib.sha256(artifact_bytes).hexdigest() != digest:
            raise EvidenceError(f"{kind} artifact digest does not match its bytes")
        by_kind[kind] = {
            "kind": kind,
            "path": str(path.relative_to(run_dir.resolve())),
            "sha256": digest,
        }
        artifact_paths[kind] = path
        artifact_payloads[kind] = artifact_bytes
    if set(by_kind) != ARTIFACT_KINDS:
        raise EvidenceError(
            "cell requires capture/reference_capture/trace/raw_samples/input_receipt artifacts"
        )

    raw_artifact = next(item for item in artifacts if item["kind"] == "raw_samples")
    safe_artifact(artifact_root, raw_artifact["path"])
    try:
        raw = json.loads(artifact_payloads["raw_samples"].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EvidenceError("raw-samples artifact is not valid UTF-8 JSON") from error
    if raw.get("schema") != RAW_SCHEMA or raw.get("version") != 1:
        raise EvidenceError("unsupported raw-sample schema")
    metrics_raw = raw.get("metrics")
    if not isinstance(metrics_raw, dict) or set(metrics_raw) != set(METRIC_UNITS):
        raise EvidenceError("raw samples do not cover the exact metric set")
    metrics = {
        name: metric_statistic(name, metrics_raw[name], manifest) for name in METRIC_UNITS
    }
    validate_gpu_timer_calibration(raw, manifest)
    validate_fresh_process_ledger(raw, receipt, manifest)
    trace_artifact = next(item for item in artifacts if item["kind"] == "trace")
    trace_path = safe_artifact(artifact_root, trace_artifact["path"])
    trace_ids = validate_trace(
        raw, manifest, trace_path, state["trace_analyzer"], receipt["attempt_nonce"],
        scenario["kind"],
    )

    observed_dpr = receipt.get("observed_dpr")
    if (
        isinstance(observed_dpr, bool)
        or not isinstance(observed_dpr, (int, float))
        or observed_dpr <= 0
    ):
        raise EvidenceError("receipt lacks a positive observed DPR")
    logical_input_passed = validate_logical_input(raw, scenario, float(observed_dpr))
    validate_adaptive_evidence(raw, cell["mode"], manifest)
    fidelity_raw = raw.get("fidelity")
    if not isinstance(fidelity_raw, dict):
        raise EvidenceError("fidelity oracle receipt is missing")
    comparison = fidelity_raw.get("comparison")
    capture_digest = by_kind["capture"]["sha256"]
    reference_digest = by_kind["reference_capture"]["sha256"]
    if (
        not isinstance(comparison, dict)
        or comparison.get("method") not in {
            "pulp-png-pixel-comparison", "cpu-raster-scaled-vs-webgl-canvas"
        }
        or comparison.get("reference_sha256") != reference_digest
        or comparison.get("capture_sha256") != capture_digest
        or not isinstance(comparison.get("same_content_token"), str)
        or not comparison["same_content_token"]
    ):
        raise EvidenceError("fidelity comparison is not bound to same-content artifacts")
    text_contrast = fidelity_raw.get("small_text_luminance_stddev")
    stroke_coverage = fidelity_raw.get("thin_stroke_coverage")
    required_oracles = set(scenario.get("required_oracles", []))
    required_regions = scenario.get("fidelity_oracle")
    if required_oracles & {"small_text", "thin_strokes"}:
        if fidelity_raw.get("oracle_regions") != required_regions:
            raise EvidenceError("fidelity measurements are not bound to frozen oracle regions")
    if (
        isinstance(text_contrast, bool) or not isinstance(text_contrast, (int, float))
        or not math.isfinite(float(text_contrast)) or text_contrast < 0
        or isinstance(stroke_coverage, bool) or not isinstance(stroke_coverage, (int, float))
        or not math.isfinite(float(stroke_coverage)) or not 0 <= stroke_coverage <= 1
    ):
        raise EvidenceError("fidelity text/stroke measurements are malformed")
    (
        capture_width, capture_height, recomputed_content_floor,
        recomputed_similarity, recomputed_text_contrast,
        recomputed_stroke_coverage,
    ) = recompute_fidelity(
        artifact_paths["capture"], artifact_paths["reference_capture"],
        scenario, float(observed_dpr),
    )
    physical = receipt.get("physical_size")
    if physical != {"width": capture_width, "height": capture_height}:
        raise EvidenceError("committed capture dimensions differ from the receipt")
    reported_similarity = fidelity_raw.get("capture_similarity")
    if (
        fidelity_raw.get("content_floor_passed") is not recomputed_content_floor
        or isinstance(reported_similarity, bool)
        or not isinstance(reported_similarity, (int, float))
        or not math.isclose(
            float(reported_similarity), recomputed_similarity,
            rel_tol=0.0, abs_tol=1e-6,
        )
        or not math.isclose(
            float(text_contrast), recomputed_text_contrast,
            rel_tol=0.0, abs_tol=1e-6,
        )
        or not math.isclose(
            float(stroke_coverage), recomputed_stroke_coverage,
            rel_tol=0.0, abs_tol=1e-9,
        )
    ):
        raise EvidenceError("producer fidelity metrics differ from committed PNG artifacts")
    fidelity = {
        "content_floor_passed": recomputed_content_floor,
        "capture_similarity": recomputed_similarity,
        "small_text_legible": (
            "small_text" not in required_oracles
            or recomputed_text_contrast >= manifest["trial_contract"][
                "small_text_luminance_stddev_minimum"
            ]
        ),
        "thin_strokes_preserved": (
            "thin_strokes" not in required_oracles
            or recomputed_stroke_coverage >= manifest["trial_contract"][
                "thin_stroke_coverage_minimum"
            ]
        ),
        "logical_input_correct": logical_input_passed,
    }
    if (
        isinstance(fidelity["capture_similarity"], bool)
        or not isinstance(fidelity["capture_similarity"], (int, float))
        or not 0 <= fidelity["capture_similarity"] <= 1
    ):
        raise EvidenceError("capture similarity is outside 0..1")
    similarity_minimum = manifest["trial_contract"].get("capture_similarity_minimum")
    if (
        isinstance(similarity_minimum, bool)
        or not isinstance(similarity_minimum, (int, float))
        or not 0 <= similarity_minimum <= 1
    ):
        raise EvidenceError("manifest lacks a ratified capture-similarity minimum")
    similarity_passed = fidelity["capture_similarity"] >= similarity_minimum
    all_fidelity = all(
        fidelity[name]
        for name in (
            "content_floor_passed", "small_text_legible",
            "thin_strokes_preserved", "logical_input_correct",
        )
    ) and similarity_passed
    if outcome == "pass" and not all_fidelity:
        raise EvidenceError("pass receipt contains a failing fidelity oracle")
    if outcome == "fail" and all_fidelity:
        raise EvidenceError("fail receipt contains no failing fidelity oracle")

    requested_dpr = float(cell["requested_dpr"])
    configured_max: float | None = None
    adaptive: str | None = None
    if cell["mode"] == "exact":
        expected_dpr = requested_dpr
    elif cell["mode"] == "configured_max":
        configured_max = float(manifest["configured_max_dpr"])
        expected_dpr = min(requested_dpr, configured_max)
    else:
        adaptive = manifest["adaptive_profile"]["id"]
        expected_dpr = requested_dpr
    if not math.isclose(float(observed_dpr), expected_dpr, abs_tol=1e-9):
        raise EvidenceError("observed DPR differs from the requested experiment mode")
    logical = scenario["logical_size"]
    physical = receipt.get("physical_size")
    expected_physical = {
        "width": round(logical["width"] * expected_dpr),
        "height": round(logical["height"] * expected_dpr),
    }
    if physical != expected_physical:
        raise EvidenceError("physical size does not equal logical size x observed DPR")
    expected_content = source_digest(scenario, manifest_path, state["plan"]["forge_sha"])
    if receipt.get("content_digest") != expected_content:
        raise EvidenceError("receipt content digest differs from the frozen scenario")

    observation = {
        "scenario_id": cell["scenario_id"],
        "mode": cell["mode"],
        "requested_dpr": cell["requested_dpr"],
        "observed_dpr": observed_dpr,
        "configured_max_dpr": configured_max,
        "adaptive_profile_id": adaptive,
        "logical_size": logical,
        "physical_size": physical,
        "content_digest": expected_content,
        "machine_id": receipt["machine"]["id"],
        "adapter_class": receipt["adapter"]["class"],
        "metrics": metrics,
        "fidelity": fidelity,
        "trace_evidence_ids": trace_ids,
        "artifacts": [by_kind[kind] for kind in sorted(by_kind)],
    }
    return key, observation, []


def snapshot_receipt_bundle(
    run_dir: Path, state: dict[str, Any], receipt_path: Path, expected_nonce: str,
) -> tuple[Path, str]:
    """Freeze a receipt and every referenced input before semantic validation."""
    receipt = regular_json(receipt_path, "cell receipt")
    nonce = receipt.get("attempt_nonce")
    if not valid_attempt_nonce(nonce):
        raise EvidenceError("receipt lacks a valid runner attempt nonce")
    if nonce != expected_nonce:
        raise EvidenceError("receipt attempt nonce does not match the runner attempt")
    requested = receipt.get("requested_dpr")
    if isinstance(requested, bool) or not isinstance(requested, (int, float)):
        raise EvidenceError("receipt requested DPR is not numeric")
    key = cell_key(receipt.get("scenario_id", ""), receipt.get("mode", ""), float(requested))
    if key not in state["cells"]:
        raise EvidenceError(f"receipt names an unexpected matrix cell: {key}")
    if state.get("issued_attempts", {}).get(nonce) != key:
        raise EvidenceError("attempt nonce was not issued by this runner for this cell")
    cell_dir = checked_cell_directory(run_dir, key, create=True)
    evidence_dir = new_frozen_evidence_directory(run_dir, key, nonce)

    artifacts = receipt.get("artifacts")
    if receipt.get("outcome") in COMPLETE_OUTCOMES:
        if not isinstance(artifacts, list):
            raise EvidenceError("receipt artifact list is missing")
        rewritten: list[dict[str, Any]] = []
        for artifact in artifacts:
            if not isinstance(artifact, dict) or artifact.get("kind") not in ARTIFACT_KINDS:
                raise EvidenceError("receipt contains a malformed artifact")
            kind = artifact["kind"]
            source = safe_artifact(cell_dir, artifact.get("path", ""))
            suffix = source.suffix if source.suffix else ".bin"
            destination = evidence_dir / f"{kind}{suffix}"
            digest = snapshot_file(
                source, destination, f"{kind} artifact",
                expected_sha256=artifact.get("sha256"),
            )
            rewritten.append({"kind": kind, "path": destination.name, "sha256": digest})
        receipt["artifacts"] = rewritten

        build = receipt.get("build_identity")
        binary = build.get("binary") if isinstance(build, dict) else None
        manifest = load_json(Path(state["manifest_path"]))
        scenario = scenario_map(manifest)[state["cells"][key]["scenario_id"]]
        if isinstance(binary, dict) and "exact_binary_identity" in scenario["required_oracles"]:
            binary_path, binary_digest = exact_executable(binary, "Forge binary")
            pinned_binary = evidence_dir / "forge-binary"
            snapshot_file(
                binary_path, pinned_binary, "Forge binary", executable=True,
                expected_sha256=binary_digest,
            )
            binary["path"] = str(pinned_binary.resolve())
            binary["sha256"] = binary_digest
        if isinstance(build, dict) and receipt.get("measurement_attestation") is not None:
            producer_path, producer_digest = exact_executable(
                build.get("measurement_producer"), "native measurement producer"
            )
            pinned_producer = evidence_dir / "measurement-producer"
            snapshot_file(
                producer_path, pinned_producer, "native measurement producer",
                executable=True, expected_sha256=producer_digest,
            )
            build["measurement_producer"] = {
                "path": str(pinned_producer.resolve()),
                "sha256": producer_digest,
            }
            if scenario.get("kind") == "maintained_web_canary":
                script_path, script_digest = exact_executable(
                    build.get("measurement_script"), "browser measurement script"
                )
                pinned_script = evidence_dir / "browser-measurement-script"
                snapshot_file(
                    script_path, pinned_script, "browser measurement script",
                    executable=True, expected_sha256=script_digest,
                )
                build["measurement_script"] = {
                    "path": str(pinned_script.resolve()),
                    "sha256": script_digest,
                }
                web_artifacts = build.get("web_ui_artifacts")
                if not isinstance(web_artifacts, dict):
                    raise EvidenceError("browser build artifact identity is missing")
                for name in sorted(web_artifacts):
                    source, digest = exact_regular_file(
                        web_artifacts[name], f"browser build {name}"
                    )
                    pinned = evidence_dir / f"web-ui-{name}"
                    snapshot_file(
                        source, pinned, f"browser build {name}",
                        expected_sha256=digest,
                    )
                    web_artifacts[name] = {
                        "path": str(pinned.resolve()), "sha256": digest,
                    }

    receipt_snapshot = evidence_dir / "receipt.json"
    atomic_json(receipt_snapshot, receipt)
    receipt_snapshot.chmod(0o444)
    return receipt_snapshot, nonce


def validated_dependency_receipts(
    a2t_value: str, a3_budget_id: str, a3_value: str, plan: dict[str, Any],
) -> dict[str, str]:
    if not all(isinstance(value, str) and value for value in (a2t_value, a3_budget_id, a3_value)):
        raise EvidenceError("A2T receipt, A3 budget id, and A3 receipt must be non-empty")
    a2t_path = Path(a2t_value)
    a3_path = Path(a3_value)
    if not a2t_path.is_file() or not a3_path.is_file():
        raise EvidenceError("A2T and A3 dependencies must be real receipt files")
    a2t_bytes = regular_file_bytes(a2t_path, "A2T dependency receipt")
    try:
        a2t = json.loads(a2t_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EvidenceError("A2T dependency receipt is not valid JSON") from error
    if not isinstance(a2t, dict) or a2t.get("schema") != A2T_RECEIPT_SCHEMA:
        raise EvidenceError("A2T dependency has an unsupported receipt schema")
    a2t_digest = hashlib.sha256(a2t_bytes).hexdigest()

    a3_bytes = regular_file_bytes(a3_path, "A3 dependency receipt")
    try:
        a3 = json.loads(a3_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EvidenceError("A3 dependency receipt is not valid JSON") from error
    if not isinstance(a3, dict) or a3.get("schema") != A3_RECEIPT_SCHEMA:
        raise EvidenceError("A3 dependency has an unsupported receipt schema")
    try:
        terminal = a3_acceptance.validate_receipt(a3, a3_path.parent)
    except (a3_acceptance.AcceptanceError, OSError, subprocess.SubprocessError) as error:
        raise EvidenceError(f"A3 dependency failed its checked-in acceptance contract: {error}") from error
    if terminal is not True:
        raise EvidenceError("A3 dependency is not a terminal acceptance bundle")

    budget = a3["budget"]
    same_instance = a3["same_instance_a2t"]
    if (
        budget["id"] != a3_budget_id or budget["status"] != "ratified"
    ):
        raise EvidenceError("A3 receipt does not carry the matching ratified budget")
    if same_instance["a2t_receipt"]["sha256"] != a2t_digest:
        raise EvidenceError("A3 receipt does not bind the exact A2T receipt")

    causal_campaigns = [
        campaign for campaign in a3["campaigns"]
        if campaign["identity"]["campaign_id"] == same_instance["campaign_id"]
    ]
    if len(causal_campaigns) != 1:
        raise EvidenceError("validated A3 dependency lacks one causal campaign identity")
    identity = causal_campaigns[0]["identity"]
    binding = {
        "plan_revision": a3["plan"]["revision"],
        "pulp_sha": identity["pulp_revision"],
        "machine_id": identity["machine_id"],
        "build_id": identity["build_id"],
        "instance_id": identity["instance_id"],
    }
    if (
        binding["plan_revision"] != plan["plan_revision"]
        or binding["pulp_sha"] != plan["pulp_sha"]
    ):
        raise EvidenceError("A2T/A3 dependencies are not bound to this plan/build campaign")
    return {
        "a2t_receipt": f"sha256:{a2t_digest}",
        "a3_budget_id": a3_budget_id,
        "a3_receipt": f"sha256:{hashlib.sha256(a3_bytes).hexdigest()}",
    }
