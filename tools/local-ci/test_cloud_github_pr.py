#!/usr/bin/env python3
"""Tests for GitHub PR helper wrappers."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_github_pr.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CloudGithubPrTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_pr_helpers_handle_success_paths(self):
        calls = []

        def fake_run(cmd, **kwargs):
            calls.append((cmd, kwargs))
            if cmd[:3] == ["gh", "pr", "create"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="https://github.com/danielraffel/pulp/pull/42\n", stderr="")
            if cmd[:3] == ["gh", "pr", "comment"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[:3] == ["gh", "pr", "merge"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[:3] == ["gh", "pr", "list"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps([{"number": 42}]), stderr="")
            if cmd[:3] == ["gh", "pr", "view"]:
                return subprocess.CompletedProcess(
                    cmd,
                    0,
                    stdout=json.dumps({"number": 42, "headRefName": "feature/cloud", "headRefOid": "abc123"}),
                    stderr="",
                )
            raise AssertionError(cmd)

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            self.assertEqual(self.mod.gh_pr_create("feature/cloud"), 42)
            self.assertTrue(self.mod.gh_pr_comment(42, "body"))
            self.assertTrue(self.mod.gh_pr_merge(42))
            self.assertEqual(self.mod.gh_pr_list_open(), [{"number": 42}])
            self.assertEqual(self.mod.gh_pr_head("latest"), (42, "feature/cloud", "abc123"))

        self.assertIn("--no-maintainer-edit", calls[0][0])
        self.assertEqual(calls[2][0], ["gh", "pr", "merge", "42", "--squash", "--delete-branch"])

    def test_pr_create_reuses_existing_pr_and_reports_missing_head(self):
        existing = subprocess.CompletedProcess(
            ["gh"],
            0,
            stdout=json.dumps({"number": 99}),
            stderr="",
        )
        missing = subprocess.CompletedProcess(["gh"], 1, stdout="", stderr="create failed")

        with mock.patch.object(self.mod.subprocess, "run", side_effect=[missing, existing]):
            self.assertEqual(self.mod.gh_pr_create("feature/existing"), 99)

        with mock.patch.object(self.mod.subprocess, "run", side_effect=[missing, missing]):
            self.assertIsNone(self.mod.gh_pr_create("feature/missing"))

    def test_pr_head_reports_empty_latest_and_failed_lookup(self):
        printed = []
        self.assertIsNone(self.mod.gh_pr_head("latest", gh_pr_list_open_fn=lambda: [], print_fn=printed.append))
        self.assertEqual(printed, ["No open PRs found."])

        failed = subprocess.CompletedProcess(["gh"], 1, stdout="", stderr="missing")
        printed = []
        with mock.patch.object(self.mod.subprocess, "run", return_value=failed):
            self.assertIsNone(self.mod.gh_pr_head("42", print_fn=printed.append))
        self.assertEqual(printed, ["  Could not find PR 42: missing"])


if __name__ == "__main__":
    unittest.main()
