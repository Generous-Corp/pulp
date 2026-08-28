#!/usr/bin/env python3
"""Mutation tests for verify_gpu_probe_acceptance.py."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = Path(__file__).with_name("verify_gpu_probe_acceptance.py")
SPEC = importlib.util.spec_from_file_location("verify_gpu_probe_acceptance", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
FIXTURE = ROOT / "docs/validation/gpu-probes/m3-a2-real-probes-20260828"


class VerifyGpuProbeAcceptanceTest(unittest.TestCase):
    def test_checked_in_receipt_passes(self) -> None:
        self.assertEqual(MODULE.verify(FIXTURE), [])

    def test_mutated_positive_result_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            path = copied / "compute-run1.json"
            result = json.loads(path.read_text())
            result["verdict"] = "fail"
            path.write_text(json.dumps(result))
            errors = MODULE.verify(copied)
            self.assertTrue(any("digest mismatch" in error for error in errors))
            self.assertTrue(any("both positive runs must pass" in error for error in errors))

    def test_mcp_failure_projection_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            path = copied / "mcp-transcript.jsonl"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[2]["result"]["isError"] = False
            path.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
            errors = MODULE.verify(copied)
            self.assertTrue(any("digest mismatch" in error for error in errors))
            self.assertTrue(any("isError=true" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
