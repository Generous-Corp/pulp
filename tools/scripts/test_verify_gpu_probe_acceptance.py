#!/usr/bin/env python3
"""Mutation tests for verify_gpu_probe_acceptance.py."""

from __future__ import annotations

import hashlib
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
    @staticmethod
    def _rebind(copied: Path, name: str) -> None:
        receipt_path = copied / "receipt.json"
        receipt = json.loads(receipt_path.read_text())
        receipt["raw_sha256"][name] = hashlib.sha256(
            (copied / name).read_bytes()
        ).hexdigest()
        receipt_path.write_text(json.dumps(receipt))

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

    def test_digest_rebound_schema_violation_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            name = "compute-run1.json"
            path = copied / name
            result = json.loads(path.read_text())
            result["passes"][0]["undeclared"] = True
            path.write_text(json.dumps(result))
            self._rebind(copied, name)
            errors = MODULE.verify(copied)
            self.assertTrue(any(f"{name}: schema:" in error for error in errors))

    def test_digest_rebound_semantic_pass_failure_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            name = "compute-run1.json"
            path = copied / name
            result = json.loads(path.read_text())
            result["passes"][0]["verdict"] = "fail"
            path.write_text(json.dumps(result))
            self._rebind(copied, name)
            errors = MODULE.verify(copied)
            self.assertTrue(any("non-passing semantic pass" in error for error in errors))

    def test_digest_rebound_numeric_sample_drift_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            name = "compute-run1.json"
            path = copied / name
            result = json.loads(path.read_text())
            result["numeric_sample_count"] = 0
            path.write_text(json.dumps(result))
            self._rebind(copied, name)
            errors = MODULE.verify(copied)
            self.assertTrue(any("numeric sample count changed" in error for error in errors))

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

    def test_digest_rebound_role_swap_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            receipt_path = copied / "receipt.json"
            receipt = json.loads(receipt_path.read_text())
            receipt["run_groups"]["compute"]["binary_role"] = "scene3d_cpp_cli"
            receipt_path.write_text(json.dumps(receipt))
            errors = MODULE.verify(copied)
            self.assertTrue(any("wrong executable role" in error for error in errors))

    def test_digest_rebound_unrelated_mcp_evidence_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            transcript_path = copied / "mcp-transcript.jsonl"
            rows = [json.loads(line) for line in transcript_path.read_text().splitlines()]
            evidence = rows[1]["result"]["structuredContent"]["evidence"]
            evidence["numeric_sample_count"] = 255
            rows[1]["result"]["content"][0]["text"] = json.dumps(evidence)
            transcript_path.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
            self._rebind(copied, "mcp-transcript.jsonl")
            errors = MODULE.verify(copied)
            self.assertTrue(any("not the installed CLI recipe result" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
