#!/usr/bin/env python3
"""No-network tests for target submission metadata printing."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("target_submission_print.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetSubmissionPrintTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.lines: list[str] = []

    def print_line(self, *parts) -> None:
        self.lines.append(" ".join(str(part) for part in parts))

    def test_print_submission_metadata_includes_summary_targets_and_warnings(self) -> None:
        self.mod.print_submission_metadata(
            {
                "branch": "feature/topic",
                "sha": "a" * 40,
                "priority": "normal",
                "targets": ["mac", "windows"],
                "submitted_root": "/repo",
                "cwd": "/repo",
                "cwd_git_root": "/repo",
                "config_path": "/repo/config.json",
                "config_source": "worktree-local",
                "provenance": {"execution_kind": "direct"},
                "config_drift": ["windows: drift"],
                "target_hosts": {
                    "mac": {"transport_mode": "local"},
                    "windows": {
                        "transport_mode": "ssh",
                        "resolved_host": "win",
                        "status": "fallback-up",
                        "repo_path": "C:\\Pulp",
                    },
                },
                "warnings": ["windows: fallback win is up"],
            },
            short_sha_fn=lambda sha: sha[:8],
            provenance_summary_fn=lambda provenance: provenance["execution_kind"] if provenance else "",
            print_fn=self.print_line,
        )

        self.assertEqual(self.lines[0], "Submitting: feature/topic @ aaaaaaaa priority=normal targets=mac,windows")
        self.assertIn("  cwd git root: /repo", self.lines)
        self.assertIn("  provenance: direct", self.lines)
        self.assertIn("  config drift: windows: drift", self.lines)
        self.assertIn("  mac: local transport", self.lines)
        self.assertIn("  windows: host=win status=fallback-up transport=ssh repo=C:\\Pulp", self.lines)
        self.assertIn("  warning: windows: fallback win is up", self.lines)

    def test_print_submission_metadata_handles_empty_targets_and_missing_remote_fields(self) -> None:
        self.mod.print_submission_metadata(
            {
                "branch": "feature/topic",
                "sha": "b" * 40,
                "priority": "low",
                "targets": [],
                "submitted_root": "/repo",
                "cwd": "/repo/subdir",
                "config_path": "/repo/config.json",
                "config_source": "env-override",
                "target_hosts": {"ubuntu": {"transport_mode": "ssh"}},
            },
            short_sha_fn=lambda sha: sha[:7],
            provenance_summary_fn=lambda _provenance: "",
            print_fn=self.print_line,
        )

        self.assertEqual(self.lines[0], "Submitting: feature/topic @ bbbbbbb priority=low targets=none")
        self.assertNotIn("  cwd git root: ", self.lines)


if __name__ == "__main__":
    unittest.main()
