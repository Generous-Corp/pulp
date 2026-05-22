#!/usr/bin/env python3
"""Tests for local CI cloud provider CLI helpers."""

from __future__ import annotations

import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
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

    def test_cloud_record_storage_and_refresh_edges(self):
        with tempfile.TemporaryDirectory() as tmp:
            cloud_dir = Path(tmp)
            valid = cloud_dir / "valid.json"
            invalid = cloud_dir / "invalid.json"
            valid.write_text(json.dumps({"dispatch_id": "run-ok", "updated_at": "2026-04-04T12:00:00+00:00"}) + "\n")
            invalid.write_text("{bad")

            with mock.patch.object(self.mod, "ensure_state_dirs"):
                with mock.patch.object(self.mod, "cloud_runs_dir", return_value=cloud_dir):
                    records = self.mod.list_cloud_records()
                    self.assertEqual([record["dispatch_id"] for record in records], ["run-ok"])
                    self.assertEqual(self.mod.list_cloud_records(limit=0), [])

        self.assertEqual(self.mod.refresh_cloud_record({"dispatch_id": "no-run"}, "repo")["dispatch_id"], "no-run")
        with mock.patch.object(self.mod, "gh_run_view", return_value=None):
            self.assertEqual(self.mod.refresh_cloud_record({"dispatch_id": "missing", "run_id": 7}, "repo")["run_id"], 7)
            with self.assertRaisesRegex(RuntimeError, "Failed to refresh GitHub run 7"):
                self.mod.refresh_cloud_record({"dispatch_id": "missing", "run_id": 7}, "repo", require_snapshot=True)

        saved = {}
        snapshot = {
            "databaseId": 7,
            "workflowName": "Build",
            "headBranch": "feature/cloud",
            "headSha": "a" * 40,
            "status": "completed",
            "conclusion": "success",
            "url": "https://example.test/runs/7",
            "createdAt": "2026-04-04T12:00:00+00:00",
            "updatedAt": "2026-04-04T12:01:00+00:00",
            "jobs": [],
        }
        with mock.patch.object(self.mod, "gh_run_view", return_value=snapshot):
            with mock.patch.object(self.mod, "enrich_cloud_record_provider_metadata", side_effect=lambda record: record):
                with mock.patch.object(self.mod, "save_cloud_record", side_effect=lambda record: saved.setdefault("record", record)):
                    refreshed = self.mod.refresh_cloud_record({"dispatch_id": "ok", "run_id": 7}, "repo")
        self.assertEqual(refreshed["run_id"], 7)
        self.assertEqual(refreshed["completed_at"], "2026-04-04T12:01:00+00:00")
        self.assertEqual(saved["record"]["status"], "completed")

    def test_billing_summary_and_printing_edges(self):
        config = {
            "telemetry": {
                "billing": {
                    "enable_provider_reported_totals": True,
                    "billing_period_start_day": 1,
                }
            }
        }
        with self.assertRaisesRegex(ValueError, "must be an integer"):
            self.mod.resolve_billing_settings(
                {"telemetry": {"billing": {"billing_period_start_day": "first"}}}
            )
        self.assertEqual(self.mod.resolve_billing_settings({"telemetry": {"billing": []}})["currency"], "USD")
        self.assertEqual(self.mod.format_currency_amount("bad"), "")

        with mock.patch.object(self.mod, "gh_available", return_value=False):
            self.assertEqual(
                self.mod.fetch_github_repo_actions_billing_summary("danielraffel/pulp", config)["reason"],
                "gh CLI unavailable",
            )
        with mock.patch.object(self.mod, "gh_available", return_value=True):
            with mock.patch.object(self.mod, "gh_api_json", return_value=(None, "denied")):
                self.assertIn(
                    "repo lookup failed",
                    self.mod.fetch_github_repo_actions_billing_summary("danielraffel/pulp", config)["reason"],
                )
            with mock.patch.object(self.mod, "gh_api_json", return_value=({"owner": {}}, "")):
                self.assertEqual(
                    self.mod.fetch_github_repo_actions_billing_summary("danielraffel/pulp", config)["reason"],
                    "repo owner unknown",
                )
            with mock.patch.object(
                self.mod,
                "gh_api_json",
                return_value=({"owner": {"login": "bot", "type": "Bot"}}, ""),
            ):
                self.assertIn(
                    "unsupported owner type",
                    self.mod.fetch_github_repo_actions_billing_summary("danielraffel/pulp", config)["reason"],
                )

        responses = [
            ({"owner": {"login": "danielraffel", "type": "User"}}, ""),
            (
                {
                    "usageItems": [
                        {
                            "product": "actions",
                            "repositoryName": "danielraffel/pulp",
                            "date": "2026-04-04",
                            "netAmount": "",
                            "grossAmount": "1.25",
                        },
                        {"product": "storage", "repositoryName": "danielraffel/pulp", "date": "2026-04-04", "netAmount": "9"},
                        {"product": "actions", "repositoryName": "other/repo", "date": "2026-04-04", "netAmount": "9"},
                        {"product": "actions", "repositoryName": "danielraffel/pulp", "date": "bad", "netAmount": "9"},
                    ]
                },
                "",
            ),
            ({"usageItems": []}, ""),
        ]
        with mock.patch.object(self.mod, "gh_available", return_value=True):
            with mock.patch.object(self.mod, "billing_period_window", return_value=(
                datetime(2026, 4, 1, tzinfo=timezone.utc),
                datetime(2026, 5, 1, tzinfo=timezone.utc),
            )):
                with mock.patch.object(self.mod, "gh_api_json", side_effect=responses):
                    summary = self.mod.fetch_github_repo_actions_billing_summary("danielraffel/pulp", config)
        self.assertEqual(summary["status"], "actual")
        self.assertEqual(summary["matched_items"], 1)
        self.assertEqual(summary["actual_total"], 1.25)

        buf = io.StringIO()
        with redirect_stdout(buf):
            self.mod.print_github_repo_billing_summary(summary, indent="")
            self.mod.print_github_repo_billing_summary({"status": "disabled"}, indent="")
            self.mod.print_billing_period_summary({"status": "estimated", "estimated_total": None}, indent="")
        self.assertIn("github repo billing: actual $1.25 current period", buf.getvalue())

    def test_namespace_metadata_and_github_cli_edge_paths(self):
        non_namespace = self.mod.enrich_cloud_record_provider_metadata(
            {"provider_requested": "github-hosted", "provider_metadata": {"stale": True}, "usage_summary": {"stale": True}}
        )
        self.assertEqual(non_namespace["provider_metadata"], {})
        self.assertEqual(non_namespace["usage_summary"], {})

        with mock.patch.object(self.mod, "nsc_logged_in", return_value=False):
            namespace = self.mod.enrich_cloud_record_provider_metadata({"provider_requested": "namespace", "run_id": 99})
            self.assertEqual(namespace["provider_requested"], "namespace")
            self.assertEqual(namespace["provider_metadata"], {})

        instances = [
            {
                "cluster_id": "cluster-1",
                "duration_secs": 60,
                "os": "linux",
                "arch": "amd64",
                "virtual_cpu": 4,
                "memory_megabytes": 8192,
                "profile_tag": "fast",
            }
        ]
        with mock.patch.object(self.mod, "nsc_logged_in", return_value=True):
            with mock.patch.object(self.mod, "namespace_instances_for_run", return_value=instances):
                enriched = self.mod.enrich_cloud_record_provider_metadata(
                    {"provider_requested": "namespace", "repository": "danielraffel/pulp", "run_id": 99}
                )
        self.assertEqual(enriched["provider_metadata"]["namespace_instances"], instances)
        self.assertEqual(enriched["usage_summary"]["instances_count"], 1)
        self.assertEqual(enriched["cost_summary"]["status"], "unavailable")

        def fake_run(cmd, **kwargs):
            if cmd[:3] == ["gh", "variable", "list"]:
                return subprocess.CompletedProcess(
                    cmd,
                    0,
                    stdout=json.dumps(
                        [
                            {"name": "KEEP", "value": "yes"},
                            {"name": "", "value": "ignored"},
                            {"name": "EMPTY", "value": ""},
                        ]
                    ),
                    stderr="",
                )
            if cmd[:3] == ["gh", "pr", "create"]:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="create failed")
            if cmd[:3] == ["gh", "pr", "view"]:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="missing")
            if cmd[:3] == ["gh", "pr", "list"]:
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="denied")
            raise AssertionError(cmd)

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            self.assertEqual(self.mod.gh_repo_variables("danielraffel/pulp"), {"KEEP": "yes"})
            self.assertIsNone(self.mod.gh_pr_create("feature/missing"))
            self.assertEqual(self.mod.gh_pr_list_open(), [])
            self.assertIsNone(self.mod.gh_pr_head("latest"))

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 0, stdout="{bad", stderr=""),
        ):
            self.assertEqual(self.mod.gh_repo_variables("danielraffel/pulp"), {})

        with mock.patch.object(self.mod, "nsc_available", return_value=True):
            with mock.patch.object(self.mod, "nsc_logged_in", return_value=False):
                with mock.patch.object(self.mod, "nsc_run", return_value=SimpleNamespace(returncode=1)):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_cloud_namespace_setup(SimpleNamespace())
        self.assertEqual(exit_code, 1)
        self.assertIn("Namespace login: failed", buf.getvalue())

        comment = self.mod.format_ci_comment(
            {
                "overall": "fail",
                "job_id": "job123",
                "sha": "a" * 40,
                "validation": "smoke",
                "completed_at": "2026-04-04T12:00:00+00:00",
                "provenance": {"run_url": "https://example.test/runs/99"},
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 1},
                    {"target": "windows", "status": "fail", "exit_code": 2, "stderr_tail": "boom"},
                ],
            }
        )
        self.assertIn("Local CI Smoke Results", comment)
        self.assertIn("Run URL: https://example.test/runs/99", comment)
        self.assertIn("Validation: `smoke`", comment)
        self.assertIn("### windows (exit 2)", comment)
        self.assertIn("boom", comment)


if __name__ == "__main__":
    unittest.main()
