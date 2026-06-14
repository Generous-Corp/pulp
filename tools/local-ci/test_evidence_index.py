#!/usr/bin/env python3
"""Tests for evidence-index persistence and display helpers."""

import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

import evidence_index


class EvidenceIndexTests(unittest.TestCase):
    def setUp(self):
        self.mod = evidence_index

    def test_evidence_command_line_fragments(self) -> None:
        self.assertEqual(
            self.mod.evidence_scope_header_line("feature/evidence", None),
            "Evidence for branch `feature/evidence`:",
        )
        self.assertEqual(
            self.mod.evidence_scope_header_line(None, "f" * 40),
            "Evidence for sha `ffffffffffff`:",
        )
        self.assertIsNone(self.mod.evidence_scope_header_line(None, None))
        self.assertEqual(self.mod.evidence_empty_line(has_header=True), "  (none)")
        self.assertEqual(
            self.mod.evidence_empty_line(has_header=False),
            "No local CI evidence recorded.",
        )

    def test_update_evidence_index_keeps_latest_passing_target_per_branch_sha_validation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            state_dir = Path(tmp) / "state"
            result_path_one = state_dir / "results" / "one.json"
            result_path_two = state_dir / "results" / "two.json"
            result_path_one.parent.mkdir(parents=True)
            with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(state_dir)}, clear=True):
                self.mod.update_evidence_index(
                    {
                        "job_id": "job111",
                        "branch": "feature/evidence",
                        "sha": "1" * 40,
                        "validation": "full",
                        "completed_at": "2026-04-01T00:10:00+00:00",
                        "results": [
                            {"target": "mac", "status": "pass", "duration_secs": 10.0},
                            {"target": "ubuntu", "status": "fail", "duration_secs": 20.0},
                        ],
                    },
                    result_path_one,
                )
                self.mod.update_evidence_index(
                    {
                        "job_id": "job112",
                        "branch": "feature/evidence",
                        "sha": "1" * 40,
                        "validation": "full",
                        "completed_at": "2026-04-01T00:20:00+00:00",
                        "results": [
                            {"target": "ubuntu", "status": "pass", "duration_secs": 12.0},
                        ],
                    },
                    result_path_two,
                )

                index = self.mod.load_evidence_index()

        self.assertIn(
            self.mod.evidence_entry_key("feature/evidence", "1" * 40, "mac", "full"),
            index["entries"],
        )
        ubuntu_key = self.mod.evidence_entry_key("feature/evidence", "1" * 40, "ubuntu", "full")
        self.assertEqual(index["entries"][ubuntu_key]["job_id"], "job112")

    def test_collect_evidence_groups_is_branch_scoped_for_shared_sha(self) -> None:
        shared_sha = "4" * 40
        index = self.mod.empty_evidence_index()
        self.mod.merge_result_into_evidence_index(
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
        self.mod.merge_result_into_evidence_index(
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

    def test_print_evidence_summary_groups_branch_results(self) -> None:
        groups = {
            "smoke": [
                {
                    "sha": "2" * 40,
                    "completed_at": "2026-04-01T01:10:00+00:00",
                    "provenance": {"execution_kind": "direct"},
                    "targets": {
                        "mac": {"target": "mac"},
                        "windows": {"target": "windows"},
                    },
                }
            ]
        }

        buf = io.StringIO()
        with redirect_stdout(buf):
            found = self.mod.print_evidence_summary_from_groups(groups, limit=5)

        output = buf.getvalue()
        self.assertTrue(found)
        self.assertIn("smoke:", output)
        self.assertIn("mac=pass, windows=pass", output)
        self.assertIn("222222222222", output)


if __name__ == "__main__":
    unittest.main()
