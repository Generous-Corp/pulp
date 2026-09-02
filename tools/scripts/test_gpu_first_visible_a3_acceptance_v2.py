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
BLOB = "3" * 40
DIGEST = "4" * 64
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
                    "signatures_present": ["source", "shader", "content"],
                })
            row[key] = samples
        states.append(row)
    return {
        "schema": "pulp.gpu-first-visible-a3-role-samples.v2", "version": 2,
        "role_id": role, "manifest_seed": seed, "trial_order": order,
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
 print(json.dumps({{"schema":"pulp.gpu-first-visible-evidence-verification.v1","kind":a.kind,"artifact_sha256":d,"implementation_head":"{SHA}","valid":True}},sort_keys=True))
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
    support_payload = write_artifact(root, "policy/support-matrix-payload.json", {"protected": True})
    a1_payload = write_artifact(root, "policy/a1-evidence-payload.json", {"authentic": True})
    support_ref = write_artifact(root, "policy/support-matrix.json", {
        "schema": "pulp.gpu-first-visible-generated-evidence.v1", "version": 1,
        "kind": "support-matrix", "implementation_head": SHA, "producer": producer_ref,
        "producer_provenance": producer_provenance_ref, "artifact": support_payload,
    })
    a1_ref = write_artifact(root, "policy/a1-evidence.json", {
        "schema": "pulp.gpu-first-visible-generated-evidence.v1", "version": 1,
        "kind": "a1-evidence", "implementation_head": SHA, "producer": producer_ref,
        "producer_provenance": producer_provenance_ref, "artifact": a1_payload,
    })
    policy = {
        "schema": "pulp.gpu-first-visible-budget-authority.v1", "version": 1,
        "budget_id": "a3-v2-test-authority",
        "source": {"repository": "danielraffel/pulp-planning", "revision": SHA, "path": "research/evidence/gpu-ux/a3-budget/a3-v2-test-authority.product-policy.json", "blob": BLOB},
        "approval": {"github_user_id": 25807, "mode": "approval", "pr_url": "https://github.com/danielraffel/pulp-planning/pull/1", "approved_head": SHA},
        "clock": "mach_continuous_time", "editor_open_origin": "editor-open-requested",
        "first_nonblank_endpoint": "first-nonblank-presented-frame", "statistic": "p95",
        "first_applicable_pulp_revision": SHA,
        "first_applicable_forge_revision": FORGE_SHA,
        "reference_host": {"host_id": "m5", "machine_id": "fleet-m5", "hardware": "M5", "os": "macOS", "display": "main", "refresh_hz": 60},
        "roles": [
            {"role_id": role, "first_visible_p95_ns": 1000,
             "first_interaction": interaction(role),
             "steady_cpu_frame_p95_ns": 1000, "steady_gpu_frame_p95_ns": 1000}
            for role in v2.ROLE_IDS
        ],
        "required_coverage": {"predicate": "supported-constrained-metal-adapter", "adapter": "authority-metal-constrained", "configuration": "authority-config", "support_matrix": support_ref, "a1_evidence": [a1_ref]},
        "a4_scenario_budgets": [
            {"scenario_id": scenario, "role_ids": ["pulp-standalone"], "frame_budget_ns": 1000}
            for scenario in v2.A4_SCENARIOS
        ],
        "canary": {"binary_sha256": DIGEST, "content_sha256": DIGEST, "signature_sha256": DIGEST, "editor_open_origin": "editor-open-requested", "interaction_lifecycle": "manifest-bound", "steady_state_workload": "manifest-bound"},
    }
    policy_ref = write_artifact(root, "policy/product-policy.json", policy)
    validation_ref = write_artifact(root, "policy/validation.json", {
        "schema": "pulp.gpu-first-visible-product-policy-validation.v1", "version": 1,
        "status": "pass", "checked_at": "2026-08-29T00:00:00Z",
        "fresh_live_state": True, "protected_commit": True,
        "repository": policy["source"]["repository"], "revision": SHA,
        "path": policy["source"]["path"], "blob": BLOB,
        "github_user_id": 25807, "approved_head": SHA,
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
        identity = {
                "pulp_revision": SHA, "forge_revision": FORGE_SHA if role in v2.FORGE_ROLES else None,
                "machine_id": "fleet-m5", "host_kind": host,
                "host_bundle_id": "fixture.host", "host_version": "1", "host_sha256": DIGEST,
                "application_kind": application, "application_sha256": DIGEST,
                "plugin_format": plugin_format, "plugin_sha256": DIGEST,
                "provider_sha256": DIGEST, "build_sha256": DIGEST,
                "signature_sha256": DIGEST, "content_sha256": DIGEST,
                "adapter": "authority-metal-constrained" if role == "constrained-adapter" else "metal",
                "display_id": "main", "refresh_hz": 60,
        }
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


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        receipt = make_fixture(root)
        with mock.patch.object(v2, "live_protected_main_errors", return_value=[]), mock.patch.object(
            v2.trace_producer_overhead, "validate_receipt", return_value=None
        ):
            assert v2.validate_v2(receipt, root, receipt_path=root / "canonical.json", repository=root) is True
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
        ("false unavailable", lambda r, _p: r["campaigns"][0].update(status="unavailable")),
        ("executor disposition", lambda r, _p: r.update(disposition="queue-B4")),
        ("caller self-attestation", lambda r, _p: r["publication"].update(protected=True, head=SHA, required_checks=[])),
        ("wrong policy head", lambda r, p: rewrite_artifact(p, r["product_policy"]["authority"], lambda policy: policy["source"].update(revision="9" * 40))),
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
        f"gpu first-visible A3 v2 acceptance: positive=1 "
        f"cli_outcomes=3 planted_negatives={len(negatives)} live_check_controls=6"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
