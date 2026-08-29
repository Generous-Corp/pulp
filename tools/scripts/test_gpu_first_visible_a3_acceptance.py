#!/usr/bin/env python3
"""Positive and planted-negative tests for the closed A3 acceptance receipt."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any
from unittest import mock

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import gpu_first_visible_a3_acceptance as a3  # noqa: E402
import gpu_trace_overhead_acceptance as a2t  # noqa: E402
from test_gpu_health_read_contract import startup_document  # noqa: E402

PULP_REVISION = subprocess.check_output(
    ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
).strip()
FORGE_REVISION = "2" * 40
PLAN_REVISION = "3" * 40
TRACE_SOURCE_PATH = Path("test/fixtures/perfetto-gpu/healthy.pftrace")
REFERENCE_HOST = {
    "host_id": "m5-blackbook", "machine_id": "Mac15,14", "refresh_rate_hz": 60,
}


def write_json(root: Path, name: str, payload: Any) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def auto(path: str) -> dict[str, str]:
    return {"path": path, "sha256": "auto"}


def identity(role: str, index: int) -> dict[str, Any]:
    plugin_format = {
        "standalone": "standalone",
        "headless-constrained": "headless",
        "daw": "vst3",
        "forge": "standalone",
    }[role]
    return {
        "pulp_revision": PULP_REVISION,
        "forge_revision": FORGE_REVISION if role == "forge" else None,
        "build_id": f"pulp-build-{index}",
        "product_id": f"dev.pulp.product-{index}",
        "product_name": f"GPU Product {index}",
        "plugin_format": plugin_format,
        "machine_id": "m5-blackbook",
        "instance_id": f"instance-{index}",
        "campaign_id": f"campaign-{index}",
    }


def samples(cache_state: str) -> list[dict[str, Any]]:
    start = 0 if cache_state == "cold" else 10
    return [
        {"sequence": sequence, "duration_ms": 10 + sequence, "hitch_ms": 1 + sequence / 10}
        for sequence in range(start, start + 10)
    ]


def health_result(campaign_identity: dict[str, Any], budget: dict[str, Any]) -> dict[str, Any]:
    health = copy.deepcopy(json.loads(
        (ROOT / "test/fixtures/gpu-ux/pass-hardware.json").read_text(encoding="utf-8")
    ))
    health["run_id"] = campaign_identity["campaign_id"]
    document = startup_document(health)
    target = document["startup"]["identity"]["expected_target_signature_sha256"]
    document["startup"]["budget"] = {
        "budget_id": budget["budget_id"],
        "version": budget["budget_version"],
        "status": "ratified",
        "clock_origin": budget["clock_origin"],
        "endpoint": budget["endpoint"],
        "interaction_hitch_metric": budget["interaction_hitch_metric"],
        "trial_count": 20,
        "cold_trial_count": 10,
        "warm_trial_count": 10,
        "percentile": 95,
        "threshold_ms": budget["threshold_ms"],
        "threshold_source": budget["threshold_source"],
        "reference_hosts": [
            {"host_id": host["host_id"], "refresh_rate_hz": host["refresh_rate_hz"]}
            for host in budget["reference_hosts"]
        ],
    }
    document["startup"]["identity"]["pulp_build_id"] = campaign_identity["build_id"]
    campaign_number = int(campaign_identity["campaign_id"].rsplit("-", 1)[1])
    document["startup"]["correlation"] = {
        "gpu_evidence_id": f"{campaign_number:032x}",
        "trace_evidence_id": f"trace-{campaign_identity['campaign_id']}",
    }
    document["startup"]["pipeline_contribution"] = "unverified"
    document["startup"]["causal_attribution"] = "unverified"
    document["startup"]["disposition"] = None
    combined = samples("cold") + samples("warm")
    headless = campaign_identity["plugin_format"] == "headless"
    document["startup"]["measurement_endpoint"] = (
        "headless-capture-complete" if headless else "native-compositor-presentation"
    )
    document["startup"]["capture"]["missing_trace_categories"] = (
        ["native_present"] if headless else []
    )
    document["startup"]["trials"] = [
        {
            "sequence": sample["sequence"],
            "cache_state": "cold" if sample["sequence"] < 10 else "warm",
            "editor_open_to_first_nonblank_ms": sample["duration_ms"],
            "interaction_hitch_ms": sample["hitch_ms"],
            "shader_compile_ms": 2 if sample["sequence"] < 10 else 0,
            "upload_ms": 1,
            "hidden_frame_ms": 1,
            "present_ms": None if headless else 2,
            "observed_target_signature_sha256": target,
            "content_floor_passed": True,
            "visible_state": "prepared",
            "verdict": "pass",
            "diagnostic_code": "gpu.startup.pass",
        }
        for sample in combined
    ]
    document["startup"]["observed_percentile_ms"] = a3.nearest_rank(
        [float(sample["duration_ms"]) for sample in combined], 95
    )
    document["startup"]["interaction_hitch_percentile_ms"] = a3.nearest_rank(
        [float(sample["hitch_ms"]) for sample in combined], 95
    )
    return document


def make_fixture(root: Path) -> dict[str, Any]:
    budget_cold = {
        "schema": "pulp.gpu-first-visible-budget-raw.v1", "version": 1,
        "reference_host": REFERENCE_HOST, "cache_state": "cold", "samples": samples("cold"),
    }
    budget_warm = {
        "schema": "pulp.gpu-first-visible-budget-raw.v1", "version": 1,
        "reference_host": REFERENCE_HOST, "cache_state": "warm", "samples": samples("warm"),
    }
    write_json(root, "budget-cold.json", budget_cold)
    write_json(root, "budget-warm.json", budget_warm)
    budget = {
        "schema": "pulp.gpu-first-visible-budget.v1",
        "version": 1,
        "budget_id": "pulp.editor-first-visible.v1",
        "budget_version": 1,
        "status": "ratified",
        "plan_revision": PLAN_REVISION,
        "pulp_revision": PULP_REVISION,
        "clock_origin": "editor_open_requested",
        "endpoint": "first_nonblank_presented_frame",
        "interaction_hitch_metric": "max_present_interval_before_first_nonblank_ms",
        "trial_count": 20,
        "cold_trial_count": 10,
        "warm_trial_count": 10,
        "percentile": 95,
        "threshold_ms": 46,
        "threshold_policy": a3.BUDGET_THRESHOLD_POLICY,
        "threshold_source": a3.BUDGET_THRESHOLD_SOURCE,
        "reference_hosts": [REFERENCE_HOST],
        "cold_raw_sha256": a3.sha256_bytes((root / "budget-cold.json").read_bytes()),
        "warm_raw_sha256": a3.sha256_bytes((root / "budget-warm.json").read_bytes()),
    }
    write_json(root, "budget.json", budget)

    campaigns = []
    roles = ["standalone", "headless-constrained", "daw", "forge"]
    for index, role in enumerate(roles, start=1):
        campaign_identity = identity(role, index)
        for cache_state in ("cold", "warm"):
            write_json(root, f"{role}-{cache_state}.json", {
                "schema": "pulp.gpu-first-visible-campaign-raw.v1",
                "version": 1,
                "identity": campaign_identity,
                "cache_state": cache_state,
                "samples": samples(cache_state),
            })
        result = health_result(campaign_identity, budget)
        write_json(root, f"{role}-health.json", result)
        (root / f"{role}-product.bin").write_bytes(f"product:{role}".encode())
        (root / f"{role}-host.bin").write_bytes(f"host:{role}".encode())
        trace_bytes = (
            (ROOT / TRACE_SOURCE_PATH).read_bytes()
            if role == "forge" else f"trace:{role}".encode()
        )
        (root / f"{role}-trace.pftrace").write_bytes(trace_bytes)
        correlation = result["startup"]["correlation"]
        write_json(root, f"{role}-trace-analysis.json", {
            "schema": "pulp.gpu-first-visible-campaign-trace.v1",
            "version": 1,
            "question": "gpu-startup",
            "verdict": result["startup"]["verdict"],
            "capture_complete": role != "headless-constrained",
            "measurement_endpoint": result["startup"]["measurement_endpoint"],
            "capture_integrity": "lossless",
            "instrumentation_coverage": (
                "partial" if result["startup"]["capture"]["missing_trace_categories"]
                else "complete"
            ),
            "missing_trace_categories": result["startup"]["capture"]["missing_trace_categories"],
            "campaign_id": campaign_identity["campaign_id"],
            "instance_id": campaign_identity["instance_id"],
            "build_id": campaign_identity["build_id"],
            "gpu_evidence_id": correlation["gpu_evidence_id"],
            "trace_evidence_id": correlation["trace_evidence_id"],
            "trace_sha256": a3.sha256_bytes(trace_bytes),
            "health_result_sha256": a3.sha256_bytes((root / f"{role}-health.json").read_bytes()),
            "evidence_ids": [correlation["gpu_evidence_id"]],
        })
        campaigns.append({
            "role": role,
            "measurement_endpoint": result["startup"]["measurement_endpoint"],
            "status": "pass",
            "identity": campaign_identity,
            "health_result": auto(f"{role}-health.json"),
            "raw_cold": auto(f"{role}-cold.json"),
            "raw_warm": auto(f"{role}-warm.json"),
            "product_artifact": auto(f"{role}-product.bin"),
            "host_artifact": auto(f"{role}-host.bin"),
            "trace": auto(f"{role}-trace.pftrace"),
            "trace_analysis": auto(f"{role}-trace-analysis.json"),
        })

    causal = campaigns[-1]
    causal_health = json.loads((root / "forge-health.json").read_text(encoding="utf-8"))
    gpu_id = causal_health["startup"]["correlation"]["gpu_evidence_id"]
    trace_id = causal_health["startup"]["correlation"]["trace_evidence_id"]
    analysis = {
        "schema": "pulp.trace-gpu-analysis.v1",
        "question": "gpu-startup",
        "verdict": "pass",
        "capture_complete": True,
        "dominant_stage": "pipeline-prepare",
        "contributors": [{
            "rank": 1, "stage": "pipeline-prepare", "duration_ns": 6_000_000,
            "evidence_id": gpu_id, "frame_index": 0, "health_state": "healthy", "sequence": 1,
        }],
        "evidence_ids": [gpu_id],
    }
    write_json(root, "trace-analysis.json", analysis)
    forge_trace_digest = a3.sha256_bytes((root / "forge-trace.pftrace").read_bytes())
    analyzer_source = f'''#!/usr/bin/python3
import hashlib, json, pathlib, sys
trace = pathlib.Path(sys.argv[sys.argv.index("--trace") + 1]).read_bytes()
if hashlib.sha256(trace).hexdigest() == {forge_trace_digest!r}:
    payload = {analysis!r}
    code = 0
else:
    payload = {{"schema":"pulp.trace-gpu-analysis.v1","question":"gpu-startup","verdict":"unavailable","capture_complete":False,"evidence_ids":[]}}
    code = 2
print(json.dumps(payload, sort_keys=True))
raise SystemExit(code)
'''
    (root / "pulp-analyzer").write_text(analyzer_source, encoding="utf-8")
    (root / "pulp-analyzer").chmod(0o700)
    analyzer_digest = a3.sha256_bytes((root / "pulp-analyzer").read_bytes())
    trace_digest = a3.sha256_bytes((root / "forge-trace.pftrace").read_bytes())
    source_binding = a2t.source_binding(ROOT, PULP_REVISION, ROOT / TRACE_SOURCE_PATH)
    write_json(root, "a2t.json", {
        "schema": "pulp.gpu-trace-overhead-acceptance.v1",
        "generated_utc": "2026-08-28T12:00:00Z",
        "source_revision": PULP_REVISION,
        "mcp_source_revision": PULP_REVISION,
        "integration_head": source_binding["integration_head"],
        "source_blobs": source_binding["source_blobs"],
        "scope": "offline-installed-cli-mcp-analysis",
        "producer_overhead_disposition": {
            "status": "not-applicable-no-added-producer-cost",
            "formal_plan_status": "accepted-canonical-plan",
            "formal_plan_revision": PLAN_REVISION,
            "formal_plan_sha256": "4" * 64,
        },
        "machine": {"machine_id": "m5-blackbook"},
        "adapter_relevance": "saved-trace analysis performs no GPU work",
        "artifacts": {
            "install_prefix_role": "isolated-measurement-prefix",
            "sibling_binding": {"verified_same_resolved_parent": True, "mechanism": "same prefix"},
            "cli": {"role": "installed-prefix/bin/pulp", "sha256": analyzer_digest, "bytes": len(analyzer_source.encode())},
            "mcp": {"role": "installed-prefix/bin/pulp-mcp", "sha256": "8" * 64, "bytes": 1},
            "trace": {
                "role": f"repository/{TRACE_SOURCE_PATH.as_posix()}",
                "sha256": trace_digest,
                "bytes": (root / "forge-trace.pftrace").stat().st_size,
            },
            "trace_processor": {"role": "pinned trace processor", "sha256": "9" * 64, "bytes": 1},
        },
        "protocol": {
            "question": "gpu-startup", "warmups": 5, "measured_paired_trials": 30,
            "fresh_start_paired_trials": 20, "order": "alternating cli-first/mcp-first",
            "mcp_lifecycle": "persistent and fresh", "environment_path": "/usr/bin:/bin",
        },
        "measurement_environment": {"interpretation": "fixture"},
        "semantic_result": analysis,
        "measured": {"raw_samples": [{"trial": i} for i in range(30)]},
        "fresh_start": {"raw_samples": [{"trial": i} for i in range(20)]},
        "acceptance": {
            "semantic_parity": "pass", "same_installed_prefix": "pass",
            "human_perfetto_ui_correlation": "pass",
            "offline_latency_budget": "unverified-no-ratified-budget",
            "producer_overhead_budget": "not-applicable-horizon-a-no-producer-delta",
            "xrun_check": "not-applicable-offline-no-audio-thread",
        },
        "human_perfetto_ui_correlation": {
            "artifact_sha256": trace_digest,
            "reviewer": "human reviewer",
            "reviewed_utc": "2026-08-28T05:34:54Z",
            "ui_revision": "v58.3-11fbaed8",
            "delivery": "official localhost embedding protocol",
            "observed_spans": [{"name": "gpu_pipeline_prepare", "duration_ns": 6_000_000}],
        },
    })
    binding = {
        "schema": "pulp.gpu-first-visible-a2t-binding.v1",
        "version": 1,
        "campaign_id": causal["identity"]["campaign_id"],
        "instance_id": causal["identity"]["instance_id"],
        "build_id": causal["identity"]["build_id"],
        "gpu_evidence_id": gpu_id,
        "trace_evidence_id": trace_id,
        "a2t_receipt_sha256": a3.sha256_bytes((root / "a2t.json").read_bytes()),
        "analyzer_sha256": analyzer_digest,
        "trace_sha256": a3.sha256_bytes((root / "forge-trace.pftrace").read_bytes()),
        "trace_analysis_sha256": a3.sha256_bytes((root / "trace-analysis.json").read_bytes()),
        "health_result_sha256": a3.sha256_bytes((root / "forge-health.json").read_bytes()),
    }
    write_json(root, "binding.json", binding)
    write_json(root, "blank.json", {
        "schema": "pulp.gpu-first-visible-blank-negative.v1",
        "version": 1,
        "injection": "transparent-first-frame",
        "expected_diagnostic_code": "gpu.startup.blank",
        "observed_diagnostic_code": "gpu.startup.blank",
        "caught": True,
    })
    observations = [
        {"entry_point": entry_point, "audio_thread_events": 0,
         "non_audio_thread_events": 20 if index < 2 else 0}
        for index, entry_point in enumerate(a3.AUDIO_PROVIDER_ENTRY_POINTS)
    ]
    write_json(root, "audio.json", {
        "schema": "pulp.gpu-first-visible-audio-thread-exclusion.v1",
        "version": 1,
        "policy": "gpu-health-work-must-not-run-on-audio-thread",
        "proof_scope": "external-instrumented-harness",
        "provider_type": "pulp::inspect::ControlGpuHealthProvider",
        "instrumentation_entry_points": a3.AUDIO_PROVIDER_ENTRY_POINTS,
        "thread_classification_source": "external-harness-explicit-thread-registration",
        "known_audio_thread_ids": [101],
        "entry_point_observations": observations,
        "observed_audio_thread_events": 0,
        "positive_control_non_audio_events": 40,
        "runtime_claim": "external-harness-only-not-product-runtime-proof",
    })
    b4_reason = "The correlated trace shows startup rendering is not material."
    write_json(root, "b4.json", {
        "schema": "pulp.gpu-first-visible-b4-disposition.v1",
        "version": 1,
        "policy": "pulp.b4-disposition-policy.v1",
        "disposition": "no-change",
        "campaign_id": causal["identity"]["campaign_id"],
        "startup_verdict": causal_health["startup"]["verdict"],
        "observed_percentile_ms": causal_health["startup"]["observed_percentile_ms"],
        "threshold_ms": budget["threshold_ms"],
        "analyzer_verdict": analysis["verdict"],
        "capture_complete": analysis["capture_complete"],
        "dominant_stage": analysis["dominant_stage"],
        "dominant_duration_ns": analysis["contributors"][0]["duration_ns"],
        "materiality_floor_ns": 4_600_000,
        "campaign_health_sha256": a3.sha256_bytes((root / "forge-health.json").read_bytes()),
        "a2t_binding_sha256": a3.sha256_bytes((root / "binding.json").read_bytes()),
        "trace_analysis_sha256": a3.sha256_bytes((root / "trace-analysis.json").read_bytes()),
        "reason": b4_reason,
        "capture_integrity": "lossless",
        "instrumentation_coverage": "complete",
        "missing_causal_fields": [],
        "instrumentation_gaps": [],
        "observed_interval": {
            "clock_origin": "editor_open_requested",
            "endpoint": "native-compositor-presentation",
            "p95_ms": causal_health["startup"]["observed_percentile_ms"],
        },
        "ownership_projection_sha256": a3.sha256_bytes(
            (ROOT / ".github/vellum-ownership.json").read_bytes()
        ),
    })
    return {
        "$schema": "../contracts/gpu-first-visible-a3-acceptance-v1.schema.json",
        "schema": "dev.pulp.gpu-first-visible-a3-acceptance",
        "version": 1,
        "status": "complete",
        "recorded_at": "2026-08-28T12:00:00Z",
        "plan": {
            "document": "research/2026-08-27-vgpu-gpu-ux-inspiration-audit-and-plan.md",
            "revision": PLAN_REVISION,
            "sha256": "4" * 64,
        },
        "identity": causal["identity"],
        "budget": {
            "id": "pulp.editor-first-visible.v1", "version": 1, "status": "ratified",
            "receipt": auto("budget.json"), "raw_cold": auto("budget-cold.json"),
            "raw_warm": auto("budget-warm.json"),
        },
        "campaigns": campaigns,
        "same_instance_a2t": {
            "status": "pass",
            "campaign_id": causal["identity"]["campaign_id"],
            "instance_id": causal["identity"]["instance_id"],
            "build_id": causal["identity"]["build_id"],
            "gpu_evidence_id": gpu_id,
            "trace_evidence_id": trace_id,
            "a2t_receipt": auto("a2t.json"),
            "analyzer": auto("pulp-analyzer"),
            "trace": auto("forge-trace.pftrace"),
            "trace_analysis": auto("trace-analysis.json"),
            "binding_receipt": auto("binding.json"),
        },
        "blank_negative": {
            "status": "caught", "diagnostic_code": "gpu.startup.blank",
            "receipt": auto("blank.json"),
        },
        "audio_thread_exclusion": {"status": "pass", "receipt": auto("audio.json")},
        "b4": {
            "disposition": "no-change", "status": "closed-no-change",
            "reason": b4_reason, "evidence": auto("b4.json"),
        },
        "observations": [],
        "missing_evidence": [],
    }


def expect_failure(receipt: dict[str, Any], root: Path, needle: str) -> None:
    try:
        a3.validate_receipt(receipt, root)
    except a3.AcceptanceError as error:
        if needle not in str(error):
            raise AssertionError(f"expected {needle!r}, got {error!r}") from error
    else:
        raise AssertionError(f"planted negative unexpectedly passed: {needle}")


def rehash(receipt: dict[str, Any], root: Path, ref: dict[str, str]) -> None:
    ref["sha256"] = a3.sha256_bytes((root / ref["path"]).read_bytes())


def instrumentation_gaps() -> list[dict[str, Any]]:
    route_by_field = {
        "hidden_frame_ms": "core/render/src/render_loop.cpp",
        "present_ms": "core/render/src/render_loop_apple.mm",
        "shader_compile_ms": "core/render/src/gpu_surface_dawn.cpp",
        "shader_signature_sha256": "core/render/src/gpu_surface_dawn.cpp",
        "source_signature_sha256": "core/render/src/gpu_surface_dawn.cpp",
        "upload_ms": "core/render/src/gpu_surface_dawn.cpp",
    }
    return [{
        "field": field,
        "missing_event": a3.CAUSAL_GAP_EVENTS[field],
        "required_arguments": sorted(a3.CAUSAL_GAP_ARGUMENTS[field]),
        "route_slice": "render-skia-dawn",
        "route_path": route_by_field[field],
        "route_repository": "Generous-Corp/vellum",
    } for field in sorted(route_by_field)]


def make_partial_causal_fixture(root: Path, *, budget_fail: bool) -> dict[str, Any]:
    receipt = make_fixture(root)
    forge = next(campaign for campaign in receipt["campaigns"] if campaign["role"] == "forge")
    health_path = root / forge["health_result"]["path"]
    health = json.loads(health_path.read_text(encoding="utf-8"))
    startup = health["startup"]
    startup["identity"]["source_signature_sha256"] = None
    startup["identity"]["shader_signature_sha256"] = None
    startup["capture"]["missing_trace_categories"] = [
        "hidden_frame", "native_present_timing", "pipeline_compile",
        "resource_upload", "shader_identity", "source_identity",
    ]
    startup["pipeline_contribution"] = "unattributed" if budget_fail else "unverified"
    startup["causal_attribution"] = "incomplete" if budget_fail else "unavailable"
    startup["disposition"] = "queue-B4-investigation" if budget_fail else "no-change"
    duration = 60 if budget_fail else None
    for trial in startup["trials"]:
        for field in a3.CAUSAL_TRIAL_FIELDS:
            trial[field] = None
        if duration is not None:
            trial["editor_open_to_first_nonblank_ms"] = duration
            trial["verdict"] = "fail"
            trial["diagnostic_code"] = "gpu.startup.budget_exceeded"
    if duration is not None:
        startup["observed_percentile_ms"] = duration
        startup["verdict"] = "fail"
        for cache_state in ("cold", "warm"):
            raw_ref = forge[f"raw_{cache_state}"]
            raw = json.loads((root / raw_ref["path"]).read_text(encoding="utf-8"))
            for sample in raw["samples"]:
                sample["duration_ms"] = duration
            write_json(root, raw_ref["path"], raw)
    write_json(root, forge["health_result"]["path"], health)

    campaign_analysis_ref = forge["trace_analysis"]
    campaign_analysis = json.loads(
        (root / campaign_analysis_ref["path"]).read_text(encoding="utf-8")
    )
    campaign_analysis["verdict"] = startup["verdict"]
    campaign_analysis["capture_complete"] = False
    campaign_analysis["instrumentation_coverage"] = "partial"
    campaign_analysis["missing_trace_categories"] = startup["capture"]["missing_trace_categories"]
    write_json(root, campaign_analysis_ref["path"], campaign_analysis)

    gpu_id = startup["correlation"]["gpu_evidence_id"]
    analysis = {
        "schema": "pulp.trace-gpu-analysis.v1",
        "question": "gpu-startup",
        "verdict": "unverified",
        "capture_complete": False,
        "dominant_stage": None,
        "contributors": [],
        "evidence_ids": [gpu_id],
    }
    write_json(root, "trace-analysis.json", analysis)
    forge_trace_digest = a3.sha256_bytes((root / "forge-trace.pftrace").read_bytes())
    analyzer_source = f'''#!/usr/bin/python3
import hashlib, json, pathlib, sys
trace = pathlib.Path(sys.argv[sys.argv.index("--trace") + 1]).read_bytes()
if hashlib.sha256(trace).hexdigest() == {forge_trace_digest!r}:
    payload = {analysis!r}
else:
    payload = {{"schema":"pulp.trace-gpu-analysis.v1","question":"gpu-startup","verdict":"unavailable","capture_complete":False,"evidence_ids":[]}}
print(json.dumps(payload, sort_keys=True))
raise SystemExit(2)
'''
    (root / "pulp-analyzer").write_text(analyzer_source, encoding="utf-8")
    (root / "pulp-analyzer").chmod(0o700)

    a2t = json.loads((root / "a2t.json").read_text(encoding="utf-8"))
    a2t["semantic_result"] = analysis
    a2t["artifacts"]["cli"]["sha256"] = a3.sha256_bytes((root / "pulp-analyzer").read_bytes())
    a2t["artifacts"]["cli"]["bytes"] = len((root / "pulp-analyzer").read_bytes())
    write_json(root, "a2t.json", a2t)

    receipt = a3.materialize_auto_hashes(receipt, root)
    forge = next(campaign for campaign in receipt["campaigns"] if campaign["role"] == "forge")
    campaign_analysis = json.loads((root / campaign_analysis_ref["path"]).read_text(encoding="utf-8"))
    campaign_analysis["health_result_sha256"] = forge["health_result"]["sha256"]
    write_json(root, campaign_analysis_ref["path"], campaign_analysis)
    rehash(receipt, root, forge["trace_analysis"])

    same = receipt["same_instance_a2t"]
    rehash(receipt, root, same["analyzer"])
    rehash(receipt, root, same["trace_analysis"])
    rehash(receipt, root, same["a2t_receipt"])
    binding = json.loads((root / same["binding_receipt"]["path"]).read_text(encoding="utf-8"))
    binding["health_result_sha256"] = forge["health_result"]["sha256"]
    binding["trace_analysis_sha256"] = same["trace_analysis"]["sha256"]
    binding["analyzer_sha256"] = same["analyzer"]["sha256"]
    binding["a2t_receipt_sha256"] = same["a2t_receipt"]["sha256"]
    write_json(root, same["binding_receipt"]["path"], binding)
    rehash(receipt, root, same["binding_receipt"])

    disposition = "queue-B4-investigation" if budget_fail else "no-change"
    receipt["b4"] = {
        "disposition": disposition,
        "status": "queued-investigation" if budget_fail else "closed-no-change",
        "reason": "Exact transferred instrumentation gaps are named for the causal rerun."
                  if budget_fail else "The ratified end-to-end budget passes despite causal gaps.",
        "evidence": receipt["b4"]["evidence"],
    }
    b4 = json.loads((root / receipt["b4"]["evidence"]["path"]).read_text(encoding="utf-8"))
    b4.update({
        "disposition": disposition,
        "startup_verdict": startup["verdict"],
        "observed_percentile_ms": startup["observed_percentile_ms"],
        "analyzer_verdict": "unverified",
        "capture_complete": False,
        "dominant_stage": None,
        "dominant_duration_ns": 0,
        "campaign_health_sha256": forge["health_result"]["sha256"],
        "a2t_binding_sha256": same["binding_receipt"]["sha256"],
        "trace_analysis_sha256": same["trace_analysis"]["sha256"],
        "reason": receipt["b4"]["reason"],
        "instrumentation_coverage": "partial",
        "missing_causal_fields": sorted(a3.CAUSAL_GAP_ARGUMENTS),
        "instrumentation_gaps": instrumentation_gaps(),
        "observed_interval": {
            "clock_origin": "editor_open_requested",
            "endpoint": "native-compositor-presentation",
            "p95_ms": startup["observed_percentile_ms"],
        },
    })
    write_json(root, receipt["b4"]["evidence"]["path"], b4)
    rehash(receipt, root, receipt["b4"]["evidence"])
    return receipt


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pulp-a3-acceptance-") as temporary:
        root = Path(temporary)
        template = make_fixture(root)
        write_json(root, "template.json", template)
        output = root / "receipt.json"
        run = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "gpu_first_visible_a3_acceptance.py"),
             "generate", str(root / "template.json"), "--output", str(output),
             "--evidence-root", str(root)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
        )
        assert run.returncode == 0, run.stderr
        receipt = json.loads(output.read_text(encoding="utf-8"))
        assert a3.validate_receipt(receipt, root) is True

        causal_startup = json.loads((root / "forge-health.json").read_text(encoding="utf-8"))["startup"]
        causal_analysis = json.loads((root / "trace-analysis.json").read_text(encoding="utf-8"))
        assert a3.derive_b4_disposition(causal_startup, causal_analysis)[0] == "no-change"
        missed = copy.deepcopy(causal_startup)
        missed["verdict"] = "fail"
        assert a3.derive_b4_disposition(missed, causal_analysis)[0] == "queue-B4"
        unattributed = copy.deepcopy(causal_analysis)
        unattributed["dominant_stage"] = None
        unattributed["contributors"] = []
        assert a3.derive_b4_disposition(missed, unattributed)[0] == "queue-B4-investigation"
        other = copy.deepcopy(causal_analysis)
        other["dominant_stage"] = "resource-upload"
        other["contributors"][0]["stage"] = "resource-upload"
        assert a3.derive_b4_disposition(missed, other)[0] == "no-change"

        with tempfile.TemporaryDirectory(prefix="pulp-a3-partial-pass-") as partial:
            partial_root = Path(partial)
            partial_pass = make_partial_causal_fixture(partial_root, budget_fail=False)
            assert a3.validate_receipt(partial_pass, partial_root) is True

        with tempfile.TemporaryDirectory(prefix="pulp-a3-investigation-") as partial:
            partial_root = Path(partial)
            investigation = make_partial_causal_fixture(partial_root, budget_fail=True)
            assert a3.validate_receipt(investigation, partial_root) is True

            wrong_endpoint = copy.deepcopy(investigation)
            headless = next(
                campaign for campaign in wrong_endpoint["campaigns"]
                if campaign["role"] == "headless-constrained"
            )
            headless["measurement_endpoint"] = "native-compositor-presentation"
            expect_failure(wrong_endpoint, partial_root, "required role endpoint")

            missing_argument = copy.deepcopy(investigation)
            evidence_ref = missing_argument["b4"]["evidence"]
            evidence_path = partial_root / evidence_ref["path"]
            original_evidence = evidence_path.read_text(encoding="utf-8")
            evidence = json.loads(original_evidence)
            evidence["instrumentation_gaps"][0]["required_arguments"] = [
                "debug.gpu_evidence_id"
            ]
            write_json(partial_root, evidence_ref["path"], evidence)
            rehash(missing_argument, partial_root, evidence_ref)
            expect_failure(missing_argument, partial_root, "exact missing event or arguments")
            evidence_path.write_text(original_evidence, encoding="utf-8")

            wrong_event = copy.deepcopy(investigation)
            evidence_ref = wrong_event["b4"]["evidence"]
            evidence = json.loads(original_evidence)
            evidence["instrumentation_gaps"][0]["missing_event"] = "gpu_a3_missing"
            write_json(partial_root, evidence_ref["path"], evidence)
            rehash(wrong_event, partial_root, evidence_ref)
            expect_failure(wrong_event, partial_root, "exact missing event or arguments")
            evidence_path.write_text(original_evidence, encoding="utf-8")

            wrong_route = copy.deepcopy(investigation)
            evidence_ref = wrong_route["b4"]["evidence"]
            evidence = json.loads(original_evidence)
            evidence["instrumentation_gaps"][0]["route_path"] = "inspect/src/control_gpu_health_provider.cpp"
            write_json(partial_root, evidence_ref["path"], evidence)
            rehash(wrong_route, partial_root, evidence_ref)
            expect_failure(wrong_route, partial_root, "transferred Vellum path")
            evidence_path.write_text(original_evidence, encoding="utf-8")

            lossy = copy.deepcopy(investigation)
            forge = next(campaign for campaign in lossy["campaigns"] if campaign["role"] == "forge")
            health_ref = forge["health_result"]
            health_path = partial_root / health_ref["path"]
            original_health = health_path.read_text(encoding="utf-8")
            health = json.loads(original_health)
            health["startup"]["capture"]["dropped_event_count"] = 1
            health["startup"]["capture"]["truncated"] = True
            write_json(partial_root, health_ref["path"], health)
            rehash(lossy, partial_root, health_ref)
            expect_failure(lossy, partial_root, "capture integrity is lossy")
            health_path.write_text(original_health, encoding="utf-8")

            unnamed = copy.deepcopy(investigation)
            forge = next(campaign for campaign in unnamed["campaigns"] if campaign["role"] == "forge")
            health_ref = forge["health_result"]
            health = json.loads(original_health)
            health["startup"]["capture"]["missing_trace_categories"] = []
            write_json(partial_root, health_ref["path"], health)
            rehash(unnamed, partial_root, health_ref)
            expect_failure(unnamed, partial_root, "nullable causal fields without named")
            health_path.write_text(original_health, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        mutated["unexpected"] = True
        expect_failure(mutated, root, "schema violation")

        incomplete = copy.deepcopy(receipt)
        incomplete["status"] = "incomplete"
        incomplete["missing_evidence"] = ["standalone campaign"]
        incomplete["b4"] = {
            "disposition": None, "status": "withheld", "reason": "Acceptance is incomplete.",
            "evidence": None,
        }
        assert a3.validate_receipt(incomplete, root) is False
        incomplete["status"] = "complete"
        expect_failure(incomplete, root, "cannot list missing evidence")

        mutated = copy.deepcopy(receipt)
        mutated["budget"]["status"] = "unratified"
        expect_failure(mutated, root, "ratified budget")

        mutated = copy.deepcopy(receipt)
        budget_ref = mutated["budget"]["receipt"]
        budget_path = root / budget_ref["path"]
        original_budget = budget_path.read_text(encoding="utf-8")
        absurd_budget = json.loads(original_budget)
        absurd_budget["threshold_ms"] = 300000
        write_json(root, budget_ref["path"], absurd_budget)
        rehash(mutated, root, budget_ref)
        expect_failure(mutated, root, "not derived from the bound reference-host raw samples")
        budget_path.write_text(original_budget, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        budget_cold_ref = mutated["budget"]["raw_cold"]
        budget_cold_path = root / budget_cold_ref["path"]
        original_budget_cold = budget_cold_path.read_text(encoding="utf-8")
        wrong_host = json.loads(original_budget_cold)
        wrong_host["reference_host"]["machine_id"] = "different-machine"
        write_json(root, budget_cold_ref["path"], wrong_host)
        rehash(mutated, root, budget_cold_ref)
        budget_ref = mutated["budget"]["receipt"]
        budget_path = root / budget_ref["path"]
        original_budget = budget_path.read_text(encoding="utf-8")
        rebound_budget = json.loads(original_budget)
        rebound_budget["cold_raw_sha256"] = budget_cold_ref["sha256"]
        write_json(root, budget_ref["path"], rebound_budget)
        rehash(mutated, root, budget_ref)
        expect_failure(mutated, root, "reference_host does not match")
        budget_cold_path.write_text(original_budget_cold, encoding="utf-8")
        budget_path.write_text(original_budget, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        standalone = mutated["campaigns"][0]
        health_path = root / standalone["health_result"]["path"]
        original_health = health_path.read_text(encoding="utf-8")
        health = json.loads(original_health)
        health["startup"]["observed_percentile_ms"] = 0
        write_json(root, standalone["health_result"]["path"], health)
        rehash(mutated, root, standalone["health_result"])
        expect_failure(mutated, root, "nearest-rank p95")
        health_path.write_text(original_health, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        standalone = mutated["campaigns"][0]
        health_path = root / standalone["health_result"]["path"]
        original_health = health_path.read_text(encoding="utf-8")
        health = json.loads(original_health)
        health["startup"]["trials"][0]["present_ms"] = None
        write_json(root, standalone["health_result"]["path"], health)
        rehash(mutated, root, standalone["health_result"])
        expect_failure(mutated, root, "causal field present_ms")
        health_path.write_text(original_health, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        standalone = mutated["campaigns"][0]
        health_path = root / standalone["health_result"]["path"]
        original_health = health_path.read_text(encoding="utf-8")
        health = json.loads(original_health)
        health["startup"]["correlation"]["gpu_evidence_id"] = "gpu-campaign-1"
        write_json(root, standalone["health_result"]["path"], health)
        rehash(mutated, root, standalone["health_result"])
        expect_failure(mutated, root, "lacks GPU/trace evidence identifiers")
        health_path.write_text(original_health, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        raw_ref = mutated["campaigns"][0]["raw_cold"]
        raw_path = root / raw_ref["path"]
        original_raw = raw_path.read_text(encoding="utf-8")
        raw = json.loads(original_raw)
        raw["unexpected"] = True
        write_json(root, raw_ref["path"], raw)
        rehash(mutated, root, raw_ref)
        expect_failure(mutated, root, "keys differ")
        raw_path.write_text(original_raw, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        trace_ref = mutated["campaigns"][0]["trace"]
        trace_path = root / trace_ref["path"]
        original_trace = trace_path.read_bytes()
        trace_path.write_bytes(b"")
        rehash(mutated, root, trace_ref)
        expect_failure(mutated, root, "trace must be non-empty")
        trace_path.write_bytes(original_trace)

        mutated = copy.deepcopy(receipt)
        trace_analysis_ref = mutated["campaigns"][0]["trace_analysis"]
        trace_analysis_path = root / trace_analysis_ref["path"]
        original_campaign_analysis = trace_analysis_path.read_text(encoding="utf-8")
        campaign_analysis = json.loads(original_campaign_analysis)
        campaign_analysis["evidence_ids"] = ["f" * 32]
        write_json(root, trace_analysis_ref["path"], campaign_analysis)
        rehash(mutated, root, trace_analysis_ref)
        expect_failure(mutated, root, "does not exactly corroborate the campaign")
        trace_analysis_path.write_text(original_campaign_analysis, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        mutated["same_instance_a2t"]["instance_id"] = "different-instance"
        expect_failure(mutated, root, "not cross-bound")

        a2t_path = root / receipt["same_instance_a2t"]["a2t_receipt"]["path"]
        original_a2t = a2t_path.read_bytes()
        a2t_path.write_bytes(original_a2t + b"\n")
        expect_failure(copy.deepcopy(receipt), root, "digest mismatch")
        a2t_path.write_bytes(original_a2t)

        mutated = copy.deepcopy(receipt)
        mutated["blank_negative"]["status"] = "not-caught"
        expect_failure(mutated, root, "caught blank negative")

        mutated = copy.deepcopy(receipt)
        audio_ref = mutated["audio_thread_exclusion"]["receipt"]
        audio_path = root / audio_ref["path"]
        original_audio = audio_path.read_text(encoding="utf-8")
        audio = json.loads(original_audio)
        audio["observed_audio_thread_events"] = 1
        write_json(root, audio_ref["path"], audio)
        rehash(mutated, root, audio_ref)
        expect_failure(mutated, root, "audio-thread exclusion")
        audio_path.write_text(original_audio, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        audio_ref = mutated["audio_thread_exclusion"]["receipt"]
        audio_path = root / audio_ref["path"]
        original_audio = audio_path.read_text(encoding="utf-8")
        audio = json.loads(original_audio)
        audio["instrumentation_entry_points"] = audio["instrumentation_entry_points"][:-1]
        audio["entry_point_observations"] = audio["entry_point_observations"][:-1]
        write_json(root, audio_ref["path"], audio)
        rehash(mutated, root, audio_ref)
        expect_failure(mutated, root, "positive control or observed zero")
        audio_path.write_text(original_audio, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        mutated["b4"]["status"] = "queued"
        expect_failure(mutated, root, "legal B4 disposition")

        mutated = copy.deepcopy(receipt)
        a2t_ref = mutated["same_instance_a2t"]["a2t_receipt"]
        binding_ref = mutated["same_instance_a2t"]["binding_receipt"]
        a2t_path = root / a2t_ref["path"]
        binding_path = root / binding_ref["path"]
        original_a2t = a2t_path.read_text(encoding="utf-8")
        original_binding = binding_path.read_text(encoding="utf-8")
        stale_a2t = json.loads(original_a2t)
        stale_a2t["producer_overhead_disposition"]["formal_plan_status"] = "requires-canonical-plan-closeout-approval"
        write_json(root, a2t_ref["path"], stale_a2t)
        rehash(mutated, root, a2t_ref)
        binding = json.loads(original_binding)
        binding["a2t_receipt_sha256"] = a2t_ref["sha256"]
        write_json(root, binding_ref["path"], binding)
        rehash(mutated, root, binding_ref)
        expect_failure(mutated, root, "not accepted by the exact A3 plan")
        a2t_path.write_text(original_a2t, encoding="utf-8")
        binding_path.write_text(original_binding, encoding="utf-8")

        for field in ("reviewer", "reviewed_utc", "ui_revision", "delivery", "observed_spans"):
            mutated = copy.deepcopy(receipt)
            a2t_ref = mutated["same_instance_a2t"]["a2t_receipt"]
            binding_ref = mutated["same_instance_a2t"]["binding_receipt"]
            a2t_path = root / a2t_ref["path"]
            binding_path = root / binding_ref["path"]
            original_a2t = a2t_path.read_text(encoding="utf-8")
            original_binding = binding_path.read_text(encoding="utf-8")
            stripped_a2t = json.loads(original_a2t)
            del stripped_a2t["human_perfetto_ui_correlation"][field]
            write_json(root, a2t_ref["path"], stripped_a2t)
            rehash(mutated, root, a2t_ref)
            binding = json.loads(original_binding)
            binding["a2t_receipt_sha256"] = a2t_ref["sha256"]
            write_json(root, binding_ref["path"], binding)
            rehash(mutated, root, binding_ref)
            expect_failure(mutated, root, "A2T human Perfetto review lacks")
            a2t_path.write_text(original_a2t, encoding="utf-8")
            binding_path.write_text(original_binding, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        mutated["campaigns"] = mutated["campaigns"][:-1]
        expect_failure(mutated, root, "every required role")

        mutated = copy.deepcopy(receipt)
        forge = next(campaign for campaign in mutated["campaigns"] if campaign["role"] == "forge")
        forge["identity"]["plugin_format"] = "auv2"
        expect_failure(mutated, root, "standalone shell")

        for mutation, needle in (
            (lambda payload: payload.pop("integration_head"), "keys differ"),
            (
                lambda payload: payload["source_blobs"].__setitem__(
                    sorted(payload["source_blobs"])[0], "0" * 40,
                ),
                "source blob mismatch",
            ),
        ):
            mutated = copy.deepcopy(receipt)
            a2t_ref = mutated["same_instance_a2t"]["a2t_receipt"]
            binding_ref = mutated["same_instance_a2t"]["binding_receipt"]
            a2t_path = root / a2t_ref["path"]
            binding_path = root / binding_ref["path"]
            original_a2t = a2t_path.read_text(encoding="utf-8")
            original_binding = binding_path.read_text(encoding="utf-8")
            a2t_payload = json.loads(original_a2t)
            mutation(a2t_payload)
            write_json(root, a2t_ref["path"], a2t_payload)
            rehash(mutated, root, a2t_ref)
            binding = json.loads(original_binding)
            binding["a2t_receipt_sha256"] = a2t_ref["sha256"]
            write_json(root, binding_ref["path"], binding)
            rehash(mutated, root, binding_ref)
            expect_failure(mutated, root, needle)
            a2t_path.write_text(original_a2t, encoding="utf-8")
            binding_path.write_text(original_binding, encoding="utf-8")

        mutated = copy.deepcopy(receipt)
        mutated["budget"]["raw_cold"]["sha256"] = "0" * 64
        expect_failure(mutated, root, "digest mismatch")

        mutated = copy.deepcopy(receipt)
        link = root / "product-link.bin"
        link.symlink_to("standalone-product.bin")
        mutated["campaigns"][0]["product_artifact"] = {
            "path": link.name,
            "sha256": receipt["campaigns"][0]["product_artifact"]["sha256"],
        }
        expect_failure(mutated, root, "safely snapshot")

        mutated = copy.deepcopy(receipt)
        mutated["same_instance_a2t"]["analyzer"]["sha256"] = "0" * 64
        expect_failure(mutated, root, "digest mismatch")

        always_source = b'''#!/usr/bin/python3
import json
print(json.dumps({"schema":"pulp.trace-gpu-analysis.v1","question":"gpu-startup","verdict":"pass","capture_complete":True,"evidence_ids":["forged"]}))
'''
        always = a3.ArtifactSnapshot(Path("always-pass"), always_source, a3.sha256_bytes(always_source))
        fake = a3.ArtifactSnapshot(Path("fake.pftrace"), b"fake", a3.sha256_bytes(b"fake"))
        try:
            a3.replay_analyzer(always, fake)
        except a3.AcceptanceError as error:
            assert "negative control" in str(error)
        else:
            raise AssertionError("always-pass analyzer accepted a fake trace")

        mutated = copy.deepcopy(receipt)
        a2t_ref = mutated["same_instance_a2t"]["a2t_receipt"]
        binding_ref = mutated["same_instance_a2t"]["binding_receipt"]
        a2t_path = root / a2t_ref["path"]
        binding_path = root / binding_ref["path"]
        saved_a2t = a2t_path.read_text(encoding="utf-8")
        saved_binding = binding_path.read_text(encoding="utf-8")
        write_json(root, a2t_ref["path"], {"schema": "pulp.gpu-trace-overhead-acceptance.v1"})
        rehash(mutated, root, a2t_ref)
        binding = json.loads(saved_binding)
        binding["a2t_receipt_sha256"] = a2t_ref["sha256"]
        write_json(root, binding_ref["path"], binding)
        rehash(mutated, root, binding_ref)
        expect_failure(mutated, root, "keys differ")
        a2t_path.write_text(saved_a2t, encoding="utf-8")
        binding_path.write_text(saved_binding, encoding="utf-8")

        changing = root / "changing.bin"
        changing.write_bytes(b"before")
        original_read = a3.os.read
        changed = False
        def mutate_during_read(fd: int, count: int) -> bytes:
            nonlocal changed
            data = original_read(fd, count)
            if data and not changed:
                changed = True
                changing.write_bytes(b"changed-after-read")
            return data
        with mock.patch.object(a3.os, "read", side_effect=mutate_during_read):
            try:
                a3.snapshot_relative(changing.name, root, "changing")
            except a3.AcceptanceError as error:
                assert "changed while" in str(error)
            else:
                raise AssertionError("artifact mutation during snapshot was not caught")

        current_root = ROOT / "docs" / "validation"
        current_receipt = json.loads(
            (current_root / "gpu-first-visible-a3-acceptance.json").read_text(
                encoding="utf-8"
            )
        )
        assert a3.validate_receipt(current_receipt, current_root) is False

        print(
            "gpu-first-visible-a3-acceptance: positive=8 planted_negatives=38 "
            "checked_in_nonterminal=verified"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
