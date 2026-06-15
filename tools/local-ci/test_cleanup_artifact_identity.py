#!/usr/bin/env python3
"""Tests for cleanup artifact identity helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cleanup_artifact_identity.py", module_name="pulp_cleanup_artifact_identity")


class CleanupArtifactIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_result_file_job_id_extracts_stable_component(self) -> None:
        self.assertEqual(self.mod.result_file_job_id(Path("20260404-120000-job123-feature.json")), "job123")
        self.assertIsNone(self.mod.result_file_job_id(Path("not-json.txt")))
        self.assertIsNone(self.mod.result_file_job_id(Path("too-short.json")))

    def test_artifact_entry_sort_key_uses_mtime_and_path(self) -> None:
        self.assertEqual(
            self.mod.artifact_entry_sort_key({"mtime": "4.5", "path": Path("artifact")}),
            (4.5, "artifact"),
        )


if __name__ == "__main__":
    unittest.main()
