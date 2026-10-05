#!/usr/bin/env python3
"""Fail-closed audit for the Spectr/Forge/Magenta metadata lane."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RECEIPT = ROOT / "docs/status/spectr-gpu-nam-paced-generation-receipt-20261002.json"
CATALOG = ROOT / "docs/status/forge-catalog.json"
EXECUTION = ROOT / "docs/reports/neural-mlx-execution-receipt-20261002.md"
STATUS = ROOT / "docs/status/neural-audio-program-status-20261002.md"


def main() -> None:
    receipt = json.loads(RECEIPT.read_text())
    assert receipt["status"] == "blocked"
    assert receipt["measurements_claimed"] is False
    assert receipt["forge_exposure"] == {
        "status": "metadata_only",
        "catalog_registered": False,
        "named_consumer": None,
    }
    audit = receipt["metadata_audit"]
    assert audit["origin_main"] == "e41946bedea98001f68bcc8712aedaa4dd5b7075"
    assert audit["magenta_artifact_available"] is False
    assert audit["forge_catalog_rows"] == 0
    assert audit["sustained_magenta_generation_claimed"] is False
    assert audit["synthetic_mlx_100000_blocks_is_product_evidence"] is False
    catalog = json.loads(CATALOG.read_text())
    assert not any(n.get("key") == "spectr.gpu_nam" for n in catalog["nodes"])
    execution = EXECUTION.read_text()
    status = STATUS.read_text()
    assert "gate not claimed" in execution
    assert "metadata-only closure" in status
    print("Spectr/Forge/Magenta metadata audit passed")


if __name__ == "__main__":
    main()
