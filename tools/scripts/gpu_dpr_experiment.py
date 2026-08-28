#!/usr/bin/env python3
"""Validate and expand the evidence-only A4 DPR experiment contract."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
DEFAULT_MANIFEST = ROOT / "test" / "fixtures" / "gpu-ux" / "dpr" / "manifest.json"
DEFAULT_SCHEMA = ROOT / "docs" / "contracts" / "gpu-dpr-experiment-v1.schema.json"
sys.path.insert(0, str(SCRIPT_DIR))
import json_schema_lite  # noqa: E402

REQUESTED_DPRS = [1, 1.5, 2, 3]
MODES = ["exact", "configured_max", "adaptive_simulation"]
REQUIRED_SCENARIOS = {
    "dense-text-thin-strokes",
    "shader-heavy-controls",
    "meters-waveforms",
    "threejs-audio-reactive",
    "forge-modular-native",
    "forge-modular-daw",
    "super-convolver-web",
}
POLICY_DISPOSITIONS = {
    "no-change", "configured-max-candidate", "adaptive-candidate"
}


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_sha256(document: Any) -> str:
    payload = json.dumps(document, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def schema_for_lite(schema: dict[str, Any]) -> dict[str, Any]:
    """Resolve local refs so Pulp's dependency-free validator can check v1."""
    definitions = schema.get("$defs", {})

    def expand(node: Any) -> Any:
        if isinstance(node, list):
            return [expand(item) for item in node]
        if not isinstance(node, dict):
            return node
        if "$ref" in node:
            prefix = "#/$defs/"
            reference = node["$ref"]
            if not reference.startswith(prefix):
                raise ValueError(f"unsupported schema reference: {reference}")
            name = reference.removeprefix(prefix)
            if name not in definitions:
                raise ValueError(f"missing schema definition: {name}")
            merged = copy.deepcopy(definitions[name])
            merged.update({key: value for key, value in node.items() if key != "$ref"})
            return expand(merged)
        return {
            key: expand(value)
            for key, value in node.items()
            if key != "$defs"
        }

    return expand(schema)


def manifest_errors(manifest: dict[str, Any], manifest_path: Path) -> list[str]:
    errors: list[str] = []
    if manifest.get("schema") != "pulp.gpu-dpr-corpus.v1":
        errors.append("unexpected corpus schema")
    if manifest.get("version") != 1:
        errors.append("unexpected corpus version")
    if manifest.get("requested_dprs") != REQUESTED_DPRS:
        errors.append("DPR matrix must be exactly 1, 1.5, 2, 3")
    if manifest.get("modes") != MODES:
        errors.append("mode matrix must be exact, configured_max, adaptive_simulation")

    scenarios = manifest.get("scenarios", [])
    ids = [scenario.get("id") for scenario in scenarios]
    if len(ids) != len(set(ids)):
        errors.append("scenario ids are not unique")
    if set(ids) != REQUIRED_SCENARIOS:
        errors.append(
            "scenario coverage drift: "
            f"missing={sorted(REQUIRED_SCENARIOS - set(ids))} "
            f"unexpected={sorted(set(ids) - REQUIRED_SCENARIOS)}"
        )
    corpus_dir = manifest_path.parent
    for scenario in scenarios:
        source_hash = scenario.get("source_sha256")
        if source_hash is None:
            continue
        source = corpus_dir / scenario["source"]
        if not source.is_file():
            errors.append(f"{scenario['id']}: missing source {source}")
        elif sha256_file(source) != source_hash:
            errors.append(f"{scenario['id']}: source digest drift")
        if not scenario.get("required_oracles"):
            errors.append(f"{scenario['id']}: no required oracle")

    adaptive = manifest.get("adaptive_profile", {})
    if adaptive.get("shipping") is not False:
        errors.append("adaptive profile must remain non-shipping")
    if adaptive.get("scale_ladder") != REQUESTED_DPRS:
        errors.append("adaptive scale ladder differs from the DPR matrix")
    trial = manifest.get("trial_contract", {})
    if trial.get("warmups", 0) < 1 or trial.get("measured_trials", 0) < 1:
        errors.append("trial counts must be positive")
    for gate in ("small_text_legible", "thin_strokes_preserved", "logical_input_correct"):
        if gate not in trial.get("fidelity_gates", []):
            errors.append(f"missing fidelity gate: {gate}")
    return errors


def expected_matrix(manifest: dict[str, Any]) -> set[tuple[str, str, float]]:
    return {
        (scenario["id"], mode, float(dpr))
        for scenario in manifest["scenarios"]
        for mode in MODES
        for dpr in REQUESTED_DPRS
    }


def result_semantic_errors(
    result: dict[str, Any], manifest: dict[str, Any], manifest_digest: str
) -> list[str]:
    errors: list[str] = []
    matrix = result["matrix"]
    scenario_ids = [scenario["id"] for scenario in manifest["scenarios"]]
    if matrix["manifest_sha256"] != manifest_digest:
        errors.append("result references a different corpus manifest")
    if matrix["requested_dprs"] != REQUESTED_DPRS or matrix["modes"] != MODES:
        errors.append("result matrix differs from the frozen corpus")
    if matrix["scenario_ids"] != scenario_ids:
        errors.append("result scenario ordering differs from the frozen corpus")

    observations = result["observations"]
    keys = [
        (item["scenario_id"], item["mode"], float(item["requested_dpr"]))
        for item in observations
    ]
    if len(keys) != len(set(keys)):
        errors.append("observation matrix keys are not unique")
    unexpected = set(keys) - expected_matrix(manifest)
    if unexpected:
        errors.append(f"unexpected observation keys: {sorted(unexpected)}")

    source_sizes = {
        scenario["id"]: scenario["logical_size"] for scenario in manifest["scenarios"]
    }
    content_by_scenario: dict[str, str] = {}
    for item in observations:
        scenario = item["scenario_id"]
        if item["logical_size"] != source_sizes[scenario]:
            errors.append(f"{scenario}: logical size changed between content legs")
        digest = item["content_digest"]
        if scenario in content_by_scenario and content_by_scenario[scenario] != digest:
            errors.append(f"{scenario}: content digest changed between DPR legs")
        content_by_scenario[scenario] = digest
        if item["observed_dpr"] <= 0:
            errors.append(f"{scenario}: observed DPR is not positive")
        expected_width = round(item["logical_size"]["width"] * item["observed_dpr"])
        expected_height = round(item["logical_size"]["height"] * item["observed_dpr"])
        if item["physical_size"] != {"width": expected_width, "height": expected_height}:
            errors.append(f"{scenario}: physical size does not match logical size x DPR")
        if item["mode"] == "exact":
            if item["observed_dpr"] != item["requested_dpr"]:
                errors.append(f"{scenario}: exact mode changed DPR")
            if item["configured_max_dpr"] is not None or item["adaptive_profile_id"] is not None:
                errors.append(f"{scenario}: exact mode carries policy simulation fields")
        elif item["mode"] == "configured_max":
            maximum = item["configured_max_dpr"]
            if maximum is None or item["observed_dpr"] != min(item["requested_dpr"], maximum):
                errors.append(f"{scenario}: configured max was not applied deterministically")
            if item["adaptive_profile_id"] is not None:
                errors.append(f"{scenario}: configured max carries adaptive profile")
        else:
            if item["configured_max_dpr"] is not None:
                errors.append(f"{scenario}: adaptive simulation carries configured max")
            if item["adaptive_profile_id"] != manifest["adaptive_profile"]["id"]:
                errors.append(f"{scenario}: adaptive profile differs from the corpus")
        for statistic in item["metrics"].values():
            if statistic["p95"] < statistic["median"]:
                errors.append(f"{scenario}: p95 is below median")
        artifact_kinds = {artifact["kind"] for artifact in item["artifacts"]}
        if not {"capture", "trace", "raw_samples"}.issubset(artifact_kinds):
            errors.append(f"{scenario}: observation lacks capture/trace/raw artifacts")

    status = result["status"]
    dependencies = result["dependencies"]
    if status == "complete":
        missing = expected_matrix(manifest) - set(keys)
        if missing:
            errors.append(f"complete result is missing {len(missing)} matrix cells")
        if any(value is None for value in dependencies.values()):
            errors.append("complete result lacks A2T/A3 dependency receipts")
        if result["disposition"] not in POLICY_DISPOSITIONS:
            errors.append("complete result lacks an A4 disposition")
    elif result["disposition"] is not None:
        errors.append("non-complete result carries an A4 disposition")

    if result["evidence_kind"] == "synthetic_fixture":
        if result["eligible_for_policy"]:
            errors.append("synthetic evidence is eligible for policy")
    elif result["status"] == "complete":
        fidelity_passed = all(
            item["fidelity"][gate]
            for item in observations
            for gate in (
                "content_floor_passed", "small_text_legible",
                "thin_strokes_preserved", "logical_input_correct"
            )
        )
        if result["eligible_for_policy"] != fidelity_passed:
            errors.append("policy eligibility disagrees with the fidelity gates")
    elif result["eligible_for_policy"]:
        errors.append("incomplete measured evidence is eligible for policy")
    return errors


def planned_result(args: argparse.Namespace, manifest: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": "pulp.gpu-dpr-experiment.v1",
        "version": 1,
        "evidence_kind": "measured",
        "status": "planned",
        "experiment_id": args.experiment_id,
        "plan_revision": args.plan_revision,
        "pulp_sha": args.pulp_sha,
        "forge_sha": args.forge_sha,
        "dependencies": {
            "a2t_receipt": None, "a3_budget_id": None, "a3_receipt": None
        },
        "matrix": {
            "manifest_sha256": canonical_sha256(manifest),
            "requested_dprs": REQUESTED_DPRS,
            "modes": MODES,
            "scenario_ids": [scenario["id"] for scenario in manifest["scenarios"]],
        },
        "observations": [],
        "disposition": None,
        "eligible_for_policy": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("validate-manifest")
    validate_result = subparsers.add_parser("validate-result")
    validate_result.add_argument("result", type=Path)
    emit = subparsers.add_parser("emit-plan")
    emit.add_argument("--experiment-id", required=True)
    emit.add_argument("--plan-revision", required=True)
    emit.add_argument("--pulp-sha", required=True)
    emit.add_argument("--forge-sha")
    args = parser.parse_args()

    manifest = load_json(args.manifest)
    problems = manifest_errors(manifest, args.manifest)
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    if args.command == "validate-manifest":
        print(f"gpu_dpr_manifest_valid=true matrix_cells={len(expected_matrix(manifest))}")
        return 0
    if args.command == "emit-plan":
        document = planned_result(args, manifest)
        schema_problems = json_schema_lite.validate(
            document, schema_for_lite(load_json(args.schema))
        )
        if schema_problems:
            print("\n".join(schema_problems), file=sys.stderr)
            return 1
        print(json.dumps(document, sort_keys=True, indent=2))
        return 0

    document = load_json(args.result)
    schema_problems = json_schema_lite.validate(
        document, schema_for_lite(load_json(args.schema))
    )
    semantic_problems = [] if schema_problems else result_semantic_errors(
        document, manifest, canonical_sha256(manifest)
    )
    if schema_problems or semantic_problems:
        print("\n".join([*schema_problems, *semantic_problems]), file=sys.stderr)
        return 1
    print(f"gpu_dpr_result_valid=true status={document['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

