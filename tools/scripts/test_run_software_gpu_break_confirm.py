#!/usr/bin/env python3
"""Focused contract tests for the software-GPU break-confirm wrapper."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "run_software_gpu_break_confirm", ROOT / "run_software_gpu_break_confirm.py"
)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SoftwareGpuBreakConfirmTests(unittest.TestCase):
    def test_baseline_requires_authentic_software_and_png_digest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            payload = b"PNG fixture"
            (directory / "final.png").write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            document = {
                "schema": "pulp.gpu-probe-result.v1",
                "verdict": "unverified",
                "adapter": {"status": "authentic", "class": "software", "backend": "Vulkan"},
                "dimensions": {"width": 128, "height": 128, "work_items": 16384},
                "artifacts": [{"name": "final.png", "bytes": len(payload), "sha256": digest}],
            }
            receipt = MODULE._check_baseline(document, directory, "linux")
            self.assertEqual(receipt["png_sha256"], digest)

    def test_baseline_rejects_wrong_backend(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            payload = b"PNG fixture"
            (directory / "final.png").write_bytes(payload)
            document = {
                "schema": "pulp.gpu-probe-result.v1",
                "verdict": "unverified",
                "adapter": {"status": "authentic", "class": "software", "backend": "D3D12"},
                "dimensions": {"width": 128, "height": 128, "work_items": 16384},
                "artifacts": [{
                    "name": "final.png", "bytes": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }],
            }
            with self.assertRaises(RuntimeError):
                MODULE._check_baseline(document, directory, "linux")

    def test_negative_requires_seeded_content_failure(self) -> None:
        document = {
            "schema": "pulp.gpu-probe-result.v1",
            "verdict": "fail",
            "mutation": "pre-submit-framebuffer-downscale",
            "passes": [{"code": "portable_structure_mismatch"}],
        }
        receipt = MODULE._check_negative(document)
        self.assertEqual(receipt["failing_pass"], "portable_structure_mismatch")

    def test_negative_pass_is_rejected(self) -> None:
        document = {
            "schema": "pulp.gpu-probe-result.v1",
            "verdict": "pass",
            "mutation": "pre-submit-framebuffer-downscale",
            "passes": [{"code": "portable_structure_mismatch"}],
        }
        with self.assertRaises(RuntimeError):
            MODULE._check_negative(document)


if __name__ == "__main__":
    unittest.main()
