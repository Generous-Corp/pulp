#!/usr/bin/env python3
"""Tests for local CI cloud provider CLI helpers."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("cloud.py")
MODULE_DIR = MODULE_PATH.parent


def load_module():
    sys.path.insert(0, str(MODULE_DIR))
    try:
        spec = importlib.util.spec_from_file_location("pulp_local_ci_cloud", MODULE_PATH)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


class CloudCliHelperTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_github_auth_api_repository_and_dispatch_helpers(self):
        calls = []

        def fake_run(cmd, **kwargs):
            calls.append((cmd, kwargs))
            if cmd[:3] == ["gh", "auth", "status"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="Token scopes: 'repo, workflow, read:org'\n", stderr="")
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
            self.assertEqual(self.mod.gh_token_scopes(), {"repo", "workflow", "read:org"})
            self.assertEqual(self.mod.gh_current_login(), "danielraffel")
            self.assertEqual(self.mod.gh_repo_name(), "danielraffel/pulp")
            payload, detail = self.mod.gh_api_json("/repos/danielraffel/pulp/actions", fields={"per_page": 1})
            self.assertEqual(payload, {"ok": True})
            self.assertEqual(detail, "")
            self.mod.gh_workflow_dispatch("danielraffel/pulp", "build.yml", "feature/cloud", {"provider": "namespace"})

        self.assertIn("-f", calls[-1][0])
        self.assertIn("provider=namespace", calls[-1][0])

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 1, stdout="", stderr="denied"),
        ):
            self.assertFalse(self.mod.gh_available())
            self.assertEqual(self.mod.gh_auth_status_text(), "")
            self.assertEqual(self.mod.gh_token_scopes(), set())
            payload, detail = self.mod.gh_api_json("/bad")
            self.assertIsNone(payload)
            self.assertEqual(detail, "denied")
            self.assertIsNone(self.mod.gh_repo_name())
            self.assertIsNone(self.mod.gh_current_login())
            with self.assertRaisesRegex(RuntimeError, "Failed to dispatch build.yml: denied"):
                self.mod.gh_workflow_dispatch("danielraffel/pulp", "build.yml", "main", {})

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 0, stdout="{bad", stderr=""),
        ):
            payload, detail = self.mod.gh_api_json("/bad-json")
            self.assertIsNone(payload)
            self.assertEqual(detail, "gh api returned invalid JSON")
            self.assertIsNone(self.mod.gh_repo_name())

    def test_namespace_cli_helpers_parse_availability_workspace_and_instances(self):
        def fake_run(cmd, **kwargs):
            if cmd == ["nsc", "version"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="v0.0.493\n", stderr="")
            if cmd == ["nsc", "auth", "check-login"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd == ["nsc", "workspace", "describe"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="Name: Personal\nTenant ID: tenant_123\n", stderr="")
            if cmd[:5] == ["nsc", "instance", "history", "--all", "-o"]:
                return subprocess.CompletedProcess(
                    cmd,
                    0,
                    stdout=json.dumps(
                        [
                            {
                                "cluster_id": "cluster-2",
                                "created_at": "2026-04-04T12:01:00Z",
                                "github_workflow": {"repository": "other/repo", "run_id": 123},
                            },
                            {
                                "cluster_id": "cluster-1",
                                "created_at": "2026-04-04T12:00:00Z",
                                "github_workflow": {"repository": "danielraffel/pulp", "run_id": 123},
                            },
                        ]
                    ),
                    stderr="",
                )
            raise AssertionError(cmd)

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            self.assertTrue(self.mod.nsc_available())
            self.assertEqual(self.mod.nsc_version(), "v0.0.493")
            self.assertTrue(self.mod.nsc_logged_in())
            self.assertEqual(self.mod.nsc_workspace_info()["Name"], "Personal")
            self.assertEqual(self.mod.parse_colon_separated_fields("A: one\nbad\nB: two:three"), {"A": "one", "B": "two:three"})
            history = self.mod.nsc_instance_history(max_entries=7)
            self.assertEqual(len(history), 2)
            matches = self.mod.namespace_instances_for_run("danielraffel/pulp", 123)
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]["cluster_id"], "cluster-1")

        with mock.patch.object(self.mod.subprocess, "run", side_effect=FileNotFoundError):
            self.assertIsNone(self.mod.nsc_run(["version"]))
            self.assertFalse(self.mod.nsc_available())
            self.assertIsNone(self.mod.nsc_version())
            self.assertFalse(self.mod.nsc_logged_in())
            self.assertIsNone(self.mod.nsc_workspace_info())
            self.assertEqual(self.mod.nsc_instance_history(), [])

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["nsc"], 0, stdout="{bad", stderr=""),
        ):
            self.assertEqual(self.mod.nsc_instance_history(), [])

    def test_github_run_and_pr_helpers_handle_success_existing_and_invalid_json(self):
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

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 0, stdout="{bad", stderr=""),
        ):
            self.assertIsNone(self.mod.gh_run_view("danielraffel/pulp", 2))


if __name__ == "__main__":
    unittest.main()
