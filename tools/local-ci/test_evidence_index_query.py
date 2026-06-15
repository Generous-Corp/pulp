#!/usr/bin/env python3
"""Tests for evidence index grouping helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module


def load_core_module():
    return load_local_ci_module("evidence_index_core.py", module_name="pulp_evidence_index_core_for_query", add_module_dir=True)


def load_module():
    return load_local_ci_module("evidence_index_query.py", module_name="pulp_evidence_index_query", add_module_dir=True)


class EvidenceIndexQueryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.core = load_core_module()
        self.mod = load_module()

    def test_collect_groups_is_branch_scoped_for_shared_sha(self) -> None:
        shared_sha = "4" * 40
        index = self.core.empty_evidence_index()
        self.core.merge_result_into_evidence_index(
            index,
            {
                "job_id": "job401",
                "branch": "feature/alpha",
                "sha": shared_sha,
                "validation": "full",
                "completed_at": "2026-04-01T03:10:00+00:00",
                "results": [{"target": "mac", "status": "pass", "duration_secs": 8.0}],
            },
            Path("/tmp/alpha.json"),
        )
        self.core.merge_result_into_evidence_index(
            index,
            {
                "job_id": "job402",
                "branch": "main",
                "sha": shared_sha,
                "validation": "full",
                "completed_at": "2026-04-01T03:20:00+00:00",
                "results": [{"target": "mac", "status": "pass", "duration_secs": 7.5}],
            },
            Path("/tmp/main.json"),
        )

        groups = self.mod.collect_evidence_groups_from_index(index, branch="feature/alpha")

        self.assertEqual(len(groups["full"]), 1)
        self.assertEqual(groups["full"][0]["sha"], shared_sha)
        self.assertEqual(groups["full"][0]["branch"], "feature/alpha")
        self.assertIn("mac", groups["full"][0]["targets"])


if __name__ == "__main__":
    unittest.main()
