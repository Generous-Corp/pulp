#!/usr/bin/env python3
"""Tests for evidence index core helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("evidence_index_core.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_evidence_index_core")


class EvidenceIndexCoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_evidence_record_carries_normalized_provenance(self) -> None:
        record = self.mod.evidence_record_from_result(
            {
                "job_id": "job123",
                "branch": "feature/evidence",
                "sha": "c" * 40,
                "validation": "full",
                "completed_at": "2026-04-04T12:00:00+00:00",
                "provenance": {
                    "execution_kind": "hosted",
                    "hosted_orchestrator": "github-actions",
                    "runner_provider": "namespace",
                    "runner_selector": "mac-arm64",
                    "run_id": "12345",
                    "run_url": "https://example.test/runs/12345",
                },
            },
            {"target": "mac", "status": "pass", "duration_secs": 12},
            Path("/tmp/result.json"),
        )

        self.assertEqual(record["provenance"]["hosted_orchestrator"], "github-actions")
        self.assertEqual(record["provenance"]["runner_provider"], "namespace")
        self.assertEqual(record["provenance"]["runner_selector"], "mac-arm64")

    def test_merge_result_keeps_latest_passing_target(self) -> None:
        index = self.mod.empty_evidence_index()
        older = {
            "job_id": "old",
            "branch": "feature/evidence",
            "sha": "1" * 40,
            "validation": "full",
            "completed_at": "2026-04-01T00:10:00+00:00",
            "results": [{"target": "mac", "status": "pass", "duration_secs": 8}],
        }
        newer = {**older, "job_id": "new", "completed_at": "2026-04-01T00:20:00+00:00"}

        self.assertTrue(self.mod.merge_result_into_evidence_index(index, older, Path("/tmp/old.json")))
        self.assertTrue(self.mod.merge_result_into_evidence_index(index, newer, Path("/tmp/new.json")))

        key = self.mod.evidence_entry_key("feature/evidence", "1" * 40, "mac", "full")
        self.assertEqual(index["entries"][key]["job_id"], "new")


if __name__ == "__main__":
    unittest.main()
