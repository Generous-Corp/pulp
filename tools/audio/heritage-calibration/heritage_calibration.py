#!/usr/bin/env python3
"""Heritage capture verification, cyclic calibration, and blinded A/B packs."""

from __future__ import annotations

import argparse
from array import array
import bisect
import hashlib
import hmac
import json
import math
import os
import struct
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Optional, Sequence


CAPTURE_SCHEMA = "pulp.heritage.capture-session.v1"
BOOTSTRAP_SCHEMA = "pulp.heritage.cyclic-bootstrap.v1"
LISTENING_SCHEMA = "pulp.heritage.listening-pack.v1"
ANSWERS_SCHEMA = "pulp.heritage.listening-answers.v1"
TRADEMARK_NOTICE = (
    "Hardware manufacturer and product names are trademarks of their respective "
    "owners and identify measured equipment only. No affiliation or endorsement "
    "is implied."
)
FORBIDDEN_HOST_KEYS = {
    "computer", "computer_name", "development_host", "host", "host_machine",
    "hostname", "machine_name", "username", "workstation",
}
FORBIDDEN_HOST_SUFFIXES = {
    "buildhost", "capturehost", "computername", "developmenthost", "hostname",
    "machinename", "recordinghost", "username", "workstation",
}


class HeritageCalibrationError(ValueError):
    pass


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical_json(value))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _require(mapping: dict[str, Any], key: str, expected: type, where: str) -> Any:
    value = mapping.get(key)
    if not isinstance(value, expected) or (expected is str and not value.strip()):
        raise HeritageCalibrationError(f"{where}.{key} must be a non-empty {expected.__name__}")
    return value


def _reject_host_identity(value: Any, where: str = "manifest") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = key.lower().replace("-", "_")
            collapsed = normalized.replace("_", "")
            if ("host" in collapsed or normalized in FORBIDDEN_HOST_KEYS or
                    any(collapsed.endswith(suffix) for suffix in FORBIDDEN_HOST_SUFFIXES)):
                raise HeritageCalibrationError(
                    f"{where}.{key} records a development-host identity; omit it")
            _reject_host_identity(child, f"{where}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _reject_host_identity(child, f"{where}[{index}]")


def verify_capture_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HeritageCalibrationError(f"cannot read capture manifest: {exc}") from exc
    if not isinstance(manifest, dict) or manifest.get("schema") != CAPTURE_SCHEMA:
        raise HeritageCalibrationError(f"capture manifest schema must be {CAPTURE_SCHEMA}")
    _reject_host_identity(manifest)
    _require(manifest, "session_id", str, "manifest")
    captured_at = _require(manifest, "captured_at", str, "manifest")
    try:
        captured_time = datetime.fromisoformat(captured_at.replace("Z", "+00:00"))
    except ValueError as exc:
        raise HeritageCalibrationError("manifest.captured_at must be ISO-8601") from exc
    if captured_time.tzinfo is None:
        raise HeritageCalibrationError("manifest.captured_at must include a UTC offset")
    _require(manifest, "operator_id", str, "manifest")
    target = _require(manifest, "target", dict, "manifest")
    for key in ("manufacturer", "model", "serial", "revision"):
        _require(target, key, str, "manifest.target")
    conditions = _require(manifest, "conditions", dict, "manifest")
    _require(conditions, "psu_and_calibration_state", str, "manifest.conditions")
    _require(conditions, "gain_staging", str, "manifest.conditions")
    temperature = conditions.get("temperature_c")
    if (not isinstance(temperature, (int, float)) or isinstance(temperature, bool) or
            not math.isfinite(float(temperature))):
        raise HeritageCalibrationError("manifest.conditions.temperature_c must be finite")
    chain = _require(manifest, "capture_chain", list, "manifest")
    if not chain:
        raise HeritageCalibrationError("manifest.capture_chain must identify every signal-chain stage")
    for index, stage in enumerate(chain):
        where = f"manifest.capture_chain[{index}]"
        if not isinstance(stage, dict):
            raise HeritageCalibrationError(f"{where} must be an object")
        _require(stage, "stage", str, where)
        _require(stage, "description", str, where)
    artifacts = _require(manifest, "artifacts", list, "manifest")
    if not artifacts:
        raise HeritageCalibrationError("manifest.artifacts must not be empty")
    if manifest.get("trademark_notice") != TRADEMARK_NOTICE:
        raise HeritageCalibrationError("manifest.trademark_notice must use the standard notice")

    root = path.parent.resolve()
    seen: set[str] = set()
    verified: list[dict[str, Any]] = []
    for index, artifact in enumerate(artifacts):
        where = f"manifest.artifacts[{index}]"
        if not isinstance(artifact, dict):
            raise HeritageCalibrationError(f"{where} must be an object")
        relative = _require(artifact, "path", str, where)
        expected_hash = _require(artifact, "sha256", str, where).lower()
        _require(artifact, "test_id", str, where)
        _require(artifact, "role", str, where)
        if len(expected_hash) != 64 or any(c not in "0123456789abcdef" for c in expected_hash):
            raise HeritageCalibrationError(f"{where}.sha256 must be lowercase SHA-256")
        artifact_path = (root / relative).resolve()
        if root != artifact_path and root not in artifact_path.parents:
            raise HeritageCalibrationError(f"{where}.path escapes the session directory")
        if relative in seen:
            raise HeritageCalibrationError(f"duplicate artifact path: {relative}")
        seen.add(relative)
        if not artifact_path.is_file():
            raise HeritageCalibrationError(f"missing capture artifact: {relative}")
        actual_hash = sha256_file(artifact_path)
        if actual_hash != expected_hash:
            raise HeritageCalibrationError(f"SHA-256 mismatch for {relative}")
        expected_bytes = artifact.get("bytes")
        if not isinstance(expected_bytes, int) or expected_bytes != artifact_path.stat().st_size:
            raise HeritageCalibrationError(f"byte count mismatch for {relative}")
        verified.append({"path": relative, "sha256": actual_hash, "bytes": expected_bytes})
    return {"ok": True, "schema": CAPTURE_SCHEMA, "artifacts": verified}


@dataclass(frozen=True)
class WavData:
    sample_rate: int
    channels: int
    samples: list[float]

    @property
    def frames(self) -> int:
        return len(self.samples) // self.channels


def read_wav(path: Path) -> WavData:
    payload = path.read_bytes()
    if len(payload) < 12 or payload[:4] != b"RIFF" or payload[8:12] != b"WAVE":
        raise HeritageCalibrationError(f"not a RIFF/WAVE file: {path}")
    offset = 12
    fmt: Optional[bytes] = None
    audio: Optional[bytes] = None
    while offset + 8 <= len(payload):
        chunk_id = payload[offset:offset + 4]
        size = struct.unpack_from("<I", payload, offset + 4)[0]
        begin = offset + 8
        end = begin + size
        if end > len(payload):
            raise HeritageCalibrationError(f"truncated WAV chunk in {path}")
        if chunk_id == b"fmt ":
            fmt = payload[begin:end]
        elif chunk_id == b"data":
            audio = payload[begin:end]
        offset = end + (size & 1)
    if fmt is None or audio is None or len(fmt) < 16:
        raise HeritageCalibrationError(f"WAV needs fmt and data chunks: {path}")
    format_tag, channels, sample_rate, _, block_align, bits = struct.unpack_from("<HHIIHH", fmt)
    if format_tag == 0xFFFE and len(fmt) >= 40:
        format_tag = struct.unpack_from("<I", fmt, 24)[0]
    if channels < 1 or sample_rate < 1 or block_align < 1:
        raise HeritageCalibrationError(f"invalid WAV format values: {path}")
    if bits % 8 or block_align != channels * (bits // 8):
        raise HeritageCalibrationError(f"unsupported WAV block alignment: {path}")
    if len(audio) % block_align:
        raise HeritageCalibrationError(f"WAV data is not frame aligned: {path}")
    samples: list[float]
    if format_tag == 3 and bits == 32:
        decoded = array("f")
        decoded.frombytes(audio)
        if sys.byteorder != "little":
            decoded.byteswap()
        samples = decoded.tolist()
    elif format_tag == 1 and bits == 16:
        decoded_int16 = array("h")
        decoded_int16.frombytes(audio)
        if sys.byteorder != "little":
            decoded_int16.byteswap()
        samples = [v / 32768.0 for v in decoded_int16]
    elif format_tag == 1 and bits == 24:
        samples = []
        for index in range(0, len(audio), 3):
            raw = int.from_bytes(audio[index:index + 3], "little", signed=False)
            if raw & 0x800000:
                raw -= 1 << 24
            samples.append(raw / 8388608.0)
    elif format_tag == 1 and bits == 32:
        decoded_int32 = array("i")
        decoded_int32.frombytes(audio)
        if sys.byteorder != "little":
            decoded_int32.byteswap()
        samples = [v / 2147483648.0 for v in decoded_int32]
    else:
        raise HeritageCalibrationError(
            f"unsupported WAV encoding tag={format_tag} bits={bits}: {path}")
    if len(samples) % channels or not all(math.isfinite(v) for v in samples):
        raise HeritageCalibrationError(f"WAV contains invalid samples: {path}")
    return WavData(sample_rate, channels, samples)


def write_float_wav(path: Path, wav: WavData) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = array("f", wav.samples)
    if sys.byteorder != "little":
        encoded.byteswap()
    data = encoded.tobytes()
    byte_rate = wav.sample_rate * wav.channels * 4
    fmt = struct.pack("<HHIIHH", 3, wav.channels, wav.sample_rate, byte_rate,
                      wav.channels * 4, 32)
    fact = struct.pack("<I", wav.frames)
    riff_size = 4 + 8 + len(fmt) + 8 + len(fact) + 8 + len(data)
    path.write_bytes(
        b"RIFF" + struct.pack("<I", riff_size) + b"WAVE" +
        b"fmt " + struct.pack("<I", len(fmt)) + fmt +
        b"fact" + struct.pack("<I", len(fact)) + fact +
        b"data" + struct.pack("<I", len(data)) + data)


def _rms(samples: Sequence[float]) -> float:
    return math.sqrt(math.fsum(value * value for value in samples) / max(1, len(samples)))


def _round_anchor(value: float) -> int:
    return math.floor(value + 0.5)


def make_indexed_impulse_basis(frames: int) -> list[float]:
    if frames < 2:
        raise HeritageCalibrationError("impulse basis needs at least two frames")
    # Each sample is the coefficient of one unit impulse in the discrete-time
    # basis. The monotonic coefficient identifies the source index exactly.
    return [0.05 + 0.9 * index / (frames - 1) for index in range(frames)]


def make_sparse_impulse_fixture(frames: int) -> list[float]:
    source = [0.0] * frames
    amplitudes = (1.0, -0.75, 0.5, -0.25)
    for sequence, index in enumerate(range(19, frames, 37)):
        source[index] = amplitudes[sequence % len(amplitudes)]
    return source


def oracle_cyclic(source: list[float], output_frames: int, factor: float,
                  cycle_frames: int, splice_frames: int,
                  *, wrong_law: bool = False) -> list[float]:
    if not (0.25 <= factor <= 20.0) or cycle_frames < 4:
        raise HeritageCalibrationError("invalid cyclic fixture dimensions")
    if splice_frames < 0 or splice_frames > cycle_frames // 2:
        raise HeritageCalibrationError("splice_frames must be in [0, cycle_frames/2]")
    output: list[float] = []
    for frame in range(output_frames):
        cycle = frame // cycle_frames
        phase = frame % cycle_frames
        if wrong_law:
            anchor = _round_anchor(cycle * cycle_frames * factor)
        else:
            anchor = _round_anchor(cycle * cycle_frames / factor)
        position = anchor + phase
        value = source[position] if position < len(source) else 0.0
        if cycle > 0 and phase < splice_frames:
            previous = (_round_anchor((cycle - 1) * cycle_frames * factor)
                        if wrong_law else
                        _round_anchor((cycle - 1) * cycle_frames / factor))
            old_position = previous + cycle_frames + phase
            old = source[old_position] if old_position < len(source) else 0.0
            weight = 1.0 if splice_frames == 1 else phase / (splice_frames - 1)
            value = old + (value - old) * weight
        output.append(value)
    return output


def recover_cyclic(source: list[float], capture: list[float]) -> dict[str, Any]:
    if len(source) < 2 or len(capture) < 128:
        raise HeritageCalibrationError("cyclic recovery needs a >=128-frame capture")
    step = source[1] - source[0]
    if step == 0.0:
        raise HeritageCalibrationError("cyclic recovery requires the indexed impulse basis")
    deviations = [abs((capture[i] - capture[i - 1]) - step) for i in range(1, len(capture))]
    threshold = max(abs(step) * 0.1, 1.0e-7)
    anomaly = [index + 1 for index, value in enumerate(deviations) if value > threshold]
    runs: list[tuple[int, int]] = []
    for index in anomaly:
        if not runs or index > runs[-1][1] + 1:
            runs.append((index, index))
        else:
            runs[-1] = (runs[-1][0], index)
    starts = [begin for begin, _ in runs if begin > 1]
    if len(starts) < 4:
        raise HeritageCalibrationError("could not find repeated cyclic splice boundaries")
    spacings = [b - a for a, b in zip(starts, starts[1:])]
    cycle_frames = sorted(spacings)[len(spacings) // 2]
    aligned = [(begin, end) for begin, end in runs
               if abs((begin - 1) % cycle_frames) <= 1]
    if len(aligned) < 3:
        raise HeritageCalibrationError("splice anomalies are not cycle-periodic")
    splice_frames = sorted(end - begin + 2 for begin, end in aligned)[len(aligned) // 2]
    splice_frames = min(splice_frames, cycle_frames // 2)
    anchors: list[int] = []
    cycle_indices: list[int] = []
    for cycle in range(1, len(capture) // cycle_frames):
        phase = min(max(splice_frames, 1), cycle_frames - 1)
        index = cycle * cycle_frames + phase
        if index >= len(capture):
            break
        insertion = bisect.bisect_left(source, capture[index])
        candidates = [position for position in (insertion - 1, insertion)
                      if 0 <= position < len(source)]
        source_position = min(candidates, key=lambda position: abs(
            source[position] - capture[index]))
        anchor = source_position - phase
        if 0 <= anchor < len(source):
            cycle_indices.append(cycle)
            anchors.append(anchor)
    if len(anchors) < 3:
        raise HeritageCalibrationError("could not decode cyclic source anchors")
    denominator = math.fsum(k * k for k in cycle_indices)
    anchor_slope = math.fsum(k * anchor for k, anchor in zip(cycle_indices, anchors)) / denominator
    factor = cycle_frames / anchor_slope
    predicted = [_round_anchor(k * cycle_frames / factor) for k in cycle_indices]
    anchor_error = max(abs(a - b) for a, b in zip(anchors, predicted))
    return {
        "factor": factor,
        "cycle_frames": cycle_frames,
        "splice_frames": splice_frames,
        "anchor_max_error_frames": anchor_error,
        "cycles_observed": len(anchors),
    }


def _product_render(renderer: Path, out_dir: Path, fixture: str, factor: float,
                    cycle_frames: int, splice_frames: int,
                    output_frames: int) -> tuple[Path, Path, list[float], list[float]]:
    source_path = out_dir / f"{fixture}-source.wav"
    capture_path = out_dir / f"{fixture}-product-capture.wav"
    command = [
        str(renderer), "--source-out", str(source_path), "--capture-out", str(capture_path),
        "--fixture", fixture, "--factor", str(factor), "--cycle-frames", str(cycle_frames),
        "--splice-frames", str(splice_frames), "--output-frames", str(output_frames),
    ]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if completed.returncode != 0:
        raise HeritageCalibrationError(
            "Pulp cyclic renderer failed: " + (completed.stderr.strip() or completed.stdout.strip()))
    source_wav = read_wav(source_path)
    capture_wav = read_wav(capture_path)
    if ((source_wav.sample_rate, source_wav.channels) != (48000, 1) or
            (capture_wav.sample_rate, capture_wav.channels) != (48000, 1) or
            capture_wav.frames != output_frames):
        raise HeritageCalibrationError("Pulp cyclic renderer returned an invalid WAV contract")
    return source_path, capture_path, source_wav.samples, capture_wav.samples


def bootstrap_cyclic(out_dir: Path, factor: float, cycle_frames: int,
                     splice_frames: int, renderer: Path) -> dict[str, Any]:
    if abs(factor - 1.0) < 1.0e-12:
        raise HeritageCalibrationError(
            "factor 1 is transparent and cannot identify a cyclic snap law")
    if splice_frames < 2:
        raise HeritageCalibrationError(
            "splice widths 0 and 1 are behaviorally indistinguishable in this fixture")
    if not renderer.is_file():
        raise HeritageCalibrationError(f"Pulp cyclic renderer is unavailable: {renderer}")
    if out_dir.exists() and any(out_dir.iterdir()):
        raise HeritageCalibrationError("cyclic bootstrap output directory must be empty")
    out_dir.mkdir(parents=True, exist_ok=True)
    output_frames = cycle_frames * 24
    source_path, capture_path, source, capture = _product_render(
        renderer, out_dir, "indexed", factor, cycle_frames, splice_frames, output_frames)
    recovered = recover_cyclic(source, capture)
    impulse_source_path, impulse_capture_path, impulse_source, impulse_capture = _product_render(
        renderer, out_dir, "sparse", factor, cycle_frames, splice_frames, output_frames)
    impulse_prediction = oracle_cyclic(
        impulse_source, output_frames, recovered["factor"],
        recovered["cycle_frames"], recovered["splice_frames"])
    impulse_error = max(abs(a - b) for a, b in zip(impulse_capture, impulse_prediction))
    wrong_impulse = oracle_cyclic(
        impulse_source, output_frames, factor, cycle_frames, splice_frames, wrong_law=True)
    wrong_impulse_error = max(abs(a - b) for a, b in zip(impulse_capture, wrong_impulse))
    wrong_rejected = wrong_impulse_error >= 0.1
    impulse_holdout_passed = impulse_error <= 1.0e-6
    passed = (
        abs(recovered["factor"] - factor) <= 0.02 and
        recovered["cycle_frames"] == cycle_frames and
        recovered["splice_frames"] == splice_frames and
        recovered["anchor_max_error_frames"] <= 1 and
        impulse_holdout_passed and wrong_rejected)
    report = {
        "schema": BOOTSTRAP_SCHEMA,
        "passed": passed,
        "fixture": {
            "kind": "indexed-unit-impulse-basis",
            "sample_rate": 48000,
            "factor": factor,
            "cycle_frames": cycle_frames,
            "splice_frames": splice_frames,
        },
        "recovered": recovered,
        "impulse_holdout": {
            "matched": impulse_holdout_passed,
            "maximum_error": impulse_error,
            "wrong_law_maximum_error": wrong_impulse_error,
        },
        "negative_control": {
            "kind": "input-domain-cycle-spacing",
            "rejected": wrong_rejected,
            "maximum_error": wrong_impulse_error,
        },
        "artifacts": [
            {"path": source_path.name, "sha256": sha256_file(source_path)},
            {"path": capture_path.name, "sha256": sha256_file(capture_path)},
            {"path": impulse_source_path.name, "sha256": sha256_file(impulse_source_path)},
            {"path": impulse_capture_path.name, "sha256": sha256_file(impulse_capture_path)},
        ],
        "renderer": {"path": renderer.name, "sha256": sha256_file(renderer)},
        "scope": "Calibration-pipeline bootstrap only; this is not a G1-G3 product-engine gate.",
    }
    write_json(out_dir / "report.json", report)
    return report


def _load_pairs(path: Path) -> list[dict[str, str]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HeritageCalibrationError(f"cannot read pair manifest: {exc}") from exc
    if not isinstance(document, dict) or document.get("schema") != "pulp.heritage.listening-pairs.v1":
        raise HeritageCalibrationError("pair manifest schema must be pulp.heritage.listening-pairs.v1")
    pairs = _require(document, "pairs", list, "pairs")
    result: list[dict[str, str]] = []
    ids: set[str] = set()
    for index, pair in enumerate(pairs):
        where = f"pairs[{index}]"
        if not isinstance(pair, dict):
            raise HeritageCalibrationError(f"{where} must be an object")
        pair_id = _require(pair, "pair_id", str, where)
        if pair_id in ids:
            raise HeritageCalibrationError(f"duplicate pair_id: {pair_id}")
        ids.add(pair_id)
        result.append({
            "pair_id": pair_id,
            "reference": _require(pair, "reference", str, where),
            "candidate": _require(pair, "candidate", str, where),
        })
    if not result:
        raise HeritageCalibrationError("pair manifest must not be empty")
    return result


def _assignment(key: bytes, pair_id: str) -> tuple[str, str]:
    digest = hmac.new(key, f"pulp-heritage-ab-v1\0{pair_id}".encode(), hashlib.sha256).digest()
    return ("reference", "candidate") if digest[0] & 1 == 0 else ("candidate", "reference")


def _load_key(path: Path) -> bytes:
    key = path.read_bytes()
    if len(key) < 16:
        raise HeritageCalibrationError("listening key must contain at least 16 bytes")
    return key


def _answers_mac(key: bytes, schema: str, key_id: str,
                 answers: list[dict[str, Any]]) -> str:
    authenticated = {"schema": schema, "key_id": key_id, "answers": answers}
    return hmac.new(key, canonical_json(authenticated), hashlib.sha256).hexdigest()


def generate_listening_pack(pairs_path: Path, out_dir: Path, answers_path: Path,
                            key_path: Path) -> dict[str, Any]:
    pairs = _load_pairs(pairs_path)
    key = _load_key(key_path)
    root = pairs_path.parent.resolve()
    resolved_out = out_dir.resolve()
    resolved_answers = answers_path.resolve()
    resolved_key = key_path.resolve()
    resolved_pairs = pairs_path.resolve()
    source_paths: dict[str, tuple[Path, Path]] = {}
    for pair in pairs:
        reference_path = (root / pair["reference"]).resolve()
        candidate_path = (root / pair["candidate"]).resolve()
        for role, source_path in (("reference", reference_path), ("candidate", candidate_path)):
            if root != source_path and root not in source_path.parents:
                raise HeritageCalibrationError(
                    f"{role} path escapes the pair-manifest directory: {pair['pair_id']}")
        source_paths[pair["pair_id"]] = (reference_path, candidate_path)
    protected_inputs = {resolved_pairs, resolved_key}
    for paths in source_paths.values():
        protected_inputs.update(paths)
    if resolved_answers in protected_inputs or resolved_key in ({resolved_pairs} | {
            path for paths in source_paths.values() for path in paths}):
        raise HeritageCalibrationError(
            "the answer, key, pair manifest, and source WAV paths must be distinct")
    if (resolved_answers == resolved_out or resolved_out in resolved_answers.parents or
            resolved_key == resolved_out or resolved_out in resolved_key.parents):
        raise HeritageCalibrationError(
            "the answer manifest and key must remain outside the listener-facing pack")
    if out_dir.exists() and any(out_dir.iterdir()):
        raise HeritageCalibrationError("listening output directory must be empty")
    out_dir.mkdir(parents=True, exist_ok=True)
    public_pairs: list[dict[str, Any]] = []
    answers: list[dict[str, Any]] = []
    for pair_index, pair in enumerate(pairs, start=1):
        reference_path, candidate_path = source_paths[pair["pair_id"]]
        reference = read_wav(reference_path)
        candidate = read_wav(candidate_path)
        if (reference.sample_rate, reference.channels) != (candidate.sample_rate, candidate.channels):
            raise HeritageCalibrationError(f"sample-rate/channel mismatch for {pair['pair_id']}")
        if reference.frames != candidate.frames:
            raise HeritageCalibrationError(
                f"frame-count mismatch for {pair['pair_id']}; align the pair before packing")
        ref_rms = _rms(reference.samples)
        cand_rms = _rms(candidate.samples)
        if ref_rms <= 1.0e-12 or cand_rms <= 1.0e-12:
            raise HeritageCalibrationError(f"silent input cannot be level matched: {pair['pair_id']}")
        gain = ref_rms / cand_rms
        matched_samples = [value * gain for value in candidate.samples]
        peak = max(max(abs(value) for value in reference.samples),
                   max(abs(value) for value in matched_samples))
        common_gain = min(1.0, 0.98 / peak) if peak > 0.0 else 1.0
        safe_reference = WavData(
            reference.sample_rate, reference.channels,
            [value * common_gain for value in reference.samples])
        safe_candidate = WavData(
            candidate.sample_rate, candidate.channels,
            [value * common_gain for value in matched_samples])
        role_wavs = {"reference": safe_reference, "candidate": safe_candidate}
        first_role, second_role = _assignment(key, pair["pair_id"])
        token = hmac.new(key, f"filename\0{pair['pair_id']}".encode(), hashlib.sha256).hexdigest()[:12]
        first_name = f"pair-{pair_index:03d}-{token}-a.wav"
        second_name = f"pair-{pair_index:03d}-{token}-b.wav"
        write_float_wav(out_dir / first_name, role_wavs[first_role])
        write_float_wav(out_dir / second_name, role_wavs[second_role])
        artifacts = [
            {"label": "A", "path": first_name, "sha256": sha256_file(out_dir / first_name)},
            {"label": "B", "path": second_name, "sha256": sha256_file(out_dir / second_name)},
        ]
        packed_hashes = {
            first_role: artifacts[0]["sha256"],
            second_role: artifacts[1]["sha256"],
        }
        public_pairs.append({
            "pair": pair_index,
            "artifacts": artifacts,
            "sample_rate": reference.sample_rate,
            "channels": reference.channels,
            "level_match": "candidate-rms-to-reference",
        })
        answers.append({
            "pair": pair_index,
            "pair_id": pair["pair_id"],
            "roles": {"A": first_role, "B": second_role},
            "source_sha256": {
                "reference": sha256_file(reference_path),
                "candidate": sha256_file(candidate_path),
            },
            "packed_sha256": packed_hashes,
            "candidate_gain": gain,
            "common_attenuation_gain": common_gain,
        })
    key_id = hashlib.sha256(key).hexdigest()[:16]
    answer_document = {
        "schema": ANSWERS_SCHEMA,
        "key_id": key_id,
        "answers": answers,
        "authentication": {
            "algorithm": "HMAC-SHA256",
            "mac": _answers_mac(key, ANSWERS_SCHEMA, key_id, answers),
        },
    }
    write_json(answers_path, answer_document)
    manifest = {
        "schema": LISTENING_SCHEMA,
        "key_id": key_id,
        "randomization": "HMAC-SHA256",
        "pairs": public_pairs,
        "instructions": "Listen level matched and blinded; record A/B/no-preference before unblinding.",
    }
    write_json(out_dir / "manifest.json", manifest)
    return manifest


def verify_listening_pack(manifest_path: Path, answers_path: Path,
                          key_path: Path) -> dict[str, Any]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    answers_doc = json.loads(answers_path.read_text(encoding="utf-8"))
    key = _load_key(key_path)
    key_id = hashlib.sha256(key).hexdigest()[:16]
    if manifest.get("schema") != LISTENING_SCHEMA or answers_doc.get("schema") != ANSWERS_SCHEMA:
        raise HeritageCalibrationError("listening manifest or answer schema is invalid")
    if manifest.get("key_id") != key_id or answers_doc.get("key_id") != key_id:
        raise HeritageCalibrationError("listening key does not match key_id")
    answers = answers_doc.get("answers")
    if not isinstance(answers, list) or not answers:
        raise HeritageCalibrationError("answer manifest must contain answers")
    authentication = answers_doc.get("authentication")
    expected_mac = _answers_mac(key, ANSWERS_SCHEMA, key_id, answers)
    if (not isinstance(authentication, dict) or
            authentication.get("algorithm") != "HMAC-SHA256" or
            not hmac.compare_digest(str(authentication.get("mac", "")), expected_mac)):
        raise HeritageCalibrationError("answer manifest authentication failed")
    root = manifest_path.parent.resolve()
    public_pairs = manifest.get("pairs")
    if not isinstance(public_pairs, list) or not public_pairs:
        raise HeritageCalibrationError("listening manifest must contain pairs")
    answer_by_pair = {item["pair"]: item for item in answers if isinstance(item, dict) and "pair" in item}
    if len(answer_by_pair) != len(answers) or len(answers) != len(public_pairs):
        raise HeritageCalibrationError("listening pair/answer inventory mismatch")
    verified = 0
    seen_pairs: set[int] = set()
    for pair in public_pairs:
        if not isinstance(pair, dict) or not isinstance(pair.get("pair"), int):
            raise HeritageCalibrationError("listening pair record is invalid")
        if pair["pair"] in seen_pairs:
            raise HeritageCalibrationError(f"duplicate listening pair: {pair['pair']}")
        seen_pairs.add(pair["pair"])
        answer = answer_by_pair.get(pair.get("pair"))
        if not answer:
            raise HeritageCalibrationError(f"missing answer for pair {pair.get('pair')}")
        expected_roles = _assignment(key, answer["pair_id"])
        if answer.get("roles") != {"A": expected_roles[0], "B": expected_roles[1]}:
            raise HeritageCalibrationError(f"keyed order mismatch for pair {pair['pair']}")
        artifacts = pair.get("artifacts")
        if (not isinstance(artifacts, list) or len(artifacts) != 2 or
                {item.get("label") for item in artifacts if isinstance(item, dict)} != {"A", "B"}):
            raise HeritageCalibrationError(f"pair {pair['pair']} must contain A and B artifacts")
        wavs: list[WavData] = []
        for artifact in artifacts:
            path = (root / artifact["path"]).resolve()
            if root != path and root not in path.parents:
                raise HeritageCalibrationError("listening artifact escapes pack directory")
            actual_hash = sha256_file(path)
            if actual_hash != artifact.get("sha256"):
                raise HeritageCalibrationError(f"listening artifact hash mismatch: {artifact['path']}")
            role = answer["roles"].get(artifact["label"])
            if answer.get("packed_sha256", {}).get(role) != actual_hash:
                raise HeritageCalibrationError(
                    f"listening artifact is not bound to its keyed role: {artifact['path']}")
            wavs.append(read_wav(path))
        if len(wavs) != 2:
            raise HeritageCalibrationError(f"pair {pair['pair']} must contain two artifacts")
        if ((wavs[0].sample_rate, wavs[0].channels, wavs[0].frames) !=
                (wavs[1].sample_rate, wavs[1].channels, wavs[1].frames)):
            raise HeritageCalibrationError(f"pair {pair['pair']} WAV formats do not match")
        if (pair.get("sample_rate"), pair.get("channels")) != (
                wavs[0].sample_rate, wavs[0].channels):
            raise HeritageCalibrationError(f"pair {pair['pair']} manifest format does not match WAVs")
        rms_a, rms_b = _rms(wavs[0].samples), _rms(wavs[1].samples)
        delta_db = 20.0 * math.log10(rms_a / rms_b)
        if abs(delta_db) > 0.01:
            raise HeritageCalibrationError(f"pair {pair['pair']} is not level matched ({delta_db:.4f} dB)")
        if max(abs(value) for wav in wavs for value in wav.samples) > 0.981:
            raise HeritageCalibrationError(f"pair {pair['pair']} exceeds the listening peak ceiling")
        verified += 1
    return {"ok": True, "schema": LISTENING_SCHEMA, "pairs": verified, "key_id": key_id}


def _print_result(value: dict[str, Any]) -> None:
    sys.stdout.buffer.write(canonical_json(value))


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    capture = sub.add_parser("capture-verify", help="validate provenance and artifact hashes")
    capture.add_argument("manifest", type=Path)
    bootstrap = sub.add_parser("cyclic-bootstrap", help="prove cyclic recovery on pseudo hardware")
    bootstrap.add_argument("--out", type=Path, required=True)
    bootstrap.add_argument("--factor", type=float, default=1.75)
    bootstrap.add_argument("--cycle-frames", type=int, default=64)
    bootstrap.add_argument("--splice-frames", type=int, default=8)
    bootstrap.add_argument(
        "--renderer", type=Path,
        default=Path(os.environ["PULP_HERITAGE_CYCLIC_RENDER_WAV"])
        if "PULP_HERITAGE_CYCLIC_RENDER_WAV" in os.environ else None)
    pack = sub.add_parser("listening-pack", help="make a keyed level-matched blinded A/B pack")
    pack.add_argument("--pairs", type=Path, required=True)
    pack.add_argument("--out", type=Path, required=True)
    pack.add_argument("--answers-out", type=Path, required=True)
    pack.add_argument("--key-file", type=Path, required=True)
    verify = sub.add_parser("listening-verify", help="verify a keyed listening pack")
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--answers", type=Path, required=True)
    verify.add_argument("--key-file", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "capture-verify":
            result = verify_capture_manifest(args.manifest)
        elif args.command == "cyclic-bootstrap":
            if args.renderer is None:
                raise HeritageCalibrationError(
                    "--renderer or PULP_HERITAGE_CYCLIC_RENDER_WAV is required")
            result = bootstrap_cyclic(
                args.out, args.factor, args.cycle_frames, args.splice_frames, args.renderer)
            if not result["passed"]:
                _print_result(result)
                return 1
        elif args.command == "listening-pack":
            result = generate_listening_pack(args.pairs, args.out, args.answers_out, args.key_file)
        else:
            result = verify_listening_pack(args.manifest, args.answers, args.key_file)
        _print_result(result)
        return 0
    except (HeritageCalibrationError, OSError, KeyError, TypeError, ValueError) as exc:
        _print_result({"ok": False, "error": str(exc)})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
