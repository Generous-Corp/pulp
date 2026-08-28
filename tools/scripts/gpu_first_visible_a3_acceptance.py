#!/usr/bin/env python3
"""Generate and verify closed GPU first-visible A3 acceptance receipts."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import re
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import json_schema_lite  # noqa: E402

SCHEMA_PATH = ROOT / "docs/contracts/gpu-first-visible-a3-acceptance-v1.schema.json"
HEALTH_SCHEMA_PATH = ROOT / "docs/contracts/gpu-health-read-result-v1.schema.json"
CAMPAIGN_ROLES = {"standalone", "headless-constrained", "daw", "forge"}
IDENTITY_KEYS = {
    "pulp_revision", "forge_revision", "build_id", "product_id", "product_name",
    "plugin_format", "machine_id", "instance_id", "campaign_id",
}
ARTIFACT_KEYS = {"path", "sha256"}
GPU_EVIDENCE_ID = re.compile(r"^[0-9a-f]{32}$")
BUDGET_THRESHOLD_POLICY = "max-cache-state-p95-plus-one-refresh-interval-ceil-ms-v1"
BUDGET_THRESHOLD_SOURCE = "derived-from-bound-reference-host-raw-v1"
AUDIO_PROVIDER_ENTRY_POINTS = [
    "pulp::inspect::ControlGpuHealthProvider::begin_editor_open",
    "pulp::inspect::ControlGpuHealthProvider::record_presented_frame",
    "pulp::inspect::ControlGpuHealthProvider::record_timeout",
    "pulp::inspect::ControlGpuHealthProvider::record_instance_lost",
    "pulp::inspect::ControlGpuHealthProvider::record_dropped_events",
    "pulp::inspect::ControlGpuHealthProvider::snapshot",
]


class AcceptanceError(Exception):
    """The receipt cannot substantiate its declared state."""


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AcceptanceError(f"cannot read JSON artifact {path}: {error}") from error


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class ArtifactSnapshot:
    path: Path
    data: bytes
    sha256: str


def snapshot_relative(path_text: str, evidence_root: Path, label: str) -> ArtifactSnapshot:
    relative = Path(path_text)
    if relative.is_absolute() or not relative.parts or ".." in relative.parts:
        raise AcceptanceError(f"{label}.path must be a safe relative path")
    flags_dir = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
    flags_file = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    descriptors: list[int] = []
    try:
        directory_fd = os.open(evidence_root, flags_dir)
        descriptors.append(directory_fd)
        for component in relative.parts[:-1]:
            directory_fd = os.open(component, flags_dir, dir_fd=directory_fd)
            descriptors.append(directory_fd)
        file_fd = os.open(relative.parts[-1], flags_file, dir_fd=directory_fd)
        descriptors.append(file_fd)
        before = os.fstat(file_fd)
        if not stat.S_ISREG(before.st_mode):
            raise AcceptanceError(f"{label} must be a regular file")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(file_fd, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        after = os.fstat(file_fd)
        signature = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
        if signature(before) != signature(after):
            raise AcceptanceError(f"{label} changed while it was being snapshotted")
        data = b"".join(chunks)
        return ArtifactSnapshot(evidence_root / relative, data, sha256_bytes(data))
    except OSError as error:
        raise AcceptanceError(f"cannot safely snapshot {label}: {error}") from error
    finally:
        for descriptor in reversed(descriptors):
            os.close(descriptor)


def exact_keys(value: Any, keys: set[str], label: str) -> None:
    if not isinstance(value, dict):
        raise AcceptanceError(f"{label} must be an object")
    actual = set(value)
    if actual != keys:
        raise AcceptanceError(
            f"{label} keys differ: missing={sorted(keys - actual)} "
            f"unexpected={sorted(actual - keys)}"
        )


def resolve_artifact(ref: dict[str, str], evidence_root: Path, label: str) -> ArtifactSnapshot:
    exact_keys(ref, ARTIFACT_KEYS, label)
    snapshot = snapshot_relative(ref["path"], evidence_root, label)
    if snapshot.sha256 != ref["sha256"]:
        raise AcceptanceError(
            f"{label} digest mismatch: declared={ref['sha256']} actual={snapshot.sha256}"
        )
    return snapshot


def artifact_json(ref: dict[str, str], evidence_root: Path, label: str) -> dict[str, Any]:
    snapshot = resolve_artifact(ref, evidence_root, label)
    try:
        payload = json.loads(snapshot.data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AcceptanceError(f"cannot parse JSON artifact {label}: {error}") from error
    if not isinstance(payload, dict):
        raise AcceptanceError(f"{label} JSON root must be an object")
    return payload


def validate_declared_artifacts(value: Any, evidence_root: Path, label: str = "receipt") -> None:
    """Digest-check every artifact a receipt declares, including nonterminal receipts."""
    if isinstance(value, list):
        for index, item in enumerate(value):
            validate_declared_artifacts(item, evidence_root, f"{label}[{index}]")
        return
    if not isinstance(value, dict):
        return
    if set(value) == ARTIFACT_KEYS:
        resolve_artifact(value, evidence_root, label)
        return
    for key, item in value.items():
        validate_declared_artifacts(item, evidence_root, f"{label}.{key}")


def nearest_rank(values: list[float], percentile: float) -> float:
    if not values:
        raise AcceptanceError("cannot derive percentile from no trials")
    rank = max(1, math.ceil(percentile * len(values) / 100.0))
    return sorted(values)[rank - 1]


def validate_raw_samples(
    payload: dict[str, Any], *, schema: str, cache_state: str, label: str,
    identity: dict[str, Any] | None = None,
    reference_host: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    keys = {"schema", "version", "cache_state", "samples"}
    if identity is not None:
        keys.add("identity")
    if reference_host is not None:
        keys.add("reference_host")
    exact_keys(payload, keys, label)
    if payload["schema"] != schema or payload["version"] != 1:
        raise AcceptanceError(f"{label} has the wrong schema or version")
    if payload["cache_state"] != cache_state:
        raise AcceptanceError(f"{label} cache_state must be {cache_state}")
    if identity is not None:
        exact_keys(payload["identity"], IDENTITY_KEYS, f"{label}.identity")
        if payload["identity"] != identity:
            raise AcceptanceError(f"{label} identity does not match its campaign")
    if reference_host is not None:
        exact_keys(payload["reference_host"], {"host_id", "machine_id", "refresh_rate_hz"}, f"{label}.reference_host")
        if payload["reference_host"] != reference_host:
            raise AcceptanceError(f"{label} reference_host does not match budget ratification")
    samples = payload["samples"]
    if not isinstance(samples, list) or len(samples) != 10:
        raise AcceptanceError(f"{label} must contain exactly 10 samples")
    sequences: set[int] = set()
    for index, sample in enumerate(samples):
        exact_keys(sample, {"sequence", "duration_ms", "hitch_ms"}, f"{label}.samples[{index}]")
        if not isinstance(sample["sequence"], int) or isinstance(sample["sequence"], bool):
            raise AcceptanceError(f"{label}.samples[{index}].sequence must be an integer")
        for metric in ("duration_ms", "hitch_ms"):
            value = sample[metric]
            if (
                not isinstance(value, (int, float)) or isinstance(value, bool)
                or not math.isfinite(value) or value < 0
            ):
                raise AcceptanceError(f"{label}.samples[{index}].{metric} must be finite and non-negative")
        sequences.add(sample["sequence"])
    if len(sequences) != len(samples):
        raise AcceptanceError(f"{label} sample sequences must be unique")
    return samples


def validate_reference_host(host: Any, label: str) -> None:
    exact_keys(host, {"host_id", "machine_id", "refresh_rate_hz"}, label)
    if not isinstance(host["host_id"], str) or not host["host_id"]:
        raise AcceptanceError(f"{label}.host_id must be non-empty")
    if not isinstance(host["machine_id"], str) or not host["machine_id"]:
        raise AcceptanceError(f"{label}.machine_id must be non-empty")
    rate = host["refresh_rate_hz"]
    if (
        not isinstance(rate, (int, float)) or isinstance(rate, bool)
        or not math.isfinite(rate) or rate < 24 or rate > 360
    ):
        raise AcceptanceError(f"{label}.refresh_rate_hz must be finite and within [24, 360]")


def derive_budget_threshold_ms(
    cold_samples: list[dict[str, Any]], warm_samples: list[dict[str, Any]],
    reference_host: dict[str, Any],
) -> int:
    """Derive the v1 threshold from the bound reference-host raw observations."""
    cold_p95 = nearest_rank([float(row["duration_ms"]) for row in cold_samples], 95)
    warm_p95 = nearest_rank([float(row["duration_ms"]) for row in warm_samples], 95)
    refresh_interval_ms = 1000.0 / float(reference_host["refresh_rate_hz"])
    return math.ceil(max(cold_p95, warm_p95) + refresh_interval_ms)


def validate_budget(receipt: dict[str, Any], evidence_root: Path) -> dict[str, Any]:
    budget = receipt["budget"]
    refs = (budget["receipt"], budget["raw_cold"], budget["raw_warm"])
    if budget["status"] != "ratified" or any(ref is None for ref in refs):
        raise AcceptanceError("complete receipt requires a ratified budget and all budget artifacts")
    cold = artifact_json(budget["raw_cold"], evidence_root, "budget.raw_cold")
    warm = artifact_json(budget["raw_warm"], evidence_root, "budget.raw_warm")
    ratification = artifact_json(budget["receipt"], evidence_root, "budget.receipt")
    keys = {
        "schema", "version", "budget_id", "budget_version", "status", "plan_revision",
        "pulp_revision", "clock_origin", "endpoint", "interaction_hitch_metric",
        "trial_count", "cold_trial_count", "warm_trial_count", "percentile",
        "threshold_ms", "threshold_policy", "threshold_source", "reference_hosts",
        "cold_raw_sha256", "warm_raw_sha256",
    }
    exact_keys(ratification, keys, "budget.receipt")
    expected = {
        "schema": "pulp.gpu-first-visible-budget.v1",
        "version": 1,
        "budget_id": budget["id"],
        "budget_version": budget["version"],
        "status": "ratified",
        "plan_revision": receipt["plan"]["revision"],
        "pulp_revision": receipt["identity"]["pulp_revision"],
        "clock_origin": "editor_open_requested",
        "endpoint": "first_nonblank_presented_frame",
        "interaction_hitch_metric": "max_present_interval_before_first_nonblank_ms",
        "trial_count": 20,
        "cold_trial_count": 10,
        "warm_trial_count": 10,
        "percentile": 95,
        "threshold_policy": BUDGET_THRESHOLD_POLICY,
        "threshold_source": BUDGET_THRESHOLD_SOURCE,
        "cold_raw_sha256": budget["raw_cold"]["sha256"],
        "warm_raw_sha256": budget["raw_warm"]["sha256"],
    }
    for key, value in expected.items():
        if ratification[key] != value:
            raise AcceptanceError(f"budget.receipt.{key} must equal {value!r}")
    hosts = ratification["reference_hosts"]
    if not isinstance(hosts, list) or len(hosts) != 1:
        raise AcceptanceError("budget.receipt.reference_hosts must contain exactly one derivation host")
    validate_reference_host(hosts[0], "budget.receipt.reference_hosts[0]")
    cold_samples = validate_raw_samples(
        cold, schema="pulp.gpu-first-visible-budget-raw.v1", cache_state="cold",
        label="budget.raw_cold", reference_host=hosts[0],
    )
    warm_samples = validate_raw_samples(
        warm, schema="pulp.gpu-first-visible-budget-raw.v1", cache_state="warm",
        label="budget.raw_warm", reference_host=hosts[0],
    )
    derived_threshold = derive_budget_threshold_ms(cold_samples, warm_samples, hosts[0])
    if ratification["threshold_ms"] != derived_threshold:
        raise AcceptanceError(
            "budget.receipt.threshold_ms is not derived from the bound reference-host raw samples: "
            f"declared={ratification['threshold_ms']!r} derived={derived_threshold!r}"
        )
    return ratification


def validate_health(
    payload: dict[str, Any], *, identity: dict[str, Any], budget_receipt: dict[str, Any],
    cold_samples: list[dict[str, Any]], warm_samples: list[dict[str, Any]], label: str,
) -> None:
    health_schema = load_json(HEALTH_SCHEMA_PATH)
    problems = json_schema_lite.validate(payload, health_schema)
    if problems:
        raise AcceptanceError(f"{label} violates gpu-health-read-result.v1: {problems[0]}")
    startup = payload["startup"]
    health = payload["health"]
    if health["run_id"] != identity["campaign_id"] or health["verdict"] != "pass" or health["health_state"] != "healthy" or health["render_requested"] is not True:
        raise AcceptanceError(f"{label} run_id/verdict does not bind a passing campaign")
    required = [probe for probe in health["probes"] if probe["required"]]
    if not required or any(
        probe["verdict"] != "pass" or probe["adapter"]["status"] != "authentic"
        or probe["adapter"]["class"] != "hardware"
        or probe["measurements"]["readback_completed"] is not True
        or probe["measurements"]["pixel_output_produced"] is not True
        or probe["measurements"]["content_floor_passed"] is not True
        for probe in required
    ):
        raise AcceptanceError(f"{label} nested health result lacks required authentic readback proof")
    sequence = 0
    for probe in health["probes"]:
        stages: set[str] = set()
        for event in probe["events"]:
            if event["sequence"] != sequence or event["stage"] in stages:
                raise AcceptanceError(f"{label} nested health event sequence/stages are invalid")
            sequence += 1
            stages.add(event["stage"])
    if startup["status"] != "complete" or startup["verdict"] not in {"pass", "fail"}:
        raise AcceptanceError(f"{label} startup result is not a complete measured verdict")
    expected_budget = {
        "budget_id": budget_receipt["budget_id"],
        "version": budget_receipt["budget_version"],
        "status": "ratified",
        "clock_origin": budget_receipt["clock_origin"],
        "endpoint": budget_receipt["endpoint"],
        "interaction_hitch_metric": budget_receipt["interaction_hitch_metric"],
        "trial_count": 20,
        "cold_trial_count": 10,
        "warm_trial_count": 10,
        "percentile": 95,
        "threshold_ms": budget_receipt["threshold_ms"],
        "threshold_source": budget_receipt["threshold_source"],
        "reference_hosts": [
            {"host_id": host["host_id"], "refresh_rate_hz": host["refresh_rate_hz"]}
            for host in budget_receipt["reference_hosts"]
        ],
    }
    if startup["budget"] != expected_budget:
        raise AcceptanceError(f"{label} budget differs from the ratified budget")
    if startup["identity"]["pulp_build_id"] != identity["build_id"]:
        raise AcceptanceError(f"{label} build identity differs from its campaign")
    if startup["identity"]["adapter_class"] != "hardware":
        raise AcceptanceError(f"{label} must use an authentic hardware adapter")
    capture = startup["capture"]
    if capture["event_count"] > capture["event_capacity"] or capture["dropped_event_count"] != 0 or capture["truncated"] or capture["missing_trace_categories"]:
        raise AcceptanceError(f"{label} capture is lossy or incomplete")
    correlation = startup["correlation"]
    if (
        not isinstance(correlation["gpu_evidence_id"], str)
        or GPU_EVIDENCE_ID.fullmatch(correlation["gpu_evidence_id"]) is None
        or not isinstance(correlation["trace_evidence_id"], str)
        or not correlation["trace_evidence_id"]
        or correlation["trace_evidence_id"] == correlation["gpu_evidence_id"]
    ):
        raise AcceptanceError(f"{label} lacks GPU/trace evidence identifiers")
    trials = startup["trials"]
    if len(trials) != 20:
        raise AcceptanceError(f"{label} must contain exactly 20 trials")
    if any(
        trial["verdict"] in {"unavailable", "unverified"}
        or trial["diagnostic_code"] not in {"gpu.startup.pass", "gpu.startup.budget_exceeded"}
        or (trial["diagnostic_code"] == "gpu.startup.pass" and trial["verdict"] != "pass")
        or (trial["diagnostic_code"] == "gpu.startup.budget_exceeded" and trial["verdict"] != "fail")
        or trial["content_floor_passed"] is not True or trial["visible_state"] == "unknown"
        or any(
            not isinstance(trial[field], (int, float)) or isinstance(trial[field], bool)
            or not math.isfinite(trial[field]) or trial[field] < 0
            for field in ("shader_compile_ms", "upload_ms", "hidden_frame_ms", "present_ms")
        )
        or trial["observed_target_signature_sha256"] != startup["identity"]["expected_target_signature_sha256"]
        or trial["sequence"] != index
        for index, trial in enumerate(trials)
    ):
        raise AcceptanceError(
            f"{label} contains a non-passing, unbound, or native-present-uncorroborated trial"
        )
    if any(startup["identity"][key] is None for key in (
        "source_signature_sha256", "shader_signature_sha256", "expected_target_signature_sha256"
    )):
        raise AcceptanceError(f"{label} lacks source, shader, or target identity")
    cold = [trial for trial in trials if trial["cache_state"] == "cold"]
    warm = [trial for trial in trials if trial["cache_state"] == "warm"]
    if len(cold) != 10 or len(warm) != 10:
        raise AcceptanceError(f"{label} must contain 10 cold and 10 warm trials")
    projected = lambda rows: [
        {"sequence": row["sequence"], "duration_ms": row["editor_open_to_first_nonblank_ms"], "hitch_ms": row["interaction_hitch_ms"]}
        for row in rows
    ]
    if projected(cold) != cold_samples or projected(warm) != warm_samples:
        raise AcceptanceError(f"{label} raw cold/warm samples differ from its health result")
    durations = [float(trial["editor_open_to_first_nonblank_ms"]) for trial in trials]
    hitches = [float(trial["interaction_hitch_ms"]) for trial in trials]
    if startup["observed_percentile_ms"] != nearest_rank(durations, 95):
        raise AcceptanceError(f"{label} observed percentile is not nearest-rank p95")
    if startup["interaction_hitch_percentile_ms"] != nearest_rank(hitches, 95):
        raise AcceptanceError(f"{label} hitch percentile is not nearest-rank p95")
    expected_verdict = "pass" if startup["observed_percentile_ms"] <= budget_receipt["threshold_ms"] else "fail"
    if startup["verdict"] != expected_verdict:
        raise AcceptanceError(f"{label} verdict is not derived from the ratified budget")


def validate_campaign_trace(
    campaign: dict[str, Any], health: dict[str, Any], evidence_root: Path, label: str,
) -> None:
    trace = resolve_artifact(campaign["trace"], evidence_root, f"{label}.trace")
    if not trace.data:
        raise AcceptanceError(f"{label}.trace must be non-empty")
    analysis = artifact_json(campaign["trace_analysis"], evidence_root, f"{label}.trace_analysis")
    keys = {
        "schema", "version", "question", "verdict", "capture_complete", "campaign_id",
        "instance_id", "build_id", "gpu_evidence_id", "trace_evidence_id", "trace_sha256",
        "health_result_sha256", "evidence_ids",
    }
    exact_keys(analysis, keys, f"{label}.trace_analysis")
    identity = campaign["identity"]
    correlation = health["startup"]["correlation"]
    expected = {
        "schema": "pulp.gpu-first-visible-campaign-trace.v1",
        "version": 1,
        "question": "gpu-startup",
        "verdict": health["startup"]["verdict"],
        "capture_complete": True,
        "campaign_id": identity["campaign_id"],
        "instance_id": identity["instance_id"],
        "build_id": identity["build_id"],
        "gpu_evidence_id": correlation["gpu_evidence_id"],
        "trace_evidence_id": correlation["trace_evidence_id"],
        "trace_sha256": campaign["trace"]["sha256"],
        "health_result_sha256": campaign["health_result"]["sha256"],
        "evidence_ids": [correlation["gpu_evidence_id"]],
    }
    if analysis != expected:
        raise AcceptanceError(f"{label}.trace_analysis does not exactly corroborate the campaign")


def validate_campaigns(
    receipt: dict[str, Any], evidence_root: Path, budget_receipt: dict[str, Any],
) -> dict[str, tuple[dict[str, Any], dict[str, Any]]]:
    campaigns = receipt["campaigns"]
    roles = [campaign["role"] for campaign in campaigns]
    if len(campaigns) != 4 or set(roles) != CAMPAIGN_ROLES or len(set(roles)) != 4:
        raise AcceptanceError("complete receipt requires exactly one campaign for every required role")
    results: dict[str, tuple[dict[str, Any], dict[str, Any]]] = {}
    for campaign in campaigns:
        role = campaign["role"]
        label = f"campaigns[{role}]"
        if campaign["status"] != "pass":
            raise AcceptanceError(f"{label} is not pass")
        if any(campaign[key] is None for key in (
            "health_result", "raw_cold", "raw_warm", "product_artifact", "host_artifact",
            "trace", "trace_analysis",
        )):
            raise AcceptanceError(f"{label} is missing a required artifact")
        identity = campaign["identity"]
        if identity["pulp_revision"] != receipt["identity"]["pulp_revision"]:
            raise AcceptanceError(f"{label} Pulp revision differs from the receipt")
        if role == "forge" and (
            identity["forge_revision"] is None
            or identity["forge_revision"] != receipt["identity"]["forge_revision"]
        ):
            raise AcceptanceError("Forge campaign is not bound to the receipt's Forge revision")
        if role == "headless-constrained" and identity["plugin_format"] != "headless":
            raise AcceptanceError("headless-constrained campaign must use headless format")
        if role == "standalone" and identity["plugin_format"] != "standalone":
            raise AcceptanceError("standalone campaign must use standalone format")
        if role in {"daw", "forge"} and identity["plugin_format"] not in {"auv2", "vst3", "clap"}:
            raise AcceptanceError(f"{label} must use a real plugin format")
        cold_payload = artifact_json(campaign["raw_cold"], evidence_root, f"{label}.raw_cold")
        warm_payload = artifact_json(campaign["raw_warm"], evidence_root, f"{label}.raw_warm")
        cold = validate_raw_samples(
            cold_payload, schema="pulp.gpu-first-visible-campaign-raw.v1", cache_state="cold",
            label=f"{label}.raw_cold", identity=identity,
        )
        warm = validate_raw_samples(
            warm_payload, schema="pulp.gpu-first-visible-campaign-raw.v1", cache_state="warm",
            label=f"{label}.raw_warm", identity=identity,
        )
        health = artifact_json(campaign["health_result"], evidence_root, f"{label}.health_result")
        validate_health(
            health, identity=identity, budget_receipt=budget_receipt,
            cold_samples=cold, warm_samples=warm, label=f"{label}.health_result",
        )
        resolve_artifact(campaign["product_artifact"], evidence_root, f"{label}.product_artifact")
        resolve_artifact(campaign["host_artifact"], evidence_root, f"{label}.host_artifact")
        validate_campaign_trace(campaign, health, evidence_root, label)
        results[role] = (campaign, health)
    return results


def canonical_analysis(payload: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(payload)
    correlation = result.get("ui_correlation")
    if isinstance(correlation, dict) and isinstance(correlation.get("open_command"), str):
        correlation["open_command"] = "pulp trace open <snapshotted-trace>"
    return result


def replay_analyzer(analyzer: ArtifactSnapshot, trace: ArtifactSnapshot) -> dict[str, Any]:
    def run_once(analyzer_path: Path, trace_path: Path, trace_bytes: bytes) -> dict[str, Any]:
        trace_path.write_bytes(trace_bytes)
        run = subprocess.run(
            [str(analyzer_path), "trace", "gpu-startup", "--trace", str(trace_path), "--json"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=60, check=False,
        )
        try:
            payload = json.loads(run.stdout)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise AcceptanceError(f"snapshotted analyzer returned invalid JSON: {error}") from error
        if not isinstance(payload, dict):
            raise AcceptanceError("snapshotted analyzer returned a non-object")
        expected_exit = {"pass": 0, "fail": 1, "unavailable": 2, "unverified": 2}
        if payload.get("verdict") not in expected_exit or run.returncode != expected_exit[payload["verdict"]]:
            raise AcceptanceError("snapshotted analyzer exit code disagrees with its verdict")
        return payload

    with tempfile.TemporaryDirectory(prefix="pulp-a3-analyzer-") as temporary:
        directory = Path(temporary)
        analyzer_path = directory / "pulp"
        trace_path = directory / "capture.pftrace"
        analyzer_path.write_bytes(analyzer.data)
        analyzer_path.chmod(0o700)
        payload = run_once(analyzer_path, trace_path, trace.data)
        negative = run_once(analyzer_path, trace_path, b"PULP-A3-INVALID-TRACE-NEGATIVE")
    if negative.get("verdict") == "pass" or negative.get("capture_complete") is True:
        raise AcceptanceError("snapshotted analyzer failed the invalid-trace negative control")
    return payload


def validate_a2t_receipt(
    payload: dict[str, Any], receipt: dict[str, Any], same: dict[str, Any],
    analyzer: ArtifactSnapshot, trace: ArtifactSnapshot, derived: dict[str, Any],
) -> None:
    root_keys = {
        "acceptance", "adapter_relevance", "artifacts", "fresh_start", "generated_utc",
        "human_perfetto_ui_correlation", "machine", "mcp_source_revision", "measured",
        "measurement_environment", "producer_overhead_disposition", "protocol", "schema",
        "scope", "semantic_result", "source_revision",
    }
    exact_keys(payload, root_keys, "same_instance_a2t.a2t_receipt")
    if payload["schema"] != "pulp.gpu-trace-overhead-acceptance.v1":
        raise AcceptanceError("same_instance_a2t.a2t_receipt has the wrong schema")
    if payload["source_revision"] != receipt["identity"]["pulp_revision"]:
        raise AcceptanceError("A2T source revision does not match the A3 Pulp revision")
    if payload["mcp_source_revision"] != payload["source_revision"]:
        raise AcceptanceError("A2T CLI and MCP source revisions differ")
    if payload["scope"] != "offline-installed-cli-mcp-analysis":
        raise AcceptanceError("A2T receipt has the wrong scope")
    producer = payload["producer_overhead_disposition"]
    if not isinstance(producer, dict):
        raise AcceptanceError("A2T producer overhead disposition must be an object")
    if (
        producer.get("formal_plan_status") != "accepted-canonical-plan"
        or not isinstance(producer.get("formal_plan_revision"), str)
        or re.fullmatch(r"[0-9a-f]{40}", producer["formal_plan_revision"]) is None
        or not isinstance(producer.get("formal_plan_sha256"), str)
        or re.fullmatch(r"[0-9a-f]{64}", producer["formal_plan_sha256"]) is None
        or producer["formal_plan_revision"] != receipt["plan"]["revision"]
        or producer["formal_plan_sha256"] != receipt["plan"]["sha256"]
    ):
        raise AcceptanceError("A2T no-producer disposition is not accepted by the exact A3 plan")
    acceptance = payload["acceptance"]
    exact_keys(acceptance, {
        "human_perfetto_ui_correlation", "offline_latency_budget", "producer_overhead_budget",
        "same_installed_prefix", "semantic_parity", "xrun_check",
    }, "same_instance_a2t.a2t_receipt.acceptance")
    if acceptance["semantic_parity"] != "pass" or acceptance["same_installed_prefix"] != "pass":
        raise AcceptanceError("A2T receipt lacks CLI/MCP semantic and installed-prefix parity")
    if acceptance["human_perfetto_ui_correlation"] != "pass":
        raise AcceptanceError("A2T receipt lacks human Perfetto UI correlation")
    protocol = payload["protocol"]
    exact_keys(protocol, {
        "environment_path", "fresh_start_paired_trials", "mcp_lifecycle",
        "measured_paired_trials", "order", "question", "warmups",
    }, "same_instance_a2t.a2t_receipt.protocol")
    if (
        protocol["question"] != "gpu-startup" or protocol["warmups"] != 5
        or protocol["measured_paired_trials"] != 30
        or protocol["fresh_start_paired_trials"] != 20
        or protocol["order"] != "alternating cli-first/mcp-first"
    ):
        raise AcceptanceError("A2T receipt does not contain the required measurement protocol")
    artifacts = payload["artifacts"]
    exact_keys(artifacts, {"cli", "install_prefix_role", "mcp", "sibling_binding", "trace", "trace_processor"}, "same_instance_a2t.a2t_receipt.artifacts")
    for name in ("cli", "mcp", "trace", "trace_processor"):
        exact_keys(artifacts[name], {"role", "sha256", "bytes"}, f"A2T artifacts.{name}")
        if not isinstance(artifacts[name]["bytes"], int) or artifacts[name]["bytes"] <= 0:
            raise AcceptanceError(f"A2T artifacts.{name}.bytes must be positive")
    exact_keys(artifacts["sibling_binding"], {"mechanism", "verified_same_resolved_parent"}, "A2T artifacts.sibling_binding")
    if artifacts["sibling_binding"]["verified_same_resolved_parent"] is not True:
        raise AcceptanceError("A2T analyzer and MCP are not bound to one installed prefix")
    if artifacts["cli"]["sha256"] != analyzer.sha256 or artifacts["cli"]["bytes"] != len(analyzer.data):
        raise AcceptanceError("A2T CLI digest does not pin the supplied analyzer")
    if artifacts["trace"]["sha256"] != trace.sha256 or artifacts["trace"]["bytes"] != len(trace.data):
        raise AcceptanceError("A2T trace digest does not pin the supplied trace")
    for section, count in (("measured", 30), ("fresh_start", 20)):
        rows = payload[section].get("raw_samples") if isinstance(payload[section], dict) else None
        if not isinstance(rows, list) or len(rows) != count:
            raise AcceptanceError(f"A2T {section} raw sample count is not {count}")
    if canonical_analysis(payload["semantic_result"]) != canonical_analysis(derived):
        raise AcceptanceError("A2T semantic result differs from analyzer replay")
    human = payload["human_perfetto_ui_correlation"]
    if not isinstance(human, dict) or human.get("artifact_sha256") != trace.sha256:
        raise AcceptanceError("A2T human Perfetto review is not bound to the trace")
    if same["gpu_evidence_id"] not in derived.get("evidence_ids", []):
        raise AcceptanceError("A2T analyzer replay lacks the same-instance GPU evidence id")


def validate_same_instance(
    receipt: dict[str, Any], evidence_root: Path,
    campaigns: dict[str, tuple[dict[str, Any], dict[str, Any]]],
) -> dict[str, Any]:
    same = receipt["same_instance_a2t"]
    if same["status"] != "pass" or any(same[key] is None for key in (
        "campaign_id", "instance_id", "build_id", "gpu_evidence_id", "trace_evidence_id",
        "a2t_receipt", "analyzer", "trace", "trace_analysis", "binding_receipt",
    )):
        raise AcceptanceError("complete receipt requires passing same-instance A2T evidence")
    matches = [item for item in campaigns.values() if item[0]["identity"]["campaign_id"] == same["campaign_id"]]
    if len(matches) != 1:
        raise AcceptanceError("same-instance campaign_id must identify exactly one campaign")
    campaign, health = matches[0]
    if campaign["trace"]["sha256"] != same["trace"]["sha256"]:
        raise AcceptanceError("same-instance trace is not the causal campaign trace")
    identity = campaign["identity"]
    correlation = health["startup"]["correlation"]
    expected = {
        "campaign_id": identity["campaign_id"],
        "instance_id": identity["instance_id"],
        "build_id": identity["build_id"],
        "gpu_evidence_id": correlation["gpu_evidence_id"],
        "trace_evidence_id": correlation["trace_evidence_id"],
    }
    for key, value in expected.items():
        if same[key] != value:
            raise AcceptanceError(f"same_instance_a2t.{key} is not cross-bound to the campaign")
    a2t = artifact_json(same["a2t_receipt"], evidence_root, "same_instance_a2t.a2t_receipt")
    analyzer_snapshot = resolve_artifact(same["analyzer"], evidence_root, "same_instance_a2t.analyzer")
    analysis = artifact_json(same["trace_analysis"], evidence_root, "same_instance_a2t.trace_analysis")
    if (
        analysis.get("schema") != "pulp.trace-gpu-analysis.v1"
        or analysis.get("question") != "gpu-startup"
        or analysis.get("capture_complete") is not True
        or same["gpu_evidence_id"] not in analysis.get("evidence_ids", [])
    ):
        raise AcceptanceError("same-instance trace analysis is incomplete or lacks the GPU evidence id")
    trace_snapshot = resolve_artifact(same["trace"], evidence_root, "same_instance_a2t.trace")
    if not trace_snapshot.data:
        raise AcceptanceError("same-instance trace artifact is empty")
    derived = replay_analyzer(analyzer_snapshot, trace_snapshot)
    if canonical_analysis(analysis) != canonical_analysis(derived):
        raise AcceptanceError("submitted trace analysis differs from exact analyzer replay")
    validate_a2t_receipt(a2t, receipt, same, analyzer_snapshot, trace_snapshot, derived)
    binding = artifact_json(same["binding_receipt"], evidence_root, "same_instance_a2t.binding_receipt")
    binding_keys = {
        "schema", "version", "campaign_id", "instance_id", "build_id", "gpu_evidence_id",
        "trace_evidence_id", "a2t_receipt_sha256", "analyzer_sha256", "trace_sha256",
        "trace_analysis_sha256", "health_result_sha256",
    }
    exact_keys(binding, binding_keys, "same_instance_a2t.binding_receipt")
    binding_expected = {
        "schema": "pulp.gpu-first-visible-a2t-binding.v1", "version": 1,
        **expected,
        "a2t_receipt_sha256": same["a2t_receipt"]["sha256"],
        "analyzer_sha256": same["analyzer"]["sha256"],
        "trace_sha256": same["trace"]["sha256"],
        "trace_analysis_sha256": same["trace_analysis"]["sha256"],
        "health_result_sha256": campaign["health_result"]["sha256"],
    }
    if binding != binding_expected:
        raise AcceptanceError("same-instance binding receipt does not exactly bind all causal artifacts")
    return derived


def validate_controls(receipt: dict[str, Any], evidence_root: Path) -> None:
    blank = receipt["blank_negative"]
    if blank["status"] != "caught" or blank["diagnostic_code"] != "gpu.startup.blank" or blank["receipt"] is None:
        raise AcceptanceError("complete receipt requires a caught blank negative")
    blank_payload = artifact_json(blank["receipt"], evidence_root, "blank_negative.receipt")
    blank_keys = {"schema", "version", "injection", "expected_diagnostic_code", "observed_diagnostic_code", "caught"}
    exact_keys(blank_payload, blank_keys, "blank_negative.receipt")
    if blank_payload != {
        "schema": "pulp.gpu-first-visible-blank-negative.v1", "version": 1,
        "injection": "transparent-first-frame", "expected_diagnostic_code": "gpu.startup.blank",
        "observed_diagnostic_code": "gpu.startup.blank", "caught": True,
    }:
        raise AcceptanceError("blank negative receipt does not prove the intended failure")
    audio = receipt["audio_thread_exclusion"]
    if audio["status"] != "pass" or audio["receipt"] is None:
        raise AcceptanceError("complete receipt requires audio-thread exclusion proof")
    audio_payload = artifact_json(audio["receipt"], evidence_root, "audio_thread_exclusion.receipt")
    audio_keys = {
        "schema", "version", "policy", "proof_scope", "provider_type",
        "instrumentation_entry_points", "thread_classification_source",
        "known_audio_thread_ids", "entry_point_observations", "observed_audio_thread_events",
        "positive_control_non_audio_events", "runtime_claim",
    }
    exact_keys(audio_payload, audio_keys, "audio_thread_exclusion.receipt")
    if (
        audio_payload["schema"] != "pulp.gpu-first-visible-audio-thread-exclusion.v1"
        or audio_payload["version"] != 1
        or audio_payload["policy"] != "gpu-health-work-must-not-run-on-audio-thread"
        or audio_payload["proof_scope"] != "external-instrumented-harness"
        or audio_payload["provider_type"] != "pulp::inspect::ControlGpuHealthProvider"
        or audio_payload["instrumentation_entry_points"] != AUDIO_PROVIDER_ENTRY_POINTS
        or audio_payload["thread_classification_source"] != "external-harness-explicit-thread-registration"
        or not audio_payload["known_audio_thread_ids"]
        or audio_payload["observed_audio_thread_events"] != 0
        or audio_payload["runtime_claim"] != "external-harness-only-not-product-runtime-proof"
    ):
        raise AcceptanceError("audio-thread exclusion receipt lacks a positive control or observed zero")
    if any(
        not isinstance(thread_id, int) or isinstance(thread_id, bool) or thread_id <= 0
        for thread_id in audio_payload["known_audio_thread_ids"]
    ):
        raise AcceptanceError("audio-thread exclusion receipt has invalid registered audio thread ids")
    observations = audio_payload["entry_point_observations"]
    if not isinstance(observations, list) or len(observations) != len(AUDIO_PROVIDER_ENTRY_POINTS):
        raise AcceptanceError("audio-thread exclusion receipt does not cover every provider entry point")
    total_non_audio = 0
    for index, (observation, entry_point) in enumerate(zip(observations, AUDIO_PROVIDER_ENTRY_POINTS)):
        exact_keys(
            observation, {"entry_point", "audio_thread_events", "non_audio_thread_events"},
            f"audio_thread_exclusion.receipt.entry_point_observations[{index}]",
        )
        if (
            observation["entry_point"] != entry_point
            or observation["audio_thread_events"] != 0
            or not isinstance(observation["non_audio_thread_events"], int)
            or isinstance(observation["non_audio_thread_events"], bool)
            or observation["non_audio_thread_events"] < 0
        ):
            raise AcceptanceError("audio-thread exclusion receipt has invalid entry-point observations")
        total_non_audio += observation["non_audio_thread_events"]
    if total_non_audio < 1 or audio_payload["positive_control_non_audio_events"] != total_non_audio:
        raise AcceptanceError("audio-thread exclusion receipt lacks an exact non-audio positive control")


def derive_b4_disposition(
    startup: dict[str, Any], derived_analysis: dict[str, Any],
) -> tuple[str, str | None, int, int]:
    contributors = derived_analysis.get("contributors")
    if not isinstance(contributors, list):
        contributors = []
    dominant_stage = derived_analysis.get("dominant_stage")
    dominant_duration_ns = 0
    for contributor in contributors:
        if not isinstance(contributor, dict) or contributor.get("stage") != dominant_stage:
            continue
        value = contributor.get("duration_ns")
        if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
            dominant_duration_ns = value
            break
    threshold_ms = startup["budget"]["threshold_ms"]
    materiality_floor_ns = math.ceil(float(threshold_ms) * 1_000_000 * 0.10)
    if startup["verdict"] == "pass":
        disposition = "no-change"
    elif (
        derived_analysis.get("capture_complete") is True
        and dominant_stage == "pipeline-prepare"
        and dominant_duration_ns >= materiality_floor_ns
    ):
        disposition = "queue-B4"
    elif derived_analysis.get("capture_complete") is True and dominant_stage is not None:
        disposition = "no-change"
    else:
        disposition = "queue-B4-investigation"
    return disposition, dominant_stage, dominant_duration_ns, materiality_floor_ns


def validate_b4(
    receipt: dict[str, Any], evidence_root: Path,
    campaigns: dict[str, tuple[dict[str, Any], dict[str, Any]]],
    derived_analysis: dict[str, Any],
) -> None:
    b4 = receipt["b4"]
    status_by_disposition = {
        "queue-B4": "queued",
        "queue-B4-investigation": "queued-investigation",
        "no-change": "closed-no-change",
    }
    causal = [item for item in campaigns.values()
              if item[0]["identity"]["campaign_id"] == receipt["same_instance_a2t"]["campaign_id"]]
    if len(causal) != 1:
        raise AcceptanceError("B4 cannot identify one causal campaign")
    causal_campaign, causal_health = causal[0]
    startup = causal_health["startup"]
    threshold_ms = causal_health["startup"]["budget"]["threshold_ms"]
    (derived_disposition, dominant_stage, dominant_duration_ns,
     materiality_floor_ns) = derive_b4_disposition(startup, derived_analysis)
    disposition = b4["disposition"]
    if (
        disposition != derived_disposition
        or disposition not in status_by_disposition
        or b4["status"] != status_by_disposition[disposition]
    ):
        raise AcceptanceError("complete receipt requires exactly one legal B4 disposition/status pair")
    if b4["evidence"] is None:
        raise AcceptanceError("complete receipt requires B4 disposition evidence")
    payload = artifact_json(b4["evidence"], evidence_root, "b4.evidence")
    evidence_keys = {
        "schema", "version", "policy", "disposition", "campaign_id", "startup_verdict",
        "observed_percentile_ms", "threshold_ms", "analyzer_verdict", "capture_complete",
        "dominant_stage", "dominant_duration_ns", "materiality_floor_ns",
        "campaign_health_sha256", "a2t_binding_sha256", "trace_analysis_sha256", "reason",
    }
    exact_keys(payload, evidence_keys, "b4.evidence")
    expected = {
        "schema": "pulp.gpu-first-visible-b4-disposition.v1",
        "version": 1,
        "policy": "pulp.b4-disposition-policy.v1",
        "disposition": derived_disposition,
        "campaign_id": receipt["same_instance_a2t"]["campaign_id"],
        "startup_verdict": startup["verdict"],
        "observed_percentile_ms": startup["observed_percentile_ms"],
        "threshold_ms": threshold_ms,
        "analyzer_verdict": derived_analysis.get("verdict"),
        "capture_complete": derived_analysis.get("capture_complete"),
        "dominant_stage": dominant_stage,
        "dominant_duration_ns": dominant_duration_ns,
        "materiality_floor_ns": materiality_floor_ns,
        "campaign_health_sha256": causal_campaign["health_result"]["sha256"],
        "a2t_binding_sha256": receipt["same_instance_a2t"]["binding_receipt"]["sha256"],
        "trace_analysis_sha256": receipt["same_instance_a2t"]["trace_analysis"]["sha256"],
        "reason": b4["reason"],
    }
    if payload != expected:
        raise AcceptanceError("B4 evidence does not bind the selected disposition")


def validate_receipt(receipt: dict[str, Any], evidence_root: Path) -> bool:
    schema = load_json(SCHEMA_PATH)
    problems = json_schema_lite.validate(receipt, schema)
    if problems:
        raise AcceptanceError(f"receipt schema violation: {problems[0]}")
    validate_declared_artifacts(receipt, evidence_root)
    if receipt["status"] == "incomplete":
        if not receipt["missing_evidence"]:
            raise AcceptanceError("incomplete receipt must enumerate missing_evidence")
        if receipt["b4"]["disposition"] is not None or receipt["b4"]["status"] != "withheld":
            raise AcceptanceError("incomplete receipt must withhold B4 disposition")
        return False
    if receipt["missing_evidence"]:
        raise AcceptanceError("complete receipt cannot list missing evidence")
    if receipt["identity"]["forge_revision"] is None:
        raise AcceptanceError("complete receipt must bind the exact Forge revision")
    budget = validate_budget(receipt, evidence_root)
    campaigns = validate_campaigns(receipt, evidence_root, budget)
    derived_analysis = validate_same_instance(receipt, evidence_root, campaigns)
    validate_controls(receipt, evidence_root)
    validate_b4(receipt, evidence_root, campaigns, derived_analysis)
    return True


def materialize_auto_hashes(value: Any, evidence_root: Path) -> Any:
    if isinstance(value, list):
        return [materialize_auto_hashes(item, evidence_root) for item in value]
    if not isinstance(value, dict):
        return value
    if set(value) == ARTIFACT_KEYS and value.get("sha256") == "auto":
        ref = copy.deepcopy(value)
        relative = Path(ref["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise AcceptanceError("auto-hashed artifact path must be safe and relative")
        ref["sha256"] = snapshot_relative(ref["path"], evidence_root, "auto-hashed artifact").sha256
        return ref
    return {key: materialize_auto_hashes(item, evidence_root) for key, item in value.items()}


def atomic_write(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify = subparsers.add_parser("verify")
    verify.add_argument("receipt", type=Path)
    verify.add_argument("--evidence-root", type=Path)
    generate = subparsers.add_parser("generate")
    generate.add_argument("template", type=Path)
    generate.add_argument("--output", required=True, type=Path)
    generate.add_argument("--evidence-root", type=Path)
    args = parser.parse_args(argv)
    try:
        source = args.receipt if args.command == "verify" else args.template
        evidence_root = (args.evidence_root or source.parent).resolve()
        receipt = load_json(source)
        if args.command == "generate":
            receipt = materialize_auto_hashes(receipt, evidence_root)
        terminal = validate_receipt(receipt, evidence_root)
        if args.command == "generate":
            atomic_write(args.output, receipt)
        print("A3 acceptance: PASS" if terminal else "A3 acceptance: NONTERMINAL")
        return 0 if terminal else 2
    except AcceptanceError as error:
        print(f"A3 acceptance: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
