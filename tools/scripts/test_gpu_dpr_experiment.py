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
    return {"unit": unit, "median": median, "p95": p95, "sample_count": 30}


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
            {"kind": "trace", "path": "fixture/trace.pftrace", "sha256": DIGEST_B},
            {"kind": "raw_samples", "path": "fixture/samples.json", "sha256": DIGEST_A},
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

    schema = contract.schema_for_lite(contract.load_json(contract.DEFAULT_SCHEMA))
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
    synthetic_policy = copy.deepcopy(fixture)
    synthetic_policy["eligible_for_policy"] = True
    mutations.append(("synthetic evidence eligible for policy", synthetic_policy))
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
        "emit-plan", "--experiment-id", "a4-planned-control",
        "--plan-revision", SHA_A, "--pulp-sha", SHA_B,
    ]
    emitted = subprocess.run(command, check=True, capture_output=True, text=True)
    planned = json.loads(emitted.stdout)
    assert planned["status"] == "planned"
    assert planned["observations"] == []
    assert planned["disposition"] is None
    assert planned["eligible_for_policy"] is False

    print(
        "gpu_dpr_contract_selftest=true "
        f"matrix_cells={len(contract.expected_matrix(manifest))} "
        f"semantic_mutations={len(mutations)} schema_mutations=2"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
