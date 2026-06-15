#!/usr/bin/env python3
"""Tests for GitHub CLI/API wrappers used by cloud CI helpers."""

from __future__ import annotations

import json
import subprocess
import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud_github.py", add_module_dir=True)


class CloudGithubTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_auth_api_repo_and_dispatch_helpers(self) -> None:
        calls = []

        def fake_run(cmd, **kwargs):
            calls.append(cmd)
            if cmd[:3] == ["gh", "auth", "status"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="Token scopes: 'repo, workflow'\n", stderr="")
            if cmd[:2] == ["gh", "api"] and cmd[2] == "user":
                return subprocess.CompletedProcess(cmd, 0, stdout="danielraffel\n", stderr="")
            if cmd[:2] == ["gh", "api"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps({"ok": True}), stderr="")
            if cmd[:3] == ["gh", "repo", "view"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps({"nameWithOwner": "danielraffel/pulp"}), stderr="")
            if cmd[:3] == ["gh", "workflow", "run"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            raise AssertionError(cmd)

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            self.assertTrue(self.mod.gh_available())
            self.assertEqual(self.mod.gh_token_scopes(), {"repo", "workflow"})
            self.assertEqual(self.mod.gh_current_login(), "danielraffel")
            self.assertEqual(self.mod.gh_repo_name(), "danielraffel/pulp")
            self.assertEqual(self.mod.gh_api_json("/repos/danielraffel/pulp")[0], {"ok": True})
            self.mod.gh_workflow_dispatch("danielraffel/pulp", "build.yml", "feature/cloud", {"provider": "namespace"})

        self.assertIn("provider=namespace", calls[-1])

    def test_error_and_invalid_json_edges(self) -> None:
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 1, stdout="", stderr="denied"),
        ):
            self.assertFalse(self.mod.gh_available())
            self.assertEqual(self.mod.gh_auth_status_text(), "")
            self.assertEqual(self.mod.gh_token_scopes(), set())
            self.assertEqual(self.mod.gh_api_json("/bad"), (None, "denied"))
            self.assertIsNone(self.mod.gh_repo_name())
            self.assertIsNone(self.mod.gh_current_login())
            with self.assertRaisesRegex(RuntimeError, "Failed to dispatch build.yml: denied"):
                self.mod.gh_workflow_dispatch("danielraffel/pulp", "build.yml", "main", {})

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 0, stdout="{bad", stderr=""),
        ):
            self.assertEqual(self.mod.gh_api_json("/bad-json"), (None, "gh api returned invalid JSON"))
            self.assertIsNone(self.mod.gh_repo_name())
            self.assertEqual(self.mod.gh_repo_variables("danielraffel/pulp"), {})
            self.assertIsNone(self.mod.gh_run_view("danielraffel/pulp", 7))

    def test_run_lookup_and_pr_helpers(self) -> None:
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
        ]

        def fake_run(cmd, **kwargs):
            if cmd[:3] == ["gh", "run", "list"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps(run_payload), stderr="")
            if cmd[:3] == ["gh", "run", "view"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps({"databaseId": 2}), stderr="")
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
            matched = self.mod.gh_find_dispatched_run(
                "danielraffel/pulp",
                "build.yml",
                "feature/cloud",
                "2026-04-04T12:00:00Z",
                timeout_secs=1,
            )
            self.assertEqual(matched["databaseId"], 2)
            self.assertTrue(matched["match_ambiguous"])
            self.assertEqual(self.mod.gh_run_view("danielraffel/pulp", 2), {"databaseId": 2})
            self.assertEqual(self.mod.gh_pr_create("feature/cloud"), 42)
            self.assertTrue(self.mod.gh_pr_comment(42, "body"))
            self.assertTrue(self.mod.gh_pr_merge(42))
            self.assertEqual(self.mod.gh_pr_list_open(), [{"number": 42}])
            self.assertEqual(self.mod.gh_pr_head("latest"), (42, "feature/cloud", "abc123"))

    def test_pr_create_existing_and_latest_no_pr_edges(self) -> None:
        def fake_run(cmd, **kwargs):
            if cmd[:3] == ["gh", "pr", "create"]:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="exists")
            if cmd[:3] == ["gh", "pr", "view"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps({"number": 99, "headRefName": "feature/x", "headRefOid": "abc"}), stderr="")
            raise AssertionError(cmd)

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            self.assertEqual(self.mod.gh_pr_create("feature/x"), 99)

        printed = []
        self.assertIsNone(self.mod.gh_pr_head("latest", gh_pr_list_open_fn=lambda: [], print_fn=printed.append))
        self.assertEqual(printed, ["No open PRs found."])


if __name__ == "__main__":
    unittest.main()
