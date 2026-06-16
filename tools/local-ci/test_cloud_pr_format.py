#!/usr/bin/env python3
"""Tests for PR list and CI comment formatting helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud_pr_format.py", add_module_dir=True)


class CloudPrFormatTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_open_pr_list_lines_empty_and_labelled_pr(self) -> None:
        self.assertEqual(self.mod.open_pr_list_lines([]), ["No open PRs."])
        self.assertEqual(self.mod.no_open_prs_line(), "No open PRs.")
        self.assertEqual(self.mod.open_prs_header_line(2), "Open PRs (2):")

        lines = self.mod.open_pr_list_lines(
            [
                {
                    "number": 7,
                    "title": "Refactor local CI",
                    "headRefName": "feature/local-ci",
                    "author": {"login": "dev"},
                    "labels": [{"name": "ci"}, {"name": "refactor"}],
                }
            ]
        )

        self.assertEqual(
            lines,
            [
                "Open PRs (1):",
                "",
                "  #   7  Refactor local CI",
                "         feature/local-ci by dev [ci, refactor]",
            ],
        )

    def test_format_ci_comment_includes_smoke_failure_details(self) -> None:
        comment = self.mod.format_ci_comment(
            {
                "overall": "fail",
                "job_id": "job123",
                "sha": "a" * 40,
                "validation": "smoke",
                "completed_at": "2026-04-04T12:00:00+00:00",
                "provenance": {
                    "execution_kind": "hosted",
                    "hosted_orchestrator": "github-actions",
                    "runner_provider": "github-hosted",
                    "run_url": "https://example.test/runs/99",
                },
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 1},
                    {"target": "windows", "status": "fail", "exit_code": 2, "stderr_tail": "boom"},
                ],
            }
        )

        self.assertIn("Local CI Smoke Results", comment)
        self.assertIn("Overall: FAIL", comment)
        self.assertIn("Commit: `aaaaaaaaaaaa`", comment)
        self.assertIn("Execution: `hosted via github-actions/github-hosted`", comment)
        self.assertIn("Run URL: https://example.test/runs/99", comment)
        self.assertIn("Validation: `smoke`", comment)
        self.assertIn("| windows | :x: FAIL | 0s |", comment)
        self.assertIn("### windows (exit 2)", comment)
        self.assertIn("boom", comment)

    def test_format_ci_comment_uses_submission_provenance_fallback(self) -> None:
        comment = self.mod.format_ci_comment(
            {
                "overall": "pass",
                "job_id": "job124",
                "sha": "",
                "completed_at": "2026-04-04T12:00:00+00:00",
                "submission": {
                    "provenance": {
                        "execution_kind": "direct",
                        "direct_backend": "local-ci",
                    }
                },
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 2},
                ],
            }
        )

        self.assertIn("Local CI Results", comment)
        self.assertIn("Commit: `?`", comment)
        self.assertIn("Execution: `direct via local-ci`", comment)
        self.assertNotIn("Failure details", comment)


if __name__ == "__main__":
    unittest.main()
