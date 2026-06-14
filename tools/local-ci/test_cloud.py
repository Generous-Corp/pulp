#!/usr/bin/env python3
"""Tests for local CI cloud provider CLI helpers."""

from __future__ import annotations

import io
import json
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud.py", module_name="pulp_local_ci_cloud", add_module_dir=True)


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

    def test_open_pr_list_lines_match_cli_display(self):
        self.assertEqual(self.mod.open_pr_list_lines([]), ["No open PRs."])

        prs = [
            {
                "number": 7,
                "title": "Refactor local CI",
                "headRefName": "feature/local-ci",
                "author": {"login": "dev"},
                "labels": [{"name": "ci"}, {"name": "refactor"}],
            }
        ]

        self.assertEqual(
            self.mod.open_pr_list_lines(prs),
            [
                "Open PRs (1):",
                "",
                "  #   7  Refactor local CI",
                "         feature/local-ci by dev [ci, refactor]",
            ],
        )

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


class CloudCommandIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"
        self.config_path = root / "config.json"
        self.config_path.write_text(
            json.dumps(
                {
                    "targets": {
                        "mac": {"type": "local", "enabled": True},
                    },
                    "github_actions": {
                        "repository": "danielraffel/pulp",
                        "defaults": {
                            "workflow": "build",
                            "provider": "github-hosted",
                            "wait_poll_secs": 1,
                            "match_timeout_secs": 30,
                        },
                        "workflows": {
                            "build": {
                                "providers": {
                                    "namespace": {
                                        "linux_runner_selector_json": "\"namespace-profile-default\"",
                                        "windows_runner_selector_json": "\"namespace-profile-default\"",
                                    }
                                }
                            },
                            "docs-check": {
                                "providers": {
                                    "namespace": {
                                        "runner_selector_json": "\"namespace-profile-default\""
                                    }
                                }
                            },
                        },
                    },
                    "defaults": {"targets": ["mac"]},
                }
            )
            + "\n"
        )
        self.prev_home = os.environ.get("PULP_LOCAL_CI_HOME")
        self.prev_config = os.environ.get("PULP_LOCAL_CI_CONFIG")
        os.environ["PULP_LOCAL_CI_HOME"] = str(self.state_dir)
        os.environ["PULP_LOCAL_CI_CONFIG"] = str(self.config_path)

    def tearDown(self):
        if self.prev_home is None:
            os.environ.pop("PULP_LOCAL_CI_HOME", None)
        else:
            os.environ["PULP_LOCAL_CI_HOME"] = self.prev_home

        if self.prev_config is None:
            os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        else:
            os.environ["PULP_LOCAL_CI_CONFIG"] = self.prev_config

        self.tmpdir.cleanup()

    def test_cmd_cloud_run_rejects_unsupported_provider(self):
        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="validate",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("does not support provider", output)

    def test_cmd_cloud_run_build_namespace_dispatches_selector_fields(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso
        original_repo_variables = self.mod.gh_repo_variables

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_repo_variables = lambda repository: {}
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso
            self.mod.gh_repo_variables = original_repo_variables

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "linux_runner_selector_json": "\"namespace-profile-default\"",
                "windows_runner_selector_json": "\"namespace-profile-default\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(records[0]["dispatch_fields"]["linux_runner_selector_json"], "\"namespace-profile-default\"")
        self.assertEqual(records[0]["dispatch_fields"]["windows_runner_selector_json"], "\"namespace-profile-default\"")

    def test_cmd_cloud_run_build_namespace_uses_repo_variable_selector_defaults(self):
        config = json.loads(self.config_path.read_text())
        del config["github_actions"]["workflows"]["build"]["providers"]["namespace"]["linux_runner_selector_json"]
        del config["github_actions"]["workflows"]["build"]["providers"]["namespace"]["windows_runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso
        original_repo_variables = self.mod.gh_repo_variables

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_repo_variables = lambda repository: {
            "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON": "\"namespace-profile-linux-repo\"",
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": "\"namespace-profile-windows-repo\"",
        }
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso
            self.mod.gh_repo_variables = original_repo_variables

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "linux_runner_selector_json": "\"namespace-profile-linux-repo\"",
                "windows_runner_selector_json": "\"namespace-profile-windows-repo\"",
            },
        )

    def test_cmd_cloud_run_build_namespace_includes_optional_macos_selector_when_present(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["workflows"]["build"]["providers"]["namespace"][
            "macos_runner_selector_json"
        ] = "\"namespace-profile-macos\""
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"]["macos_runner_selector_json"],
            "\"namespace-profile-macos\"",
        )

    def test_cmd_cloud_run_build_cli_override_adds_one_off_macos_selector(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json="\"namespace-profile-big-apple\"",
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"]["macos_runner_selector_json"],
            "\"namespace-profile-big-apple\"",
        )

    def test_cmd_cloud_run_rejects_build_leg_override_for_docs_check(self):
        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json="\"namespace-profile-big-apple\"",
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("--macos-runner-selector-json is not supported", output)

    def test_cmd_cloud_run_dispatches_waits_and_persists_record(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_view = self.mod.gh_run_view
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: {
            "databaseId": 98765,
            "headBranch": ref,
            "headSha": "e" * 40,
            "status": "in_progress",
            "conclusion": "",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:05+00:00",
            "workflowName": "Docs Consistency",
            "match_ambiguous": False,
        }
        self.mod.gh_run_view = lambda repository, run_id: {
            "databaseId": run_id,
            "status": "completed",
            "conclusion": "success",
            "url": "https://example.test/runs/98765",
            "headSha": "e" * 40,
            "headBranch": "feature/cloud",
            "workflowName": "Docs Consistency",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:10+00:00",
            "jobs": [
                {
                    "name": "Validate docs consistency",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:06+00:00",
                    "completedAt": "2026-04-04T12:00:10+00:00",
                }
            ],
        }
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=True,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.gh_run_view = original_view
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(dispatched["workflow_file"], "docs-check.yml")
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "runner_selector_json": "\"namespace-profile-default\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["run_id"], 98765)
        self.assertEqual(records[0]["provider_resolved"], "namespace")
        self.assertEqual(records[0]["runner_selector_json"], "\"namespace-profile-default\"")
        self.assertEqual(records[0]["conclusion"], "success")

    def test_cmd_cloud_run_explicit_runner_selector_overrides_config_default(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json="\"namespace-profile-big-apple\"",
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "runner_selector_json": "\"namespace-profile-big-apple\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(records[0]["runner_selector_json"], "\"namespace-profile-big-apple\"")

    def test_cmd_cloud_status_shows_runner_selector(self):
        self.mod.save_cloud_record(
            {
                "dispatch_id": "sel123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "runner_selector_json": "\"namespace-profile-default\"",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "started_at": "2026-04-04T12:00:06+00:00",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "queue_delay_secs": 1,
                "duration_secs": 24,
                "usage_summary": {
                    "instances_count": 2,
                    "provider_runtime_secs": 75,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 2,
                            "duration_secs": 75,
                        }
                    ],
                },
                "cost_summary": {
                    "status": "unavailable",
                    "reason": "Namespace CLI does not expose billing totals; provider runtime is shown instead.",
                },
                "jobs": [
                    {
                        "name": "Validate docs consistency",
                        "status": "completed",
                        "conclusion": "success",
                        "started_at": "2026-04-04T12:00:09+00:00",
                        "completed_at": "2026-04-04T12:00:30+00:00",
                    }
                ],
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_status(
                SimpleNamespace(identifier="latest", refresh=False, limit=5)
            )

        self.assertEqual(exit_code, 0)
        self.assertIn("runner selector: namespace-profile-default", buf.getvalue())
        self.assertIn("queue delay: 1s", buf.getvalue())
        self.assertIn("elapsed: 24s", buf.getvalue())
        self.assertIn("provider usage: 2 Namespace instance(s) runtime=1m15s", buf.getvalue())
        self.assertIn("namespace-profile-default: linux/amd64 4 vCPU 8 GB x2 runtime=1m15s", buf.getvalue())
        self.assertIn("cost: unavailable", buf.getvalue())
        self.assertIn("duration=21s", buf.getvalue())

    def test_cmd_cloud_defaults_reports_effective_providers_and_sources(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["defaults"]["provider"] = "namespace"
        del config["github_actions"]["workflows"]["docs-check"]["providers"]["namespace"]["runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        original_repo_variables = self.mod.gh_repo_variables
        self.mod.gh_available = lambda: True
        self.mod.gh_repo_variables = lambda repository: {
            "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": "\"namespace-profile-docs\"",
            "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON": "\"namespace-profile-linux\"",
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": "\"namespace-profile-windows\"",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_defaults(SimpleNamespace())
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.gh_repo_variables = original_repo_variables

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("configured default provider: namespace", output)
        self.assertIn("billing estimates: USD period-day=1 (estimated; verify provider pricing)", output)
        self.assertIn("provider billing truth: disabled (opt-in; off by default)", output)
        self.assertIn("build: Build and Test (build.yml)", output)
        self.assertIn("linux_runner_selector_json: namespace-profile-default", output)
        self.assertIn("docs-check: Docs Consistency (docs-check.yml)", output)
        self.assertIn("runner_selector_json: namespace-profile-docs (repo variable PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON)", output)
        self.assertIn("validate: Plugin Validation (validate.yml)", output)
        self.assertIn("default provider: github-hosted (workflow fallback", output)

    def test_cmd_cloud_defaults_handles_invalid_timing_config(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["defaults"]["wait_poll_secs"] = "not-an-int"
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: False
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_defaults(SimpleNamespace())
        finally:
            self.mod.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("repository: danielraffel/pulp", output)
        self.assertIn("note: github_actions.defaults.wait_poll_secs must be an integer.", output)
        self.assertIn("configured default workflow: build", output)
        self.assertIn("configured default provider: github-hosted", output)

    def test_estimate_cloud_record_cost_uses_namespace_profile_rate(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }

        summary = self.mod.estimate_cloud_record_cost(
            {
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 7200,
                        }
                    ]
                },
            },
            config,
        )

        self.assertEqual(summary["status"], "estimated")
        self.assertEqual(summary["currency"], "USD")
        self.assertAlmostEqual(summary["estimated_total"], 1.0)
        self.assertEqual(summary["reason"], "estimated; verify provider pricing")

    def test_fetch_github_repo_actions_billing_summary_sums_repo_usage(self):
        config = {
            "telemetry": {
                "billing": {
                    "enable_provider_reported_totals": True,
                }
            }
        }

        original_gh_available = self.mod.gh_available
        original_gh_api_json = self.mod.gh_api_json
        original_billing_window = self.mod.billing_period_window
        self.mod.gh_available = lambda: True
        self.mod.billing_period_window = lambda start_day, now_dt=None: (
            datetime(2026, 3, 15, tzinfo=timezone.utc),
            datetime(2026, 4, 15, tzinfo=timezone.utc),
        )

        def fake_gh_api_json(path, fields=None):
            if path == "/repos/danielraffel/pulp":
                return ({"owner": {"login": "danielraffel", "type": "User"}}, "")
            if path == "/users/danielraffel/settings/billing/usage":
                if fields == {"year": 2026, "month": 3}:
                    return (
                        {
                            "usageItems": [
                                {
                                    "date": "2026-03-14",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 1.0,
                                },
                                {
                                    "date": "2026-03-15",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 2.0,
                                },
                            ]
                        },
                        "",
                    )
                if fields == {"year": 2026, "month": 4}:
                    return (
                        {
                            "usageItems": [
                                {
                                    "date": "2026-04-01",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 3.5,
                                },
                                {
                                    "date": "2026-04-02",
                                    "product": "Packages",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 9.0,
                                },
                                {
                                    "date": "2026-04-03",
                                    "product": "Actions",
                                    "repositoryName": "other/repo",
                                    "netAmount": 7.0,
                                },
                            ]
                        },
                        "",
                    )
            return (None, "unexpected call")

        self.mod.gh_api_json = fake_gh_api_json
        try:
            summary = self.mod.fetch_github_repo_actions_billing_summary("danielraffel/pulp", config)
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.gh_api_json = original_gh_api_json
            self.mod.billing_period_window = original_billing_window

        self.assertEqual(summary["status"], "actual")
        self.assertEqual(summary["currency"], "USD")
        self.assertAlmostEqual(summary["actual_total"], 5.5)
        self.assertEqual(summary["matched_items"], 2)
        self.assertEqual(summary["reason"], "actual when available")

    def test_cmd_cloud_history_shows_estimated_cost_and_period_total(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.mod.save_cloud_record(
            {
                "dispatch_id": "hist123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "duration_secs": 24,
                "completed_at": "2026-04-04T12:00:30+00:00",
                "usage_summary": {
                    "instances_count": 1,
                    "provider_runtime_secs": 3600,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 1,
                            "duration_secs": 3600,
                        }
                    ],
                },
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 3600,
                        }
                    ]
                },
            }
        )

        original_billing_period_window = self.mod.billing_period_window
        self.mod.billing_period_window = lambda start_day, now_dt=None: (
            datetime(2026, 4, 1, tzinfo=timezone.utc),
            datetime(2026, 5, 1, tzinfo=timezone.utc),
        )
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_history(
                    SimpleNamespace(workflow=None, provider=None, limit=10)
                )
        finally:
            self.mod.billing_period_window = original_billing_period_window

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("cost=est $0.50", output)
        self.assertIn("period cost: est $0.50 over 1 run(s); estimated; verify provider pricing", output)

    def test_cmd_cloud_history_shows_provider_reported_github_billing_when_enabled(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "enable_provider_reported_totals": True,
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.mod.save_cloud_record(
            {
                "dispatch_id": "histgh123456",
                "workflow_key": "build",
                "workflow_name": "Build and Test",
                "workflow_file": "build.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "run_id": 12345,
                "duration_secs": 30,
                "completed_at": "2026-04-04T12:00:30+00:00",
            }
        )

        original_fetch = self.mod.fetch_github_repo_actions_billing_summary
        self.mod.fetch_github_repo_actions_billing_summary = lambda repository, cfg: {
            "status": "actual",
            "currency": "USD",
            "actual_total": 2.7,
            "reason": "actual when available",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_history(
                    SimpleNamespace(workflow=None, provider=None, limit=10)
                )
        finally:
            self.mod.fetch_github_repo_actions_billing_summary = original_fetch

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("github repo billing: actual $2.70 current period (repo-wide)", output)

    def test_cmd_cloud_compare_reports_provider_medians(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "github_hosted_job_os_rates_per_minute": {
                    "linux": 0.01
                },
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.mod.save_cloud_record(
            {
                "dispatch_id": "cmpns123456",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "duration_secs": 120,
                "queue_delay_secs": 5,
                "usage_summary": {
                    "provider_runtime_secs": 3600,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 1,
                            "duration_secs": 3600,
                        }
                    ],
                },
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 3600,
                        }
                    ]
                },
            }
        )
        self.mod.save_cloud_record(
            {
                "dispatch_id": "cmpgh123456",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:10:30+00:00",
                "duration_secs": 180,
                "queue_delay_secs": 15,
                "jobs": [
                    {
                        "name": "Linux (x64) [github-hosted]",
                        "started_at": "2026-04-04T12:07:30+00:00",
                        "completed_at": "2026-04-04T12:10:30+00:00",
                    }
                ],
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_compare(SimpleNamespace(workflow="build"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn(
            "github-hosted: runs=1 success=1/1 median_elapsed=3m00s median_queue=15s median_cost=est $0.03 latest_success=2026-04-04T12:10:30+00:00",
            output,
        )
        self.assertIn(
            "namespace: runs=1 success=1/1 median_elapsed=2m00s median_queue=5s median_provider_time=1h00m00s median_cost=est $0.50 latest_success=2026-04-04T12:00:30+00:00",
            output,
        )
        self.assertIn("note: estimated; verify provider pricing", output)

    def test_cmd_cloud_recommend_prefers_fastest_observed_provider(self):
        self.mod.save_cloud_record(
            {
                "dispatch_id": "recns123456",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "duration_secs": 120,
            }
        )
        self.mod.save_cloud_record(
            {
                "dispatch_id": "recgh123456",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:10:30+00:00",
                "duration_secs": 180,
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_recommend(SimpleNamespace(workflow="build"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Recommended provider for build: namespace (fastest observed median)", output)
        self.assertIn("note: estimated; verify provider pricing", output)

    def test_cmd_cloud_run_wait_fails_when_refresh_cannot_fetch_github_state(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_view = self.mod.gh_run_view
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_workflow_dispatch = lambda repository, workflow_file, ref, fields: None
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: {
            "databaseId": 98765,
            "workflowName": "Docs Consistency",
            "headBranch": "feature/cloud",
            "headSha": "a" * 40,
            "status": "in_progress",
            "conclusion": "",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:06+00:00",
            "jobs": [],
        }
        self.mod.gh_run_view = lambda repository, run_id: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=True,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.gh_run_view = original_view
            self.mod.now_iso = original_now_iso

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("Error: Failed to refresh GitHub run 98765 from danielraffel/pulp.", output)


    def test_cmd_cloud_status_refresh_uses_record_repository(self):
        self.mod.save_cloud_record(
            {
                "dispatch_id": "repo123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "other-owner/other-repo",
                "requested_ref": "feature/cloud",
                "provider_requested": "github-hosted",
                "status": "in_progress",
                "run_id": 77777,
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )

        original_gh_available = self.mod.gh_available
        original_view = self.mod.gh_run_view
        seen = {}
        self.mod.gh_available = lambda: True
        self.mod.gh_run_view = lambda repository, run_id: (
            seen.update({"repository": repository, "run_id": run_id}) or {
                "databaseId": 77777,
                "workflowName": "Docs Consistency",
                "headBranch": "feature/cloud",
                "headSha": "a" * 40,
                "status": "completed",
                "conclusion": "success",
                "url": "https://example.test/runs/77777",
                "createdAt": "2026-04-04T12:00:05+00:00",
                "updatedAt": "2026-04-04T12:00:30+00:00",
                "jobs": [],
            }
        )
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_status(
                    SimpleNamespace(identifier="latest", refresh=True, limit=5)
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.gh_run_view = original_view

        self.assertEqual(exit_code, 0)
        self.assertEqual(seen["repository"], "other-owner/other-repo")
        self.assertEqual(seen["run_id"], 77777)



if __name__ == "__main__":
    unittest.main()
