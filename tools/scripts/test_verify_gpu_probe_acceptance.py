#!/usr/bin/env python3
"""Mutation tests for verify_gpu_probe_acceptance.py."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


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

    def test_checked_in_historical_receipt_integrity_passes(self) -> None:
        head = MODULE._git_blobs("HEAD", MODULE.EXPECTED_SOURCE_BLOBS)
        with mock.patch.object(MODULE, "_checkout_blobs", return_value=head):
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

    def test_renderer_negative_dimension_drift_fails_closed(self) -> None:
        mutations = [
            {"width": 128, "height": 128, "work_items": 16_384},
            {"width": 31, "height": 32, "work_items": 992},
        ]
        for dimensions in mutations:
            with self.subTest(dimensions=dimensions), tempfile.TemporaryDirectory() as temporary:
                copied = Path(temporary) / "receipt"
                shutil.copytree(FIXTURE, copied)
                name = "renderer-negative.json"
                path = copied / name
                result = json.loads(path.read_text())
                result["dimensions"] = dimensions
                path.write_text(json.dumps(result))
                self._rebind(copied, name)
                errors = MODULE.verify(copied)
                self.assertTrue(any("execution dimensions are not recipe-bound" in error
                                    for error in errors))

    def test_renderer_rgba_size_drift_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            name = "renderer-negative.json"
            path = copied / name
            result = json.loads(path.read_text())
            rgba = next(artifact for artifact in result["artifacts"]
                        if artifact["name"] == "observed.rgba8")
            rgba["bytes"] -= 4
            path.write_text(json.dumps(result))
            self._rebind(copied, name)
            errors = MODULE.verify(copied)
            self.assertTrue(any("RGBA artifact does not match declared dimensions" in error
                                for error in errors))

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

    def test_digest_rebound_mcp_positive_cannot_report_is_error(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            path = copied / "mcp-transcript.jsonl"
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            rows[1]["result"]["isError"] = True
            path.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
            self._rebind(copied, "mcp-transcript.jsonl")
            errors = MODULE.verify(copied)
            self.assertTrue(any("preserve isError=false" in error for error in errors))

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

    def test_missing_source_or_binary_binding_fails_closed(self) -> None:
        mutations = [
            (
                "source_blobs",
                "tools/cli/gpu_probe/src/native_recipes.cpp",
                "exact recipe source set",
            ),
            ("binaries", "installed_cpp_delegate", "exact installed executable set"),
            ("binaries", "installed_mcp", "exact installed executable set"),
        ]
        for section, key, expected_error in mutations:
            with self.subTest(section=section, key=key), tempfile.TemporaryDirectory() as temporary:
                copied = Path(temporary) / "receipt"
                shutil.copytree(FIXTURE, copied)
                receipt_path = copied / "receipt.json"
                receipt = json.loads(receipt_path.read_text())
                del receipt[section][key]
                receipt_path.write_text(json.dumps(receipt))
                errors = MODULE.verify(copied)
                self.assertTrue(any(expected_error in error for error in errors))

    def test_replaced_source_blob_or_integration_head_fails_closed(self) -> None:
        mutations = [
            ("source_blob", "0" * 40, "source blob mismatch"),
            ("integration_head", "0" * 40, "changed its recorded integration_head"),
        ]
        for name, replacement, expected in mutations:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                copied = Path(temporary) / "receipt"
                shutil.copytree(FIXTURE, copied)
                receipt_path = copied / "receipt.json"
                receipt = json.loads(receipt_path.read_text())
                if name == "source_blob":
                    receipt["source_blobs"][
                        "tools/cli/gpu_probe/src/native_recipes.cpp"
                    ] = replacement
                else:
                    receipt["integration_head"] = replacement
                receipt_path.write_text(json.dumps(receipt))
                errors = MODULE.verify(copied)
                self.assertTrue(any(expected in error for error in errors))

    @staticmethod
    def _promote_copy_to_v2(copied: Path) -> None:
        receipt_path = copied / "receipt.json"
        receipt = json.loads(receipt_path.read_text())
        receipt["schema"] = "pulp.gpu-probe-acceptance-receipt.v2"
        receipt["integration_head"] = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        receipt["source_blobs"] = MODULE._git_blobs(
            "HEAD", MODULE.EXPECTED_SOURCE_BLOBS_V2
        )
        receipt_path.write_text(json.dumps(receipt))

    def test_historical_v1_ignores_current_head_source_drift(self) -> None:
        target = "tools/cli/gpu_probe/src/native_recipes.cpp"
        historical = MODULE._git_blobs(
            json.loads((FIXTURE / "receipt.json").read_text())["integration_head"],
            MODULE.EXPECTED_SOURCE_BLOBS,
        ).get(target)
        self.assertIsNotNone(historical)
        original = MODULE._git_blobs

        def drifted_head(commit: str, paths: set[str]) -> dict[str, str]:
            blobs = original(commit, paths)
            if commit == "HEAD":
                blobs[target] = "f" * 40
            return blobs

        with mock.patch.object(MODULE, "_git_blobs", side_effect=drifted_head):
            errors = MODULE.verify(FIXTURE)
        self.assertFalse(any(f"current HEAD source blob drift for {target}" in error for error in errors))
        self.assertFalse(any(f"source blob mismatch for {target}" in error for error in errors))

    def test_v2_current_head_source_drift_fails_closed(self) -> None:
        target = "tools/cli/gpu_probe/src/native_recipes.cpp"
        original = MODULE._git_blobs

        def drifted_head(commit: str, paths: set[str]) -> dict[str, str]:
            blobs = original(commit, paths)
            if commit == "HEAD":
                blobs[target] = "f" * 40
            return blobs

        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            self._promote_copy_to_v2(copied)
            with mock.patch.object(MODULE, "_git_blobs", side_effect=drifted_head):
                errors = MODULE.verify(copied)
        self.assertTrue(any(f"current HEAD source blob drift for {target}" in error for error in errors))

    def test_v2_unresolvable_current_head_blob_fails_closed(self) -> None:
        target = "tools/cli/gpu_probe/src/probe_result_json.cpp"
        original = MODULE._git_blobs

        def missing_head(commit: str, paths: set[str]) -> dict[str, str]:
            blobs = original(commit, paths)
            if commit == "HEAD":
                blobs.pop(target, None)
            return blobs

        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            self._promote_copy_to_v2(copied)
            with mock.patch.object(MODULE, "_git_blobs", side_effect=missing_head):
                errors = MODULE.verify(copied)
        self.assertTrue(any(f"cannot resolve {target} at current HEAD" in error for error in errors))

    def test_historical_v1_ignores_current_checkout_source_drift(self) -> None:
        target = "tools/cli/gpu_probe/src/native_acceptance.cpp"
        original = MODULE._checkout_blobs

        def drifted_checkout(paths: set[str]) -> dict[str, str]:
            blobs = original(paths)
            blobs[target] = "e" * 40
            return blobs

        with mock.patch.object(MODULE, "_checkout_blobs", side_effect=drifted_checkout):
            errors = MODULE.verify(FIXTURE)
        self.assertFalse(any(f"current checkout source blob drift for {target}" in error for error in errors))

    def test_v2_current_checkout_source_drift_fails_closed(self) -> None:
        target = "tools/cli/gpu_probe/src/native_acceptance.cpp"
        original = MODULE._checkout_blobs

        def drifted_checkout(paths: set[str]) -> dict[str, str]:
            blobs = original(paths)
            blobs[target] = "e" * 40
            return blobs

        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            self._promote_copy_to_v2(copied)
            with mock.patch.object(MODULE, "_checkout_blobs", side_effect=drifted_checkout):
                errors = MODULE.verify(copied)
        self.assertTrue(any(f"current checkout source blob drift for {target}" in error for error in errors))

    def test_non_object_binding_sections_fail_closed_without_crashing(self) -> None:
        for section in ["source_blobs", "binaries", "raw_sha256", "run_groups"]:
            with self.subTest(section=section), tempfile.TemporaryDirectory() as temporary:
                copied = Path(temporary) / "receipt"
                shutil.copytree(FIXTURE, copied)
                receipt_path = copied / "receipt.json"
                receipt = json.loads(receipt_path.read_text())
                receipt[section] = []
                receipt_path.write_text(json.dumps(receipt))
                errors = MODULE.verify(copied)
                self.assertTrue(any(f"{section} must be an object" in error for error in errors))

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

    def test_reused_gpu_evidence_id_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "receipt"
            shutil.copytree(FIXTURE, copied)
            first = json.loads((copied / "compute-run1.json").read_text())
            second_path = copied / "stft-run1.json"
            second = json.loads(second_path.read_text())
            second["gpu_evidence_id"] = first["gpu_evidence_id"]
            second_path.write_text(json.dumps(second))
            self._rebind(copied, "stft-run1.json")
            errors = MODULE.verify(copied)
            self.assertTrue(any("gpu_evidence_id was reused" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
