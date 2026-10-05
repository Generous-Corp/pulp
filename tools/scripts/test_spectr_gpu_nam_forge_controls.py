#!/usr/bin/env python3
"""Validate the checked-in Spectr/GPU-NAM Forge metadata example."""

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = ROOT / "docs/examples/spectr-gpu-nam-forge-controls.json"
FORGE_CATALOG = ROOT / "docs/status/forge-catalog.json"
PACED_RECEIPT = ROOT / "docs/status/spectr-gpu-nam-paced-generation-receipt-20261002.json"
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def validate(value: dict) -> None:
    assert value["schema"] == "pulp.spectr-gpu-nam-forge-controls.v1"
    node = value["node"]
    assert node["type_id"] == "spectr.gpu_nam" and node["version"] >= 1
    model = value["model"]
    assert model["model_id"] and "/" in model["model_id"]
    assert model["provider"] and "/" not in model["provider"]
    assert SHA256.fullmatch(model["checkpoint_sha256"])
    assert model["license"] and model["redistributable"] is True
    execution = value["execution"]
    assert execution["latency_samples"] == execution["block_size"]
    if execution["fallback"] == "cpu":
        assert execution.get("fallback_primed") is True
    receipt = value["receipt"]
    assert receipt["status"] == "pass"
    assert receipt["observed_path"] == "gpu"
    assert receipt["provider_identity"]
    assert SHA256.fullmatch(receipt["artifact_sha256"])
    exposure = value["exposure"]
    assert exposure == {
        "status": "metadata_only",
        "catalog_registered": False,
        "named_consumer": None,
    }
    catalog = json.loads(FORGE_CATALOG.read_text())
    assert all(node.get("key") != value["node"]["type_id"] for node in catalog["nodes"])

    # A metadata-only example must fail closed if it claims a catalog exposure.
    invalid_exposure = json.loads(json.dumps(value))
    invalid_exposure["exposure"]["catalog_registered"] = True
    try:
        assert invalid_exposure["exposure"] == exposure
    except AssertionError:
        pass
    else:
        raise AssertionError("metadata-only example accepted catalog registration")

    paced = json.loads(PACED_RECEIPT.read_text())
    assert paced["candidate_consumer"] == "magenta-rt2"
    assert paced["status"] == "blocked"
    assert paced["measurements_claimed"] is False
    assert paced["forge_exposure"] == exposure
    assert paced["local_probe"]["status"] == "unavailable"
    assert paced["local_probe"]["rechecked_parent_head"]
    assert paced["local_probe"]["closure"] == "closed_without_catalog_promotion"
    assert "/Users/danielraffel/.pulp/magenta" in paced["local_probe"]["checked_paths"]
    assert paced["local_probe"]["forge_catalog_rows_for_spectr_gpu_nam"] == 0
    assert any("generated mrt2 fixture" in finding for finding in paced["local_probe"]["findings"])
    assert paced["minimum_next_experiment"]["paced_blocks"] == 100000
    assert paced["minimum_next_experiment"]["named_model_required"] is True
    assert paced["minimum_next_experiment"]["named_apple_silicon_host_required"] is True

    # A pass receipt without reproducible artifact evidence must fail closed.
    invalid = json.loads(json.dumps(value))
    del invalid["receipt"]["artifact_sha256"]
    try:
        _validate_pass_receipt(invalid)
    except AssertionError:
        pass
    else:
        raise AssertionError("pass receipt without artifact hash was accepted")


def _validate_pass_receipt(value: dict) -> None:
    receipt = value["receipt"]
    assert receipt["status"] != "pass" or SHA256.fullmatch(receipt.get("artifact_sha256", ""))


if __name__ == "__main__":
    validate(json.loads(EXAMPLE.read_text()))
    print(f"validated {EXAMPLE.relative_to(ROOT)}")
