#!/usr/bin/env python3
"""Tests for GitHub Actions run helper wrappers."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cloud_github_runs.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudGithubRunsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_find_dispatched_run_matches_latest_candidate(self):
        run_payload = [
            {
                "databaseId": 2,
                "headBranch": "feature/cloud",
                "event": "workflow_dispatch",
                "createdAt": "2026-04-04T12:00:06Z",
            },
            {
                "databaseId": 1,
                "headBranch": "feature/cloud",
                "event": "workflow_dispatch",
                "createdAt": "2026-04-04T12:00:01Z",
            },
            {
                "databaseId": 3,
                "headBranch": "other",
                "event": "workflow_dispatch",
                "createdAt": "2026-04-04T12:00:07Z",
            },
        ]

        def fake_run(cmd, **kwargs):
            if cmd[:3] == ["gh", "run", "list"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps(run_payload), stderr="")
            raise AssertionError(cmd)

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run), \
                mock.patch.object(self.mod.time, "sleep"):
            matched = self.mod.gh_find_dispatched_run(
                "danielraffel/pulp",
                "build.yml",
                "feature/cloud",
                "2026-04-04T12:00:00Z",
                timeout_secs=1,
            )

        self.assertEqual(matched["databaseId"], 2)
        self.assertTrue(matched["match_ambiguous"])

    def test_find_dispatched_run_ignores_invalid_json_until_timeout(self):
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 0, stdout="{bad", stderr=""),
        ), mock.patch.object(self.mod.time, "sleep"), mock.patch.object(
            self.mod.time,
            "time",
            side_effect=[0, 0, 2],
        ):
            self.assertIsNone(
                self.mod.gh_find_dispatched_run(
                    "danielraffel/pulp",
                    "build.yml",
                    "feature/cloud",
                    "2026-04-04T12:00:00Z",
                    timeout_secs=1,
                )
            )

    def test_run_view_handles_success_failure_and_invalid_json(self):
        success = subprocess.CompletedProcess(["gh"], 0, stdout=json.dumps({"databaseId": 7}), stderr="")
        with mock.patch.object(self.mod.subprocess, "run", return_value=success):
            self.assertEqual(self.mod.gh_run_view("danielraffel/pulp", 7), {"databaseId": 7})

        failed = subprocess.CompletedProcess(["gh"], 1, stdout="", stderr="missing")
        with mock.patch.object(self.mod.subprocess, "run", return_value=failed):
            self.assertIsNone(self.mod.gh_run_view("danielraffel/pulp", 7))

        invalid = subprocess.CompletedProcess(["gh"], 0, stdout="{bad", stderr="")
        with mock.patch.object(self.mod.subprocess, "run", return_value=invalid):
            self.assertIsNone(self.mod.gh_run_view("danielraffel/pulp", 7))


if __name__ == "__main__":
    unittest.main()
