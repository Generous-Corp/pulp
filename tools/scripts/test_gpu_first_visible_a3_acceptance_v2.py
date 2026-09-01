#!/usr/bin/env python3
"""Positive fixture and planted negatives for the closed A3 v2 contract."""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
from unittest import mock
from pathlib import Path
from typing import Any, Callable

import gpu_first_visible_a3_acceptance as a3

v2 = a3.a3_v2
SHA = "1" * 40
FORGE_SHA = "2" * 40
POLICY_PULP_SHA = "3" * 40
DIGEST = "4" * 64
EXPECTED_SIGNATURES = ["content", "shader", "source"]
SIGNATURE_DIGEST = hashlib.sha256(
    (json.dumps(sorted(EXPECTED_SIGNATURES), separators=(",", ":")) + "\n").encode()
).hexdigest()
PUBLICATION_SHA = "5" * 40
APPROVED_SHA = "6" * 40
PLANNING_MAIN_SHA = "7" * 40
POLICY_PATH = "research/evidence/gpu-ux/a3-budget/a3-v2-test-authority.product-policy.json"
POLICY_PR = "https://github.com/danielraffel/pulp-planning/pull/1"
SCRIPT = Path(__file__).with_name("gpu_first_visible_a3_acceptance_v2.py")
ROOT = Path(__file__).resolve().parents[2]


def write_artifact(root: Path, name: str, value: Any, *, raw: bool = False) -> dict[str, str]:
    path = root / name
    data = value if raw else (json.dumps(value, sort_keys=True) + "\n").encode()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return {"path": name, "sha256": hashlib.sha256(data).hexdigest()}


def write_executable(root: Path, name: str, source: str) -> dict[str, str]:
    ref = write_artifact(root, name, source.encode(), raw=True)
    (root / name).chmod(0o755)
    return ref


def rewrite_artifact(root: Path, ref: dict[str, str], mutate: Callable[[Any], None]) -> None:
    path = root / ref["path"]
    value = json.loads(path.read_text())
    mutate(value)
    data = (json.dumps(value, sort_keys=True) + "\n").encode()
    path.write_bytes(data)
    ref["sha256"] = hashlib.sha256(data).hexdigest()


def interaction(role: str) -> dict[str, Any]:
    if role == "headless-reference":
        return {
            "status": "not-applicable-by-authority", "origin": None,
            "stimulus": None, "expected_state_change": None,
            "endpoint": None, "p95_ns": None,
        }
    return {
        "status": "measured", "origin": "first-visible-frame",
        "stimulus": "manifest-bound-input", "expected_state_change": "changed",
        "endpoint": "next-nonblank-present", "p95_ns": 1000,
    }


def role_expected_signatures(role: str) -> list[str]:
    if role in v2.FORGE_ROLES:
        return [*EXPECTED_SIGNATURES, f"forge-role:{role}"]
    return list(EXPECTED_SIGNATURES)


def policy_role(role: str) -> dict[str, Any]:
    build_authority = None
    if role in v2.FORGE_ROLES:
        signatures = role_expected_signatures(role)
        build_authority = {
            "application_sha256": DIGEST,
            "plugin_sha256": DIGEST,
            "provider_sha256": DIGEST,
            "build_sha256": DIGEST,
            "content_sha256": DIGEST,
            "signature_sha256": v2.signature_set_digest(
                signatures, f"{role} fixture signatures",
            )[0],
            "expected_signatures": signatures,
        }
    return {
        "role_id": role,
        "first_visible_p95_ns": 1000,
        "first_interaction": interaction(role),
        "steady_cpu_frame_p95_ns": 1000,
        "steady_gpu_frame_p95_ns": 1000,
        "build_authority": build_authority,
    }


def raw_campaign(role: str, policy_sha256: str) -> dict[str, Any]:
    seed = v2.derived_seed(policy_sha256, role)
    order = v2.derived_trial_order(policy_sha256, role)
    positions = {token: index for index, token in enumerate(order)}
    states = []
    for state in v2.STATES:
        row: dict[str, Any] = {"state": state}
        for kind, count, key in (("warmup", 5, "warmups"), ("warm", 30, "warm"), ("cold", 20, "cold")):
            samples = []
            for index in range(count):
                token = f"{state}:{kind}:{index}"
                samples.append({
                    "trial_id": token, "order": positions[token], "seed": seed,
                    "first_visible_ns": 100,
                    "first_interaction_ns": None if role == "headless-reference" else 100,
                    "steady_cpu_frame_ns": 100, "steady_gpu_frame_ns": 100,
                    "xrun_count": 0, "audio_thread_work_events": 0,
                    "blank": False, "fallback_state": "prepared",
                    "trace_categories": list(v2.TRACE_CATEGORIES)
                    if state == "candidate-active-session" else [],
                    "signatures_present": role_expected_signatures(role),
                })
            row[key] = samples
        states.append(row)
    return {
        "schema": "pulp.gpu-first-visible-a3-role-samples.v2", "version": 2,
        "role_id": role, "manifest_seed": seed, "trial_order": order,
        "interaction_authority": None if role == "headless-reference" else {
            "origin": interaction(role)["origin"],
            "stimulus": interaction(role)["stimulus"],
            "expected_state": interaction(role)["expected_state_change"],
            "measurement_endpoint": interaction(role)["endpoint"],
        },
        "timer_noise_samples_ns": [1] * 10000, "states": states,
    }


def make_fixture(root: Path) -> dict[str, Any]:
    producer_ref = write_executable(root, "tooling/evidence-producer", f'''#!/usr/bin/env python3
import argparse, hashlib, json, sys
mode = sys.argv[1]
p = argparse.ArgumentParser(); p.add_argument("--json", action="store_true")
if mode == "verify-a3-evidence":
 p.add_argument("--kind"); p.add_argument("--artifact"); a=p.parse_args(sys.argv[2:])
 d=hashlib.sha256(open(a.artifact,"rb").read()).hexdigest()
 print(json.dumps({{"schema":"pulp.gpu-first-visible-evidence-verification.v1","kind":a.kind,"artifact_sha256":d,"implementation_head":"{POLICY_PULP_SHA}","valid":True}},sort_keys=True))
else:
 p.add_argument("--raw"); p.add_argument("--trace"); p.add_argument("--identity-sha256"); a=p.parse_args(sys.argv[2:])
 h=lambda x: hashlib.sha256(open(x,"rb").read()).hexdigest()
 print(json.dumps({{"schema":"pulp.gpu-first-visible-sample-verification.v2","raw_samples_sha256":h(a.raw),"trace_sha256":h(a.trace),"identity_sha256":a.identity_sha256,"valid":True}},sort_keys=True))
''')
    producer_identity = {
        "pulp_revision": SHA, "forge_revision": None, "build_id": "producer-build",
        "product_id": "a3-evidence-producer", "product_name": "A3 evidence producer",
        "plugin_format": "standalone",
    }
    embedded_build_ref = write_artifact(root, "tooling/evidence-producer-build-verifier.json", {
        "schema": "pulp.gpu-first-visible-build-verification-receipt.v1", "version": 1,
        "attempt_nonce": "attempt", "control": "real", "outcome": "pass", "reason": None,
        "verification_method": "embedded-canonical-build-identity",
        "product_identity": producer_identity,
        "product_sha256": producer_ref["sha256"],
        "observed_product_sha256": producer_ref["sha256"], "marker_sha256": DIGEST,
    })
    source_build_ref = write_artifact(root, "tooling/evidence-producer-source-build.json", {
        "schema": "pulp.gpu-first-visible-source-build-receipt.v1", "version": 1,
        "attempt_nonce": "attempt", "role": "a3-evidence-producer", "outcome": "pass",
        "reason": None, "identity": producer_identity, "source_revisions": {"pulp": SHA},
        "build_command": ["build-evidence-producer"], "builder_id": "fixture-builder",
        "build_started_utc": "2026-08-29T00:00:00Z", "build_finished_utc": "2026-08-29T00:00:01Z",
        "driver_sha256": DIGEST, "product_path": "evidence-producer",
        "product_sha256": producer_ref["sha256"],
        "bundle_path": None, "bundle_tree_sha256": None,
    })
    producer_provenance_ref = write_artifact(root, "tooling/evidence-producer-provenance.json", {
        "schema": "pulp.gpu-first-visible-evidence-producer.v1", "version": 1,
        "pulp_revision": SHA, "producer_sha256": producer_ref["sha256"],
        "build_verifier_receipt": embedded_build_ref,
        "source_build_receipt": source_build_ref,
    })
    coverage_identity = dict(producer_identity, pulp_revision=POLICY_PULP_SHA)
    coverage_embedded_ref = write_artifact(root, "tooling/coverage-producer-build-verifier.json", {
        "schema": "pulp.gpu-first-visible-build-verification-receipt.v1", "version": 1,
        "attempt_nonce": "coverage-attempt", "control": "real", "outcome": "pass", "reason": None,
        "verification_method": "embedded-canonical-build-identity",
        "product_identity": coverage_identity, "product_sha256": producer_ref["sha256"],
        "observed_product_sha256": producer_ref["sha256"], "marker_sha256": DIGEST,
    })
    coverage_source_ref = write_artifact(root, "tooling/coverage-producer-source-build.json", {
        "schema": "pulp.gpu-first-visible-source-build-receipt.v1", "version": 1,
        "attempt_nonce": "coverage-attempt", "role": "a3-evidence-producer", "outcome": "pass",
        "reason": None, "identity": coverage_identity,
        "source_revisions": {"pulp": POLICY_PULP_SHA},
        "build_command": ["build-evidence-producer"], "builder_id": "fixture-builder",
        "build_started_utc": "2026-08-28T00:00:00Z", "build_finished_utc": "2026-08-28T00:00:01Z",
        "driver_sha256": DIGEST, "product_path": "evidence-producer",
        "product_sha256": producer_ref["sha256"], "bundle_path": None,
        "bundle_tree_sha256": None,
    })
    coverage_provenance_ref = write_artifact(root, "tooling/coverage-producer-provenance.json", {
        "schema": "pulp.gpu-first-visible-evidence-producer.v1", "version": 1,
        "pulp_revision": POLICY_PULP_SHA, "producer_sha256": producer_ref["sha256"],
        "build_verifier_receipt": coverage_embedded_ref,
        "source_build_receipt": coverage_source_ref,
    })
    support_payload = write_artifact(root, v2.SUPPORT_MATRIX, {"protected": True})
    a1_payload = write_artifact(root, v2.A0_GPU_BASELINE, {"authentic_a1": True})
    support_ref = write_artifact(root, "policy/support-matrix.json", {
        "schema": "pulp.gpu-first-visible-generated-evidence.v1", "version": 1,
        "kind": "support-matrix", "implementation_head": POLICY_PULP_SHA, "producer": producer_ref,
        "producer_provenance": coverage_provenance_ref, "artifact": support_payload,
    })
    a1_ref = write_artifact(root, "policy/a1-evidence.json", {
        "schema": "pulp.gpu-first-visible-generated-evidence.v1", "version": 1,
        "kind": "a1-evidence", "implementation_head": POLICY_PULP_SHA, "producer": producer_ref,
        "producer_provenance": coverage_provenance_ref, "artifact": a1_payload,
    })
    policy = {
        "schema": "pulp.gpu-first-visible-budget-authority.v1", "version": 1,
        "budget_id": "a3-v2-test-authority",
        "clock": "mach_continuous_time", "editor_open_origin": "editor-open-requested",
        "first_nonblank_endpoint": "first-nonblank-presented-frame", "statistic": "p95",
        "first_applicable_pulp_revision": POLICY_PULP_SHA,
        "first_applicable_forge_revision": FORGE_SHA,
        "reference_host": {"host_id": "m5", "machine_id": "fleet-m5", "hardware": "M5", "os": "macOS", "display": "main", "refresh_hz": 60},
        "roles": [policy_role(role) for role in v2.ROLE_IDS],
        "required_coverage": {"predicate": "supported-constrained-metal-adapter", "adapter": "authority-metal-constrained", "configuration": "authority-config", "support_matrix": support_ref, "a1_evidence": [a1_ref]},
        "a4_scenario_budgets": [
            {"scenario_id": scenario, "role_ids": ["pulp-standalone"], "frame_budget_ns": 1000}
            for scenario in v2.A4_SCENARIOS
        ],
        "canary": {"binary_sha256": DIGEST, "content_sha256": DIGEST, "signature_sha256": SIGNATURE_DIGEST, "editor_open_origin": "editor-open-requested", "interaction_lifecycle": "manifest-bound", "steady_state_workload": "manifest-bound"},
    }
    policy_ref = write_artifact(root, "policy/product-policy.json", policy)
    policy_blob = v2.git_blob_digest((root / policy_ref["path"]).read_bytes())
    validation_ref = write_artifact(root, "policy/validation.json", {
        "schema": "pulp.gpu-first-visible-product-policy-validation.v1", "version": 1,
        "status": "pass", "checked_at": "2026-08-29T00:00:00Z",
        "fresh_live_state": True, "protected_commit": True,
        "repository": "danielraffel/pulp-planning",
        "publication_commit": PUBLICATION_SHA, "path": POLICY_PATH,
        "blob": policy_blob, "github_user_id": 25807,
        "approval_mode": "approval", "pr_url": POLICY_PR,
        "approved_head": APPROVED_SHA,
    })
    campaigns = []
    analyzer_ref = write_executable(root, "tooling/pulp-analyzer", '''#!/usr/bin/env python3
import json
print(json.dumps({"schema":"pulp.trace-gpu-analysis.v1","question":"gpu-startup","verdict":"unverified","capture_complete":True,"evidence_ids":["gpu-evidence"],"category_scope":{"evidence_id":"gpu-evidence","process_upid":7,"process_pid":42}}))
raise SystemExit(2)
''')
    analyzer_provenance_ref = write_artifact(root, "tooling/analyzer-provenance.json", {
        "schema": "pulp.gpu-first-visible-prepared-trace-analyzer.v1", "version": 1,
        "pulp_revision": SHA, "source_files": [], "source_snapshot_sha256": DIGEST,
        "cargo": {}, "rustc": {}, "cargo_home_mode": "fresh-config-free-linked-locked-cache",
        "target_directory_fresh": True, "analyzer_sha256": analyzer_ref["sha256"],
    })
    for role, host, application, plugin_format, _endpoint in v2.ROLE_SPECS:
        raw_ref = write_artifact(root, f"campaigns/{role}-raw.json", raw_campaign(role, policy_ref["sha256"]))
        trace_ref = write_artifact(root, f"campaigns/{role}.pftrace", b"fixture-trace", raw=True)
        analysis_ref = write_artifact(root, f"campaigns/{role}-analysis.json", {
            "schema": "pulp.gpu-first-visible-a3-trace-analysis.v2", "version": 2,
            "role_id": role, "categories": list(v2.TRACE_CATEGORIES),
            "trace_complete": True, "dropped_events": 0, "flush_complete": True,
            "trace_sha256": trace_ref["sha256"], "campaign_id": f"campaign-{role}",
            "instance_id": f"instance-{role}", "build_id": f"build-{role}",
            "gpu_evidence_id": "gpu-evidence", "trace_evidence_id": f"trace-{role}",
            "process_pid": 42, "process_upid": 7,
        })
        expected_signatures = role_expected_signatures(role)
        identity = {
                "pulp_revision": SHA, "forge_revision": FORGE_SHA if role in v2.FORGE_ROLES else None,
                "machine_id": "fleet-m5", "hardware": "M5", "os": "macOS", "host_kind": host,
                "host_bundle_id": "fixture.host", "host_version": "1", "host_sha256": DIGEST,
                "application_kind": application, "application_sha256": DIGEST,
                "plugin_format": plugin_format, "plugin_sha256": DIGEST,
                "provider_sha256": DIGEST, "build_sha256": DIGEST,
                "signature_sha256": v2.signature_set_digest(
                    expected_signatures, f"{role} fixture signatures",
                )[0],
                "expected_signatures": expected_signatures, "content_sha256": DIGEST,
                "adapter": "authority-metal-constrained" if role == "constrained-adapter" else "metal",
                "display_id": "main", "refresh_hz": 60,
        }
        if role in {"pulp-standalone", "constrained-adapter"}:
            identity.update({
                "adapter_configuration": (
                    "authority-config" if role == "constrained-adapter" else "default"
                ),
                "editor_open_origin": "editor-open-requested",
                "interaction_lifecycle": "manifest-bound",
                "steady_state_workload": "manifest-bound",
            })
        if role == "constrained-adapter":
            identity["adapter_predicate"] = "supported-constrained-metal-adapter"
        if role != "headless-reference":
            role_interaction = interaction(role)
            identity.update({
                "interaction_origin": role_interaction["origin"],
                "interaction_stimulus": role_interaction["stimulus"],
                "interaction_expected_state": role_interaction["expected_state_change"],
                "interaction_measurement_endpoint": role_interaction["endpoint"],
            })
        identity_digest = hashlib.sha256(
            (json.dumps(identity, sort_keys=True, separators=(",", ":")) + "\n").encode()
        ).hexdigest()
        sample_provenance_ref = write_artifact(root, f"campaigns/{role}-provenance.json", {
            "schema": "pulp.gpu-first-visible-a3-sample-provenance.v2", "version": 2,
            "implementation_head": SHA, "role_id": role, "producer": producer_ref,
            "producer_provenance": producer_provenance_ref,
            "raw_samples_sha256": raw_ref["sha256"], "trace_sha256": trace_ref["sha256"],
            "identity_sha256": identity_digest,
        })
        campaigns.append({
            "role_id": role, "status": "pass", "identity": identity,
            "raw_samples": raw_ref, "trace": trace_ref, "trace_analysis": analysis_ref,
            "sample_provenance": sample_provenance_ref,
            "trace_analyzer": analyzer_ref,
            "trace_analyzer_provenance": analyzer_provenance_ref,
            "trace_binding": {
                "campaign_id": f"campaign-{role}", "instance_id": f"instance-{role}",
                "build_id": f"build-{role}", "gpu_evidence_id": "gpu-evidence",
                "trace_evidence_id": f"trace-{role}", "process_pid": 42, "process_upid": 7,
            },
            "causal_attribution": {"render_pipeline_material": False, "instrumentation_complete": True, "missing_events": [], "transferred_vellum_routes": []},
        })
    trace_digests = sorted(campaign["trace"]["sha256"] for campaign in campaigns)
    blank_ref = write_artifact(root, "controls/blank.json", {
        "schema": "pulp.gpu-first-visible-blank-negative.v2", "version": 2,
        "implementation_head": SHA, "campaign_trace_sha256s": trace_digests,
        "injected_blank_sha256": DIGEST, "diagnostic_code": "gpu.startup.blank", "detected": True,
    })
    audio_executable = write_executable(root, "controls/audio-harness", "#!/bin/sh\nexit 0\n")
    audio_ref = write_artifact(root, "controls/audio.json", {
        "schema": "pulp.gpu-first-visible-audio-thread-exclusion.v2", "version": 2,
        "implementation_head": SHA, "campaign_trace_sha256s": trace_digests,
        "executable": audio_executable, "scope": "external-instrumented-harness",
        "provider_entry_points": ["begin_editor_open", "record_presented_frame", "record_timeout", "record_instance_lost", "record_dropped_events", "snapshot"],
        "audio_thread_events": 0, "non_audio_thread_events": 6,
    })
    overhead_ref = write_artifact(root, "controls/overhead.json", {
        "candidate_revision": SHA, "verdict": "pass", "states": list(v2.STATES),
    })
    receipt = {
        "$schema": "../contracts/gpu-first-visible-a3-acceptance-v2.schema.json",
        "schema": "dev.pulp.gpu-first-visible-a3-acceptance", "version": 2,
        "status": "complete", "recorded_at": "2026-08-29T00:00:00Z",
        "implementation_head": SHA,
        "plan": {"document": "research/plan.md", "revision": SHA, "lines": "949-1118", "sha256": DIGEST},
        "product_policy": {"status": "bound", "authority": policy_ref, "validation": validation_ref, "required_coverage": "bound"},
        "protocol": v2.canonical_protocol(), "campaigns": campaigns,
        "blank_negative": {"status": "pass", "receipt": blank_ref},
        "audio_thread_exclusion": {"status": "pass", "receipt": audio_ref},
        "trace_producer_overhead": {"status": "pass", "reason": None, "receipt": overhead_ref},
        "publication": {"repository": "Generous-Corp/pulp", "branch": "main", "receipt_path": "docs/validation/gpu-first-visible-a3-acceptance.json", "artifact_sha256s": []},
        "disposition": "no-change", "observations": [], "blockers": [],
    }
    receipt["publication"]["artifact_sha256s"] = sorted(
        set(v2.collect_artifact_sha256s(receipt)) | {
            support_ref["sha256"], a1_ref["sha256"], producer_ref["sha256"],
            producer_provenance_ref["sha256"], support_payload["sha256"], a1_payload["sha256"],
            coverage_provenance_ref["sha256"], coverage_embedded_ref["sha256"],
            coverage_source_ref["sha256"],
            audio_executable["sha256"], embedded_build_ref["sha256"], source_build_ref["sha256"],
        }
    )
    return receipt


def expect_rejected(label: str, mutate: Callable[[dict[str, Any], Path], None]) -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        receipt = make_fixture(root)
        mutate(receipt, root)
        try:
            with mock.patch.object(v2, "live_protected_main_errors", return_value=[]), mock.patch.object(
                v2.trace_producer_overhead, "validate_receipt", return_value=None
            ):
                v2.validate_v2(receipt, root, receipt_path=root / "canonical.json", repository=root)
        except v2.V2AcceptanceError:
            return
    raise AssertionError(f"planted negative was accepted: {label}")


def replace_publication_digest(receipt: dict[str, Any], old: str, new: str) -> None:
    digests = set(receipt["publication"]["artifact_sha256s"])
    digests.remove(old)
    digests.add(new)
    receipt["publication"]["artifact_sha256s"] = sorted(digests)


def rebind_campaign_identity(
    receipt: dict[str, Any], root: Path, role_id: str,
    mutate: Callable[[dict[str, Any]], None],
) -> None:
    campaign = next(row for row in receipt["campaigns"] if row["role_id"] == role_id)
    mutate(campaign["identity"])
    identity_digest = hashlib.sha256(
        (json.dumps(campaign["identity"], sort_keys=True, separators=(",", ":")) + "\n").encode()
    ).hexdigest()
    provenance_ref = campaign["sample_provenance"]
    old = provenance_ref["sha256"]
    rewrite_artifact(
        root, provenance_ref,
        lambda provenance: provenance.update(identity_sha256=identity_digest),
    )
    replace_publication_digest(receipt, old, provenance_ref["sha256"])


def rebind_raw_signatures(
    receipt: dict[str, Any], root: Path, signatures: list[str],
) -> None:
    campaign = receipt["campaigns"][0]
    raw_ref = campaign["raw_samples"]
    old_raw = raw_ref["sha256"]
    rewrite_artifact(
        root, raw_ref,
        lambda raw: raw["states"][1]["warm"][0].update(signatures_present=signatures),
    )
    replace_publication_digest(receipt, old_raw, raw_ref["sha256"])
    provenance_ref = campaign["sample_provenance"]
    old_provenance = provenance_ref["sha256"]
    rewrite_artifact(
        root, provenance_ref,
        lambda provenance: provenance.update(raw_samples_sha256=raw_ref["sha256"]),
    )
    replace_publication_digest(receipt, old_provenance, provenance_ref["sha256"])


def rebind_all_raw_signatures(
    receipt: dict[str, Any], root: Path, role_id: str, signatures: list[str],
) -> None:
    campaign = next(row for row in receipt["campaigns"] if row["role_id"] == role_id)
    raw_ref = campaign["raw_samples"]
    old_raw = raw_ref["sha256"]

    def mutate(raw: dict[str, Any]) -> None:
        for state in raw["states"]:
            for group in ("warmups", "warm", "cold"):
                for sample in state[group]:
                    sample["signatures_present"] = list(signatures)

    rewrite_artifact(root, raw_ref, mutate)
    replace_publication_digest(receipt, old_raw, raw_ref["sha256"])
    provenance_ref = campaign["sample_provenance"]
    old_provenance = provenance_ref["sha256"]
    rewrite_artifact(
        root, provenance_ref,
        lambda provenance: provenance.update(raw_samples_sha256=raw_ref["sha256"]),
    )
    replace_publication_digest(receipt, old_provenance, provenance_ref["sha256"])


def plant_headless_content_substitution(receipt: dict[str, Any], root: Path) -> None:
    rebind_campaign_identity(
        receipt, root, "headless-reference",
        lambda identity: identity.update(content_sha256="8" * 64),
    )


def plant_headless_signature_substitution(receipt: dict[str, Any], root: Path) -> None:
    substituted = ["headless-only-signature"]
    digest = v2.signature_set_digest(substituted, "test substitution")[0]
    rebind_campaign_identity(
        receipt, root, "headless-reference",
        lambda identity: identity.update(
            signature_sha256=digest,
            expected_signatures=substituted,
        ),
    )


def plant_forge_content_substitution(receipt: dict[str, Any], root: Path) -> None:
    rebind_campaign_identity(
        receipt, root, "forge-modular-vst3-reaper",
        lambda identity: identity.update(content_sha256="8" * 64),
    )


def plant_self_consistent_forged_forge_signatures(
    receipt: dict[str, Any], root: Path,
) -> None:
    role_id = "forge-modular-vst3-reaper"
    substituted = ["forged.forge.content", "forged.forge.shader"]
    digest = v2.signature_set_digest(substituted, "forged Forge signatures")[0]
    # Rebind both campaign-local claims. The previous validator accepted this
    # self-consistent forgery because it had no protected Forge signature set.
    rebind_campaign_identity(
        receipt, root, role_id,
        lambda identity: identity.update(
            signature_sha256=digest,
            expected_signatures=list(substituted),
        ),
    )
    rebind_all_raw_signatures(receipt, root, role_id, substituted)


def expect_signature_reorder_accepted() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        receipt = make_fixture(root)
        rebind_raw_signatures(receipt, root, list(reversed(EXPECTED_SIGNATURES)))
        with mock.patch.object(v2, "live_protected_main_errors", return_value=[]), mock.patch.object(
            v2.trace_producer_overhead, "validate_receipt", return_value=None
        ):
            assert v2.validate_v2(
                receipt, root, receipt_path=root / "canonical.json", repository=root,
            ) is True


def policy_publication_errors(
    validation: dict[str, Any], policy_bytes: bytes, *,
    protected: bool = True, ancestor: bool = True,
    protected_blob: str | None = None, pr_head: str = APPROVED_SHA,
    merge_commit: str = PUBLICATION_SHA, base_ref: str = "main",
    author_id: int = 7, author_type: str = "User",
    review_id: int = 25807, review_type: str = "User",
    reviews: list[tuple[str, str]] | None = None,
    created_at: str = "2026-08-29T00:00:00Z",
    merged_at: str = "2026-08-29T01:00:00Z",
) -> list[str]:
    expected_blob = validation["blob"]

    def command_json(command: list[str]) -> dict[str, Any]:
        endpoint = command[2]
        if endpoint.endswith("/branches/main"):
            return {"protected": protected, "commit": {"sha": PLANNING_MAIN_SHA}}
        if "/compare/" in endpoint:
            return {
                "base_commit": {"sha": PUBLICATION_SHA},
                "merge_base_commit": {"sha": PUBLICATION_SHA if ancestor else "8" * 40},
                "status": "ahead" if ancestor else "diverged", "behind_by": 0,
            }
        if f"?ref={PUBLICATION_SHA}" in endpoint:
            return {"type": "file", "path": POLICY_PATH, "sha": expected_blob}
        if f"?ref={APPROVED_SHA}" in endpoint:
            return {"type": "file", "path": POLICY_PATH, "sha": expected_blob}
        if f"?ref={PLANNING_MAIN_SHA}" in endpoint:
            return {
                "type": "file", "path": POLICY_PATH,
                "sha": protected_blob or expected_blob,
            }
        if endpoint.endswith("/pulls/1"):
            return {
                "state": "closed", "created_at": created_at, "merged_at": merged_at,
                "merge_commit_sha": merge_commit, "base": {"ref": base_ref},
                "head": {"sha": pr_head},
                "user": {"id": author_id, "type": author_type},
            }
        raise AssertionError(f"unexpected live-policy endpoint: {endpoint}")

    def fetch_pages(_ghapp: str, endpoint: str, **_kwargs: Any) -> list[dict[str, Any]]:
        assert endpoint.endswith("/pulls/1/reviews"), endpoint
        states = reviews or [("APPROVED", "2026-08-29T00:30:00Z")]
        return [
            {
                "id": index, "user": {"id": review_id, "type": review_type},
                "state": state, "commit_id": validation["approved_head"],
                "submitted_at": submitted_at,
            }
            for index, (state, submitted_at) in enumerate(states, start=1)
        ]

    with mock.patch.object(v2, "_command_json", side_effect=command_json), mock.patch.object(
        v2, "_fetch_all_pages", side_effect=fetch_pages
    ):
        return v2.product_policy_publication_errors(validation, policy_bytes, "ghapp")


def plant_old_policy_self_provenance(receipt: dict[str, Any], root: Path) -> None:
    def mutate(policy: dict[str, Any]) -> None:
        policy["source"] = {
            "repository": "danielraffel/pulp-planning", "revision": SHA,
            "path": POLICY_PATH, "blob": "3" * 40,
        }
        policy["approval"] = {
            "github_user_id": 25807, "mode": "approval", "pr_url": POLICY_PR,
            "approved_head": SHA,
        }

    rewrite_artifact(root, receipt["product_policy"]["authority"], mutate)


def plant_old_validation_self_provenance(receipt: dict[str, Any], root: Path) -> None:
    def mutate(validation: dict[str, Any]) -> None:
        validation["revision"] = validation.pop("publication_commit")
        validation.pop("approval_mode")
        validation.pop("pr_url")

    rewrite_artifact(root, receipt["product_policy"]["validation"], mutate)


def plant_unrelated_required_coverage(
    receipt: dict[str, Any], root: Path, key: str,
) -> None:
    policy_path = root / receipt["product_policy"]["authority"]["path"]
    policy = json.loads(policy_path.read_text())
    wrapper_ref = policy["required_coverage"][key]
    if isinstance(wrapper_ref, list):
        wrapper_ref = wrapper_ref[0]
    payload_ref = write_artifact(root, f"policy/unrelated-{key}.json", {"protected": True})
    rewrite_artifact(
        root, wrapper_ref,
        lambda wrapper: wrapper.update(artifact=payload_ref),
    )
    policy_path.write_text(json.dumps(policy, sort_keys=True) + "\n")
    receipt["product_policy"]["authority"]["sha256"] = hashlib.sha256(
        policy_path.read_bytes()
    ).hexdigest()


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        receipt = make_fixture(root)
        with mock.patch.object(v2, "live_protected_main_errors", return_value=[]), mock.patch.object(
            v2.trace_producer_overhead, "validate_receipt", return_value=None
        ):
            assert v2.validate_v2(receipt, root, receipt_path=root / "canonical.json", repository=root) is True
        policy_bytes = (root / receipt["product_policy"]["authority"]["path"]).read_bytes()
        policy = json.loads(policy_bytes)
        validation = json.loads(
            (root / receipt["product_policy"]["validation"]["path"]).read_text()
        )
        assert validation["publication_commit"] != validation["approved_head"]
        assert policy_publication_errors(validation, policy_bytes) == []
        equal_head = dict(validation, approved_head=PUBLICATION_SHA)
        assert policy_publication_errors(
            equal_head, policy_bytes, pr_head=PUBLICATION_SHA,
        ) == []
        assert "bytes" in policy_publication_errors(validation, policy_bytes + b"\n")[0]
        assert any(
            "exact approved head" in error
            for error in policy_publication_errors(validation, policy_bytes, pr_head="9" * 40)
        )
        assert any(
            "protected main" in error
            for error in policy_publication_errors(
                validation, policy_bytes, protected_blob="9" * 40,
            )
        )
        assert any(
            "effective pre-merge exact-head" in error
            for error in policy_publication_errors(
                validation, policy_bytes,
                reviews=[("APPROVED", "2026-08-29T01:30:00Z")],
            )
        )
        assert any(
            "effective pre-merge exact-head" in error
            for error in policy_publication_errors(
                validation, policy_bytes,
                reviews=[("APPROVED", "2026-08-28T23:59:59Z")],
            )
        )
        for approval_mode in ("approval", "author"):
            chronology_validation = dict(validation, approval_mode=approval_mode)
            assert any(
                "does not postdate PR creation" in error
                for error in policy_publication_errors(
                    chronology_validation, policy_bytes,
                    created_at="2026-08-29T01:00:00Z",
                    merged_at="2026-08-29T01:00:00Z",
                    author_id=25807,
                )
            )
        assert any(
            "live protected branch" in error
            for error in policy_publication_errors(validation, policy_bytes, protected=False)
        )
        assert any(
            "not an ancestor" in error
            for error in policy_publication_errors(validation, policy_bytes, ancestor=False)
        )
        for kwargs in (
            {"merge_commit": "9" * 40},
            {"base_ref": "release"},
        ):
            assert any(
                "recorded merged main PR" in error
                for error in policy_publication_errors(validation, policy_bytes, **kwargs)
            )
        assert any(
            "effective pre-merge exact-head" in error
            for error in policy_publication_errors(
                validation, policy_bytes, review_id=40003,
            )
        )
        for revoked_state in ("CHANGES_REQUESTED", "DISMISSED"):
            assert any(
                "effective pre-merge exact-head" in error
                for error in policy_publication_errors(
                    validation, policy_bytes,
                    reviews=[
                        ("APPROVED", "2026-08-29T00:20:00Z"),
                        (revoked_state, "2026-08-29T00:40:00Z"),
                    ],
                )
            )
        assert policy_publication_errors(
            validation, policy_bytes,
            reviews=[
                ("APPROVED", "2026-08-29T00:20:00Z"),
                ("COMMENTED", "2026-08-29T00:40:00Z"),
            ],
        ) == []
        author_validation = dict(validation, approval_mode="author")
        assert policy_publication_errors(
            author_validation, policy_bytes, author_id=25807,
        ) == []
        assert any(
            "author identity" in error
            for error in policy_publication_errors(
                author_validation, policy_bytes, author_id=40004,
            )
        )
        protected_blobs = {
            path: v2.git_blob_digest((root / path).read_bytes())
            for path in (v2.SUPPORT_MATRIX, v2.A0_GPU_BASELINE)
        }

        def protected_content(command: list[str]) -> dict[str, Any]:
            endpoint = command[2]
            if "/compare/" in endpoint:
                return {
                    "base_commit": {"sha": POLICY_PULP_SHA},
                    "merge_base_commit": {"sha": POLICY_PULP_SHA},
                    "status": "ahead", "ahead_by": 1, "behind_by": 0,
                }
            path = endpoint.split("/contents/", 1)[1].split("?ref=", 1)[0]
            return {"type": "file", "path": path, "sha": protected_blobs[path]}

        with mock.patch.object(v2, "_command_json", side_effect=protected_content):
            assert v2.protected_required_coverage_errors(
                policy, root, SHA, "ghapp",
            ) == []
        def nonancestor(command: list[str]) -> dict[str, Any]:
            endpoint = command[2]
            if "/compare/" in endpoint:
                return {
                    "base_commit": {"sha": POLICY_PULP_SHA},
                    "merge_base_commit": {"sha": "9" * 40},
                    "status": "diverged", "ahead_by": 1, "behind_by": 1,
                }
            path = endpoint.split("/contents/", 1)[1].split("?ref=", 1)[0]
            return {"type": "file", "path": path, "sha": protected_blobs[path]}
        with mock.patch.object(v2, "_command_json", side_effect=nonancestor):
            assert "not strict protected-main ancestry" in v2.protected_required_coverage_errors(
                policy, root, SHA, "ghapp",
            )[0]
        def identical_revision(command: list[str]) -> dict[str, Any]:
            endpoint = command[2]
            if "/compare/" in endpoint:
                return {
                    "base_commit": {"sha": POLICY_PULP_SHA},
                    "merge_base_commit": {"sha": POLICY_PULP_SHA},
                    "status": "identical", "ahead_by": 0, "behind_by": 0,
                }
            path = endpoint.split("/contents/", 1)[1].split("?ref=", 1)[0]
            return {"type": "file", "path": path, "sha": protected_blobs[path]}
        with mock.patch.object(v2, "_command_json", side_effect=identical_revision):
            assert "not strict protected-main ancestry" in v2.protected_required_coverage_errors(
                policy, root, POLICY_PULP_SHA, "ghapp",
            )[0]
        with mock.patch.object(
            v2, "_command_json",
            return_value={"type": "file", "path": v2.SUPPORT_MATRIX, "sha": "9" * 40},
        ):
            assert any(
                "exact protected Pulp blob" in error
                for error in v2.protected_required_coverage_errors(
                    policy, root, SHA, "ghapp",
                )
            )
        terminal_path = root / "terminal.json"
        terminal_path.write_text(json.dumps(receipt), encoding="utf-8")
        terminal = subprocess.run(
            [sys.executable, str(SCRIPT), "verify", str(terminal_path),
             "--evidence-root", str(root)],
            text=True, capture_output=True, check=False,
        )
        assert terminal.returncode == 1, (terminal.stdout, terminal.stderr)
        assert "A3 v2 acceptance: FAIL:" in terminal.stderr

        invalid = copy.deepcopy(receipt)
        invalid["protocol"]["ring_mib"] = 64
        invalid_path = root / "invalid.json"
        invalid_path.write_text(json.dumps(invalid), encoding="utf-8")
        rejected = subprocess.run(
            [sys.executable, str(SCRIPT), "verify", str(invalid_path),
             "--evidence-root", str(root)],
            text=True, capture_output=True, check=False,
        )
        assert rejected.returncode == 1, (rejected.stdout, rejected.stderr)
        assert "A3 v2 acceptance: FAIL:" in rejected.stderr

    canonical = ROOT / "docs/validation/gpu-first-visible-a3-acceptance.json"
    nonterminal = subprocess.run(
        [sys.executable, str(SCRIPT), "verify", str(canonical)],
        text=True, capture_output=True, check=False,
    )
    assert nonterminal.returncode == 2, (nonterminal.stdout, nonterminal.stderr)
    assert "NONTERMINAL terminal=false" in nonterminal.stdout
    assert "blockers=product-policy,required-coverage" in nonterminal.stdout

    negatives: list[tuple[str, Callable[[dict[str, Any], Path], None]]] = [
        ("omitted role", lambda r, _p: r["campaigns"].pop()),
        ("extra role", lambda r, _p: r["campaigns"].append(copy.deepcopy(r["campaigns"][0]))),
        ("substituted host", lambda r, _p: r["campaigns"][0]["identity"].update(host_kind="substitute")),
        ("substituted hardware", lambda r, _p: r["campaigns"][0]["identity"].update(hardware="OtherHardware")),
        ("substituted OS", lambda r, _p: r["campaigns"][0]["identity"].update(os="OtherOS")),
        ("substituted display", lambda r, _p: r["campaigns"][0]["identity"].update(display_id="other-display")),
        ("substituted refresh", lambda r, _p: r["campaigns"][0]["identity"].update(refresh_hz=120)),
        ("standalone canary binary", lambda r, _p: r["campaigns"][0]["identity"].update(application_sha256="8" * 64)),
        ("constrained canary content", lambda r, _p: r["campaigns"][6]["identity"].update(content_sha256="8" * 64)),
        ("constrained canary signature", lambda r, _p: r["campaigns"][6]["identity"].update(signature_sha256="8" * 64)),
        ("missing expected-signature authority", lambda r, _p: r["campaigns"][0]["identity"].pop("expected_signatures")),
        ("duplicate expected-signature authority", lambda r, _p: r["campaigns"][0]["identity"].update(expected_signatures=[*EXPECTED_SIGNATURES, EXPECTED_SIGNATURES[0]])),
        ("headless content substitution", plant_headless_content_substitution),
        ("headless signature substitution", plant_headless_signature_substitution),
        ("Forge shared-content substitution", plant_forge_content_substitution),
        ("self-consistent forged Forge signatures", plant_self_consistent_forged_forge_signatures),
        ("missing observed signature", lambda r, p: rebind_raw_signatures(r, p, EXPECTED_SIGNATURES[:-1])),
        ("extra observed signature", lambda r, p: rebind_raw_signatures(r, p, [*EXPECTED_SIGNATURES, "extra"])),
        ("duplicate observed signature", lambda r, p: rebind_raw_signatures(r, p, [*EXPECTED_SIGNATURES, EXPECTED_SIGNATURES[0]])),
        ("fabricated observed signature", lambda r, p: rebind_raw_signatures(r, p, ["fabricated.signature"])),
        ("standalone lifecycle", lambda r, _p: r["campaigns"][0]["identity"].update(interaction_lifecycle="substitute")),
        ("canary editor-open origin mismatch", lambda r, p: rewrite_artifact(p, r["product_policy"]["authority"], lambda policy: policy["canary"].update(editor_open_origin="easier-origin"))),
        ("constrained workload", lambda r, _p: r["campaigns"][6]["identity"].update(steady_state_workload="easier")),
        ("constrained predicate", lambda r, _p: r["campaigns"][6]["identity"].update(adapter_predicate="executor-selected")),
        ("constrained configuration", lambda r, _p: r["campaigns"][6]["identity"].update(adapter_configuration="executor-selected")),
        ("missing standalone configuration", lambda r, _p: r["campaigns"][0]["identity"].pop("adapter_configuration")),
        ("constrained product substitution", lambda r, _p: r["campaigns"][6]["identity"].update(build_sha256="8" * 64)),
        ("campaign interaction origin", lambda r, _p: r["campaigns"][0]["identity"].update(interaction_origin="easier-origin")),
        ("campaign interaction stimulus", lambda r, _p: r["campaigns"][1]["identity"].update(interaction_stimulus="easier-stimulus")),
        ("campaign interaction expected state", lambda r, _p: r["campaigns"][2]["identity"].update(interaction_expected_state="no-op")),
        ("campaign interaction endpoint", lambda r, _p: r["campaigns"][3]["identity"].update(interaction_measurement_endpoint="earlier-endpoint")),
        ("raw interaction origin", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["interaction_authority"].update(origin="easier-origin"))),
        ("raw interaction stimulus", lambda r, p: rewrite_artifact(p, r["campaigns"][1]["raw_samples"], lambda raw: raw["interaction_authority"].update(stimulus="easier-stimulus"))),
        ("raw interaction expected state", lambda r, p: rewrite_artifact(p, r["campaigns"][2]["raw_samples"], lambda raw: raw["interaction_authority"].update(expected_state="no-op"))),
        ("raw interaction endpoint", lambda r, p: rewrite_artifact(p, r["campaigns"][3]["raw_samples"], lambda raw: raw["interaction_authority"].update(measurement_endpoint="earlier-endpoint"))),
        ("false unavailable", lambda r, _p: r["campaigns"][0].update(status="unavailable")),
        ("executor disposition", lambda r, _p: r.update(disposition="queue-B4")),
        ("caller self-attestation", lambda r, _p: r["publication"].update(protected=True, head=SHA, required_checks=[])),
        ("old policy self-provenance", plant_old_policy_self_provenance),
        ("old validation self-provenance", plant_old_validation_self_provenance),
        ("malformed publication commit", lambda r, p: rewrite_artifact(p, r["product_policy"]["validation"], lambda validation: validation.update(publication_commit="bad"))),
        ("wrong policy P", lambda r, p: rewrite_artifact(p, r["product_policy"]["authority"], lambda policy: policy.update(first_applicable_pulp_revision="9" * 40))),
        ("wrong planning policy path", lambda r, p: rewrite_artifact(p, r["product_policy"]["validation"], lambda validation: validation.update(path=f"{v2.PLANNING_POLICY_DIRECTORY}/other.product-policy.json"))),
        ("unavailable Pulp policy source", lambda r, p: rewrite_artifact(p, r["product_policy"]["validation"], lambda validation: validation.update(repository="Generous-Corp/pulp", pr_url="https://github.com/Generous-Corp/pulp/pull/1"))),
        ("unavailable Forge policy source", lambda r, p: rewrite_artifact(p, r["product_policy"]["validation"], lambda validation: validation.update(repository="Generous-Corp/forge", pr_url="https://github.com/Generous-Corp/forge/pull/1"))),
        ("unsafe policy budget ID", lambda r, p: rewrite_artifact(p, r["product_policy"]["authority"], lambda policy: policy.update(budget_id="../other"))),
        ("unrelated support matrix", lambda r, p: plant_unrelated_required_coverage(r, p, "support_matrix")),
        ("missing A0 baseline", lambda r, p: plant_unrelated_required_coverage(r, p, "a1_evidence")),
        ("missing steady budget", lambda r, p: rewrite_artifact(p, r["product_policy"]["authority"], lambda policy: policy["roles"][0].pop("steady_gpu_frame_p95_ns"))),
        ("short samples", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][0]["warm"].pop())),
        ("wrong seed", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw.update(manifest_seed=0))),
        ("missing category", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][3]["warm"][0]["trace_categories"].pop())),
        ("blank bypass", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][1]["warm"][0].update(blank=True))),
        ("audio thread work", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][1]["warm"][0].update(audio_thread_work_events=1))),
        ("xrun", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][1]["warm"][0].update(xrun_count=1))),
        ("trace sidecar digest", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["trace_analysis"], lambda a: a.update(trace_sha256=DIGEST))),
        ("sample provenance", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["sample_provenance"], lambda a: a.update(raw_samples_sha256=DIGEST))),
        ("sample producer substitution", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["sample_provenance"], lambda a: a.update(producer=write_executable(p, "tooling/forged-producer", "#!/bin/sh\nexit 0\n")))),
        ("producer build proof", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["sample_provenance"], lambda a: a.update(producer_provenance=write_artifact(p, "tooling/forged-provenance.json", {"schema": "caller-attested"})))),
        ("analyzer substitution", lambda r, p: r["campaigns"][0].update(trace_analyzer=write_executable(p, "tooling/substitute", "#!/bin/sh\nexit 2\n"))),
        ("analyzer process", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["trace_analysis"], lambda a: a.update(process_pid=43))),
        ("blank negative digest", lambda r, p: rewrite_artifact(p, r["blank_negative"]["receipt"], lambda a: a.update(campaign_trace_sha256s=[]))),
        ("external audio events", lambda r, p: rewrite_artifact(p, r["audio_thread_exclusion"]["receipt"], lambda a: a.update(audio_thread_events=1))),
        ("overhead wrong head", lambda r, p: rewrite_artifact(p, r["trace_producer_overhead"]["receipt"], lambda a: a.update(candidate_revision="9" * 40))),
    ]
    for label, mutate in negatives:
        expect_rejected(label, mutate)
    expect_signature_reorder_accepted()
    required = {("required", 7)}
    successful = [{
        "id": 1, "name": "required", "app": {"id": 7}, "status": "completed",
        "conclusion": "success", "completed_at": "2026-08-29T01:00:00Z",
    }]
    assert v2.required_check_result_errors(required, successful, []) == []
    assert "missing" in v2.required_check_result_errors(required, [dict(successful[0], app={"id": 8})], [])[0]
    ambiguous = successful + [dict(successful[0], id=2)]
    assert "ambiguous" in v2.required_check_result_errors(required, ambiguous, [])[0]
    pending = [dict(successful[0], status="in_progress", conclusion=None)]
    assert "not successful" in v2.required_check_result_errors(required, pending, [])[0]
    pages = [[{"id": index} for index in range(100)], [{"id": 100}]]
    with mock.patch.object(v2, "_command_json", side_effect=pages):
        assert len(v2._fetch_all_pages("ghapp", "endpoint")) == 101
    try:
        with mock.patch.object(v2, "_command_json", return_value={"total_count": 2, "check_runs": [{"id": 1}]}):
            v2._fetch_all_pages("ghapp", "endpoint", object_key="check_runs")
    except v2.V2AcceptanceError:
        pass
    else:
        raise AssertionError("incomplete paginated check-runs result was accepted")
    print(
        f"gpu first-visible A3 v2 acceptance: positive=2 "
        f"cli_outcomes=3 planted_negatives={len(negatives)} live_check_controls=26"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
