#!/usr/bin/env python3
"""Negative controls for the checked-in A4 inconclusive receipt verifier."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "tools" / "scripts" / "verify_gpu_dpr_inconclusive_receipt.py"
FIXTURE = ROOT / "docs" / "validation" / "gpu-dpr" / "m3-a4-inconclusive-20260828"


def run(path: Path, expected: int, diagnostic: str | None = None) -> None:
    completed = subprocess.run(
        [sys.executable, str(VERIFIER), str(path)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    assert completed.returncode == expected, completed.stdout
    if diagnostic is not None:
        assert diagnostic in completed.stdout, completed.stdout


def rewrite_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def rebind(directory: Path, name: str) -> None:
    receipt_path = directory / "receipt.json"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
    next(item for item in receipt["artifacts"] if item["path"] == name)["sha256"] = digest
    rewrite_json(receipt_path, receipt)


def main() -> int:
    run(FIXTURE, 0)
    with tempfile.TemporaryDirectory(prefix="pulp-gpu-dpr-receipt-") as temporary:
        planted = Path(temporary) / "receipt"
        shutil.copytree(FIXTURE, planted)
        result_path = planted / "result.json"
        result = json.loads(result_path.read_text(encoding="utf-8"))
        result["disposition"] = "configured-max-candidate"
        rewrite_json(result_path, result)
        rebind(planted, "result.json")
        run(planted, 1, "result must not carry a disposition")

        shutil.rmtree(planted)
        shutil.copytree(FIXTURE, planted)
        gap_path = planted / "gap-report.json"
        gap = json.loads(gap_path.read_text(encoding="utf-8"))
        gap["result"]["measured_complete_cells"] = 36
        rewrite_json(gap_path, gap)
        rebind(planted, "gap-report.json")
        run(planted, 1, "gap must preserve zero measured cells")

        shutil.rmtree(planted)
        shutil.copytree(FIXTURE, planted)
        capture_path = planted / "capture-index.json"
        capture = json.loads(capture_path.read_text(encoding="utf-8"))
        capture["capture_count"] = 35
        rewrite_json(capture_path, capture)
        rebind(planted, "capture-index.json")
        run(planted, 1, "capture index count differs")

        (planted / "README.md").write_text("/Users/private/evidence\n", encoding="utf-8")
        run(planted, 1, "private path marker")
    print("gpu_dpr_inconclusive_receipt_selftest=true digest_binding=pass semantic_mutations=3 private_path=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
