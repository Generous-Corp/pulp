#!/usr/bin/env python3
"""Tests for shared cloud command output line helpers."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_command_format.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudCommandFormatTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cloud_history_lines_render_limited_summaries(self) -> None:
        records = [{"id": "a"}, {"id": "b"}, {"id": "c"}]
        lines = self.mod.cloud_history_lines(
            records,
            {"config": True},
            limit=2,
            summary_fn=lambda record, config: f"{record['id']} cfg={bool(config)}",
        )

        self.assertEqual(lines, ["Cloud history:", "", "  a cfg=True", "  b cfg=True"])

    def test_cloud_recent_status_lines_include_trailing_blank(self) -> None:
        lines = self.mod.cloud_recent_status_lines(
            [{"id": "a"}],
            None,
            summary_fn=lambda record, _config: f"recent {record['id']}",
        )

        self.assertEqual(lines, ["Recent cloud runs:", "", "  recent a", ""])

    def test_cloud_recommend_lines_handle_empty_and_successful_recommendations(self) -> None:
        self.assertEqual(
            self.mod.cloud_recommend_lines("build", None, "no successful runs recorded yet"),
            ["No recommendation for workflow 'build': no successful runs recorded yet."],
        )
        self.assertEqual(
            self.mod.cloud_recommend_lines("build", "github-hosted", "lower estimated cost"),
            [
                "Recommended provider for build: github-hosted (lower estimated cost)",
                "  note: estimated; verify provider pricing",
            ],
        )

    def test_cloud_workflow_lines_render_builtin_like_workflows(self) -> None:
        lines = self.mod.cloud_workflow_lines(
            {
                "build": {
                    "display_name": "Build and Test",
                    "file": "build.yml",
                    "providers": ["github-hosted", "namespace"],
                },
                "docs": {"display_name": "Docs", "file": "docs.yml"},
            }
        )

        self.assertEqual(
            lines,
            [
                "GitHub Actions workflows:",
                "",
                "  build        Build and Test (build.yml)",
                "               providers: github-hosted, namespace",
                "  docs         Docs (docs.yml)",
                "               providers: github-hosted",
            ],
        )

    def test_cloud_dispatch_and_final_status_lines(self) -> None:
        matched = self.mod.cloud_dispatch_lines(
            {"dispatch_id": "abc123", "run_id": 42, "url": "https://example.test/runs/42"},
            workflow_key="build",
            branch="feature/x",
            provider="github-hosted",
        )
        self.assertEqual(
            matched,
            [
                "Dispatched: build ref=feature/x provider=github-hosted",
                "  dispatch id: abc123",
                "  GitHub run: 42",
                "  URL: https://example.test/runs/42",
            ],
        )

        unmatched = self.mod.cloud_dispatch_lines(
            {"dispatch_id": "def456"},
            workflow_key="docs-check",
            branch="main",
            provider="namespace",
        )
        self.assertEqual(
            unmatched,
            [
                "Dispatched: docs-check ref=main provider=namespace",
                "  dispatch id: def456",
                "  warning: dispatched workflow could not be matched to a GitHub run yet",
            ],
        )
        self.assertEqual(
            self.mod.cloud_final_status_line({"status": "completed", "conclusion": "success"}),
            "  final: completed/SUCCESS",
        )
        self.assertEqual(self.mod.cloud_final_status_line({}), "  final: ?/UNKNOWN")


if __name__ == "__main__":
    unittest.main()
