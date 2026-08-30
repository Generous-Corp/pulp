#!/usr/bin/env python3
"""Positive fixture and planted negatives for the closed A3 v2 contract."""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
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
    support_ref = write_artifact(root, "policy/support-matrix.json", {"protected": True})
    a1_ref = write_artifact(root, "policy/a1-evidence.json", {"authentic": True})
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
    for role, host, application, plugin_format, _endpoint in v2.ROLE_SPECS:
        raw_ref = write_artifact(root, f"campaigns/{role}-raw.json", raw_campaign(role, policy_ref["sha256"]))
        trace_ref = write_artifact(root, f"campaigns/{role}.pftrace", b"fixture-trace", raw=True)
        analysis_ref = write_artifact(root, f"campaigns/{role}-analysis.json", {
            "schema": "pulp.gpu-first-visible-a3-trace-analysis.v2", "version": 2,
            "role_id": role, "categories": list(v2.TRACE_CATEGORIES),
            "trace_complete": True, "dropped_events": 0, "flush_complete": True,
        })
        campaigns.append({
            "role_id": role, "status": "pass",
            "identity": {
                "pulp_revision": SHA, "forge_revision": FORGE_SHA if role in v2.FORGE_ROLES else None,
                "machine_id": "fleet-m5", "host_kind": host,
                "host_bundle_id": "fixture.host", "host_version": "1", "host_sha256": DIGEST,
                "application_kind": application, "application_sha256": DIGEST,
                "plugin_format": plugin_format, "plugin_sha256": DIGEST,
                "provider_sha256": DIGEST, "build_sha256": DIGEST,
                "signature_sha256": DIGEST, "content_sha256": DIGEST,
                "adapter": "authority-metal-constrained" if role == "constrained-adapter" else "metal",
                "display_id": "main", "refresh_hz": 60,
            },
            "raw_samples": raw_ref, "trace": trace_ref, "trace_analysis": analysis_ref,
            "causal_attribution": {"render_pipeline_material": False, "instrumentation_complete": True, "missing_events": [], "transferred_vellum_routes": []},
        })
    receipt = {
        "$schema": "../contracts/gpu-first-visible-a3-acceptance-v2.schema.json",
        "schema": "dev.pulp.gpu-first-visible-a3-acceptance", "version": 2,
        "status": "complete", "recorded_at": "2026-08-29T00:00:00Z",
        "implementation_head": SHA,
        "plan": {"document": "research/plan.md", "revision": SHA, "lines": "949-1118", "sha256": DIGEST},
        "product_policy": {"status": "bound", "authority": policy_ref, "validation": validation_ref, "required_coverage": "bound"},
        "protocol": v2.canonical_protocol(), "campaigns": campaigns,
        "publication": {"repository": "Generous-Corp/pulp", "branch": "main", "head": SHA, "receipt_blob": BLOB, "protected": True, "required_checks": [{"name": "A3 v2", "conclusion": "success"}], "artifact_sha256s": []},
        "disposition": "no-change", "observations": [], "blockers": [],
    }
    receipt["publication"]["artifact_sha256s"] = sorted(
        set(v2.collect_artifact_sha256s(receipt)) | {support_ref["sha256"], a1_ref["sha256"]}
    )
    return receipt


def expect_rejected(label: str, mutate: Callable[[dict[str, Any], Path], None]) -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        receipt = make_fixture(root)
        mutate(receipt, root)
        try:
            v2.validate_v2(receipt, root)
        except v2.V2AcceptanceError:
            return
        raise AssertionError(f"planted negative was accepted: {label}")


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        receipt = make_fixture(root)
        assert v2.validate_v2(receipt, root) is True
        terminal_path = root / "terminal.json"
        terminal_path.write_text(json.dumps(receipt), encoding="utf-8")
        terminal = subprocess.run(
            [sys.executable, str(SCRIPT), "verify", str(terminal_path),
             "--evidence-root", str(root)],
            text=True, capture_output=True, check=False,
        )
        assert terminal.returncode == 0, (terminal.stdout, terminal.stderr)
        assert "PASS terminal=true" in terminal.stdout

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
        ("open result", lambda r, _p: r["publication"].update(protected=False)),
        ("wrong policy head", lambda r, p: rewrite_artifact(p, r["product_policy"]["authority"], lambda policy: policy["source"].update(revision="9" * 40))),
        ("missing steady budget", lambda r, p: rewrite_artifact(p, r["product_policy"]["authority"], lambda policy: policy["roles"][0].pop("steady_gpu_frame_p95_ns"))),
        ("short samples", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][0]["warm"].pop())),
        ("wrong seed", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw.update(manifest_seed=0))),
        ("missing category", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][3]["warm"][0]["trace_categories"].pop())),
        ("blank bypass", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][1]["warm"][0].update(blank=True))),
        ("audio thread work", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][1]["warm"][0].update(audio_thread_work_events=1))),
        ("xrun", lambda r, p: rewrite_artifact(p, r["campaigns"][0]["raw_samples"], lambda raw: raw["states"][1]["warm"][0].update(xrun_count=1))),
    ]
    for label, mutate in negatives:
        expect_rejected(label, mutate)
    print(
        f"gpu first-visible A3 v2 acceptance: positive=1 "
        f"cli_outcomes=3 planted_negatives={len(negatives)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
