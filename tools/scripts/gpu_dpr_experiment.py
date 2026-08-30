#!/usr/bin/env python3
"""Validate and expand the evidence-only A4 DPR experiment contract."""

from __future__ import annotations

import argparse
import copy
import hashlib
import itertools
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
DEFAULT_MANIFEST = ROOT / "test" / "fixtures" / "gpu-ux" / "dpr" / "manifest.json"
DEFAULT_SCHEMA_V1 = ROOT / "docs" / "contracts" / "gpu-dpr-experiment-v1.schema.json"
DEFAULT_SCHEMA = ROOT / "docs" / "contracts" / "gpu-dpr-experiment-v2.schema.json"
DEFAULT_RESULT = ROOT / "docs" / "validation" / "gpu-dpr" / "terminal-result.json"
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
POLICY_CLASSES = {"Pulp-native", "Forge-native/DAW", "web"}
EXPECTED_POLICY_CLASS = {
    "dense-text-thin-strokes": "Pulp-native",
    "shader-heavy-controls": "Pulp-native",
    "meters-waveforms": "Pulp-native",
    "threejs-audio-reactive": "Pulp-native",
    "forge-modular-native": "Forge-native/DAW",
    "forge-modular-daw": "Forge-native/DAW",
    "super-convolver-web": "web",
}
DAW_SUBRECEIPTS = [
    ("auv2", "logic", "forge-modular-auv2-logic"),
    ("vst3", "reaper", "forge-modular-vst3-reaper"),
    ("clap", "reaper", "forge-modular-clap-reaper"),
]
BOOTSTRAP_RESAMPLES = 10_000
V1_CLASSIFICATION = "historical-v1-nonterminal"


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

    protocol = manifest.get("v2_protocol")
    expected_protocol_fields = {
        "id": "a4-dpr-experiment-v2",
        "seed_encoding": "utf8-length-prefixed-u64be-v1",
        "prng": "sha256-counter-mode-u64be-v1",
        "bootstrap": {
            "method": "paired-percentile-bootstrap-v1",
            "resamples": BOOTSTRAP_RESAMPLES,
            "confidence_interval": 0.95,
            "sample_unit": "aligned-trial-p95",
            "percentile_method": "nearest-rank",
        },
        "repeat_scope": "all-84-cells",
        "daw_agreement": "terminal-verdict-gates-and-identity",
        "terminal_publication": "protected-pulp-main-with-green-required-checks",
    }
    if not isinstance(protocol, dict) or any(
        protocol.get(key) != value for key, value in expected_protocol_fields.items()
    ):
        errors.append("v2 protocol differs from the frozen contract")
    elif protocol.get("status") == "blocked-product-policy":
        if protocol.get("product_policy") is not None:
            errors.append("blocked v2 protocol carries a product policy")
    elif protocol.get("status") == "authorized":
        policy = protocol.get("product_policy")
        if not isinstance(policy, dict) or not all(
            isinstance(policy.get(key), str) and policy[key]
            for key in ("id", "path", "blob", "a3_receipt_sha256")
        ):
            errors.append("authorized v2 protocol lacks protected product-policy identity")
    else:
        errors.append("v2 protocol status must be blocked-product-policy or authorized")

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
        scenario_id = scenario.get("id")
        if scenario.get("policy_class") != EXPECTED_POLICY_CLASS.get(scenario_id):
            errors.append(f"{scenario_id}: wrong scenario policy class")
        if not isinstance(scenario.get("a3_role"), str) or not scenario["a3_role"]:
            errors.append(f"{scenario_id}: missing A3 role mapping")
        if not isinstance(scenario.get("required_provider"), str) or not scenario["required_provider"]:
            errors.append(f"{scenario_id}: missing provider binding")
        if not isinstance(scenario.get("required_host"), str) or not scenario["required_host"]:
            errors.append(f"{scenario_id}: missing host binding")
        budget = scenario.get("frame_budget_ns")
        authorized = isinstance(protocol, dict) and protocol.get("status") == "authorized"
        if authorized:
            if isinstance(budget, bool) or not isinstance(budget, int) or budget <= 0:
                errors.append(f"{scenario_id}: authorized frame budget must be positive integer ns")
        elif budget is not None:
            errors.append(f"{scenario_id}: unratified frame budget must remain null")
        if not scenario.get("required_oracles"):
            errors.append(f"{scenario['id']}: no required oracle")
        logical_size = scenario.get("logical_size", {})
        oracle = scenario.get("logical_input_oracle")
        point = oracle.get("point") if isinstance(oracle, dict) else None
        target = oracle.get("target") if isinstance(oracle, dict) else None
        if (
            not isinstance(point, list) or len(point) != 2
            or any(isinstance(value, bool) or not isinstance(value, (int, float))
                   for value in point)
            or not isinstance(target, str) or not target
            or not 0 <= float(point[0]) < float(logical_size.get("width", 0))
            or not 0 <= float(point[1]) < float(logical_size.get("height", 0))
        ):
            errors.append(f"{scenario['id']}: logical-input oracle is not a frozen in-bounds point/target")
        required_oracles = set(scenario.get("required_oracles", []))
        if required_oracles & {"small_text", "thin_strokes"}:
            regions = scenario.get("fidelity_oracle")
            for oracle_name, region_name in (
                ("small_text", "small_text_roi"),
                ("thin_strokes", "thin_stroke_roi"),
            ):
                if oracle_name not in required_oracles:
                    continue
                region = regions.get(region_name) if isinstance(regions, dict) else None
                if (
                    not isinstance(region, dict)
                    or set(region) != {"x", "y", "width", "height"}
                    or any(isinstance(region.get(field), bool)
                           or not isinstance(region.get(field), (int, float))
                           for field in region)
                    or region["x"] < 0 or region["y"] < 0
                    or region["width"] <= 0 or region["height"] <= 0
                    or region["x"] + region["width"] > logical_size.get("width", 0)
                    or region["y"] + region["height"] > logical_size.get("height", 0)
                ):
                    errors.append(
                        f"{scenario['id']}: {region_name} is not a frozen in-bounds region"
                    )
        source_hash = scenario.get("source_sha256")
        if source_hash is None:
            continue
        source = corpus_dir / scenario["source"]
        if not source.is_file():
            errors.append(f"{scenario['id']}: missing source {source}")
        elif sha256_file(source) != source_hash:
            errors.append(f"{scenario['id']}: source digest drift")

    adaptive = manifest.get("adaptive_profile", {})
    if adaptive.get("shipping") is not False:
        errors.append("adaptive profile must remain non-shipping")
    if adaptive.get("scale_ladder") != REQUESTED_DPRS:
        errors.append("adaptive scale ladder differs from the DPR matrix")
    trial = manifest.get("trial_contract", {})
    if (
        trial.get("warmups") != 5
        or trial.get("measured_trials") != 30
        or trial.get("fresh_process_first_frame_trials") != 20
        or trial.get("frames_per_warm_measured_trial") != 240
    ):
        errors.append("v2 trial counts must be exactly 5/30/20 with 240 frames")
    if trial.get("workload_sequence_id") != "a4-steady-state-workload-v1":
        errors.append("v2 workload sequence differs from the frozen protocol")
    authorized = isinstance(protocol, dict) and protocol.get("status") == "authorized"
    for field in ("timer_noise_p95_ns", "memory_sampler_resolution_bytes"):
        value = trial.get(field)
        if authorized:
            if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                errors.append(f"authorized {field} must be a positive integer")
        elif value is not None:
            errors.append(f"unratified {field} must remain null")
    similarity_minimum = trial.get("capture_similarity_minimum")
    if (
        isinstance(similarity_minimum, bool)
        or not isinstance(similarity_minimum, (int, float))
        or not 0 <= similarity_minimum <= 1
    ):
        errors.append("capture similarity minimum must be a number in 0..1")
    for field in (
        "small_text_luminance_stddev_minimum",
        "thin_stroke_coverage_minimum",
    ):
        value = trial.get(field)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
            errors.append(f"{field} must be a positive number")
    if trial.get("gpu_timer_calibration_trials", 0) < 3:
        errors.append("GPU timer calibration requires at least three trials")
    if trial.get("gpu_timer_extra_work_multiplier", 0) < 2:
        errors.append("GPU timer calibration extra-work multiplier must be at least two")
    for gate in (
        "capture_similarity", "small_text_legible",
        "thin_strokes_preserved", "logical_input_correct",
    ):
        if gate not in trial.get("fidelity_gates", []):
            errors.append(f"missing fidelity gate: {gate}")
    daw = next((item for item in scenarios if item.get("id") == "forge-modular-daw"), {})
    observed_daw = [
        (item.get("format"), item.get("host"), item.get("a3_role"))
        for item in daw.get("required_daw_subreceipts", [])
        if isinstance(item, dict)
    ]
    if observed_daw != DAW_SUBRECEIPTS:
        errors.append("Forge DAW scenario must freeze AUv2/Logic and VST3+CLAP/REAPER")
    return errors


def expected_matrix(manifest: dict[str, Any]) -> set[tuple[str, str, float]]:
    return {
        (scenario["id"], mode, float(dpr))
        for scenario in manifest["scenarios"]
        for mode in MODES
        for dpr in REQUESTED_DPRS
    }


def dpr_token(value: float | int) -> str:
    numeric = float(value)
    return str(int(numeric)) if numeric.is_integer() else format(numeric, ".15g")


def seed_material(*parts: str) -> bytes:
    """Encode seed fields without delimiter or numeric-representation ambiguity."""
    payload = bytearray()
    for part in parts:
        encoded = part.encode("utf-8")
        payload.extend(len(encoded).to_bytes(8, "big"))
        payload.extend(encoded)
    return bytes(payload)


def seed_sha256(*parts: str) -> str:
    return hashlib.sha256(seed_material(*parts)).hexdigest()


def counter_u64(seed_hex: str, counter: int) -> int:
    if len(seed_hex) != 64 or counter < 0:
        raise ValueError("invalid SHA-256 counter-mode input")
    block = hashlib.sha256(
        bytes.fromhex(seed_hex) + counter.to_bytes(8, "big")
    ).digest()
    return int.from_bytes(block[:8], "big")


def deterministic_mode_order(
    manifest_sha256: str, scenario_id: str, requested_dpr: float, trial_index: int
) -> list[str]:
    seed = seed_sha256(
        manifest_sha256, scenario_id, dpr_token(requested_dpr), str(trial_index)
    )
    permutations = list(itertools.permutations(MODES))
    return list(permutations[counter_u64(seed, 0) % len(permutations)])


def nearest_rank(values: list[float], quantile: float) -> float:
    if not values or not 0 < quantile <= 1:
        raise ValueError("nearest-rank requires samples and a quantile in (0,1]")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(quantile * len(ordered)) - 1)]


def paired_percentile_bootstrap(
    exact: list[float], candidate: list[float], seed_hex: str,
    resamples: int = BOOTSTRAP_RESAMPLES,
) -> tuple[float, float, float]:
    """Return exact-minus-candidate mean and a deterministic 95% percentile CI."""
    if len(exact) != 30 or len(candidate) != 30 or resamples != BOOTSTRAP_RESAMPLES:
        raise ValueError("v2 bootstrap requires 30 aligned pairs and 10,000 resamples")
    differences = [float(left) - float(right) for left, right in zip(exact, candidate)]
    point = statistics.fmean(differences)
    if len(set(differences)) == 1:
        return point, point, point
    estimates: list[float] = []
    counter = 0
    for _ in range(resamples):
        selected: list[float] = []
        for _ in differences:
            selected.append(differences[counter_u64(seed_hex, counter) % len(differences)])
            counter += 1
        estimates.append(statistics.fmean(selected))
    return point, nearest_rank(estimates, 0.025), nearest_rank(estimates, 0.975)


def v2_cell_key(cell: dict[str, Any]) -> tuple[str, str, float]:
    return cell.get("scenario_id", ""), cell.get("mode", ""), float(cell.get("requested_dpr", 0))


def _fidelity_passes(cell: dict[str, Any], scenario: dict[str, Any]) -> bool:
    fidelity = cell["fidelity"]
    required = set(scenario.get("required_oracles", []))
    text = fidelity["small_text_luminance_stddev"]
    stroke = fidelity["thin_stroke_coverage"]
    text_ok = (
        isinstance(text, (int, float)) and not isinstance(text, bool) and text >= 1.0
        if "small_text" in required else text == "not-applicable-by-manifest"
    )
    stroke_ok = (
        isinstance(stroke, (int, float)) and not isinstance(stroke, bool) and stroke >= 0.001
        if "thin_strokes" in required else stroke == "not-applicable-by-manifest"
    )
    return bool(
        fidelity["capture_similarity"] >= 0.99
        and fidelity["content_floor"] is True
        and fidelity["logical_input_exact"] is True
        and fidelity["identity"] is True
        and text_ok and stroke_ok
    )


def _cell_semantic_errors(
    cell: dict[str, Any], manifest: dict[str, Any], manifest_sha256: str,
    campaign: str,
) -> list[str]:
    errors: list[str] = []
    scenarios = {item["id"]: item for item in manifest["scenarios"]}
    scenario_id, mode, requested_dpr = v2_cell_key(cell)
    if scenario_id not in scenarios or (scenario_id, mode, requested_dpr) not in expected_matrix(manifest):
        return [f"unexpected {campaign} cell key: {(scenario_id, mode, requested_dpr)}"]
    scenario = scenarios[scenario_id]
    if cell.get("campaign") != campaign:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: wrong campaign label")
    if cell.get("policy_class") != scenario["policy_class"]:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: wrong policy class")
    identity = cell.get("identity", {})
    if identity.get("provider") != scenario["required_provider"]:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: wrong provider")
    if identity.get("host") != scenario["required_host"]:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: wrong host")
    if cell.get("logical_size") != scenario["logical_size"]:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: logical dimensions differ")
    if cell.get("physical_dimensions_verified") is not True:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: physical dimensions unverified")
    trials = cell.get("measured_trials", [])
    if [trial.get("trial_index") for trial in trials] != list(range(30)):
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: measured trial indices differ from 0..29")
    for trial in trials:
        index = trial.get("trial_index")
        if isinstance(index, int) and trial.get("mode_order") != deterministic_mode_order(
            manifest_sha256, scenario_id, requested_dpr, index
        ):
            errors.append(f"{scenario_id}/{mode}/{requested_dpr}: wrong deterministic mode order")
            break
        expected_affected = mode == "configured_max" and requested_dpr > 2
        if mode in {"exact", "configured_max"} and trial.get("affected") is not expected_affected:
            errors.append(f"{scenario_id}/{mode}/{requested_dpr}: affected flag disagrees with applied policy")
            break
    adaptive = cell.get("adaptive_summary")
    if mode == "adaptive_simulation":
        highest = max(value for value in REQUESTED_DPRS if value <= requested_dpr)
        if (
            not isinstance(adaptive, dict)
            or adaptive.get("initial_rung") != highest
            or adaptive.get("counters_zero") is not True
            or adaptive.get("all_transitions_one_rung") is not True
            or adaptive.get("no_state_leakage") is not True
            or adaptive.get("max_applied_dpr", 99) > requested_dpr
            or adaptive.get("min_applied_dpr", 0) < 1
            or any(trial.get("affected") is not (
                adaptive.get("min_applied_dpr", requested_dpr) < requested_dpr
            ) for trial in trials)
        ):
            errors.append(f"{scenario_id}/{mode}/{requested_dpr}: bad adaptive start/transition/state")
    elif adaptive is not None:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: non-adaptive cell carries adaptive state")
    fresh = cell.get("fresh_process_trials", [])
    if [trial.get("trial_index") for trial in fresh] != list(range(20)):
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: fresh-process trial indices differ from 0..19")
    if len({trial.get("pid") for trial in fresh}) != len(fresh):
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: fresh-process PIDs are reused")
    daw = cell.get("daw_subreceipts", [])
    if scenario_id == "forge-modular-daw":
        observed = [(item.get("format"), item.get("host"), item.get("a3_role")) for item in daw]
        if observed != DAW_SUBRECEIPTS:
            errors.append(f"{scenario_id}/{mode}/{requested_dpr}: DAW subreceipt coverage differs")
        if len({(item.get("outcome"), item.get("gates_passed")) for item in daw}) != 1:
            errors.append(f"{scenario_id}/{mode}/{requested_dpr}: DAW subreceipts disagree on verdict/gates")
    elif daw:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: non-DAW cell carries DAW subreceipts")
    artifact_kinds = [item.get("kind") for item in cell.get("artifacts", [])]
    expected_artifacts = {
        "raw_trials", "frame_sequences", "capture", "trace", "input_receipt", "identity_receipt"
    }
    if len(artifact_kinds) != len(set(artifact_kinds)) or set(artifact_kinds) != expected_artifacts:
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: artifact coverage differs")
    if cell.get("outcome") != "pass" or not _fidelity_passes(cell, scenario):
        errors.append(f"{scenario_id}/{mode}/{requested_dpr}: terminal fidelity/baseline gate failed")
    return errors


def _metric(cell: dict[str, Any], field: str, percentile: bool = False) -> float:
    values = [float(trial[field]) for trial in cell["measured_trials"]]
    return nearest_rank(values, 0.95) if percentile else statistics.median(values)


def _comparison(
    baseline: dict[str, Any], candidate: dict[str, Any],
    repeat_baseline: dict[str, Any], repeat_candidate: dict[str, Any],
    manifest_sha256: str, timer_noise_ns: int, memory_resolution: int,
    label: str,
) -> dict[str, Any]:
    scenario_id, _, requested_dpr = v2_cell_key(candidate)
    seed_label = (
        "adaptive-vs-configured" if label == "adaptive-vs-configured"
        else "candidate-vs-exact"
    )
    seed = seed_sha256(
        manifest_sha256, scenario_id, dpr_token(requested_dpr),
        label if label != "adaptive-vs-configured" else "", seed_label,
    )
    exact_gpu = [float(item["gpu_p95_ns"]) for item in baseline["measured_trials"]]
    candidate_gpu = [float(item["gpu_p95_ns"]) for item in candidate["measured_trials"]]
    point, low, high = paired_percentile_bootstrap(exact_gpu, candidate_gpu, seed)
    baseline_gpu_p95 = nearest_rank(exact_gpu, 0.95)
    candidate_gpu_p95 = nearest_rank(candidate_gpu, 0.95)
    time_material = (
        baseline_gpu_p95 - candidate_gpu_p95 >= max(timer_noise_ns, 0.10 * baseline_gpu_p95)
        and low > 0
    )
    baseline_rt = _metric(baseline, "render_target_p95_bytes", True)
    candidate_rt = _metric(candidate, "render_target_p95_bytes", True)
    baseline_resident = _metric(baseline, "resident_p95_bytes", True)
    candidate_resident = _metric(candidate, "resident_p95_bytes", True)
    baseline_upload = _metric(baseline, "upload_p95_bytes", True)
    candidate_upload = _metric(candidate, "upload_p95_bytes", True)
    memory_material = (
        candidate_rt <= 0.90 * baseline_rt
        and candidate_resident <= 0.90 * baseline_resident
        and candidate_upload <= 1.05 * baseline_upload
    )
    regression_fields = (
        ("cpu_median_ns", 1.02, False), ("cpu_p95_ns", 1.05, True),
        ("first_frame_median_ns", 1.02, False), ("first_frame_p95_ns", 1.05, True),
        ("interaction_median_ns", 1.02, False), ("interaction_p95_ns", 1.05, True),
    )
    regressions = all(
        _metric(candidate, field, percentile) <= multiplier * _metric(baseline, field, percentile)
        for field, multiplier, percentile in regression_fields
    ) and sum(item["frame_misses"] for item in candidate["measured_trials"]) <= sum(
        item["frame_misses"] for item in baseline["measured_trials"]
    ) and sum(item["xruns"] for item in candidate["measured_trials"]) <= sum(
        item["xruns"] for item in baseline["measured_trials"]
    )
    repeat_exact_gpu = [float(item["gpu_p95_ns"]) for item in repeat_baseline["measured_trials"]]
    repeat_candidate_gpu = [float(item["gpu_p95_ns"]) for item in repeat_candidate["measured_trials"]]
    repeat_delta = nearest_rank(repeat_exact_gpu, 0.95) - nearest_rank(repeat_candidate_gpu, 0.95)
    time_repeat = abs(repeat_delta - (baseline_gpu_p95 - candidate_gpu_p95)) <= max(
        timer_noise_ns, 0.05 * baseline_gpu_p95
    )
    def memory_delta(left: dict[str, Any], right: dict[str, Any], field: str) -> float:
        return _metric(left, field, True) - _metric(right, field, True)
    memory_repeat = all(
        abs(
            memory_delta(repeat_baseline, repeat_candidate, field)
            - memory_delta(baseline, candidate, field)
        ) <= max(memory_resolution, 0.05 * _metric(baseline, field, True))
        for field in ("render_target_p95_bytes", "resident_p95_bytes")
    )
    repeat_passed = (
        (not time_material or time_repeat)
        and (not memory_material or memory_repeat)
        and repeat_candidate.get("identity") == candidate.get("identity")
    )
    return {
        "scenario_id": scenario_id,
        "requested_dpr": requested_dpr,
        "candidate_mode": label,
        "seed_sha256": seed,
        "resamples": BOOTSTRAP_RESAMPLES,
        "point_estimate_ns": point,
        "ci_low_ns": low,
        "ci_high_ns": high,
        "affected": any(item["affected"] for item in candidate["measured_trials"]),
        "time_material": bool(time_material),
        "memory_material": bool(memory_material),
        "regression_gates_passed": bool(regressions),
        "repeat_passed": bool(repeat_passed),
    }


def compute_v2_analysis(
    cells: list[dict[str, Any]], repeat_cells: list[dict[str, Any]],
    manifest: dict[str, Any], manifest_sha256: str,
) -> dict[str, Any]:
    original = {v2_cell_key(cell): cell for cell in cells}
    repeated = {v2_cell_key(cell): cell for cell in repeat_cells}
    noise = manifest["trial_contract"]["timer_noise_p95_ns"]
    memory = manifest["trial_contract"]["memory_sampler_resolution_bytes"]
    if not isinstance(noise, int) or not isinstance(memory, int):
        raise ValueError("v2 analysis requires manifest-bound timer and memory resolution")
    comparisons: list[dict[str, Any]] = []
    candidate_support: dict[str, set[str]] = {
        "configured_max": set(), "adaptive_simulation": set(),
        "adaptive-vs-configured": set(),
    }
    scenarios = {item["id"]: item for item in manifest["scenarios"]}
    for scenario_id in [item["id"] for item in manifest["scenarios"]]:
        for requested_dpr in REQUESTED_DPRS:
            exact = original[(scenario_id, "exact", float(requested_dpr))]
            repeat_exact = repeated[(scenario_id, "exact", float(requested_dpr))]
            configured = original[(scenario_id, "configured_max", float(requested_dpr))]
            adaptive = original[(scenario_id, "adaptive_simulation", float(requested_dpr))]
            repeat_configured = repeated[(scenario_id, "configured_max", float(requested_dpr))]
            repeat_adaptive = repeated[(scenario_id, "adaptive_simulation", float(requested_dpr))]
            for mode, candidate, repeat_candidate in (
                ("configured_max", configured, repeat_configured),
                ("adaptive_simulation", adaptive, repeat_adaptive),
            ):
                comparison = _comparison(
                    exact, candidate, repeat_exact, repeat_candidate,
                    manifest_sha256, noise, memory, mode,
                )
                comparisons.append(comparison)
                qualifies = (
                    comparison["affected"]
                    and (comparison["time_material"] or comparison["memory_material"])
                    and comparison["regression_gates_passed"]
                    and comparison["repeat_passed"]
                )
                if requested_dpr == 3 and qualifies:
                    candidate_support[mode].add(scenarios[scenario_id]["policy_class"])
            direct = _comparison(
                configured, adaptive, repeat_configured, repeat_adaptive,
                manifest_sha256, noise, memory, "adaptive-vs-configured",
            )
            comparisons.append(direct)
            if (
                requested_dpr == 3 and direct["affected"]
                and (direct["time_material"] or direct["memory_material"])
                and direct["regression_gates_passed"] and direct["repeat_passed"]
            ):
                candidate_support["adaptive-vs-configured"].add(
                    scenarios[scenario_id]["policy_class"]
                )
    configured_qualifies = candidate_support["configured_max"] == POLICY_CLASSES
    adaptive_qualifies = candidate_support["adaptive_simulation"] == POLICY_CLASSES
    if configured_qualifies and adaptive_qualifies:
        disposition = (
            "adaptive-candidate"
            if candidate_support["adaptive-vs-configured"] == POLICY_CLASSES
            else "configured-max-candidate"
        )
    elif adaptive_qualifies:
        disposition = "adaptive-candidate"
    elif configured_qualifies:
        disposition = "configured-max-candidate"
    else:
        disposition = "no-change"
    selected = (
        "adaptive_simulation" if disposition == "adaptive-candidate"
        else "configured_max" if disposition == "configured-max-candidate" else None
    )
    support = candidate_support[selected] if selected else set()
    return {
        "computed": True,
        "comparisons": comparisons,
        "class_support": {name: name in support for name in sorted(POLICY_CLASSES)},
        "disposition": disposition,
        "reasons": [],
    }


def v2_semantic_errors(
    result: dict[str, Any], manifest: dict[str, Any], manifest_sha256: str,
    *, verify_live_publication: bool = False,
) -> list[str]:
    errors: list[str] = []
    if result.get("authority", {}).get("manifest_sha256") != manifest_sha256:
        errors.append("v2 result references a different corpus manifest")
    matrix = result.get("matrix", {})
    if matrix.get("scenario_ids") != [item["id"] for item in manifest["scenarios"]]:
        errors.append("v2 result scenario ordering differs from the manifest")
    if matrix.get("modes") != MODES or matrix.get("requested_dprs") != REQUESTED_DPRS:
        errors.append("v2 result matrix axes differ from the manifest")
    cells = result.get("cells", [])
    repeats = result.get("repeat_cells", [])
    for campaign, values in (("original", cells), ("repeat", repeats)):
        keys = [v2_cell_key(cell) for cell in values]
        if len(keys) != len(set(keys)):
            errors.append(f"v2 {campaign} cell keys are not unique")
        for cell in values:
            errors.extend(_cell_semantic_errors(cell, manifest, manifest_sha256, campaign))
    status = result.get("status")
    analysis = result.get("analysis", {})
    publication = result.get("publication", {})
    gate = result.get("b5_gate", {})
    if status == "inconclusive":
        if analysis.get("computed") is not False or analysis.get("disposition") is not None:
            errors.append("inconclusive v2 result carries computed policy disposition")
        if publication.get("status") != "nonterminal":
            errors.append("inconclusive v2 result claims terminal publication")
        if gate.get("status") != "inconclusive" or gate.get("authorizes_policy_change") is not False:
            errors.append("inconclusive v2 result crosses the inert B5 gate")
        return errors
    if status != "complete":
        errors.append("v2 status must be inconclusive or complete")
        return errors
    authority = result.get("authority", {})
    if (
        result.get("evidence_kind") != "measured"
        or manifest.get("v2_protocol", {}).get("status") != "authorized"
        or authority.get("collection_authorized") is not True
        or not all(authority.get(field) for field in (
            "product_policy_id", "product_policy_blob", "a3_receipt_sha256",
            "timer_noise_p95_ns", "memory_sampler_resolution_bytes",
        ))
    ):
        errors.append("complete v2 result lacks ratified collection authority")
    expected = expected_matrix(manifest)
    if {v2_cell_key(cell) for cell in cells} != expected:
        errors.append("complete v2 result does not contain exactly 84 original cells")
    if {v2_cell_key(cell) for cell in repeats} != expected:
        errors.append("complete v2 result does not contain exactly 84 repeat cells")
    if any(cell.get("identity") != repeats[[v2_cell_key(item) for item in repeats].index(v2_cell_key(cell))].get("identity") for cell in cells) if len(repeats) == 84 else False:
        errors.append("repeat campaign differs from the frozen machine/provider/build identity")
    if not errors:
        derived = compute_v2_analysis(cells, repeats, manifest, manifest_sha256)
        if analysis != derived:
            errors.append("v2 analysis/disposition differs from deterministic recomputation")
    if (
        publication.get("status") != "verified-protected-main"
        or publication.get("protected_main_verified") is not True
        or publication.get("required_checks_green") is not True
        or gate.get("authorizes_policy_change") is not False
    ):
        errors.append("complete v2 result lacks protected-main publication evidence")
    expected_gate = (
        "cancelled-no-change" if analysis.get("disposition") == "no-change"
        else "waiting-trigger"
    )
    if gate.get("status") != expected_gate:
        errors.append("v2 B5 gate differs from the deterministic disposition")
    if verify_live_publication and publication.get("revision"):
        revision = publication["revision"]
        check = subprocess.run(
            ["git", "merge-base", "--is-ancestor", revision, "origin/main"],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        if check.returncode != 0:
            errors.append("publication revision is not on refreshed origin/main")
    return errors


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
    similarity_minimum = manifest.get("trial_contract", {}).get(
        "capture_similarity_minimum"
    )
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
            if (
                statistic["provenance"] != "unavailable"
                and statistic["p95"] < statistic["median"]
            ):
                errors.append(f"{scenario}: p95 is below median")
        similarity = item.get("fidelity", {}).get("capture_similarity")
        if (
            isinstance(similarity, bool)
            or not isinstance(similarity, (int, float))
            or not isinstance(similarity_minimum, (int, float))
            or similarity < similarity_minimum
        ):
            errors.append(f"{scenario}: capture similarity is below the ratified floor")
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
        all_metrics_available = all(
            statistic["provenance"] != "unavailable"
            for item in observations
            for statistic in item["metrics"].values()
        )
        fidelity_passed = all(
            (
                item["fidelity"][gate]
                if gate != "capture_similarity"
                else item["fidelity"][gate] >= similarity_minimum
            )
            for item in observations
            for gate in (
                "content_floor_passed", "capture_similarity", "small_text_legible",
                "thin_strokes_preserved", "logical_input_correct"
            )
        )
        policy_ready = fidelity_passed and all_metrics_available
        if result["eligible_for_policy"] != policy_ready:
            errors.append(
                "policy eligibility disagrees with fidelity gates or metric availability"
            )
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


def planned_v2_result(args: argparse.Namespace, manifest: dict[str, Any]) -> dict[str, Any]:
    digest = canonical_sha256(manifest)
    return {
        "schema": "pulp.gpu-dpr-experiment.v2",
        "version": 2,
        "status": "inconclusive",
        "evidence_kind": "structural-nonterminal",
        "experiment_id": args.experiment_id,
        "plan_revision": args.plan_revision,
        "pulp_sha": args.pulp_sha,
        "forge_sha": args.forge_sha,
        "authority": {
            "manifest_sha256": digest,
            "product_policy_id": None,
            "product_policy_blob": None,
            "a3_receipt_sha256": None,
            "timer_noise_p95_ns": None,
            "memory_sampler_resolution_bytes": None,
            "collection_authorized": False,
        },
        "matrix": {
            "scenario_ids": [item["id"] for item in manifest["scenarios"]],
            "modes": MODES,
            "requested_dprs": REQUESTED_DPRS,
            "cell_count": 84,
            "repeat_cell_count": 84,
        },
        "cells": [],
        "repeat_cells": [],
        "analysis": {
            "computed": False,
            "comparisons": [],
            "class_support": {name: False for name in sorted(POLICY_CLASSES)},
            "disposition": None,
            "reasons": [
                "blocked-product-policy", "blocked-required-coverage",
                "zero-v2-cells-collected", "repeat-campaign-not-collected",
                "protected-main-publication-not-verified",
            ],
        },
        "publication": {
            "status": "nonterminal", "repository": "Generous-Corp/pulp",
            "revision": None,
            "path": "docs/validation/gpu-dpr/terminal-result.json",
            "protected_main_verified": False, "required_checks_green": False,
        },
        "b5_gate": {
            "status": "inconclusive",
            "requires": [
                "terminal-a3-product-policy-and-seven-role-campaign",
                "84-original-and-84-repeat-cells",
                "deterministic-v2-disposition",
                "protected-pulp-main-publication",
            ],
            "authorizes_policy_change": False,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("validate-manifest")
    validate_result = subparsers.add_parser("validate-result")
    validate_result.add_argument("result", type=Path)
    for command in ("emit-plan", "emit-plan-v1"):
        emit = subparsers.add_parser(command)
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
    if args.command in {"emit-plan", "emit-plan-v1"}:
        legacy = args.command == "emit-plan-v1"
        document = planned_result(args, manifest) if legacy else planned_v2_result(args, manifest)
        schema_path = DEFAULT_SCHEMA_V1 if legacy else args.schema
        schema_problems = json_schema_lite.validate(
            document, schema_for_lite(load_json(schema_path))
        )
        if schema_problems:
            print("\n".join(schema_problems), file=sys.stderr)
            return 1
        print(json.dumps(document, sort_keys=True, indent=2))
        return 0

    document = load_json(args.result)
    v2 = document.get("schema") == "pulp.gpu-dpr-experiment.v2"
    schema_path = args.schema if v2 else DEFAULT_SCHEMA_V1
    schema_problems = json_schema_lite.validate(
        document, schema_for_lite(load_json(schema_path))
    )
    semantic_problems = [] if schema_problems else (
        v2_semantic_errors(
            document, manifest, canonical_sha256(manifest),
            verify_live_publication=document.get("status") == "complete",
        ) if v2 else result_semantic_errors(
            document, manifest, canonical_sha256(manifest)
        )
    )
    if schema_problems or semantic_problems:
        print("\n".join([*schema_problems, *semantic_problems]), file=sys.stderr)
        return 1
    classification = "v2" if v2 else V1_CLASSIFICATION
    print(
        f"gpu_dpr_result_valid=true status={document['status']} "
        f"classification={classification}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
