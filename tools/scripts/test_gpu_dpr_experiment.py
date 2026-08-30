#!/usr/bin/env python3
"""Self-test the A4 DPR evidence contract, matrix, and failure controls."""

from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any
from unittest import mock

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(SCRIPT_DIR))
import gpu_dpr_experiment as contract  # noqa: E402
import json_schema_lite  # noqa: E402

SHA_A = "a" * 40
SHA_B = "b" * 40
SHA_C = "c" * 40
DIGEST_A = "1" * 64
DIGEST_B = "2" * 64


def statistic(unit: str, median: float, p95: float) -> dict[str, Any]:
    return {
        "unit": unit, "provenance": "measured",
        "definition": "synthetic contract fixture measurement",
        "median": median, "p95": p95, "sample_count": 30,
    }


def observation(
    scenario: dict[str, Any], mode: str, requested_dpr: float,
    manifest: dict[str, Any]
) -> dict[str, Any]:
    if mode == "exact":
        observed = requested_dpr
        maximum = None
        adaptive = None
    elif mode == "configured_max":
        maximum = manifest["configured_max_dpr"]
        observed = min(requested_dpr, maximum)
        adaptive = None
    else:
        observed = requested_dpr
        maximum = None
        adaptive = manifest["adaptive_profile"]["id"]
    logical = scenario["logical_size"]
    return {
        "scenario_id": scenario["id"],
        "mode": mode,
        "requested_dpr": requested_dpr,
        "observed_dpr": observed,
        "configured_max_dpr": maximum,
        "adaptive_profile_id": adaptive,
        "logical_size": logical,
        "physical_size": {
            "width": round(logical["width"] * observed),
            "height": round(logical["height"] * observed),
        },
        "content_digest": hashlib.sha256(scenario["id"].encode()).hexdigest(),
        "machine_id": "synthetic-fixture-machine",
        "adapter_class": "hardware",
        "metrics": {
            "cpu_frame_time": statistic("ms", 2.0, 3.0),
            "gpu_frame_time": statistic("ms", 1.0, 1.5),
            "first_frame_time": statistic("ms", 10.0, 12.0),
            "interaction_latency": statistic("ms", 5.0, 7.0),
            "render_target_bytes": statistic("bytes", 1024, 1024),
            "resident_bytes": statistic("bytes", 4096, 4096),
            "upload_bytes": statistic("bytes", 512, 512),
        },
        "fidelity": {
            "content_floor_passed": True,
            "capture_similarity": 0.995,
            "small_text_legible": True,
            "thin_strokes_preserved": True,
            "logical_input_correct": True,
        },
        "trace_evidence_ids": ["synthetic:frame:1"],
        "artifacts": [
            {"kind": "capture", "path": "fixture/capture.png", "sha256": DIGEST_A},
            {"kind": "reference_capture", "path": "fixture/reference.png", "sha256": DIGEST_B},
            {"kind": "trace", "path": "fixture/trace.pftrace", "sha256": DIGEST_B},
            {"kind": "raw_samples", "path": "fixture/samples.json", "sha256": DIGEST_A},
            {"kind": "input_receipt", "path": "fixture/input.json", "sha256": DIGEST_B},
        ],
    }


def complete_fixture(manifest: dict[str, Any]) -> dict[str, Any]:
    observations = [
        observation(scenario, mode, dpr, manifest)
        for scenario in manifest["scenarios"]
        for mode in contract.MODES
        for dpr in contract.REQUESTED_DPRS
    ]
    return {
        "schema": "pulp.gpu-dpr-experiment.v1",
        "version": 1,
        "evidence_kind": "synthetic_fixture",
        "status": "complete",
        "experiment_id": "synthetic-contract-control",
        "plan_revision": SHA_A,
        "pulp_sha": SHA_B,
        "forge_sha": SHA_C,
        "dependencies": {
            "a2t_receipt": "synthetic:a2t",
            "a3_budget_id": "synthetic:a3-budget",
            "a3_receipt": "synthetic:a3",
        },
        "matrix": {
            "manifest_sha256": contract.canonical_sha256(manifest),
            "requested_dprs": contract.REQUESTED_DPRS,
            "modes": contract.MODES,
            "scenario_ids": [item["id"] for item in manifest["scenarios"]],
        },
        "observations": observations,
        "disposition": "no-change",
        "eligible_for_policy": False,
    }


def v2_cell(
    scenario: dict[str, Any], mode: str, requested_dpr: float,
    campaign: str, manifest: dict[str, Any], manifest_digest: str,
) -> dict[str, Any]:
    affected = mode in {"configured_max", "adaptive_simulation"} and requested_dpr == 3
    gpu = 1_000_000
    memory = 10_000
    if affected and mode == "configured_max":
        gpu, memory = 800_000, 8_000
    elif affected and mode == "adaptive_simulation":
        gpu, memory = 650_000, 6_500
    trials = []
    for index in range(30):
        trials.append({
            "trial_index": index,
            "mode_order": contract.deterministic_mode_order(
                manifest_digest, scenario["id"], requested_dpr, index
            ),
            "frame_count": 240,
            "gpu_p95_ns": gpu,
            "cpu_median_ns": 500_000,
            "cpu_p95_ns": 600_000,
            "first_frame_median_ns": 5_000_000,
            "first_frame_p95_ns": 6_000_000,
            "interaction_median_ns": 2_000_000,
            "interaction_p95_ns": 2_500_000,
            "render_target_p95_bytes": memory,
            "resident_p95_bytes": memory * 2,
            "upload_p95_bytes": 1_000,
            "frame_misses": 0,
            "xruns": 0,
            "affected": affected,
            "sequence_sha256": hashlib.sha256(
                f"{campaign}:{scenario['id']}:{mode}:{requested_dpr}:{index}".encode()
            ).hexdigest(),
        })
    pid_base = int(hashlib.sha256(
        f"{campaign}:{scenario['id']}:{mode}:{requested_dpr}".encode()
    ).hexdigest()[:7], 16) + 1
    fresh = [{
        "trial_index": index,
        "pid": pid_base + index,
        "first_nonblank_present_ns": 5_000_000,
        "started_at_highest_eligible_rung": True,
        "counters_zero": True,
        "sequence_sha256": hashlib.sha256(
            f"fresh:{campaign}:{scenario['id']}:{mode}:{requested_dpr}:{index}".encode()
        ).hexdigest(),
    } for index in range(20)]
    required = set(scenario["required_oracles"])
    daw = []
    if scenario["id"] == "forge-modular-daw":
        daw = [{
            "format": fmt, "host": host, "a3_role": role,
            "outcome": "pass", "gates_passed": True,
            "identity_sha256": hashlib.sha256(f"identity:{fmt}:{host}".encode()).hexdigest(),
            "receipt_sha256": hashlib.sha256(
                f"receipt:{campaign}:{mode}:{requested_dpr}:{fmt}:{host}".encode()
            ).hexdigest(),
        } for fmt, host, role in contract.DAW_SUBRECEIPTS]
    artifacts = [{
        "kind": kind, "path": f"evidence/{campaign}/{scenario['id']}/{mode}/{requested_dpr}/{kind}",
        "sha256": hashlib.sha256(
            f"{campaign}:{scenario['id']}:{mode}:{requested_dpr}:{kind}".encode()
        ).hexdigest(),
    } for kind in (
        "raw_trials", "frame_sequences", "capture", "trace", "input_receipt",
        "identity_receipt",
    )]
    return {
        "campaign": campaign,
        "scenario_id": scenario["id"],
        "policy_class": scenario["policy_class"],
        "mode": mode,
        "requested_dpr": requested_dpr,
        "outcome": "pass",
        "identity": {
            "machine_id": "m5-reference",
            "provider": scenario["required_provider"],
            "adapter": "apple-m5-hardware",
            "build_sha": SHA_B,
            "host": scenario["required_host"],
            "format": "aggregate" if scenario["id"] == "forge-modular-daw" else "standalone",
        },
        "logical_size": scenario["logical_size"],
        "physical_dimensions_verified": True,
        "warmup_count": 5,
        "measured_trials": trials,
        "fresh_process_trials": fresh,
        "adaptive_summary": ({
            "initial_rung": requested_dpr,
            "counters_zero": True,
            "min_applied_dpr": 2 if requested_dpr == 3 else requested_dpr,
            "max_applied_dpr": requested_dpr,
            "all_transitions_one_rung": True,
            "no_state_leakage": True,
        } if mode == "adaptive_simulation" else None),
        "fidelity": {
            "capture_similarity": 0.995,
            "small_text_luminance_stddev": (
                2.0 if "small_text" in required else "not-applicable-by-manifest"
            ),
            "thin_stroke_coverage": (
                0.01 if "thin_strokes" in required else "not-applicable-by-manifest"
            ),
            "content_floor": True,
            "logical_input_exact": True,
            "identity": True,
        },
        "daw_subreceipts": daw,
        "artifacts": artifacts,
    }


def complete_v2_fixture(manifest: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    authorized = copy.deepcopy(manifest)
    authorized["v2_protocol"]["status"] = "authorized"
    authorized["v2_protocol"]["product_policy"] = {
        "id": "synthetic-policy", "path": "planning/policy.json",
        "blob": SHA_A, "a3_receipt_sha256": DIGEST_A,
    }
    authorized["trial_contract"]["timer_noise_p95_ns"] = 1_000
    authorized["trial_contract"]["memory_sampler_resolution_bytes"] = 1
    for scenario in authorized["scenarios"]:
        scenario["frame_budget_ns"] = 16_666_667
    digest = contract.canonical_sha256(authorized)
    cells = [
        v2_cell(scenario, mode, dpr, "original", authorized, digest)
        for scenario in authorized["scenarios"]
        for mode in contract.MODES for dpr in contract.REQUESTED_DPRS
    ]
    repeats = copy.deepcopy(cells)
    for cell in repeats:
        cell["campaign"] = "repeat"
        for index, trial in enumerate(cell["measured_trials"]):
            trial["sequence_sha256"] = hashlib.sha256(
                f"repeat:{cell['scenario_id']}:{cell['mode']}:{cell['requested_dpr']}:{index}".encode()
            ).hexdigest()
        for index, trial in enumerate(cell["fresh_process_trials"]):
            trial["pid"] += 10_000_000
            trial["sequence_sha256"] = hashlib.sha256(
                f"repeat-fresh:{cell['scenario_id']}:{cell['mode']}:{cell['requested_dpr']}:{index}".encode()
            ).hexdigest()
        for artifact in cell["artifacts"]:
            artifact["path"] = artifact["path"].replace("/original/", "/repeat/")
    document = {
        "schema": "pulp.gpu-dpr-experiment.v2", "version": 2,
        "status": "complete", "evidence_kind": "measured",
        "experiment_id": "synthetic-v2-control", "plan_revision": SHA_A,
        "pulp_sha": SHA_B, "forge_sha": SHA_C,
        "authority": {
            "manifest_sha256": digest, "product_policy_id": "synthetic-policy",
            "product_policy_blob": SHA_A, "a3_receipt_sha256": DIGEST_A,
            "timer_noise_p95_ns": 1_000, "memory_sampler_resolution_bytes": 1,
            "collection_authorized": True,
        },
        "matrix": {
            "scenario_ids": [item["id"] for item in authorized["scenarios"]],
            "modes": contract.MODES, "requested_dprs": contract.REQUESTED_DPRS,
            "cell_count": 84, "repeat_cell_count": 84,
        },
        "cells": cells, "repeat_cells": repeats,
        "analysis": {},
        "publication": {
            "status": "candidate-awaiting-live-proof", "repository": "Generous-Corp/pulp",
            "revision": None,
            "path": "docs/validation/gpu-dpr/terminal-result.json",
            "protected_main_verified": False, "required_checks_green": False,
        },
        "b5_gate": {
            "status": "waiting-trigger", "requires": ["B0-adopted-vellum-api-refresh"],
            "authorizes_policy_change": False,
        },
    }
    document["analysis"] = contract.compute_v2_analysis(cells, repeats, authorized, digest)
    return document, authorized


def expect_semantic_failure(
    document: dict[str, Any], manifest: dict[str, Any], label: str
) -> None:
    problems = contract.result_semantic_errors(
        document, manifest, contract.canonical_sha256(manifest)
    )
    if not problems:
        raise AssertionError(f"semantic mutation unexpectedly passed: {label}")


def main() -> int:
    manifest = contract.load_json(contract.DEFAULT_MANIFEST)
    assert not contract.manifest_errors(manifest, contract.DEFAULT_MANIFEST)
    assert len(contract.expected_matrix(manifest)) == 84
    validity = contract.load_json(
        ROOT / "docs/validation/gpu-dpr/instrument-validity-state.json"
    )
    assert validity["schema"] == "pulp.gpu-dpr-evidence-validity.v1"
    assert validity["entries"]
    for entry in validity["entries"]:
        assert entry["validity"] == "SUPERSEDED"
        assert entry["disposition_eligibility"] == "NONCOUNTED"
        assert entry["counted_cells"] == 0
        assert entry["reasons"]
    manifest_mutations: list[tuple[str, Any]] = [
        ("missing", None),
        ("boolean", True),
        ("below-range", -0.01),
        ("above-range", 1.01),
    ]
    for label, value in manifest_mutations:
        mutated_manifest = copy.deepcopy(manifest)
        if value is None:
            del mutated_manifest["trial_contract"]["capture_similarity_minimum"]
        else:
            mutated_manifest["trial_contract"]["capture_similarity_minimum"] = value
        if not contract.manifest_errors(mutated_manifest, contract.DEFAULT_MANIFEST):
            raise AssertionError(
                f"capture similarity manifest mutation unexpectedly passed: {label}"
            )
    for scenario_id in ("threejs-audio-reactive", "super-convolver-web"):
        mutated_manifest = copy.deepcopy(manifest)
        scenario = next(
            item for item in mutated_manifest["scenarios"]
            if item["id"] == scenario_id
        )
        del scenario["logical_input_oracle"]
        if not contract.manifest_errors(mutated_manifest, contract.DEFAULT_MANIFEST):
            raise AssertionError(
                f"source-less scenario oracle mutation unexpectedly passed: {scenario_id}"
            )
    mutated_manifest = copy.deepcopy(manifest)
    web_scenario = next(
        item for item in mutated_manifest["scenarios"]
        if item["id"] == "super-convolver-web"
    )
    web_scenario["fidelity_oracle"]["small_text_roi"]["x"] = 1000
    if not contract.manifest_errors(mutated_manifest, contract.DEFAULT_MANIFEST):
        raise AssertionError("out-of-bounds web fidelity region unexpectedly passed")

    schema = contract.schema_for_lite(contract.load_json(contract.DEFAULT_SCHEMA_V1))
    fixture = complete_fixture(manifest)
    assert not json_schema_lite.validate(fixture, schema)
    assert not contract.result_semantic_errors(
        fixture, manifest, contract.canonical_sha256(manifest)
    )

    mutations: list[tuple[str, dict[str, Any]]] = []
    missing_cell = copy.deepcopy(fixture)
    missing_cell["observations"].pop()
    mutations.append(("missing matrix cell", missing_cell))
    duplicate_cell = copy.deepcopy(fixture)
    duplicate_cell["observations"][-1] = copy.deepcopy(duplicate_cell["observations"][0])
    mutations.append(("duplicate matrix cell", duplicate_cell))
    changed_content = copy.deepcopy(fixture)
    changed_content["observations"][1]["content_digest"] = DIGEST_B
    mutations.append(("different content between DPRs", changed_content))
    wrong_physical = copy.deepcopy(fixture)
    wrong_physical["observations"][0]["physical_size"]["width"] += 1
    mutations.append(("physical size mismatch", wrong_physical))
    blurred_text = copy.deepcopy(fixture)
    blurred_text["evidence_kind"] = "measured"
    blurred_text["eligible_for_policy"] = True
    blurred_text["observations"][0]["fidelity"]["small_text_legible"] = False
    mutations.append(("blurred text policy candidate", blurred_text))
    bad_input = copy.deepcopy(fixture)
    bad_input["evidence_kind"] = "measured"
    bad_input["eligible_for_policy"] = True
    bad_input["observations"][0]["fidelity"]["logical_input_correct"] = False
    mutations.append(("broken logical input policy candidate", bad_input))
    low_similarity = copy.deepcopy(fixture)
    low_similarity["evidence_kind"] = "measured"
    low_similarity["eligible_for_policy"] = True
    low_similarity["observations"][0]["fidelity"]["capture_similarity"] = 0.98
    mutations.append(("capture similarity below ratified floor", low_similarity))
    synthetic_policy = copy.deepcopy(fixture)
    synthetic_policy["eligible_for_policy"] = True
    mutations.append(("synthetic evidence eligible for policy", synthetic_policy))
    unavailable_policy = copy.deepcopy(fixture)
    unavailable_policy["evidence_kind"] = "measured"
    unavailable_policy["eligible_for_policy"] = True
    unavailable = unavailable_policy["observations"][0]["metrics"]["gpu_frame_time"]
    unavailable.update(
        provenance="unavailable", median=None, p95=None, sample_count=0
    )
    mutations.append(("unavailable metric eligible for policy", unavailable_policy))
    no_a3 = copy.deepcopy(fixture)
    no_a3["dependencies"]["a3_receipt"] = None
    mutations.append(("complete result without A3 receipt", no_a3))
    wrong_profile = copy.deepcopy(fixture)
    adaptive_item = next(
        item for item in wrong_profile["observations"]
        if item["mode"] == "adaptive_simulation"
    )
    adaptive_item["adaptive_profile_id"] = "unregistered-profile"
    mutations.append(("adaptive profile drift", wrong_profile))
    for label, mutated in mutations:
        expect_semantic_failure(mutated, manifest, label)

    schema_mutation = copy.deepcopy(fixture)
    del schema_mutation["matrix"]
    assert json_schema_lite.validate(schema_mutation, schema)
    schema_mutation = copy.deepcopy(fixture)
    schema_mutation["observations"][0]["mode"] = "shipping-adaptive"
    assert json_schema_lite.validate(schema_mutation, schema)

    command = [
        sys.executable, str(SCRIPT_DIR / "gpu_dpr_experiment.py"),
        "emit-plan-v1", "--experiment-id", "a4-planned-control",
        "--plan-revision", SHA_A, "--pulp-sha", SHA_B,
    ]
    emitted = subprocess.run(command, check=True, capture_output=True, text=True)
    planned = json.loads(emitted.stdout)
    assert planned["status"] == "planned"
    assert planned["observations"] == []
    assert planned["disposition"] is None
    assert planned["eligible_for_policy"] is False

    canonical = contract.load_json(contract.DEFAULT_RESULT)
    v2_schema = contract.schema_for_lite(contract.load_json(contract.DEFAULT_SCHEMA))
    assert not json_schema_lite.validate(canonical, v2_schema)
    assert not contract.v2_semantic_errors(
        canonical, manifest, contract.canonical_sha256(manifest)
    )
    assert canonical["status"] == "inconclusive"
    assert canonical["analysis"]["disposition"] is None
    assert canonical["b5_gate"]["authorizes_policy_change"] is False

    v2, authorized = complete_v2_fixture(manifest)
    authorized_digest = contract.canonical_sha256(authorized)
    schema_errors = json_schema_lite.validate(v2, v2_schema)
    if schema_errors:
        raise AssertionError(f"valid v2 fixture failed schema: {schema_errors[:3]}")
    semantic_errors = contract.v2_semantic_errors(v2, authorized, authorized_digest)
    if semantic_errors:
        raise AssertionError(f"valid v2 fixture failed semantics: {semantic_errors[:3]}")
    assert v2["analysis"]["disposition"] == "adaptive-candidate"
    with mock.patch.object(contract, "live_protected_main_errors", return_value=[]):
        assert not contract.v2_semantic_errors(
            v2, authorized, authorized_digest, verify_live_publication=True
        )
    with mock.patch.object(
        contract, "live_protected_main_errors",
        return_value=["checkout HEAD is not exact live protected main"],
    ):
        assert contract.v2_semantic_errors(
            v2, authorized, authorized_digest, verify_live_publication=True
        )
    rules_only = [{
        "type": "required_status_checks",
        "parameters": {"required_status_checks": [{
            "context": "macos", "integration_id": 42,
        }]},
    }]
    required = contract.required_check_identities({}, rules_only)
    assert required == {("macos", 42)}
    good_run = {
        "id": 1, "name": "macos", "app": {"id": 42},
        "status": "completed", "conclusion": "success",
        "completed_at": "2026-08-30T00:00:00Z",
    }
    assert not contract.required_check_result_errors(required, [good_run], [])
    wrong_app = copy.deepcopy(good_run)
    wrong_app["app"]["id"] = 43
    assert contract.required_check_result_errors(required, [wrong_app], [])
    ambiguous = [copy.deepcopy(good_run), copy.deepcopy(good_run)]
    ambiguous[1]["id"] = 2
    assert contract.required_check_result_errors(required, ambiguous, [])
    with mock.patch.object(
        contract, "_command_json", return_value={
            "total_count": 101, "check_runs": [good_run] * 100,
        },
    ):
        try:
            contract._fetch_all_pages(
                "/fake/ghapp", "repos/Generous-Corp/pulp/commits/deadbeef/check-runs",
                object_key="check_runs", max_pages=1,
            )
        except ValueError:
            pass
        else:
            raise AssertionError("truncated required-check pagination was accepted")

    v2_mutations: list[tuple[str, Any]] = []
    missing = copy.deepcopy(v2)
    missing["cells"].pop()
    v2_mutations.append(("missing original cell", missing))
    missing_repeat = copy.deepcopy(v2)
    missing_repeat["repeat_cells"].pop()
    v2_mutations.append(("missing repeat cell", missing_repeat))
    wrong_manifest = copy.deepcopy(v2)
    wrong_manifest["authority"]["manifest_sha256"] = DIGEST_B
    v2_mutations.append(("manifest digest drift", wrong_manifest))
    wrong_provider = copy.deepcopy(v2)
    wrong_provider["cells"][0]["identity"]["provider"] = "substitute-provider"
    v2_mutations.append(("wrong provider", wrong_provider))
    wrong_class = copy.deepcopy(v2)
    wrong_class["cells"][0]["policy_class"] = "web"
    v2_mutations.append(("wrong scenario class", wrong_class))
    wrong_order = copy.deepcopy(v2)
    wrong_order["cells"][0]["measured_trials"][0]["mode_order"].reverse()
    v2_mutations.append(("stale raw pair and mode order", wrong_order))
    bad_adaptive = copy.deepcopy(v2)
    adaptive_cell = next(
        cell for cell in bad_adaptive["cells"]
        if cell["mode"] == "adaptive_simulation" and cell["requested_dpr"] == 3
    )
    adaptive_cell["adaptive_summary"]["all_transitions_one_rung"] = False
    v2_mutations.append(("bad adaptive transition", bad_adaptive))
    leaked = copy.deepcopy(v2)
    leaked_cell = next(cell for cell in leaked["cells"] if cell["mode"] == "adaptive_simulation")
    leaked_cell["adaptive_summary"]["no_state_leakage"] = False
    v2_mutations.append(("adaptive state leakage", leaked))
    missing_daw = copy.deepcopy(v2)
    daw_cell = next(cell for cell in missing_daw["cells"] if cell["scenario_id"] == "forge-modular-daw")
    daw_cell["daw_subreceipts"].pop()
    v2_mutations.append(("missing DAW subreceipt", missing_daw))
    substituted_format = copy.deepcopy(v2)
    daw_cell = next(cell for cell in substituted_format["cells"] if cell["scenario_id"] == "forge-modular-daw")
    daw_cell["daw_subreceipts"][0]["host"] = "reaper"
    v2_mutations.append(("substituted DAW host", substituted_format))
    dimension_repeat = copy.deepcopy(v2)
    dimension_repeat["repeat_cells"][0]["logical_size"]["width"] += 1
    v2_mutations.append(("dimensional mismatch repeat", dimension_repeat))
    bypass = copy.deepcopy(v2)
    bypass["cells"][0]["fidelity"]["logical_input_exact"] = False
    v2_mutations.append(("fidelity input bypass", bypass))
    wrong_seed = copy.deepcopy(v2)
    wrong_seed["analysis"]["comparisons"][0]["seed_sha256"] = DIGEST_B
    v2_mutations.append(("wrong bootstrap seed", wrong_seed))
    wrong_resamples = copy.deepcopy(v2)
    wrong_resamples["analysis"]["comparisons"][0]["resamples"] = 9999
    v2_mutations.append(("wrong bootstrap resample count", wrong_resamples))
    executor_disposition = copy.deepcopy(v2)
    executor_disposition["analysis"]["disposition"] = "configured-max-candidate"
    v2_mutations.append(("executor-selected disposition", executor_disposition))
    caller_attested = copy.deepcopy(v2)
    caller_attested["publication"]["protected_main_verified"] = True
    caller_attested["publication"]["required_checks_green"] = True
    v2_mutations.append(("caller self-attested protected main", caller_attested))
    for label, mutated in v2_mutations:
        schema_failure = json_schema_lite.validate(mutated, v2_schema)
        semantic_failure = contract.v2_semantic_errors(
            mutated, authorized, authorized_digest
        ) if not schema_failure else schema_failure
        if not semantic_failure:
            raise AssertionError(f"v2 mutation unexpectedly passed: {label}")

    print(
        "gpu_dpr_contract_selftest=true "
        f"matrix_cells={len(contract.expected_matrix(manifest))} "
        f"semantic_mutations={len(mutations)} schema_mutations=2 "
        f"manifest_mutations={len(manifest_mutations)} "
        f"v2_mutations={len(v2_mutations)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
