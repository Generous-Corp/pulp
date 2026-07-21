#!/usr/bin/env python3
"""Heritage capture verification, cyclic calibration, and blinded A/B packs."""

from __future__ import annotations

import argparse
from array import array
import bisect
import hashlib
import hmac
import itertools
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
CAPTURE_PLAN_SCHEMA = "pulp.heritage.capture-plan.v2"
TARGET_APPLICABILITY_SCHEMA = "pulp.heritage.target-applicability.v1"
APPLICABILITY_EVIDENCE_SCHEMA = "pulp.heritage.applicability-evidence.v1"
ANALYSIS_REQUEST_SCHEMA = "pulp.heritage.analysis-request.v1"
ANALYSIS_REPORT_SCHEMA = "pulp.heritage.analysis-report.v1"
BOOTSTRAP_SCHEMA = "pulp.heritage.cyclic-bootstrap.v1"
ADAPTIVE_BOOTSTRAP_SCHEMA = "pulp.heritage.adaptive-bootstrap.v1"
C1_C5_BOOTSTRAP_SCHEMA = "pulp.heritage.c1-c5-bootstrap.v1"
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


def _lowercase_sha256(value: Any, where: str) -> str:
    if (not isinstance(value, str) or len(value) != 64 or
            any(character not in "0123456789abcdef" for character in value)):
        raise HeritageCalibrationError(f"{where} must be lowercase SHA-256")
    return value


def _resolve_relative_path(root: Path, value: Any, where: str) -> tuple[str, Path]:
    if not isinstance(value, str) or not value:
        raise HeritageCalibrationError(f"{where} must be a non-empty relative path")
    relative = Path(value)
    if relative.is_absolute() or any(part in ("", ".", "..") for part in relative.parts):
        raise HeritageCalibrationError(f"{where} must be a normalized relative path")
    resolved = (root / relative).resolve()
    if root != resolved and root not in resolved.parents:
        raise HeritageCalibrationError(f"{where} escapes its containing directory")
    return value, resolved


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
        relative, artifact_path = _resolve_relative_path(
            root, artifact.get("path"), where + ".path")
        expected_hash = _lowercase_sha256(artifact.get("sha256"), where + ".sha256")
        _require(artifact, "test_id", str, where)
        _require(artifact, "role", str, where)
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


def _load_json_object(path: Path, description: str) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HeritageCalibrationError(f"cannot read {description}: {exc}") from exc
    if not isinstance(document, dict):
        raise HeritageCalibrationError(f"{description} must be a JSON object")
    return document


def _row_key(protocol_id: str, parameters: dict[str, Any]) -> str:
    return protocol_id + "\0" + canonical_json(parameters).decode("utf-8")


def _matches_selector(parameters: dict[str, Any], selector: dict[str, Any]) -> bool:
    return all(parameters.get(key) == value for key, value in selector.items())


def expand_capture_plan(plan: dict[str, Any], *, required_only: bool = True) -> dict[str, dict[str, Any]]:
    """Expand the declarative plan into exact required row contracts."""
    if plan.get("schema") != CAPTURE_PLAN_SCHEMA:
        raise HeritageCalibrationError(f"capture plan schema must be {CAPTURE_PLAN_SCHEMA}")
    protocols = _require(plan, "protocols", list, "plan")
    expanded: dict[str, dict[str, Any]] = {}
    all_rows: dict[str, dict[str, Any]] = {}
    protocol_ids: set[str] = set()
    for protocol_index, protocol in enumerate(protocols):
        where = f"plan.protocols[{protocol_index}]"
        if not isinstance(protocol, dict):
            raise HeritageCalibrationError(f"{where} must be an object")
        protocol_id = _require(protocol, "id", str, where)
        if protocol_id in protocol_ids:
            raise HeritageCalibrationError(f"duplicate protocol id: {protocol_id}")
        protocol_ids.add(protocol_id)
        axes = _require(protocol, "axes", dict, where)
        if not axes:
            raise HeritageCalibrationError(f"{where}.axes must not be empty")
        for axis, values in axes.items():
            if not isinstance(axis, str) or not axis or not isinstance(values, list) or not values:
                raise HeritageCalibrationError(f"{where}.axes entries need a name and values")
            if any(isinstance(value, (dict, list)) or value is None for value in values):
                raise HeritageCalibrationError(
                    f"{where}.axes.{axis} values must be non-null JSON scalars")
            if len({json.dumps(value, sort_keys=True) for value in values}) != len(values):
                raise HeritageCalibrationError(f"{where}.axes.{axis} contains duplicate values")
        variants = _require(protocol, "variants", list, where)
        selectors = _require(protocol, "required_selectors", list, where)
        if not variants or not selectors or not all(isinstance(item, dict) for item in selectors):
            raise HeritageCalibrationError(f"{where} needs variants and required_selectors")
        candidates: dict[str, dict[str, Any]] = {}
        applicability = protocol.get("applicability")
        if applicability is not None:
            if (not isinstance(applicability, dict) or
                    not isinstance(applicability.get("capability"), str) or
                    not applicability["capability"] or
                    applicability.get("allow_omit_when_absent") is not True):
                raise HeritageCalibrationError(f"{where}.applicability is invalid")
        for variant_index, variant in enumerate(variants):
            variant_where = f"{where}.variants[{variant_index}]"
            if not isinstance(variant, dict):
                raise HeritageCalibrationError(f"{variant_where} must be an object")
            fixed = variant.get("fixed", {})
            vary = _require(variant, "vary", list, variant_where)
            roles = _require(variant, "required_artifact_roles", list, variant_where)
            if (not isinstance(fixed, dict) or not roles or
                    not all(isinstance(axis, str) and axis for axis in vary) or
                    not all(isinstance(role, str) and role for role in roles)):
                raise HeritageCalibrationError(f"{variant_where} has an invalid row contract")
            if len(set(vary)) != len(vary) or len(set(roles)) != len(roles):
                raise HeritageCalibrationError(f"{variant_where} contains duplicate axes or roles")
            for key, value in fixed.items():
                if key not in axes or value not in axes[key]:
                    raise HeritageCalibrationError(f"{variant_where}.fixed.{key} is outside its axis")
            if any(axis not in axes or axis in fixed for axis in vary):
                raise HeritageCalibrationError(f"{variant_where}.vary references an invalid axis")
            for values in itertools.product(*(axes[axis] for axis in vary)):
                parameters = dict(fixed)
                parameters.update(zip(vary, values))
                key = _row_key(protocol_id, parameters)
                if key in candidates:
                    raise HeritageCalibrationError(
                        f"capture-plan variants overlap for {protocol_id}: {parameters}")
                candidates[key] = {
                    "protocol_id": protocol_id,
                    "parameters": parameters,
                    "required_artifact_roles": sorted(roles),
                    "applicability": applicability,
                }
        all_rows.update(candidates)
        for selector_index, selector in enumerate(selectors):
            if any(key not in axes or value not in axes[key] for key, value in selector.items()):
                raise HeritageCalibrationError(
                    f"{where}.required_selectors[{selector_index}] is outside the axes")
            matches = [row for row in candidates.values()
                       if _matches_selector(row["parameters"], selector)]
            if not matches:
                raise HeritageCalibrationError(
                    f"{where}.required_selectors[{selector_index}] matches no valid row")
            for row in matches:
                expanded[_row_key(protocol_id, row["parameters"])] = row
    result = expanded if required_only else all_rows
    if not result:
        raise HeritageCalibrationError("capture plan expands to no required rows")
    return result


def _absent_target_capabilities(manifest: dict[str, Any], plan: dict[str, Any],
                                plan_path: Path, root: Path) -> set[str]:
    applicability = manifest.get("target_applicability")
    if applicability is None:
        return set()
    if (not isinstance(applicability, dict) or
            applicability.get("schema") != TARGET_APPLICABILITY_SCHEMA):
        raise HeritageCalibrationError(
            f"manifest.target_applicability.schema must be {TARGET_APPLICABILITY_SCHEMA}")
    plan_hash = sha256_file(plan_path)
    if applicability.get("capture_plan_sha256") != plan_hash:
        raise HeritageCalibrationError(
            "manifest.target_applicability.capture_plan_sha256 does not match the plan")
    allowed = {}
    for protocol in plan["protocols"]:
        contract = protocol.get("applicability")
        if isinstance(contract, dict):
            allowed[contract["capability"]] = protocol["id"]
    declarations = _require(applicability, "declarations", list,
                            "manifest.target_applicability")
    seen: set[str] = set()
    absent: set[str] = set()
    for index, declaration in enumerate(declarations):
        where = f"manifest.target_applicability.declarations[{index}]"
        if not isinstance(declaration, dict):
            raise HeritageCalibrationError(f"{where} must be an object")
        capability = _require(declaration, "capability", str, where)
        supported = declaration.get("supported")
        if capability not in allowed:
            raise HeritageCalibrationError(
                f"{where}.capability is not an omittable canonical-plan capability")
        if capability in seen or not isinstance(supported, bool):
            raise HeritageCalibrationError(f"{where} is duplicate or has invalid supported state")
        seen.add(capability)
        if supported:
            continue
        relative, evidence_path = _resolve_relative_path(
            root, declaration.get("evidence_path"), where + ".evidence_path")
        expected = _lowercase_sha256(
            declaration.get("evidence_sha256"), where + ".evidence_sha256")
        if not evidence_path.is_file() or sha256_file(evidence_path) != expected:
            raise HeritageCalibrationError(
                f"{where} applicability evidence hash does not match: {relative}")
        evidence = _load_json_object(evidence_path, where + ".evidence")
        if evidence.get("schema") != APPLICABILITY_EVIDENCE_SCHEMA:
            raise HeritageCalibrationError(
                f"{where} applicability evidence schema must be "
                f"{APPLICABILITY_EVIDENCE_SCHEMA}")
        if (evidence.get("capture_plan_sha256") != plan_hash or
                evidence.get("capability") != capability or
                evidence.get("session_id") != manifest.get("session_id") or
                evidence.get("target") != manifest.get("target") or
                evidence.get("conclusion") != "not-applicable"):
            raise HeritageCalibrationError(
                f"{where} applicability evidence binding is invalid")
        finding = evidence.get("finding")
        sources = evidence.get("sources")
        if (not isinstance(finding, str) or not finding.strip() or
                not isinstance(sources, list) or not sources or
                any(not isinstance(source, str) or not source.strip()
                    for source in sources)):
            raise HeritageCalibrationError(
                f"{where} applicability evidence needs a finding and sources")
        absent.add(capability)
    return absent


def audit_capture_readiness(manifest_path: Path, plan_path: Path) -> dict[str, Any]:
    """Verify artifacts and prove exact coverage of every mandatory plan row."""
    integrity = verify_capture_manifest(manifest_path)
    manifest = _load_json_object(manifest_path, "capture manifest")
    plan = _load_json_object(plan_path, "capture plan")
    required = expand_capture_plan(plan)
    known = expand_capture_plan(plan, required_only=False)
    binding = _require(manifest, "capture_plan", dict, "manifest")
    if binding.get("schema") != CAPTURE_PLAN_SCHEMA:
        raise HeritageCalibrationError("manifest.capture_plan.schema does not match the plan")
    if binding.get("sha256") != sha256_file(plan_path):
        raise HeritageCalibrationError("manifest.capture_plan.sha256 does not match the plan")
    absent_capabilities = _absent_target_capabilities(
        manifest, plan, plan_path, manifest_path.parent.resolve())
    effective_required = {
        key: contract for key, contract in required.items()
        if not (isinstance(contract.get("applicability"), dict) and
                contract["applicability"]["capability"] in absent_capabilities)
    }
    rows = _require(manifest, "rows", list, "manifest")
    if not rows:
        raise HeritageCalibrationError("manifest.rows must not be empty")
    artifact_roles: dict[str, set[str]] = {}
    for index, artifact in enumerate(manifest["artifacts"]):
        row_id = _require(artifact, "row_id", str, f"manifest.artifacts[{index}]")
        role = _require(artifact, "role", str, f"manifest.artifacts[{index}]")
        roles = artifact_roles.setdefault(row_id, set())
        if role in roles:
            raise HeritageCalibrationError(f"duplicate artifact role {role} for row {row_id}")
        roles.add(role)
    observed: dict[str, tuple[str, set[str]]] = {}
    row_ids: set[str] = set()
    for index, row in enumerate(rows):
        where = f"manifest.rows[{index}]"
        if not isinstance(row, dict):
            raise HeritageCalibrationError(f"{where} must be an object")
        row_id = _require(row, "row_id", str, where)
        protocol_id = _require(row, "protocol_id", str, where)
        parameters = _require(row, "parameters", dict, where)
        if row_id in row_ids:
            raise HeritageCalibrationError(f"duplicate row_id: {row_id}")
        row_ids.add(row_id)
        key = _row_key(protocol_id, parameters)
        if key not in known:
            raise HeritageCalibrationError(
                f"manifest row is not declared by the capture plan: {protocol_id} {parameters}")
        if key in observed:
            raise HeritageCalibrationError(
                f"duplicate capture-plan row coverage: {protocol_id} {parameters}")
        observed[key] = (row_id, artifact_roles.get(row_id, set()))
    orphaned = sorted(set(artifact_roles) - row_ids)
    if orphaned:
        raise HeritageCalibrationError(f"artifacts reference unknown rows: {', '.join(orphaned)}")
    missing_rows: list[dict[str, Any]] = []
    missing_roles: list[dict[str, Any]] = []
    for key, (row_id, roles) in observed.items():
        absent = sorted(set(known[key]["required_artifact_roles"]) - roles)
        if absent:
            missing_roles.append({"row_id": row_id, "roles": absent})
    for key, contract in effective_required.items():
        if key not in observed:
            missing_rows.append({
                "protocol_id": contract["protocol_id"],
                "parameters": contract["parameters"],
            })
            continue
    if missing_rows or missing_roles:
        summary = []
        if missing_rows:
            summary.append(f"{len(missing_rows)} required rows missing")
        if missing_roles:
            summary.append(f"{len(missing_roles)} rows missing artifact roles")
        raise HeritageCalibrationError("capture session is not ready: " + "; ".join(summary))
    return {
        "ok": True,
        "schema": CAPTURE_SCHEMA,
        "plan_schema": CAPTURE_PLAN_SCHEMA,
        "canonical_required_rows": len(required),
        "required_rows": len(effective_required),
        "omitted_rows": len(required) - len(effective_required),
        "absent_capabilities": sorted(absent_capabilities),
        "observed_rows": len(observed),
        "verified_artifacts": len(integrity["artifacts"]),
    }


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


def _dbfs(value: float) -> float:
    return max(-300.0, 20.0 * math.log10(max(abs(value), 1.0e-15)))


def _mono(wav: WavData) -> list[float]:
    if wav.channels == 1:
        return wav.samples
    return [math.fsum(wav.samples[index:index + wav.channels]) / wav.channels
            for index in range(0, len(wav.samples), wav.channels)]


def _require_matching_wavs(first: WavData, second: WavData, purpose: str) -> None:
    if ((first.sample_rate, first.channels, first.frames) !=
            (second.sample_rate, second.channels, second.frames)):
        raise HeritageCalibrationError(f"{purpose} WAV formats must match exactly")


def _gain_matched_null(reference: Sequence[float], measured: Sequence[float]) -> dict[str, float]:
    if len(reference) != len(measured) or not reference:
        raise HeritageCalibrationError("gain-matched null inputs must have equal nonzero length")
    energy = math.fsum(value * value for value in measured)
    if energy <= 1.0e-24:
        raise HeritageCalibrationError("gain-matched null measured input is silent")
    gain = math.fsum(a * b for a, b in zip(reference, measured)) / energy
    residual = [a - gain * b for a, b in zip(reference, measured)]
    reference_rms = _rms(reference)
    measured_rms = _rms(measured)
    if reference_rms <= 1.0e-12:
        raise HeritageCalibrationError("gain-matched null reference input is silent")
    correlation_denominator = math.sqrt(
        math.fsum(a * a for a in reference) * math.fsum(b * b for b in measured))
    correlation = (math.fsum(a * b for a, b in zip(reference, measured)) /
                   correlation_denominator if correlation_denominator > 0.0 else 0.0)
    residual_rms = _rms(residual)
    return {
        "gain_applied_to_measured": gain,
        "reference_rms_dbfs": _dbfs(reference_rms),
        "measured_rms_dbfs": _dbfs(measured_rms),
        "residual_rms_dbfs": _dbfs(residual_rms),
        "residual_db_relative_to_reference": _dbfs(residual_rms / reference_rms),
        "correlation": correlation,
    }


def _spectrum(samples: Sequence[float], sample_rate: int) -> list[tuple[float, float]]:
    available = min(len(samples), 4096)
    size = 1
    while size * 2 <= available:
        size *= 2
    if size < 64:
        raise HeritageCalibrationError("spectral analysis needs at least 64 frames")
    start = (len(samples) - size) // 2
    selected = samples[start:start + size]
    mean = math.fsum(selected) / size
    window = [0.5 - 0.5 * math.cos(2.0 * math.pi * index / (size - 1))
              for index in range(size)]
    window_sum = math.fsum(window)
    values = [complex((value - mean) * window[index], 0.0)
              for index, value in enumerate(selected)]
    j = 0
    for index in range(1, size):
        bit = size >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if index < j:
            values[index], values[j] = values[j], values[index]
    length = 2
    while length <= size:
        angle = -2.0 * math.pi / length
        root = complex(math.cos(angle), math.sin(angle))
        for offset in range(0, size, length):
            factor = 1.0 + 0.0j
            for index in range(offset, offset + length // 2):
                even = values[index]
                odd = values[index + length // 2] * factor
                values[index] = even + odd
                values[index + length // 2] = even - odd
                factor *= root
        length *= 2
    return [(bin_index * sample_rate / size, 2.0 * abs(values[bin_index]) / window_sum)
            for bin_index in range(1, size // 2 + 1)]


def _spectral_summary(wav: WavData) -> dict[str, float]:
    spectrum = _spectrum(_mono(wav), wav.sample_rate)
    dominant_frequency, dominant_magnitude = max(spectrum, key=lambda item: item[1])
    total = math.fsum(magnitude for _, magnitude in spectrum)
    total_energy = math.fsum(magnitude * magnitude for _, magnitude in spectrum)
    centroid = (math.fsum(frequency * magnitude for frequency, magnitude in spectrum) / total
                if total > 0.0 else 0.0)
    split = wav.sample_rate / 8.0
    low_energy = math.fsum(magnitude * magnitude for frequency, magnitude in spectrum
                           if frequency <= split)
    high_energy = max(0.0, total_energy - low_energy)
    return {
        "dominant_frequency_hz": dominant_frequency,
        "dominant_amplitude_dbfs": _dbfs(dominant_magnitude),
        "spectral_centroid_hz": centroid,
        "high_to_low_energy_db": 10.0 * math.log10(
            max(high_energy, 1.0e-30) / max(low_energy, 1.0e-30)),
    }


def _fold_frequency(frequency: float, sample_rate: int) -> float:
    wrapped = frequency % sample_rate
    return sample_rate - wrapped if wrapped > sample_rate / 2.0 else wrapped


def analyze_swept_fold_evolution(reference: WavData, capture: WavData,
                                  transpose_semitones: float) -> dict[str, Any]:
    if ((reference.sample_rate, reference.channels) !=
            (capture.sample_rate, capture.channels)):
        raise HeritageCalibrationError(
            "C2 swept-sine sample rates and channel counts must match")
    reference_mono = _mono(reference)
    capture_mono = _mono(capture)
    available = min(len(reference_mono), len(capture_mono))
    window_frames = 1024
    while window_frames > available // 2 and window_frames > 128:
        window_frames //= 2
    if available < window_frames or window_frames < 128:
        raise HeritageCalibrationError("C2 swept-sine analysis needs at least 256 frames")
    maximum_windows = 128
    hop = max(window_frames // 2,
              math.ceil((available - window_frames) / max(1, maximum_windows - 1)))
    starts = list(range(0, available - window_frames + 1, hop))
    last = available - window_frames
    if starts[-1] != last:
        starts.append(last)
    ratio = 2.0 ** (transpose_semitones / 12.0)
    evolution = []
    errors = []
    for start in starts:
        reference_window = WavData(reference.sample_rate, 1,
                                   reference_mono[start:start + window_frames])
        capture_window = WavData(capture.sample_rate, 1,
                                 capture_mono[start:start + window_frames])
        reference_frequency = _spectral_summary(reference_window)["dominant_frequency_hz"]
        observed_frequency = _spectral_summary(capture_window)["dominant_frequency_hz"]
        unfolded = reference_frequency * ratio
        folded = _fold_frequency(unfolded, capture.sample_rate)
        error = abs(observed_frequency - folded)
        errors.append(error)
        evolution.append({
            "start_frame": start,
            "reference_frequency_hz": reference_frequency,
            "unfolded_expected_frequency_hz": unfolded,
            "folded_expected_frequency_hz": folded,
            "observed_frequency_hz": observed_frequency,
            "absolute_fold_error_hz": error,
        })
    ordered_errors = sorted(errors)
    return {
        "window_frames": window_frames,
        "windows": evolution,
        "median_fold_error_hz": ordered_errors[len(ordered_errors) // 2],
        "maximum_fold_error_hz": max(errors),
        "first_window_start_frame": starts[0],
        "last_window_end_frame": starts[-1] + window_frames,
    }


def analyze_c1(recorded_replay: WavData, loaded_replay: WavData) -> dict[str, Any]:
    _require_matching_wavs(recorded_replay, loaded_replay, "C1")
    return {
        "protocol": "C1",
        "method": "least-squares gain-matched record-vs-load null",
        "measurements": _gain_matched_null(_mono(loaded_replay), _mono(recorded_replay)),
    }


def analyze_c2(captures: Sequence[tuple[float, str, WavData, WavData]]) -> dict[str, Any]:
    if not captures:
        raise HeritageCalibrationError("C2 needs at least one capture")
    rows = []
    for transpose, stimulus, reference, wav in sorted(
            captures, key=lambda item: (item[0], item[1])):
        if ((reference.sample_rate, reference.channels) !=
                (wav.sample_rate, wav.channels)):
            raise HeritageCalibrationError(
                "C2 stimulus/capture sample rates and channel counts must match")
        reference_spectrum = _spectral_summary(reference)
        capture_spectrum = _spectral_summary(wav)
        row = {"transpose_semitones": transpose, "stimulus": stimulus}
        row.update(capture_spectrum)
        row.update({
            "reference_dominant_frequency_hz": reference_spectrum["dominant_frequency_hz"],
            "capture_minus_reference_high_to_low_energy_db": (
                capture_spectrum["high_to_low_energy_db"] -
                reference_spectrum["high_to_low_energy_db"]),
        })
        if stimulus == "fixed-period-tone":
            expected_frequency = (reference_spectrum["dominant_frequency_hz"] *
                                  2.0 ** (transpose / 12.0))
            row.update({
                "equal_temperament_expected_frequency_hz": expected_frequency,
                "dominant_frequency_error_cents": 1200.0 * math.log2(
                    capture_spectrum["dominant_frequency_hz"] / expected_frequency),
            })
        elif stimulus == "swept-sine":
            row["fold_evolution"] = analyze_swept_fold_evolution(
                reference, wav, transpose)
        rows.append(row)
    return {"protocol": "C2", "method": "windowed FFT fold-pattern measurements", "rows": rows}


def analyze_c3(reference_level: WavData, lower_level: WavData,
               level_difference_db: float = -20.0) -> dict[str, Any]:
    _require_matching_wavs(reference_level, lower_level, "C3")
    expected_gain = 10.0 ** (level_difference_db / 20.0)
    if expected_gain <= 0.0 or not math.isfinite(expected_gain):
        raise HeritageCalibrationError("C3 level difference must be finite")
    normalized_lower = [value / expected_gain for value in _mono(lower_level)]
    null = _gain_matched_null(_mono(reference_level), normalized_lower)
    return {
        "protocol": "C3",
        "method": "declared-level normalization followed by least-squares null",
        "level_difference_db": level_difference_db,
        "measurements": null,
    }


def analyze_c4(active: WavData, idle: WavData) -> dict[str, Any]:
    if (active.sample_rate, active.channels) != (idle.sample_rate, idle.channels):
        raise HeritageCalibrationError("C4 sample rates and channel counts must match")
    def noise_metrics(wav: WavData) -> dict[str, float]:
        mono = _mono(wav)
        result = {
            "rms_dbfs": _dbfs(_rms(mono)),
            "peak_dbfs": _dbfs(max(abs(value) for value in mono)),
            "dc_dbfs": _dbfs(math.fsum(mono) / len(mono)),
        }
        result.update(_spectral_summary(wav))
        return result
    active_metrics = noise_metrics(active)
    idle_metrics = noise_metrics(idle)
    return {
        "protocol": "C4",
        "method": "active/idle level and two-band spectral comparison",
        "active": active_metrics,
        "idle": idle_metrics,
        "active_minus_idle_rms_db": active_metrics["rms_dbfs"] - idle_metrics["rms_dbfs"],
    }


def analyze_c5(responses: Sequence[tuple[float, str, WavData]]) -> dict[str, Any]:
    if not responses:
        raise HeritageCalibrationError("C5 needs at least one response")
    rows: list[dict[str, Any]] = []
    for transpose, stimulus, wav in sorted(responses, key=lambda item: (item[0], item[1])):
        mono = _mono(wav)
        if stimulus == "unit-impulse":
            energy = [value * value for value in mono]
            total = math.fsum(energy)
            if total <= 1.0e-24:
                raise HeritageCalibrationError("C5 impulse response is silent")
            row = {
                "transpose_semitones": transpose,
                "stimulus": stimulus,
                "peak_frame": max(range(len(mono)), key=lambda index: abs(mono[index])),
                "energy_centroid_frames": math.fsum(
                    index * value for index, value in enumerate(energy)) / total,
                "absolute_sum": math.fsum(abs(value) for value in mono),
            }
        elif stimulus == "unit-step":
            tail_count = max(1, min(len(mono) // 8, 1024))
            final_value = math.fsum(mono[-tail_count:]) / tail_count
            peak = max(mono)
            trough = min(mono)
            row = {
                "transpose_semitones": transpose,
                "stimulus": stimulus,
                "final_value": final_value,
                "overshoot": peak - final_value,
                "undershoot": final_value - trough,
                "largest_transition_frame": max(
                    range(1, len(mono)), key=lambda index: abs(mono[index] - mono[index - 1])),
            }
        else:
            raise HeritageCalibrationError(f"unsupported C5 stimulus: {stimulus}")
        rows.append(row)
    return {"protocol": "C5", "method": "time-domain impulse and step response metrics", "rows": rows}


def oracle_adaptive(source: list[float], segment_frames: Sequence[int], factor: float,
                    splice_frames: Sequence[int]) -> list[float]:
    if (not segment_frames or len(segment_frames) != len(splice_frames) or
            not 0.25 <= factor <= 20.0):
        raise HeritageCalibrationError("invalid adaptive fixture dimensions")
    if splice_frames[0] != 0:
        raise HeritageCalibrationError("the first adaptive segment cannot have a splice")
    output: list[float] = []
    anchor = 0.0
    previous_anchor = 0
    previous_length = 0
    for segment_index, (length, width) in enumerate(zip(segment_frames, splice_frames)):
        if length < 4 or width < 0 or width > length // 2:
            raise HeritageCalibrationError("invalid adaptive segment or splice width")
        current_anchor = _round_anchor(anchor)
        for phase in range(length):
            position = current_anchor + phase
            value = source[position] if position < len(source) else 0.0
            if segment_index > 0 and phase < width:
                old_position = previous_anchor + previous_length + phase
                old = source[old_position] if old_position < len(source) else 0.0
                weight = 1.0 if width == 1 else phase / (width - 1)
                value = old + (value - old) * weight
            output.append(value)
        previous_anchor = current_anchor
        previous_length = length
        anchor += length / factor
    return output


def recover_adaptive(source: list[float], capture: list[float]) -> dict[str, Any]:
    if len(source) < 2 or len(capture) < 128:
        raise HeritageCalibrationError("adaptive recovery needs a >=128-frame capture")
    step = source[1] - source[0]
    if step == 0.0:
        raise HeritageCalibrationError("adaptive recovery requires the indexed impulse basis")
    threshold = max(abs(step) * 0.1, 1.0e-7)
    anomalies = [index for index in range(1, len(capture))
                 if abs((capture[index] - capture[index - 1]) - step) > threshold]
    runs: list[tuple[int, int]] = []
    for index in anomalies:
        if not runs or index > runs[-1][1] + 1:
            runs.append((index, index))
        else:
            runs[-1] = (runs[-1][0], index)
    if len(runs) < 4:
        raise HeritageCalibrationError("could not find adaptive splice boundaries")
    boundaries = [begin - 1 for begin, _ in runs]
    segment_lengths = [boundaries[0]] + [right - left
                                         for left, right in zip(boundaries, boundaries[1:])]
    splice_widths = [end - begin + 2 for begin, end in runs]
    splice_anomaly_frames = [end - begin + 1 for begin, end in runs]
    anchors = [0]
    for boundary, width in zip(boundaries, splice_widths):
        probe = min(boundary + width, len(capture) - 1)
        insertion = bisect.bisect_left(source, capture[probe])
        candidates = [position for position in (insertion - 1, insertion)
                      if 0 <= position < len(source)]
        source_position = min(candidates, key=lambda position: abs(
            source[position] - capture[probe]))
        anchors.append(source_position - (probe - boundary))
    usable = 1
    while usable < len(anchors) and anchors[usable] > anchors[usable - 1]:
        usable += 1
    source_advance = anchors[usable - 1] - anchors[0]
    output_advance = boundaries[usable - 2] if usable > 1 else 0
    if source_advance <= 0:
        raise HeritageCalibrationError("adaptive source anchors do not advance")
    result = {
        "factor": output_advance / source_advance,
        "segment_frames": segment_lengths,
        "splice_frames": splice_widths,
        "splice_anomaly_frames": splice_anomaly_frames,
        "output_boundaries": boundaries,
        "source_anchors": anchors,
        "unclamped_anchors_observed": usable - 1,
        "splices_observed": len(runs),
    }
    result["segment_range_frames"] = [min(segment_lengths), max(segment_lengths)]
    result["splice_range_frames"] = [min(splice_widths), max(splice_widths)]
    result["variable_segment_lengths"] = len(set(segment_lengths)) > 1
    return result


def verify_adaptive_settings(declared: dict[str, Any], recovered: dict[str, Any],
                             sample_rate: int) -> dict[str, Any]:
    factor_percent = declared.get("factor_percent")
    if (not isinstance(factor_percent, (int, float)) or isinstance(factor_percent, bool) or
            not math.isfinite(float(factor_percent)) or factor_percent <= 0.0):
        raise HeritageCalibrationError("adaptive declared factor_percent must be positive")
    expected_factor = float(factor_percent) / 100.0
    factor_tolerance = max(0.02, expected_factor * 0.01)
    comparisons: dict[str, Any] = {
        "factor": {
            "declared": expected_factor,
            "recovered": recovered["factor"],
            "tolerance": factor_tolerance,
        }
    }
    failures = []
    if abs(recovered["factor"] - expected_factor) > factor_tolerance:
        failures.append("factor")
    expected_hop: Optional[int] = None
    if "decision_hop_samples" in declared:
        value = declared["decision_hop_samples"]
        if not isinstance(value, int) or isinstance(value, bool) or value < 1:
            raise HeritageCalibrationError("adaptive decision_hop_samples must be positive")
        expected_hop = value
    elif isinstance(declared.get("cycle_ms"), (int, float)) and not isinstance(
            declared.get("cycle_ms"), bool):
        expected_hop = int(math.floor(
            float(declared["cycle_ms"]) * sample_rate / 1000.0 + 0.5))
    if expected_hop is not None:
        observed_hops = recovered["segment_frames"]
        maximum_error = max(abs(value - expected_hop) for value in observed_hops)
        comparisons["decision_hop_samples"] = {
            "declared": expected_hop,
            "recovered_range": [min(observed_hops), max(observed_hops)],
            "maximum_error": maximum_error,
            "tolerance": 1,
        }
        if maximum_error > 1:
            failures.append("decision_hop_samples")
    if "crossfade_samples" in declared:
        expected_crossfade = declared["crossfade_samples"]
        if (not isinstance(expected_crossfade, int) or isinstance(expected_crossfade, bool) or
                expected_crossfade < 1):
            raise HeritageCalibrationError(
                "adaptive crossfade_samples must be positive for recovery")
        splice_shape = declared.get("splice_shape", "linear_blend")
        if splice_shape == "linear_blend":
            observed_widths = recovered["splice_frames"]
            expected_observed = expected_crossfade
        elif splice_shape == "equal_power_overlap_add":
            observed_widths = recovered["splice_anomaly_frames"]
            expected_observed = expected_crossfade + 1
        else:
            raise HeritageCalibrationError(
                "adaptive splice_shape must be linear_blend or equal_power_overlap_add")
        maximum_error = max(abs(value - expected_observed) for value in observed_widths)
        comparisons["crossfade_samples"] = {
            "declared": expected_crossfade,
            "splice_shape": splice_shape,
            "recovered_range": [min(observed_widths), max(observed_widths)],
            "expected_anomaly_frames": expected_observed,
            "maximum_error": maximum_error,
            "tolerance": 1,
        }
        if maximum_error > 1:
            failures.append("crossfade_samples")
    if failures:
        raise HeritageCalibrationError(
            "adaptive declared/recovered mismatch: " + ", ".join(failures))
    return {"matched": True, "comparisons": comparisons}


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


def bootstrap_c1_c5(out_dir: Path, renderer: Path) -> dict[str, Any]:
    if not renderer.is_file():
        raise HeritageCalibrationError(f"Pulp C1-C5 renderer is unavailable: {renderer}")
    if out_dir.exists() and any(out_dir.iterdir()):
        raise HeritageCalibrationError("C1-C5 bootstrap output directory must be empty")
    out_dir.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [str(renderer), "--out", str(out_dir)], capture_output=True, text=True, timeout=60)
    if completed.returncode != 0:
        raise HeritageCalibrationError(
            "Pulp C1-C5 renderer failed: " +
            (completed.stderr.strip() or completed.stdout.strip()))

    names = (
        "c1-loaded.wav", "c1-recorded.wav", "c2-source.wav", "c2-p12.wav",
        "c3-reference.wav", "c3-lower.wav", "c4-active.wav", "c4-idle.wav",
        "c5-impulse.wav", "c5-step.wav",
    )
    wavs = {name: read_wav(out_dir / name) for name in names}
    if any((wav.sample_rate, wav.channels, wav.frames) != (48000, 1, 16384)
           for wav in wavs.values()):
        raise HeritageCalibrationError("Pulp C1-C5 renderer returned an invalid WAV contract")

    c1 = analyze_c1(wavs["c1-recorded.wav"], wavs["c1-loaded.wav"])
    c2 = analyze_c2([(12.0, "fixed-period-tone", wavs["c2-source.wav"],
                      wavs["c2-p12.wav"])])
    c3 = analyze_c3(wavs["c3-reference.wav"], wavs["c3-lower.wav"], -20.0)
    c4 = analyze_c4(wavs["c4-active.wav"], wavs["c4-idle.wav"])
    c5 = analyze_c5([
        (0.0, "unit-impulse", wavs["c5-impulse.wav"]),
        (0.0, "unit-step", wavs["c5-step.wav"]),
    ])
    c1_measurements = c1["measurements"]
    c2_row = c2["rows"][0]
    c3_measurements = c3["measurements"]
    impulse = next(row for row in c5["rows"] if row["stimulus"] == "unit-impulse")
    step = next(row for row in c5["rows"] if row["stimulus"] == "unit-step")
    expected_centroid = (
        8.0 + (0.9 ** 2 + 2.0 * 0.9 ** 4 + 3.0 * 0.9 ** 6) /
        (1.0 + 0.9 ** 2 + 0.9 ** 4 + 0.9 ** 6))
    expected_hold_sum = 1.0 + 0.9 + 0.9 ** 2 + 0.9 ** 3
    expected_step_tail = expected_hold_sum / 4.0
    comparisons = {
        "C1": {
            "matched": (abs(c1_measurements["gain_applied_to_measured"] - 2.0) <= 1.0e-4 and
                        c1_measurements["residual_db_relative_to_reference"] <= -100.0),
            "expected_record_gain": 0.5,
            "gain_tolerance": 1.0e-4,
        },
        "C2": {
            "matched": abs(c2_row["dominant_frequency_error_cents"]) <= 1.0,
            "expected_transpose_semitones": 12.0,
            "cents_tolerance": 1.0,
        },
        "C3": {
            "matched": abs(c3_measurements["gain_applied_to_measured"] - 1.0) <= 0.1,
            "expected_level_difference_db": -20.0,
            "normalized_gain_tolerance": 0.1,
        },
        "C4": {
            "matched": abs(c4["active_minus_idle_rms_db"] - 20.0432) <= 1.0,
            "expected_active_minus_idle_rms_db": 20.0432,
            "rms_delta_tolerance_db": 1.0,
        },
        "C5": {
            "matched": (impulse["peak_frame"] == 8 and
                        abs(impulse["absolute_sum"] - expected_hold_sum) <= 1.0e-4 and
                        abs(impulse["energy_centroid_frames"] - expected_centroid) <= 1.0e-4 and
                        step["largest_transition_frame"] == 8 and
                        abs(step["final_value"] - expected_step_tail) <= 1.0e-4),
            "expected_hold_samples": 4,
            "expected_droop": 0.1,
            "metric_tolerance": 1.0e-4,
        },
    }

    wrong_c1 = analyze_c1(wavs["c1-loaded.wav"], wavs["c1-recorded.wav"])
    wrong_c2 = analyze_c2([(0.0, "fixed-period-tone", wavs["c2-source.wav"],
                            wavs["c2-p12.wav"])])
    wrong_c3 = analyze_c3(wavs["c3-reference.wav"], wavs["c3-lower.wav"], -10.0)
    wrong_c4 = analyze_c4(wavs["c4-idle.wav"], wavs["c4-active.wav"])
    negative_controls = {
        "C1_wrong_direction": {
            "rejected": abs(wrong_c1["measurements"]["gain_applied_to_measured"] - 2.0) > 0.1,
        },
        "C2_wrong_transpose": {
            "rejected": abs(wrong_c2["rows"][0]["dominant_frequency_error_cents"]) > 1000.0,
        },
        "C3_wrong_level_difference": {
            "rejected": abs(wrong_c3["measurements"]["gain_applied_to_measured"] - 1.0) > 1.0,
        },
        "C4_swapped_activity": {
            "rejected": wrong_c4["active_minus_idle_rms_db"] < -10.0,
        },
        "C5_bypass_hold_law": {
            "rejected": abs(impulse["absolute_sum"] - 1.0) > 1.0,
        },
    }
    passed = (all(item["matched"] for item in comparisons.values()) and
              all(item["rejected"] for item in negative_controls.values()))
    report = {
        "schema": C1_C5_BOOTSTRAP_SCHEMA,
        "passed": passed,
        "analyses": {"C1": c1, "C2": c2, "C3": c3, "C4": c4, "C5": c5},
        "declared_vs_recovered": comparisons,
        "negative_controls": negative_controls,
        "artifacts": [
            {"path": name, "sha256": sha256_file(out_dir / name)} for name in names
        ],
        "renderer": {"path": renderer.name, "sha256": sha256_file(renderer)},
        "scope": "Product-backed C1-C5 calibration-pipeline bootstrap.",
    }
    write_json(out_dir / "report.json", report)
    return report


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


def bootstrap_adaptive(out_dir: Path, factor: float, decision_hop_samples: int,
                       search_radius_samples: int, search_stride_samples: int,
                       crossfade_samples: int, renderer: Path,
                       source_frames: int = 8192) -> dict[str, Any]:
    if not renderer.is_file():
        raise HeritageCalibrationError(f"Pulp adaptive renderer is unavailable: {renderer}")
    if out_dir.exists() and any(out_dir.iterdir()):
        raise HeritageCalibrationError("adaptive bootstrap output directory must be empty")
    out_dir.mkdir(parents=True, exist_ok=True)
    source_path = out_dir / "indexed-source.wav"
    capture_path = out_dir / "adaptive-product-capture.wav"
    command = [
        str(renderer), "--source-out", str(source_path), "--capture-out", str(capture_path),
        "--factor", str(factor), "--decision-hop", str(decision_hop_samples),
        "--search-radius", str(search_radius_samples),
        "--search-stride", str(search_stride_samples),
        "--crossfade", str(crossfade_samples), "--source-frames", str(source_frames),
    ]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if completed.returncode != 0:
        raise HeritageCalibrationError(
            "Pulp adaptive renderer failed: " +
            (completed.stderr.strip() or completed.stdout.strip()))
    source = read_wav(source_path)
    capture = read_wav(capture_path)
    if ((source.sample_rate, source.channels) != (48000, 1) or
            (capture.sample_rate, capture.channels) != (48000, 1)):
        raise HeritageCalibrationError("Pulp adaptive renderer returned an invalid WAV contract")
    recovered = recover_adaptive(source.samples, capture.samples)
    declared = {
        "factor_percent": factor * 100.0,
        "decision_hop_samples": decision_hop_samples,
        "search_radius_samples": search_radius_samples,
        "search_stride_samples": search_stride_samples,
        "crossfade_samples": crossfade_samples,
        "splice_shape": "equal_power_overlap_add",
    }
    comparison = verify_adaptive_settings(declared, recovered, source.sample_rate)
    report = {
        "schema": ADAPTIVE_BOOTSTRAP_SCHEMA,
        "passed": True,
        "declared": declared,
        "recovered": recovered,
        "declared_vs_recovered": comparison,
        "artifacts": [
            {"path": source_path.name, "sha256": sha256_file(source_path)},
            {"path": capture_path.name, "sha256": sha256_file(capture_path)},
        ],
        "renderer": {"path": renderer.name, "sha256": sha256_file(renderer)},
        "scope": "Product-backed adaptive calibration-pipeline bootstrap.",
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
        _, reference_path = _resolve_relative_path(
            root, pair["reference"], f"pairs.{pair['pair_id']}.reference")
        _, candidate_path = _resolve_relative_path(
            root, pair["candidate"], f"pairs.{pair['pair_id']}.candidate")
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
            _, path = _resolve_relative_path(
                root, artifact.get("path"), f"pair[{pair['pair']}].artifact.path")
            actual_hash = sha256_file(path)
            expected_hash = _lowercase_sha256(
                artifact.get("sha256"), f"pair[{pair['pair']}].artifact.sha256")
            if actual_hash != expected_hash:
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


def _request_wav(root: Path, value: Any, where: str,
                 hashes: Optional[dict[str, str]] = None) -> WavData:
    relative, path = _resolve_relative_path(root, value, where)
    if hashes is not None:
        hashes[relative] = sha256_file(path)
    return read_wav(path)


def run_analysis_request(path: Path) -> dict[str, Any]:
    request = _load_json_object(path, "analysis request")
    if request.get("schema") != ANALYSIS_REQUEST_SCHEMA:
        raise HeritageCalibrationError(
            f"analysis request schema must be {ANALYSIS_REQUEST_SCHEMA}")
    analyses = _require(request, "analyses", list, "request")
    if not analyses:
        raise HeritageCalibrationError("request.analyses must not be empty")
    root = path.parent.resolve()
    reports: list[dict[str, Any]] = []
    ids: set[str] = set()
    for index, analysis in enumerate(analyses):
        where = f"request.analyses[{index}]"
        if not isinstance(analysis, dict):
            raise HeritageCalibrationError(f"{where} must be an object")
        analysis_id = _require(analysis, "id", str, where)
        protocol = _require(analysis, "protocol", str, where)
        inputs = _require(analysis, "inputs", dict, where)
        if analysis_id in ids:
            raise HeritageCalibrationError(f"duplicate analysis id: {analysis_id}")
        ids.add(analysis_id)
        input_hashes: dict[str, str] = {}
        if protocol == "C1":
            report = analyze_c1(
                _request_wav(root, inputs.get("recorded_replay"),
                             where + ".inputs.recorded_replay", input_hashes),
                _request_wav(root, inputs.get("loaded_replay"),
                             where + ".inputs.loaded_replay", input_hashes))
        elif protocol in ("C2", "C5"):
            captures = _require(inputs, "captures", list, where + ".inputs")
            decoded = []
            for capture_index, capture in enumerate(captures):
                capture_where = f"{where}.inputs.captures[{capture_index}]"
                if not isinstance(capture, dict):
                    raise HeritageCalibrationError(f"{capture_where} must be an object")
                transpose = capture.get("transpose_semitones")
                if (not isinstance(transpose, (int, float)) or isinstance(transpose, bool) or
                        not math.isfinite(float(transpose))):
                    raise HeritageCalibrationError(
                        f"{capture_where}.transpose_semitones must be finite")
                stimulus = _require(capture, "stimulus", str, capture_where)
                captured_wav = _request_wav(
                    root, capture.get("path"), capture_where + ".path", input_hashes)
                if protocol == "C2":
                    stimulus_wav = _request_wav(
                        root, capture.get("stimulus_path"),
                        capture_where + ".stimulus_path", input_hashes)
                    decoded.append((float(transpose), stimulus, stimulus_wav, captured_wav))
                else:
                    decoded.append((float(transpose), stimulus, captured_wav))
            report = analyze_c2(decoded) if protocol == "C2" else analyze_c5(decoded)
        elif protocol == "C3":
            level_difference = analysis.get("level_difference_db", -20.0)
            if (not isinstance(level_difference, (int, float)) or isinstance(level_difference, bool) or
                    not math.isfinite(float(level_difference))):
                raise HeritageCalibrationError(f"{where}.level_difference_db must be finite")
            report = analyze_c3(
                _request_wav(root, inputs.get("reference_level"),
                             where + ".inputs.reference_level", input_hashes),
                _request_wav(root, inputs.get("lower_level"),
                             where + ".inputs.lower_level", input_hashes),
                float(level_difference))
        elif protocol == "C4":
            report = analyze_c4(
                _request_wav(root, inputs.get("active"), where + ".inputs.active", input_hashes),
                _request_wav(root, inputs.get("idle"), where + ".inputs.idle", input_hashes))
        elif protocol == "C6-adaptive":
            source = _request_wav(
                root, inputs.get("source"), where + ".inputs.source", input_hashes)
            captures = _require(inputs, "captures", list, where + ".inputs")
            if not captures:
                raise HeritageCalibrationError(f"{where}.inputs.captures must not be empty")
            rows = []
            source_probe = source.samples[0::source.channels]
            for capture_index, capture_item in enumerate(captures):
                capture_where = f"{where}.inputs.captures[{capture_index}]"
                if not isinstance(capture_item, dict):
                    raise HeritageCalibrationError(f"{capture_where} must be an object")
                capture = _request_wav(
                    root, capture_item.get("path"), capture_where + ".path", input_hashes)
                if ((source.sample_rate, source.channels) !=
                        (capture.sample_rate, capture.channels)):
                    raise HeritageCalibrationError(
                        "C6 adaptive source/capture sample rates and channels must match")
                settings = {}
                for key in ("factor_percent", "cycle_ms", "adaptive_quality", "adaptive_width"):
                    if key not in capture_item:
                        raise HeritageCalibrationError(f"{capture_where}.{key} is required")
                    settings[key] = capture_item[key]
                for key in ("decision_hop_samples", "search_radius_samples",
                            "search_stride_samples", "crossfade_samples", "splice_shape"):
                    if key in capture_item:
                        settings[key] = capture_item[key]
                capture_probe = capture.samples[0::capture.channels]
                measurements = recover_adaptive(source_probe, capture_probe)
                setting_match = verify_adaptive_settings(
                    settings, measurements, capture.sample_rate)
                if capture.channels > 1:
                    measurements["stereo_coherence_maximum_error"] = max(
                        abs(capture.samples[frame * capture.channels + channel] -
                            capture.samples[frame * capture.channels])
                        for frame in range(capture.frames)
                        for channel in range(1, capture.channels))
                if settings["factor_percent"] == 100 and source.frames == capture.frames:
                    measurements["transparency_maximum_error"] = max(
                        abs(a - b) for a, b in zip(source.samples, capture.samples))
                rows.append({**settings, "measurements": measurements,
                             "declared_vs_recovered": setting_match})
            report = {
                "protocol": "C6-adaptive",
                "method": "indexed-basis variable-boundary and source-anchor grid recovery",
                "rows": rows,
            }
        else:
            raise HeritageCalibrationError(f"unsupported analysis protocol: {protocol}")
        reports.append({"id": analysis_id, "input_sha256": dict(sorted(input_hashes.items())),
                        **report})
    return {
        "ok": True,
        "schema": ANALYSIS_REPORT_SCHEMA,
        "request_sha256": sha256_file(path),
        "analyses": reports,
    }


def _print_result(value: dict[str, Any]) -> None:
    sys.stdout.buffer.write(canonical_json(value))


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    capture = sub.add_parser("capture-verify", help="validate provenance and artifact hashes")
    capture.add_argument("manifest", type=Path)
    ready = sub.add_parser(
        "capture-ready", help="verify hashes and reconcile every mandatory capture-plan row")
    ready.add_argument("manifest", type=Path)
    ready.add_argument("--plan", type=Path, required=True)
    analysis = sub.add_parser(
        "analyze", help="run deterministic C1-C5 or adaptive-C6 capture analyses")
    analysis.add_argument("request", type=Path)
    c1_c5 = sub.add_parser(
        "c1-c5-bootstrap", help="prove C1-C5 analyzers against the product renderer")
    c1_c5.add_argument("--out", type=Path, required=True)
    c1_c5.add_argument(
        "--renderer", type=Path,
        default=Path(os.environ["PULP_HERITAGE_C1_C5_RENDER_WAV"])
        if "PULP_HERITAGE_C1_C5_RENDER_WAV" in os.environ else None)
    bootstrap = sub.add_parser("cyclic-bootstrap", help="prove cyclic recovery on pseudo hardware")
    bootstrap.add_argument("--out", type=Path, required=True)
    bootstrap.add_argument("--factor", type=float, default=1.75)
    bootstrap.add_argument("--cycle-frames", type=int, default=64)
    bootstrap.add_argument("--splice-frames", type=int, default=8)
    bootstrap.add_argument(
        "--renderer", type=Path,
        default=Path(os.environ["PULP_HERITAGE_CYCLIC_RENDER_WAV"])
        if "PULP_HERITAGE_CYCLIC_RENDER_WAV" in os.environ else None)
    adaptive = sub.add_parser(
        "adaptive-bootstrap", help="prove adaptive recovery against the product renderer")
    adaptive.add_argument("--out", type=Path, required=True)
    adaptive.add_argument("--factor", type=float, default=1.75)
    adaptive.add_argument("--decision-hop", type=int, default=64)
    adaptive.add_argument("--search-radius", type=int, default=8)
    adaptive.add_argument("--search-stride", type=int, default=1)
    adaptive.add_argument("--crossfade", type=int, default=8)
    adaptive.add_argument("--source-frames", type=int, default=8192)
    adaptive.add_argument(
        "--renderer", type=Path,
        default=Path(os.environ["PULP_HERITAGE_ADAPTIVE_RENDER_WAV"])
        if "PULP_HERITAGE_ADAPTIVE_RENDER_WAV" in os.environ else None)
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
        elif args.command == "capture-ready":
            result = audit_capture_readiness(args.manifest, args.plan)
        elif args.command == "analyze":
            result = run_analysis_request(args.request)
        elif args.command == "c1-c5-bootstrap":
            if args.renderer is None:
                raise HeritageCalibrationError(
                    "--renderer or PULP_HERITAGE_C1_C5_RENDER_WAV is required")
            result = bootstrap_c1_c5(args.out, args.renderer)
            if not result["passed"]:
                _print_result(result)
                return 1
        elif args.command == "cyclic-bootstrap":
            if args.renderer is None:
                raise HeritageCalibrationError(
                    "--renderer or PULP_HERITAGE_CYCLIC_RENDER_WAV is required")
            result = bootstrap_cyclic(
                args.out, args.factor, args.cycle_frames, args.splice_frames, args.renderer)
            if not result["passed"]:
                _print_result(result)
                return 1
        elif args.command == "adaptive-bootstrap":
            if args.renderer is None:
                raise HeritageCalibrationError(
                    "--renderer or PULP_HERITAGE_ADAPTIVE_RENDER_WAV is required")
            result = bootstrap_adaptive(
                args.out, args.factor, args.decision_hop, args.search_radius,
                args.search_stride, args.crossfade, args.renderer, args.source_frames)
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
