#!/usr/bin/env python3
"""Tests for cloud status output line helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud_status_format.py", add_module_dir=True)


class CloudStatusFormatTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cloud_status_detail_lines_render_known_fields_in_order(self) -> None:
        lines = self.mod.cloud_status_detail_lines(
            {
                "workflow_name": "Build",
                "workflow_file": "build.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "runner_selector_json": '["self-hosted","Linux"]',
                "dispatch_fields": {
                    "empty": "",
                    "runner_provider": "namespace",
                    "runner_selector_json": '["self-hosted","Linux","ARM64"]',
                },
                "head_sha": "abcdef1234567890",
                "url": "https://example.test/runs/42",
                "matched_at": "2026-04-04T12:00:01+00:00",
                "started_at": "2026-04-04T12:00:06+00:00",
                "queue_delay_secs": 1,
                "duration_secs": 61,
                "updated_at": "2026-04-04T12:01:00+00:00",
                "completed_at": "2026-04-04T12:01:07+00:00",
            }
        )

        self.assertEqual(
            lines,
            [
                "  workflow: Build (build.yml)",
                "  repo: danielraffel/pulp",
                "  requested ref: feature/cloud",
                "  runner selector: self-hosted,Linux",
                "  runner_provider: namespace",
                "  runner_selector_json: self-hosted,Linux,ARM64",
                "  sha: abcdef123456",
                "  url: https://example.test/runs/42",
                "  matched: 2026-04-04T12:00:01+00:00",
                "  started: 2026-04-04T12:00:06+00:00",
                "  queue delay: 1s",
                "  elapsed: 1m01s",
                "  updated: 2026-04-04T12:01:00+00:00",
                "  completed: 2026-04-04T12:01:07+00:00",
            ],
        )

    def test_cloud_status_detail_lines_skip_optional_invalid_shapes(self) -> None:
        lines = self.mod.cloud_status_detail_lines(
            {
                "workflow_name": "Docs",
                "workflow_file": "docs.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "main",
                "dispatch_fields": "bad",
            }
        )
        self.assertEqual(
            lines,
            [
                "  workflow: Docs (docs.yml)",
                "  repo: danielraffel/pulp",
                "  requested ref: main",
            ],
        )

    def test_cloud_status_job_lines_render_status_conclusion_and_duration(self) -> None:
        lines = self.mod.cloud_status_job_lines(
            {
                "jobs": [
                    {
                        "name": "Validate docs",
                        "status": "completed",
                        "conclusion": "success",
                        "started_at": "2026-04-04T12:00:09+00:00",
                        "completed_at": "2026-04-04T12:00:30+00:00",
                    },
                    {"status": "queued"},
                ]
            }
        )
        self.assertEqual(
            lines,
            [
                "  jobs:",
                "    Validate docs: completed/success duration=21s",
                "    ?: queued",
            ],
        )
        self.assertEqual(self.mod.cloud_status_job_lines({}), [])


if __name__ == "__main__":
    unittest.main()
