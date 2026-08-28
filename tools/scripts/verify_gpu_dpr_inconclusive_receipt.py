#!/usr/bin/env python3
"""Verify the compact A4 DPR inconclusive publication and its byte bindings."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


RECEIPT_SCHEMA = "pulp.gpu-dpr-a4-publication-receipt.v1"
RESULT_SCHEMA = "pulp.gpu-dpr-experiment.v1"
GAP_SCHEMA = "pulp.gpu-dpr-a4-gap-report.v1"
EXPECTED_ROLES = {
    "capture-index.json": "preflight-only-cell-to-capture-digest-index",
    "gap-report.json": "exact-machine-readable-gap-and-minimal-adapter-handoff",
    "result.json": "exact-runner-projected-incomplete-result",
}
FORBIDDEN_PRIVATE_MARKERS = (b"/Users/", b"/Volumes/", b"agent-worktrees")


class ReceiptError(ValueError):
    """The publication cannot prove its declared incomplete boundary."""


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ReceiptError(f"expected JSON object: {path.name}")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ReceiptError(message)


def rendered_dpr(value: float) -> str:
    return str(int(value)) if float(value).is_integer() else str(value)


def verify(directory: Path) -> None:
    require(directory.is_dir(), f"receipt directory is missing: {directory}")
    paths = {
        name: directory / name
        for name in (
            "README.md", "receipt.json", "capture-index.json",
            "gap-report.json", "result.json",
        )
    }
    for name, path in paths.items():
        require(path.is_file(), f"publication artifact is missing: {name}")
        payload = path.read_bytes()
        require(
            not any(marker in payload for marker in FORBIDDEN_PRIVATE_MARKERS),
            f"publication artifact contains a private path marker: {name}",
        )

    receipt = load_json(paths["receipt.json"])
    require(receipt.get("schema") == RECEIPT_SCHEMA, "receipt schema differs")
    require(receipt.get("version") == 1, "receipt version differs")
    require(receipt.get("status") == "inconclusive", "receipt must remain inconclusive")
    require(receipt.get("disposition") is None, "receipt must not carry a disposition")
    require(receipt.get("b5_status") == "waiting-trigger", "B5 must remain waiting-trigger")
    artifacts = receipt.get("artifacts")
    require(isinstance(artifacts, list), "receipt artifacts are missing")
    by_path = {
        item.get("path"): item
        for item in artifacts
        if isinstance(item, dict) and isinstance(item.get("path"), str)
    }
    require(set(by_path) == set(EXPECTED_ROLES), "receipt artifact set differs")
    for name, role in EXPECTED_ROLES.items():
        item = by_path[name]
        require(item.get("role") == role, f"artifact role differs: {name}")
        require(item.get("sha256") == sha256(paths[name]), f"artifact digest differs: {name}")
    external = receipt.get("external_corpus")
    require(isinstance(external, dict), "external corpus boundary is missing")
    require(external.get("checked_in") is False, "external corpus must not claim checked-in bytes")
    require(external.get("release_evidence") is False, "external corpus must not claim release evidence")
    require(external.get("capture_count") == 36, "external capture count differs")

    capture_index = load_json(paths["capture-index.json"])
    require(capture_index.get("schema") == "pulp.gpu-dpr-a4-capture-index.v1", "capture index schema differs")
    require(capture_index.get("status") == "preflight-only-non-release-evidence", "capture index status differs")
    require(capture_index.get("capture_count") == 36, "capture index count differs")
    indexed = capture_index.get("captures")
    require(isinstance(indexed, list) and len(indexed) == 12, "capture index groups differ")
    cell_keys: list[str] = []
    for item in indexed:
        require(isinstance(item, dict), "capture index group is malformed")
        digest = item.get("capture_sha256")
        require(
            isinstance(digest, str) and len(digest) == 64
            and all(character in "0123456789abcdef" for character in digest),
            "capture index digest is malformed",
        )
        keys = item.get("cell_keys")
        require(isinstance(keys, list) and keys, "capture index cell keys are missing")
        observed_dpr = item.get("observed_dpr")
        require(isinstance(observed_dpr, (int, float)) and observed_dpr > 0, "observed DPR is malformed")
        expected_size = {"width": round(640 * observed_dpr), "height": round(360 * observed_dpr)}
        require(item.get("physical_size") == expected_size, "capture index physical size differs")
        for key in keys:
            require(isinstance(key, str), "capture index cell key is malformed")
            scenario, mode, dpr_token = key.rsplit("__", 2)
            require(scenario == item.get("scenario_id"), "capture index scenario differs from cell key")
            requested_dpr = float(dpr_token.removeprefix("dpr-"))
            expected_dpr = min(requested_dpr, 2.0) if mode == "configured_max" else requested_dpr
            require(expected_dpr == observed_dpr, "capture index observed DPR differs from cell key")
        cell_keys.extend(keys)
    require(len(cell_keys) == 36 and len(set(cell_keys)) == 36, "capture index cell coverage differs")

    result = load_json(paths["result.json"])
    require(result.get("schema") == RESULT_SCHEMA, "result schema differs")
    require(result.get("version") == 1, "result version differs")
    require(result.get("status") == "incomplete", "result must remain incomplete")
    require(result.get("disposition") is None, "result must not carry a disposition")
    require(result.get("eligible_for_policy") is False, "result must not be policy-eligible")
    require(result.get("observations") == [], "incomplete publication must have no measured observations")
    dependencies = result.get("dependencies")
    require(isinstance(dependencies, dict), "result dependencies are missing")
    require(
        set(dependencies) == {"a2t_receipt", "a3_budget_id", "a3_receipt"}
        and all(value is None for value in dependencies.values()),
        "result must preserve missing A2T/A3 prerequisites",
    )
    matrix = result.get("matrix")
    require(isinstance(matrix, dict), "result matrix is missing")
    require(len(matrix.get("scenario_ids", [])) == 7, "result scenario count differs")
    require(len(matrix.get("modes", [])) == 3, "result mode count differs")
    require(len(matrix.get("requested_dprs", [])) == 4, "result DPR count differs")
    expected_native_cells = {
        f"{scenario}__{mode}__dpr-{rendered_dpr(dpr)}"
        for scenario in matrix["scenario_ids"][:3]
        for mode in matrix["modes"]
        for dpr in matrix["requested_dprs"]
    }
    require(set(cell_keys) == expected_native_cells, "capture index does not cover the native matrix")
    require(capture_index.get("experiment_id") == result.get("experiment_id"), "capture experiment identity differs")
    require(capture_index.get("pulp_sha") == result.get("pulp_sha"), "capture Pulp identity differs")

    gap = load_json(paths["gap-report.json"])
    require(gap.get("schema") == GAP_SCHEMA, "gap-report schema differs")
    require(gap.get("experiment_id") == result.get("experiment_id"), "experiment identities differ")
    require(
        capture_index.get("binary_sha256") == gap.get("identity", {}).get("pulp_screenshot_sha256"),
        "capture binary identity differs",
    )
    summary = gap.get("result")
    require(isinstance(summary, dict), "gap result summary is missing")
    require(summary.get("status") == "incomplete", "gap result must remain incomplete")
    require(summary.get("disposition") is None, "gap result must not carry a disposition")
    require(summary.get("eligible_for_policy") is False, "gap result must not be policy-eligible")
    require(summary.get("total_cells") == 84, "gap total-cell count differs")
    require(summary.get("measured_complete_cells") == 0, "gap must preserve zero measured cells")
    require(summary.get("real_capture_preflights") == 36, "gap capture count differs")
    require(summary.get("product_cells_without_exact_runnable_artifact") == 48, "gap product-cell count differs")
    followup = gap.get("smallest_followup_by_scenario_class")
    require(isinstance(followup, dict), "scenario followup handoff is missing")
    require(
        set(followup) == {"pulp_native", "threejs_native", "forge_native_and_daw", "web_canary"},
        "scenario followup classes differ",
    )
    require(sum(item.get("cell_count", -1) for item in followup.values()) == 84, "followup cell counts differ")
    require(len(gap.get("ratified_measured_cell_contract_gaps", [])) >= 7, "measured contract gap list is incomplete")
    b5 = gap.get("b5")
    require(isinstance(b5, dict), "B5 boundary is missing")
    require(b5.get("status") == "waiting-trigger", "gap B5 must remain waiting-trigger")
    require(b5.get("trigger") == "B0-adopted-vellum-api-refresh", "gap B5 trigger differs")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        verify(args.directory.resolve())
    except (OSError, json.JSONDecodeError, ReceiptError, KeyError, TypeError) as error:
        print(f"gpu-dpr-inconclusive-receipt: FAIL: {error}")
        return 1
    print("gpu_dpr_inconclusive_receipt=true status=incomplete measured_cells=0 captures=36 b5=waiting-trigger")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
