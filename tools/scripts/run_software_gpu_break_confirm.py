#!/usr/bin/env python3
"""Run the advisory software-GPU render proof and its seeded break-confirm.

The native recipe deliberately returns exit 2 for an unverified portable
golden. That is different from an unavailable adapter, so this wrapper checks
the typed receipt rather than treating a process exit as a verdict. A healthy
software lane must produce a non-empty PNG receipt, then detect the recipe's
pre-submit framebuffer mutation with a fail verdict and exit 1.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

RECIPE = "renderer3d.hardcoded-cube.v1"
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def _run(binary: Path, output_dir: Path, negative: bool) -> tuple[int, dict[str, Any]]:
    output_dir.mkdir(parents=True, exist_ok=True)
    args = [str(binary), "gpu", "probe", "--recipe", RECIPE,
            "--artifacts", str(output_dir), "--json"]
    if negative:
        args.append("--negative-control")
    completed = subprocess.run(args, text=True, capture_output=True,
                               env={**os.environ, "PULP_GPU_SOFTWARE_ADAPTER": "1"})
    (output_dir / "stdout.txt").write_text(completed.stdout, encoding="utf-8")
    (output_dir / "stderr.txt").write_text(completed.stderr, encoding="utf-8")
    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"{('negative' if negative else 'baseline')} probe did not emit JSON: {exc}"
        ) from exc
    if not isinstance(document, dict):
        raise RuntimeError("GPU probe JSON root is not an object")
    return completed.returncode, document


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def _check_baseline(document: dict[str, Any], output_dir: Path,
                    platform: str) -> dict[str, Any]:
    adapter = document.get("adapter", {})
    _require(document.get("schema") == "pulp.gpu-probe-result.v1",
             "baseline schema is not gpu-probe-result.v1")
    _require(document.get("verdict") in {"pass", "unverified"},
             f"baseline verdict is {document.get('verdict')!r}")
    _require(adapter.get("status") == "authentic" and adapter.get("class") == "software",
             f"baseline adapter is not authentic software: {adapter}")
    expected_backend = {"linux": "Vulkan", "windows": "D3D12"}[platform]
    _require(adapter.get("backend") == expected_backend,
             f"baseline backend is {adapter.get('backend')!r}; expected {expected_backend}")
    _require(document.get("dimensions") == {"width": 128, "height": 128,
                                             "work_items": 16384},
             "baseline dimensions do not match the canonical render recipe")
    artifacts = document.get("artifacts", [])
    png = next((item for item in artifacts if item.get("name") == "final.png"), None)
    _require(isinstance(png, dict) and png.get("bytes", 0) > 0,
             "baseline did not publish a non-empty final.png artifact")
    png_path = output_dir / "final.png"
    _require(png_path.is_file() and png_path.stat().st_size > 0,
             "baseline final.png payload is missing or empty")
    digest = hashlib.sha256(png_path.read_bytes()).hexdigest()
    _require(SHA256.fullmatch(digest) and digest == png.get("sha256"),
             "baseline final.png sha256 receipt does not match its payload")
    return {"adapter": adapter, "verdict": document["verdict"],
            "png_bytes": png_path.stat().st_size, "png_sha256": digest}


def _check_negative(document: dict[str, Any]) -> dict[str, Any]:
    _require(document.get("schema") == "pulp.gpu-probe-result.v1",
             "negative schema is not gpu-probe-result.v1")
    _require(document.get("verdict") == "fail",
             f"negative control verdict is {document.get('verdict')!r}; draw break was not detected")
    _require(document.get("mutation") == "pre-submit-framebuffer-downscale",
             "negative control does not identify the canonical draw mutation")
    passes = document.get("passes", [])
    _require(passes and passes[-1].get("code") == "portable_structure_mismatch",
             "negative control did not fail at the portable content boundary")
    return {"verdict": document["verdict"], "mutation": document["mutation"],
            "failing_pass": passes[-1]["code"]}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    args = parser.parse_args()
    if not args.binary.is_file():
        parser.error(f"GPU probe binary does not exist: {args.binary}")
    args.artifacts.mkdir(parents=True, exist_ok=True)
    try:
        baseline_rc, baseline = _run(args.binary, args.artifacts / "baseline", False)
        negative_rc, negative = _run(args.binary, args.artifacts / "negative", True)
        baseline_receipt = _check_baseline(
            baseline, args.artifacts / "baseline", args.platform)
        _require(baseline_rc in {0, 2}, f"baseline exited unexpectedly: {baseline_rc}")
        _require(negative_rc == 1, f"negative control exited unexpectedly: {negative_rc}")
        negative_receipt = _check_negative(negative)
        receipt = {"schema": "pulp.software-gpu-break-confirm.v1",
                   "recipe": RECIPE, "platform": args.platform,
                   "baseline": baseline_receipt, "negative_control": negative_receipt}
        (args.artifacts / "receipt.json").write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(receipt, sort_keys=True))
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"software-gpu-break-confirm: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
