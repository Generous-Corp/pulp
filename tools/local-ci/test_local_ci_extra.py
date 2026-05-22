#!/usr/bin/env python3
"""Additional pure-helper coverage for tools/local-ci/local_ci.py."""

from __future__ import annotations

import importlib.util
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from argparse import Namespace
from contextlib import redirect_stdout
from datetime import datetime, timezone
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    spec = importlib.util.spec_from_file_location("pulp_local_ci_extra", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class LocalCiPureHelperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        # R2-1 (#2645): cloud helpers moved to cloud.py — patch the cloud module.
        self.cloud = importlib.import_module("cloud")
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_state_dir_uses_xdg_and_home_fallbacks_on_non_macos(self) -> None:
        with mock.patch.object(self.mod.Path, "home", return_value=self.root / "home"):
            with mock.patch.object(self.mod.sys, "platform", "darwin"):
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.state_dir(),
                        self.root / "home" / "Library" / "Application Support" / "Pulp" / "local-ci",
                    )

        with mock.patch.object(self.mod.Path, "home", return_value=self.root / "home"):
            with mock.patch.object(self.mod.sys, "platform", "linux"):
                with mock.patch.dict(os.environ, {"XDG_STATE_HOME": str(self.root / "xdg")}, clear=True):
                    self.assertEqual(
                        self.mod.state_dir(),
                        self.root / "xdg" / "pulp" / "local-ci",
                    )
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.state_dir(),
                        self.root / "home" / ".local" / "state" / "pulp" / "local-ci",
                    )

    def test_state_path_helpers_create_expected_directories_and_logs(self) -> None:
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
            self.mod.ensure_state_dirs()
            for path in (
                self.mod.results_dir(),
                self.mod.cloud_runs_dir(),
                self.mod.logs_dir(),
                self.mod.bundles_dir(),
                self.mod.desktop_state_dir(),
                self.mod.desktop_receipts_dir(),
            ):
                self.assertTrue(path.is_dir(), f"{path} should exist")

            log_path = self.mod.prepare_target_log("job-1", "mac")
            self.assertEqual(log_path, self.mod.target_log_path("job-1", "mac"))
            self.assertTrue(log_path.is_file())
            self.assertEqual(log_path.read_text(), "")

    def test_default_desktop_artifact_root_platform_branches(self) -> None:
        with mock.patch.object(self.mod.Path, "home", return_value=self.root / "home"):
            with mock.patch.dict(os.environ, {"PULP_DESKTOP_ARTIFACT_ROOT": str(self.root / "override")}, clear=True):
                self.assertEqual(self.mod.default_desktop_artifact_root(), self.root / "override")

            with mock.patch.object(self.mod.sys, "platform", "darwin"):
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.default_desktop_artifact_root(),
                        self.root / "home" / "Library" / "Application Support" / "Pulp" / "desktop-automation" / "runs",
                    )

            with mock.patch.object(self.mod.sys, "platform", "win32"):
                with mock.patch.dict(os.environ, {"LOCALAPPDATA": str(self.root / "localapp")}, clear=True):
                    self.assertEqual(
                        self.mod.default_desktop_artifact_root(),
                        self.root / "localapp" / "Pulp" / "desktop-automation" / "runs",
                    )

            with mock.patch.object(self.mod.sys, "platform", "linux"):
                with mock.patch.dict(os.environ, {"XDG_STATE_HOME": str(self.root / "xdg")}, clear=True):
                    self.assertEqual(
                        self.mod.default_desktop_artifact_root(),
                        self.root / "xdg" / "pulp" / "desktop-automation" / "runs",
                    )
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.default_desktop_artifact_root(),
                        self.root / "home" / ".local" / "state" / "pulp" / "desktop-automation" / "runs",
                    )

    def test_normalizers_reject_invalid_modes_and_parse_booleans(self) -> None:
        self.assertEqual(self.mod.normalize_validation_mode(" SMOKE "), "smoke")
        with self.assertRaisesRegex(ValueError, "Invalid validation mode"):
            self.mod.normalize_validation_mode("quick")

        self.assertEqual(self.mod.normalize_desktop_source_mode("exact_sha"), "exact-sha")
        with self.assertRaisesRegex(ValueError, "Invalid desktop source mode"):
            self.mod.normalize_desktop_source_mode("snapshot")

        self.assertEqual(self.mod.normalize_publish_mode("issue-comment"), "issue-comment")
        with self.assertRaisesRegex(ValueError, "Invalid desktop publish mode"):
            self.mod.normalize_publish_mode("slack")

        for value in (True, 1, 0.5, "yes", "ON"):
            self.assertTrue(self.mod.parse_config_bool(value))
        for value in (False, 0, "", "no", "off"):
            self.assertFalse(self.mod.parse_config_bool(value))
        with self.assertRaisesRegex(ValueError, "Invalid boolean value"):
            self.mod.parse_config_bool("maybe")

    def test_desktop_adapter_defaults_cover_fallbacks(self) -> None:
        self.assertEqual(self.mod.infer_desktop_adapter("mac", {"type": "local"}), "macos-local")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {"type": "local"}), "local-window")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {"type": "ssh"}), "remote-session-agent")
        self.assertEqual(self.mod.infer_desktop_adapter("custom", {}), "unknown")
        self.assertEqual(self.mod.default_desktop_bootstrap("custom"), "manual")
        self.assertEqual(self.mod.default_desktop_capability_tier("custom"), "v1")

    def test_probe_uploaded_bundle_size_handles_outputs(self) -> None:
        config = {"targets": {"windows": {"host": "win", "repo_path": r"C:\\Pulp"}}}
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="noise\n4096\n", stderr=""),
        ) as run:
            self.assertEqual(
                self.mod.probe_uploaded_bundle_size("win", "bundle.git", config=config),
                4096,
            )
            self.assertIn("cmd /V:OFF", run.call_args.args[0][-1])

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="not-a-number\n", stderr=""),
        ):
            self.assertIsNone(
                self.mod.probe_uploaded_bundle_size("ubuntu", "bundle.git", config={"targets": {}})
            )

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, stdout="", stderr="failed"),
        ):
            self.assertIsNone(
                self.mod.probe_uploaded_bundle_size("ubuntu", "bundle.git", config={"targets": {}})
            )

    def test_github_workflow_defaults_cover_config_repo_and_cli_edges(self) -> None:
        config = {
            "github_actions": {
                "repository": " danielraffel/pulp ",
                "defaults": {
                    "workflow": "docs-check",
                    "provider": "namespace",
                    "wait_poll_secs": "7",
                    "match_timeout_secs": "11",
                },
                "workflows": {
                    "docs-check": {
                        "providers": {
                            "namespace": {
                                "runner_selector_json": '["namespace-profile", "macos"]',
                            }
                        }
                    },
                    "build": {
                        "providers": {
                            "namespace": {
                                "linux_runner_selector_json": '"linux-large"',
                                "windows_runner_selector_json": "",
                            }
                        }
                    },
                },
            }
        }
        settings = self.mod.resolve_github_actions_settings(config)
        self.assertEqual(settings["repository"], "danielraffel/pulp")
        self.assertEqual(settings["workflow"], "docs-check")
        self.assertEqual(settings["provider"], "namespace")
        self.assertEqual(settings["wait_poll_secs"], 7)
        self.assertEqual(settings["match_timeout_secs"], 11)

        summary = self.mod.summarize_workflow_provider_defaults(
            config,
            {"PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": '"windows-large"'},
            settings,
            "docs-check",
        )
        self.assertEqual(summary["provider"], "namespace")
        self.assertEqual(summary["provider_source"], "github_actions.defaults.provider")
        self.assertEqual(summary["selector_input"], "runner_selector_json")
        self.assertEqual(summary["selector_value"], '["namespace-profile", "macos"]')
        self.assertEqual(
            self.mod.resolve_default_provider_for_workflow({"provider": "namespace"}, "validate"),
            ("github-hosted", "workflow fallback (default provider 'namespace' unsupported)"),
        )

        fields, sources = self.mod.resolve_workflow_dispatch_defaults(
            config,
            {
                "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": '"windows-large"',
                "PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON": '["macos", "arm64"]',
            },
            "build",
            "namespace",
            ("linux_runner_selector_json", "windows_runner_selector_json", "macos_runner_selector_json"),
        )
        self.assertEqual(
            fields,
            {
                "linux_runner_selector_json": '"linux-large"',
                "windows_runner_selector_json": '"windows-large"',
                "macos_runner_selector_json": '["macos", "arm64"]',
            },
        )
        self.assertIn("config github_actions.workflows.build.providers.namespace", sources["linux_runner_selector_json"])
        self.assertEqual(sources["windows_runner_selector_json"], "repo variable PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON")

        args = Namespace(
            linux_runner_selector_json='"linux-cli"',
            windows_runner_selector_json=None,
            macos_runner_selector_json=None,
        )
        self.assertEqual(
            self.mod.resolve_cli_dispatch_field_values(args, ("linux_runner_selector_json",)),
            {"linux_runner_selector_json": '"linux-cli"'},
        )
        with self.assertRaisesRegex(ValueError, "not supported"):
            self.mod.resolve_cli_dispatch_field_values(args, ())
        with self.assertRaisesRegex(ValueError, "must be valid JSON"):
            self.mod.normalize_runs_on_json("{", setting_name="bad.selector")
        with self.assertRaisesRegex(ValueError, "must decode to a string or array"):
            self.mod.normalize_runs_on_json('{"runs-on": "ubuntu"}', setting_name="bad.selector")
        with self.assertRaisesRegex(ValueError, "must be positive"):
            self.mod.resolve_github_actions_settings({"github_actions": {"defaults": {"wait_poll_secs": 0}}})

    def test_provenance_job_and_supersedence_helpers_cover_edges(self) -> None:
        hosted = {
            "execution_kind": "hosted",
            "hosted_orchestrator": "github-actions",
            "runner_provider": "namespace",
            "runner_selector": "linux-large",
            "run_id": "123",
        }
        self.assertEqual(
            self.mod.provenance_summary(hosted),
            "hosted via github-actions/namespace selector=linux-large run=123",
        )
        self.assertEqual(
            self.mod.provenance_summary({"direct_backend": ""}),
            "direct via local-ci",
        )
        self.assertEqual(
            self.mod.normalize_result({"submission": {"provenance": hosted}})["provenance"]["runner_provider"],
            "namespace",
        )

        legacy = self.mod.normalize_job(
            {
                "branch": "feature/local-ci",
                "sha": "a" * 40,
                "queued_at": "2026-04-30T00:00:00+00:00",
                "targets": ["windows", "mac", "windows"],
                "validation": " SMOKE ",
            }
        )
        self.assertEqual(len(legacy["id"]), 12)
        self.assertEqual(legacy["priority"], "normal")
        self.assertEqual(legacy["targets"], ["mac", "windows"])
        self.assertEqual(legacy["validation"], "smoke")
        self.assertEqual(legacy["submission"]["provenance"]["direct_backend"], "local-ci")
        self.assertIn("validation=smoke", self.mod.summarize_job(legacy))
        self.assertEqual(self.mod.summarize_active_targets(None), "")
        self.assertEqual(
            self.mod.summarize_active_targets(
                {"windows": {"status": "running"}, "mac": {"status": "queued"}},
                preferred_order=["mac"],
            ),
            "mac=queued, windows=running",
        )

        older = {
            "id": "older",
            "branch": "feature/local-ci",
            "sha": "a" * 40,
            "fingerprint": "old",
            "targets": ["mac", "windows"],
            "validation": "full",
            "priority": "normal",
            "queued_at": "2026-04-30T00:00:00+00:00",
        }
        newer_same_scope = dict(older, id="newer", sha="b" * 40, fingerprint="new")
        newer_narrower = dict(older, id="narrow", fingerprint="narrow", targets=["mac"])
        self.assertTrue(self.mod.jobs_share_supersedence_scope(newer_same_scope, older))
        self.assertEqual(self.mod.supersedence_reason(newer_same_scope, older), "newer_sha_queued")
        self.assertEqual(self.mod.supersedence_reason(newer_narrower, older), "narrower_scope_queued")
        self.assertIsNone(self.mod.supersedence_reason(older, older))
        self.assertEqual(
            self.mod.supersedence_result(older, "newer", "newer_sha_queued")["overall"],
            "superseded",
        )
        self.assertEqual(self.mod.cancellation_result(older, "operator")["canceled_reason"], "operator")

    def test_billing_and_cloud_helpers_cover_estimated_and_unavailable_paths(self) -> None:
        config = {
            "telemetry": {
                "billing": {
                    "currency": "eur",
                    "billing_period_start_day": "15",
                    "enable_provider_reported_totals": True,
                    "github_hosted_job_os_rates_per_minute": {
                        "Linux": "0.02",
                        "": "99",
                        "macOS": "-1",
                    },
                    "namespace_profile_tag_rates_per_hour": {
                        "coverage-linux": "1.5",
                        "ignored": "bad",
                    },
                    "namespace_machine_shape_rates_per_hour": [
                        {
                            "os": "linux",
                            "arch": "arm64",
                            "virtual_cpu": "4",
                            "memory_megabytes": "8192",
                            "rate": "2.25",
                        },
                        "bad",
                        {"rate": "bad"},
                    ],
                }
            }
        }
        billing = self.cloud.resolve_billing_settings(config)
        self.assertEqual(billing["currency"], "EUR")
        self.assertEqual(billing["billing_period_start_day"], 15)
        self.assertTrue(billing["enable_provider_reported_totals"])
        self.assertEqual(billing["github_hosted_job_os_rates_per_minute"], {"linux": 0.02})
        self.assertEqual(billing["namespace_profile_tag_rates_per_hour"], {"coverage-linux": 1.5})
        self.assertEqual(billing["namespace_machine_shape_rates_per_hour"][0]["virtual_cpu"], 4)

        with self.assertRaisesRegex(ValueError, "between 1 and 28"):
            self.cloud.resolve_billing_settings({"telemetry": {"billing": {"billing_period_start_day": 29}}})
        with self.assertRaisesRegex(ValueError, "must be true or false"):
            self.cloud.resolve_billing_settings({"telemetry": {"billing": {"enable_provider_reported_totals": "yes"}}})

        start, end = self.cloud.billing_period_window(
            15,
            now_dt=datetime(2026, 1, 10, tzinfo=timezone.utc),
        )
        self.assertEqual((start.year, start.month, start.day), (2025, 12, 15))
        self.assertEqual((end.year, end.month, end.day), (2026, 1, 15))
        self.assertEqual(self.cloud.iter_year_months(start, datetime(2026, 2, 15, tzinfo=timezone.utc)), [(2025, 12), (2026, 1), (2026, 2)])
        self.assertEqual(self.cloud.parse_iso_date("2026-04-30").isoformat(), "2026-04-30")
        self.assertIsNone(self.cloud.parse_iso_date("bad"))
        self.assertEqual(self.cloud.infer_job_os("docs-check", "lint"), "linux")
        self.assertEqual(self.cloud.infer_job_os("build", "mac (arm64)"), "macos")
        self.assertEqual(self.cloud.infer_job_os("build", "unknown"), "")

        namespace_record = {
            "provider_resolved": "namespace",
            "provider_metadata": {
                "namespace_instances": [
                    {"profile_tag": "coverage-linux", "duration_secs": 1800},
                    {"os": "linux", "arch": "arm64", "virtual_cpu": 4, "memory_megabytes": 8192, "duration_secs": 3600},
                    {"profile_tag": "unpriced", "duration_secs": 3600},
                ]
            },
        }
        namespace_cost = self.cloud.estimate_cloud_record_cost(namespace_record, config)
        self.assertEqual(namespace_cost["status"], "estimated")
        self.assertEqual(namespace_cost["currency"], "EUR")
        self.assertEqual(namespace_cost["estimated_total"], 3.0)
        self.assertEqual(
            self.cloud.estimate_namespace_cost({"provider_metadata": {}, "usage_summary": {}}, billing)["status"],
            "unavailable",
        )

        github_record = {
            "provider_resolved": "github-hosted",
            "workflow_key": "build",
            "jobs": [
                {"name": "resolve-provider", "started_at": "2026-04-30T00:00:00Z", "completed_at": "2026-04-30T00:01:00Z"},
                {"name": "Linux Tests", "started_at": "2026-04-30T00:00:00Z", "completed_at": "2026-04-30T00:02:30Z"},
                {"name": "Unknown", "started_at": "bad", "completed_at": "2026-04-30T00:02:30Z"},
            ],
        }
        github_cost = self.cloud.estimate_cloud_record_cost(github_record, config)
        self.assertEqual(github_cost["status"], "estimated")
        self.assertEqual(github_cost["estimated_total"], 0.05)
        self.assertEqual(
            self.cloud.estimate_cloud_record_cost({"provider_resolved": "other"}, config)["reason"],
            "no estimator for provider 'other'",
        )

    def test_cloud_record_formatting_timing_and_namespace_usage_edges(self) -> None:
        normalized = self.cloud.normalize_cloud_record(
            {
                "dispatch_id": "abc123",
                "dispatch_fields": "bad",
                "jobs": "bad",
                "provider_metadata": "bad",
                "usage_summary": "bad",
                "cost_summary": "bad",
            }
        )
        self.assertEqual(normalized["dispatch_fields"], {})
        self.assertEqual(normalized["jobs"], [])
        self.assertEqual(normalized["provider_metadata"], {})

        records = [
            {"dispatch_id": "abc111", "run_id": 42, "completed_at": "2026-04-30T00:00:00+00:00"},
            {"dispatch_id": "abc222", "run_id": 42, "updated_at": "2026-04-30T01:00:00+00:00"},
            {"dispatch_id": "xyz999", "run_id": 99, "matched_at": "2026-04-30T02:00:00+00:00"},
        ]
        self.assertEqual(self.cloud.find_cloud_record(records, "latest")["dispatch_id"], "abc111")
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.cloud.find_cloud_record(records, "abc")
        with self.assertRaisesRegex(ValueError, "matched multiple"):
            self.cloud.find_cloud_record(records, "42")
        self.assertEqual(self.cloud.cloud_record_sort_key(records[1])[0], "2026-04-30T01:00:00+00:00")

        self.assertEqual(self.cloud.summarize_runner_selector('"macos-large"'), "macos-large")
        self.assertEqual(self.cloud.summarize_runner_selector('["linux", "arm64"]'), "linux,arm64")
        self.assertEqual(self.cloud.summarize_runner_selector('{"bad": true}'), '{"bad": true}')
        self.assertEqual(self.cloud.summarize_runner_selector("{bad"), "{bad")
        self.assertEqual(self.cloud.normalize_github_timestamp("0001-01-01T00:00:00Z"), "")
        self.assertEqual(self.cloud.duration_between("2026-04-30T00:00:03Z", "2026-04-30T00:00:01Z"), 0.0)
        self.assertIsNone(self.cloud.duration_between("bad", "2026-04-30T00:00:01Z"))
        self.assertEqual(self.cloud.format_duration_secs(3661), "1h01m01s")
        self.assertEqual(self.cloud.format_duration_secs(61), "1m01s")
        self.assertEqual(self.cloud.format_duration_secs(1.25), "1.2s")
        self.assertEqual(self.cloud.format_duration_secs(-1), "")
        self.assertEqual(self.cloud.format_duration_secs("bad"), "")
        self.assertEqual(self.cloud.format_memory_megabytes(2048), "2 GB")
        self.assertEqual(self.cloud.format_memory_megabytes("bad"), "")
        self.assertEqual(self.cloud.render_selector_value('"runner"'), "runner")
        self.assertEqual(self.cloud.parse_rate_value("1.25"), 1.25)
        self.assertIsNone(self.cloud.parse_rate_value(-1))
        self.assertIsNone(self.cloud.parse_optional_bool("", "flag"))
        self.assertTrue(self.cloud.parse_optional_bool(True, "flag"))
        with self.assertRaisesRegex(ValueError, "must be true or false"):
            self.cloud.parse_optional_bool("true", "flag")

        timing = self.cloud.summarize_cloud_timing(
            {
                "createdAt": "2026-04-30T00:00:00Z",
                "updatedAt": "2026-04-30T00:02:00Z",
                "status": "completed",
                "jobs": [
                    {
                        "startedAt": "2026-04-30T00:01:00Z",
                        "completedAt": "2026-04-30T00:03:00Z",
                        "steps": [{"startedAt": "2026-04-30T00:01:05Z", "completedAt": "2026-04-30T00:02:55Z"}],
                    }
                ],
            }
        )
        self.assertEqual(timing["queue_delay_secs"], 60.0)
        self.assertEqual(timing["duration_secs"], 120.0)

        instance = self.cloud.normalize_namespace_instance(
            {
                "cluster_id": "cluster",
                "created": "2026-04-30T00:00:00Z",
                "destroyed_at": "2026-04-30T01:00:00Z",
                "shape": {"os": "linux", "machine_arch": "arm64", "virtual_cpu": 4, "memory_megabytes": 8192},
                "user_label": {"nsc.runner-profile-tag": "coverage-linux", "nsc.runner-profile-id": "profile"},
                "github_workflow": {"repository": "danielraffel/pulp", "run_id": 123, "workflow": "Build"},
            }
        )
        self.assertEqual(instance["duration_secs"], 3600.0)
        usage = self.cloud.summarize_namespace_usage([instance, dict(instance, duration_secs=1800.0)])
        self.assertEqual(usage["instances_count"], 2)
        self.assertEqual(usage["provider_runtime_secs"], 5400.0)
        self.assertEqual(usage["machine_shapes"][0]["count"], 2)

        with mock.patch.object(self.mod, "nsc_logged_in", return_value=False):
            self.assertEqual(
                self.cloud.enrich_cloud_record_provider_metadata({"provider_resolved": "github-hosted", "run_id": 123})["usage_summary"],
                {},
            )

    def test_evidence_helpers_rebuild_filter_and_skip_stale_results(self) -> None:
        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
            self.mod.ensure_state_dirs()
            result_path = self.mod.results_dir() / "result-feature-abc123.json"
            result = {
                "job_id": "job1",
                "branch": "feature/local-ci",
                "sha": "a" * 40,
                "validation": "smoke",
                "completed_at": "2026-04-30T00:00:00+00:00",
                "provenance": {"execution_kind": "hosted", "hosted_orchestrator": "github-actions"},
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 12.5},
                    {"target": "windows", "status": "fail", "duration_secs": 2},
                ],
            }
            result_path.write_text(self.mod.json.dumps(result))
            (self.mod.results_dir() / "broken.json").write_text("{")

            index, rebuilt = self.mod.load_evidence_index_unlocked()
            self.assertTrue(rebuilt)
            self.assertEqual(len(index["entries"]), 1)
            self.assertFalse(
                self.mod.merge_result_into_evidence_index(
                    index,
                    dict(result, completed_at="2026-04-29T00:00:00+00:00"),
                    result_path,
                )
            )
            newer = dict(result, completed_at="2026-05-01T00:00:00+00:00")
            self.assertTrue(self.mod.merge_result_into_evidence_index(index, newer, result_path))

            with mock.patch.object(self.mod, "load_evidence_index", return_value=index):
                groups = self.mod.collect_evidence_groups(branch="feature/local-ci", sha="a" * 40)
            self.assertEqual(list(groups), ["smoke"])
            self.assertEqual(groups["smoke"][0]["targets"]["mac"]["status"], "pass")
            self.assertEqual(
                self.mod.evidence_entry_key("branch", "sha", "target", "full"),
                "branch:sha:full:target",
            )

            legacy_path = self.mod.evidence_path()
            legacy_path.write_text('{"version": 1, "entries": []}')
            index, rebuilt = self.mod.load_evidence_index_unlocked()
            self.assertTrue(rebuilt)
            self.assertEqual(index["version"], 3)

    def test_view_tree_coordinate_and_process_helpers_cover_edge_paths(self) -> None:
        self.assertEqual(self.mod.parse_coordinate_pair(" 10.5, 20 ", flag_name="--click"), (10.5, 20.0))
        with self.assertRaisesRegex(ValueError, "X,Y form"):
            self.mod.parse_coordinate_pair("10", flag_name="--click")
        with self.assertRaisesRegex(ValueError, "numeric"):
            self.mod.parse_coordinate_pair("x,y", flag_name="--click")

        view_tree = {
            "id": "root",
            "bounds": {"x": 10, "y": 20, "width": 200, "height": 100},
            "children": [
                {"id": "hidden", "visible": False, "bounds": {"x": 1, "y": 1, "width": 20, "height": 20}},
                {"id": "zero", "type": "button", "bounds": {"x": 2, "y": 2, "width": 0, "height": 20}},
                {
                    "id": "panel",
                    "bounds": {"x": 5, "y": 6, "width": 100, "height": 80},
                    "children": [
                        {"id": "target", "type": "button", "text": "OK", "label": "Confirm", "bounds": {"x": 7, "y": 8, "width": 30, "height": 10}},
                    ],
                },
            ],
        }
        nodes = list(self.mod.iter_view_tree_nodes(view_tree))
        self.assertEqual(list(self.mod.iter_view_tree_nodes("not-a-node")), [])
        self.assertEqual(nodes[-1][1], {"x": 22.0, "y": 34.0, "width": 30.0, "height": 10.0})
        self.assertEqual(nodes[0][1]["x"], 10.0)
        self.assertEqual(nodes[0][1]["height"], 100.0)
        self.assertEqual(
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id="target",
                view_type="button",
                view_text="OK",
                view_label="Confirm",
            ),
            (37.0, 39.0),
        )
        self.assertEqual(
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id=None,
                view_type="button",
                view_text="OK",
                view_label=None,
            ),
            (37.0, 39.0),
        )
        with self.assertRaisesRegex(RuntimeError, "No visible view matched"):
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id="missing",
                view_type=None,
                view_text=None,
                view_label=None,
            )
        self.assertEqual(
            self.mod.screen_point_for_content_point(
                {"bounds": {"x": 100, "y": 50, "width": 400, "height": 300}},
                (200, 180),
                (10, 20),
            ),
            (210.0, 190.0),
        )
        self.assertEqual(
            self.mod.screen_point_for_content_point(
                {"bounds": {"x": 5, "y": 6, "width": 20, "height": 10}},
                (40, 30),
                (3, 4),
            ),
            (8.0, 10.0),
        )

        running = mock.Mock()
        running.poll.return_value = None
        running.wait.side_effect = [subprocess.TimeoutExpired(["proc"], 1), None]
        self.mod.terminate_process(running, timeout_secs=0.01)
        running.terminate.assert_called_once()
        running.kill.assert_called_once()

        complete = mock.Mock()
        complete.poll.return_value = 0
        self.mod.terminate_process(complete)
        complete.terminate.assert_not_called()

    def test_macos_capture_and_local_worktree_helpers_cover_retry_edges(self) -> None:
        output_path = self.root / "captures" / "window.png"

        def successful_capture(*_args, **_kwargs):
            output_path.write_bytes(b"png")
            return subprocess.CompletedProcess([], 0, stdout="", stderr="")

        with mock.patch.object(self.mod.subprocess, "run", side_effect=successful_capture) as run:
            self.mod.capture_macos_window(42, output_path)
        self.assertEqual(output_path.read_bytes(), b"png")
        self.assertTrue(output_path.parent.is_dir())
        self.assertEqual(run.call_args.args[0][0], "screencapture")
        self.assertIn("-l", run.call_args.args[0])
        self.assertIn("42", run.call_args.args[0])

        failed_output = self.root / "captures" / "missing.png"
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, stdout="stdout detail", stderr=""),
        ), mock.patch.object(self.mod.time, "sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, "stdout detail"):
                self.mod.capture_macos_window(99, failed_output)
        self.assertEqual(sleep.call_count, 4)
        self.assertFalse(failed_output.exists())
        self.assertEqual(sleep.call_args.args[0], 0.2)

        missing = self.root / "missing-worktree"
        self.assertFalse(self.mod._local_worktree_matches(missing, "abc123"))
        worktree = self.root / "worktree"
        worktree.mkdir()
        (worktree / ".git").write_text("gitdir: elsewhere\n", encoding="utf-8")
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="abc123\n", stderr=""),
        ) as run:
            self.assertTrue(self.mod._local_worktree_matches(worktree, "abc123"))
        self.assertEqual(run.call_args.args[0][2], str(worktree))
        self.assertEqual(run.call_args.kwargs["text"], True)
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="other\n", stderr=""),
        ):
            self.assertFalse(self.mod._local_worktree_matches(worktree, "abc123"))
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 128, stdout="", stderr="bad git"),
        ):
            self.assertFalse(self.mod._local_worktree_matches(worktree, "abc123"))

    def test_filesystem_helpers_cover_tails_atomic_writes_and_image_hashes(self) -> None:
        self.assertEqual(self.mod.tail_lines(self.root / "missing.log"), [])
        log = self.root / "job.log"
        log.write_text("one\ntwo\nthree\n")
        self.assertEqual(self.mod.tail_lines(log, limit=2), ["two\n", "three\n"])
        self.assertEqual(self.mod.trim_line(" short ", max_len=10), "short")
        self.assertEqual(self.mod.trim_line("abcdef", max_len=4), "…def")

        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
            output = self.root / "nested" / "result.json"
            output.parent.mkdir()
            self.mod.atomic_write_text(output, "payload")
            self.assertEqual(output.read_text(), "payload")

        before = self.root / "before.bin"
        after = self.root / "after.bin"
        before.write_bytes(b"abc")
        after.write_bytes(b"abd")
        summary = self.mod.image_change_summary(before, after)
        self.assertTrue(summary["changed"])
        self.assertIn(summary["method"], {"file-hash", "pixel-bbox"})

    def test_state_paths_git_helpers_and_ssh_retry_edges(self) -> None:
        with mock.patch.object(self.mod.Path, "home", return_value=self.root / "home"):
            with mock.patch.object(self.mod.sys, "platform", "darwin"):
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.state_dir(),
                        self.root / "home" / "Library" / "Application Support" / "Pulp" / "local-ci",
                    )

        script_dir = self.root / "script"
        state_dir = self.root / "state"
        shared_config = state_dir / "config.json"
        shared_config.parent.mkdir(parents=True)
        shared_config.write_text("{}\n")
        state_paths_mod = sys.modules["state_paths"]
        with mock.patch.object(state_paths_mod, "SCRIPT_DIR", script_dir):
            with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(state_dir)}, clear=True):
                self.assertEqual(self.mod.config_path(), shared_config)
                shared_config.unlink()
                self.assertEqual(self.mod.config_path(), script_dir / "config.json")
                self.assertEqual(self.mod.worktree_config_path(), script_dir / "config.json")
                self.assertEqual(self.mod.shared_config_path(), state_dir / "config.json")
                self.assertEqual(self.mod.drain_lock_path(), state_dir / "drain.lock")
                self.assertEqual(self.mod.runner_info_path(), state_dir / "runner.json")

        transient = subprocess.CompletedProcess(["ssh"], 255, stdout="", stderr="Connection reset by peer")
        success = subprocess.CompletedProcess(["ssh"], 0, stdout="ok", stderr="")
        with mock.patch.object(self.mod.subprocess, "run", side_effect=[transient, success]) as run:
            with mock.patch.object(self.mod.time, "sleep") as sleep:
                result = self.mod.run_ssh_subprocess(
                    ["ssh", "host"],
                    input="payload",
                    timeout=5,
                    retries=3,
                    retry_delay_secs=0.25,
                )
        self.assertEqual(result.stdout, "ok")
        self.assertEqual(run.call_count, 2)
        sleep.assert_called_once_with(0.25)

        permanent = subprocess.CompletedProcess(["ssh"], 255, stdout="", stderr="permission denied")
        with mock.patch.object(self.mod.subprocess, "run", return_value=permanent) as run:
            with mock.patch.object(self.mod.time, "sleep") as sleep:
                self.assertIs(self.mod.run_ssh_subprocess(["ssh", "host"]), permanent)
        run.assert_called_once()
        sleep.assert_not_called()

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr="fatal"),
        ):
            self.assertIsNone(self.mod.git_root_for(self.root))
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout=f"{self.root}\n", stderr=""),
        ):
            self.assertEqual(self.mod.git_root_for(self.root), self.root.resolve())
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout="feature/local-ci\n", stderr=""),
        ):
            self.assertEqual(self.mod.current_branch(), "feature/local-ci")
        sha = "f" * 40
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout=f"{sha}\n", stderr=""),
        ):
            self.assertEqual(self.mod.current_sha(), sha)
            self.assertEqual(self.mod.resolve_git_ref_sha("HEAD"), sha)
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr="bad ref"),
        ):
            with self.assertRaisesRegex(ValueError, "bad ref"):
                self.mod.resolve_git_ref_sha("missing")

    def test_config_workflow_and_target_resolution_edge_paths(self) -> None:
        with self.assertRaisesRegex(FileNotFoundError, "Local CI config not found"):
            self.mod.load_config_file(self.root / "missing.json")
        state_paths_mod = sys.modules["state_paths"]
        with mock.patch.object(state_paths_mod, "SCRIPT_DIR", self.root):
            with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
                self.assertIsNone(self.mod.load_optional_config())

        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json({"github_actions": {"workflows": []}}, "build", "namespace"),
            "",
        )
        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(
                {"github_actions": {"workflows": {"build": []}}},
                "build",
                "namespace",
            ),
            "",
        )
        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(
                {"github_actions": {"workflows": {"build": {"providers": []}}}},
                "build",
                "namespace",
            ),
            "",
        )
        self.assertEqual(
            self.mod.resolve_workflow_runner_selector_json(
                {"github_actions": {"workflows": {"build": {"providers": {"namespace": []}}}}},
                "build",
                "namespace",
            ),
            "",
        )
        self.assertEqual(self.mod.resolve_workflow_dispatch_field_values(None, "build", "namespace", None), {})
        self.assertEqual(
            self.mod.resolve_workflow_dispatch_field_values(
                {"github_actions": {"workflows": {"build": {"providers": {"namespace": []}}}}},
                "build",
                "namespace",
                ("linux_runner_selector_json",),
            ),
            {},
        )
        self.assertEqual(
            self.mod.resolve_default_provider_for_workflow({}, "build", explicit_provider="namespace"),
            ("namespace", "cli"),
        )
        with self.assertRaisesRegex(ValueError, "Unknown workflow"):
            self.mod.resolve_default_provider_for_workflow({}, "unknown")
        with self.assertRaisesRegex(ValueError, "does not support provider"):
            self.mod.resolve_default_provider_for_workflow({}, "validate", explicit_provider="namespace")
        self.assertEqual(self.mod.repo_variable_name_for_workflow_field("build", "github-hosted", "runner"), "")

        config = {
            "targets": {
                "mac": {"enabled": True},
                "windows": {"enabled": False},
            },
        }
        self.assertEqual(self.mod.enabled_targets(config), ["mac"])
        self.assertIsNone(self.mod.parse_targets_arg(""))
        self.assertEqual(self.mod.parse_targets_arg("windows, mac,windows"), ["mac", "windows"])
        self.assertEqual(self.mod.resolve_targets(config, None), ["mac"])
        self.assertEqual(self.mod.resolve_targets(config, []), [])
        with self.assertRaisesRegex(ValueError, "Unknown target"):
            self.mod.resolve_targets(config, ["ubuntu"])
        with self.assertRaisesRegex(ValueError, "disabled"):
            self.mod.resolve_targets(config, ["windows"])
        self.assertEqual(
            self.mod.resolve_targets(
                {"defaults": {"targets": "mac,mac"}, "targets": {"mac": {"enabled": True}}},
                None,
            ),
            ["mac"],
        )

    def test_windows_checkout_and_desktop_request_edge_paths(self) -> None:
        self.assertEqual(self.mod.windows_path_join("", r"C:\Root\\", r"\child", ""), r"C:\Root\child")
        self.assertEqual(self.mod.windows_default_repo_checkout_path(None), "pulp-validate")
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(None))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\\"))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\Users\dev", r"C:\Users\dev"))
        self.assertFalse(self.mod.windows_repo_path_is_unsafe(r"C:\Users\dev\pulp", r"C:\Users\dev"))
        self.assertFalse(self.mod.windows_repo_checkout_ready(None))
        self.assertTrue(
            self.mod.windows_repo_checkout_ready(
                {
                    "git_dir_exists": True,
                    "head_exists": True,
                    "setup_exists": True,
                    "repo_path_unsafe": False,
                }
            )
        )

        failing_probe = subprocess.CompletedProcess([], 4, stdout="", stderr="repo failed")
        with mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=failing_probe):
            with self.assertRaisesRegex(RuntimeError, "repo failed"):
                self.mod.probe_windows_repo_checkout("win", r"C:\Pulp")

        probe_payload = (
            '{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Users\\\\dev",'
            '"repo_exists":true,"git_dir_exists":true,"head_exists":true,'
            '"setup_exists":true,"origin_url":"https://example.test/pulp.git"}'
        )
        with mock.patch.object(
            self.mod,
            "run_windows_ssh_powershell",
            return_value=subprocess.CompletedProcess([], 0, stdout=probe_payload, stderr=""),
        ):
            probe = self.mod.probe_windows_repo_checkout("win", r"C:\Users\dev")
        self.assertTrue(probe["repo_path_unsafe"])

        bootstrap_payload = (
            '{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Users\\\\dev\\\\pulp-validate",'
            '"repo_exists":true,"git_dir_exists":true,"head_exists":true,'
            '"setup_exists":true,"origin_url":"https://example.test/pulp.git"}'
        )
        with mock.patch.object(
            self.mod,
            "probe_windows_repo_checkout",
            return_value={"home_dir": r"C:\Users\dev", "repo_path": r"C:\Users\dev", "head_exists": False, "setup_exists": False},
        ):
            with mock.patch.object(
                self.mod,
                "run_windows_ssh_powershell",
                return_value=subprocess.CompletedProcess([], 0, stdout=bootstrap_payload, stderr=""),
            ) as run_ps:
                ensured = self.mod.ensure_windows_remote_repo_checkout(
                    "win",
                    r"C:\Users\dev",
                    remote_url="https://example.test/pulp.git",
                    bundle_name="bundle.git",
                    bundle_ref="refs/pulp-ci-bundles/job",
                )
        self.assertFalse(ensured["repo_path_unsafe"])
        self.assertIn("git fetch bundle", run_ps.call_args.args[1])
        self.assertIn("pulp-validate", run_ps.call_args.args[1])

        cleanup_output = 'banner\n{"found":true,"matched":true,"killed":true,"children":[456]}\n'
        with mock.patch.object(
            self.mod,
            "run_logged_command",
            return_value={"returncode": 0, "output": cleanup_output},
        ) as run_cleanup:
            cleanup = self.mod.cleanup_stale_windows_validator("win", 123, "2026-05-01T00:00:00Z")
        self.assertTrue(cleanup["killed"])
        self.assertEqual(cleanup["children"], [456])
        self.assertIn("$PidToKill = 123", run_cleanup.call_args.kwargs["input_text"])
        self.assertIn("$ExpectedStart = '2026-05-01T00:00:00Z'", run_cleanup.call_args.kwargs["input_text"])

        with mock.patch.object(
            self.mod,
            "run_logged_command",
            return_value={"returncode": 7, "output": "not json\n"},
        ):
            cleanup = self.mod.cleanup_stale_windows_validator("win", 123, "")
        self.assertEqual(cleanup["error"], "not json")

        contract = self.mod.desktop_target_contract(
            "windows",
            {"adapter": "windows-session-agent", "remote_root": r"C:\agent", "task_name": "Pulp Agent"},
        )
        request = self.mod.build_windows_session_agent_request(
            "windows",
            contract,
            "pulp smoke",
            repo_path=r"C:\Pulp",
            action_name="smoke",
            label=None,
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point="10,20",
            click_view_id="ok",
            click_view_type="button",
            click_view_text="OK",
            click_view_label="Confirm",
            capture_before=True,
            settle_secs=0.75,
            timeout_secs=9.0,
        )
        self.assertEqual(request["label"], "pulp")
        self.assertEqual(request["outputs"]["ui_snapshot"], self.mod.windows_path_join(contract["results_dir"], request["job_id"], "ui-tree.json"))
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_TYPE"], "button")
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_TEXT"], "OK")
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_LABEL"], "Confirm")
        self.assertEqual(request["env"]["PULP_AUTOMATION_AFTER_DELAY_MS"], "750")
        self.assertEqual(self.mod.desktop_target_contract("mac", {"adapter": "macos-local"}), {})
        with self.assertRaisesRegex(ValueError, "Unknown desktop target"):
            self.mod.resolve_desktop_target({"desktop_automation": {"targets": {}}}, "missing")
        with self.assertRaisesRegex(ValueError, "disabled"):
            self.mod.resolve_desktop_target(
                {"desktop_automation": {"targets": {"windows": {"enabled": False}}}},
                "windows",
            )

    def test_webdriver_and_windows_detail_helpers_cover_edges(self) -> None:
        class FakeResponse:
            def __init__(self, payload: bytes) -> None:
                self.payload = payload

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc, tb):
                return False

            def read(self) -> bytes:
                return self.payload

        self.assertEqual(
            self.mod.webdriver_status_url("http://127.0.0.1:4444/wd/hub?ignored=1"),
            "http://127.0.0.1:4444/wd/hub/status",
        )
        with self.assertRaisesRegex(ValueError, "scheme and host"):
            self.mod.webdriver_status_url("localhost:4444")

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            side_effect=[
                FakeResponse(b'{"value":{"ready":true,"message":"nested ready"}}'),
                FakeResponse(b'{"ready":false,"message":"top ready"}'),
            ],
        ):
            nested = self.mod.probe_webdriver_endpoint("http://127.0.0.1:4444", timeout=1.0)
            top_level = self.mod.probe_webdriver_endpoint("http://127.0.0.1:4444/status", timeout=1.0)
        self.assertTrue(nested["ready"])
        self.assertEqual(nested["message"], "nested ready")
        self.assertFalse(top_level["ready"])
        self.assertEqual(top_level["message"], "top ready")

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            side_effect=self.mod.urllib.error.URLError("refused"),
        ):
            with self.assertRaisesRegex(RuntimeError, "refused"):
                self.mod.probe_webdriver_endpoint("http://127.0.0.1:4444")

        self.assertEqual(self.mod.windows_desktop_session_user(None), "")
        self.assertEqual(self.mod.windows_desktop_session_user({"logged_on_user": " dev "}), "dev")
        self.assertEqual(self.mod.windows_desktop_session_state(None), "")
        self.assertEqual(self.mod.windows_desktop_session_state({"session_state": " Active "}), "Active")
        self.assertEqual(self.mod.windows_repo_checkout_detail(None, fallback_path=r"C:\Pulp"), r"C:\Pulp")
        self.assertIn(
            "not a git checkout",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "repo_exists": True, "git_dir_exists": False}),
        )
        self.assertIn(
            "empty git repo",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "git_dir_exists": True, "head_exists": False}),
        )
        self.assertIn(
            "setup.sh missing",
            self.mod.windows_repo_checkout_detail(
                {"repo_path": r"C:\Pulp", "git_dir_exists": True, "head_exists": True, "setup_exists": False}
            ),
        )

    def test_config_bundle_and_desktop_helper_fallback_edges(self) -> None:
        explicit_config = {"targets": {"mac": {"type": "local"}}}
        self.assertIs(self.mod.config_for_bundle_probe({}, explicit_config), explicit_config)
        with mock.patch.object(self.mod, "load_optional_config", return_value={"targets": {"ubuntu": {}}}):
            self.assertEqual(self.mod.config_for_bundle_probe({})["targets"], {"ubuntu": {}})
        with mock.patch.object(self.mod, "load_optional_config", return_value=None):
            self.assertEqual(self.mod.config_for_bundle_probe({}), {"targets": {}})

        self.assertIsNone(self.mod.target_name_for_ssh_host({"targets": {}}, "ubuntu"))
        self.assertTrue(self.mod.ssh_host_uses_windows_shell({"targets": {"custom": {"host": "ssh-host", "repo_path": r"C:\Pulp"}}}, "ssh-host"))
        self.assertFalse(self.mod.ssh_host_uses_windows_shell({"targets": {"ubuntu": {"host": "ubuntu", "repo_path": "/tmp/pulp"}}}, "ubuntu"))

        optional_caps = self.mod.desktop_capabilities_for(
            "macos-local",
            "v3",
            {
                "webview_driver": True,
                "debug_attach": True,
                "video_capture": True,
                "frame_stats": True,
            },
        )
        for capability in ("pulp_app_automation", "type_text", "webview_dom", "debug_command", "video_capture", "frame_stats"):
            self.assertIn(capability, optional_caps)
        self.assertEqual(self.mod.desktop_capabilities_for("linux-xvfb", "v2").count("coordinate_click"), 1)
        self.assertEqual(
            self.mod.default_windows_session_task_name(" win target! "),
            "PulpDesktopAutomationAgent-win-target",
        )
        config = {}
        self.mod.update_target_repo_path(config, "windows", r"C:\Pulp")
        self.assertEqual(config["targets"]["windows"]["repo_path"], r"C:\Pulp")
        self.assertEqual(config["desktop_automation"]["targets"]["windows"]["repo_path"], r"C:\Pulp")

    def test_desktop_source_request_manifest_and_command_edges(self) -> None:
        args = Namespace(
            source_mode="exact_sha",
            branch="feature/source",
            sha="abc123",
            prepare_command="  ./setup.sh --desktop  ",
            prepare_timeout=12,
        )
        request = self.mod.make_desktop_source_request(args)

        self.assertEqual(request["mode"], "exact-sha")
        self.assertEqual(request["prepare_command"], "./setup.sh --desktop")
        self.assertEqual(request["prepare_timeout_secs"], 12.0)
        self.assertEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key({**request, "mode": "live", "branch": "main"}),
        )
        self.assertNotEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key({**request, "prepare_command": "cmake --build build"}),
        )

        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
            source_root = self.mod.desktop_source_root("windows", request)
        self.assertEqual(source_root.parent, self.root / "state" / "desktop-source" / "windows")

        self.assertIsNone(self.mod._command_path_rewrite_candidate("/tmp/outside-tool"))
        self.assertIsNone(self.mod._command_path_rewrite_candidate("pulp-ui-preview"))
        self.assertEqual(
            self.mod._command_path_rewrite_candidate("./tools/local-ci/local_ci.py"),
            self.mod.ROOT / "tools" / "local-ci" / "local_ci.py",
        )
        self.assertIsNone(self.mod.rewrite_launch_command_for_source_root(None, self.root / "prepared"))
        self.assertEqual(
            self.mod.rewrite_launch_command_for_source_root('"unterminated', self.root / "prepared"),
            '"unterminated',
        )
        self.assertEqual(
            self.mod.rewrite_launch_command_for_posix_root("pulp-ui-preview --flag", "$HOME/source"),
            "pulp-ui-preview --flag",
        )

        local_command = f"'{self.mod.ROOT / 'tools' / 'local-ci' / 'local_ci.py'}' --json"
        rewritten_local = self.mod.rewrite_launch_command_for_source_root(local_command, self.root / "prepared source")
        rewritten_posix = self.mod.rewrite_launch_command_for_posix_root(local_command, "$HOME/prepared source")
        rewritten_windows = self.mod.rewrite_launch_command_for_windows_root(
            r".\tools\local-ci\local_ci.py --json",
            r"C:\Prepared Source",
        )
        self.assertIn(str(self.root / "prepared source" / "tools" / "local-ci" / "local_ci.py"), rewritten_local)
        self.assertIn("'$HOME/prepared source/tools/local-ci/local_ci.py'", rewritten_posix)
        self.assertIn(r'"C:\Prepared Source\tools\local-ci\local_ci.py" --json', rewritten_windows)

        commands = self.mod.split_windows_prepare_commands('echo "one;two"; cmake --build build\nctest -C Debug')
        self.assertEqual(commands, ['echo "one;two"', "cmake --build build", "ctest -C Debug"])
        self.mod.validate_windows_prepare_commands(['cmake -G "Visual Studio 17 2022"'])
        with self.assertRaisesRegex(ValueError, "single-quoted tokens"):
            self.mod.validate_windows_prepare_commands(["cmake -G 'Visual Studio 17 2022'"])

        manifest: dict = {}
        self.mod.attach_desktop_source_to_manifest(manifest, None)
        self.assertEqual(manifest, {})
        self.mod.attach_desktop_source_to_manifest(
            manifest,
            {
                "mode": "prepared",
                "branch": "feature/source",
                "sha": "abc123",
                "prepare_command": "./setup.sh",
                "prepare_timeout_secs": 12.0,
                "prepared_root": "/real/root",
                "prepared_root_display": "$STATE/root",
                "launch_cwd": "/real/root/examples",
                "launch_cwd_display": "$STATE/root/examples",
                "prepare_log": "prepare.log",
            },
        )
        self.assertEqual(manifest["source"]["mode"], "prepared")
        self.assertEqual(manifest["source"]["sha"], "abc123")
        self.assertEqual(manifest["source"]["prepared_root"], "$STATE/root")
        self.assertEqual(manifest["source"]["launch_cwd"], "$STATE/root/examples")
        self.assertEqual(manifest["artifacts"]["prepare_log"], "prepare.log")
        self.assertEqual(self.mod.slugify_token(" UI Preview / Smoke! "), "ui-preview-smoke")
        self.assertEqual(self.mod.slugify_token("!!!"), "run")
        self.assertEqual(len(self.mod.slugify_token("x" * 80, max_len=12)), 12)

    def test_exact_sha_prepare_and_remote_artifact_helpers_cover_edges(self) -> None:
        bundle_dir = self.root / "bundle"
        bundle_dir.mkdir()
        source_request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "abc123",
            "prepare_command": "echo prepare",
            "prepare_timeout_secs": 10,
        }

        with mock.patch.object(self.mod, "desktop_source_root", return_value=self.root / "prepared"), \
             mock.patch.object(self.mod, "_local_worktree_matches", return_value=False), \
             mock.patch.object(self.mod, "_reset_local_worktree") as reset_worktree, \
             mock.patch.object(self.mod, "run_logged_command", return_value={"timed_out": False, "returncode": 0}), \
             mock.patch.object(self.mod, "rewrite_launch_command_for_source_root", return_value="prepared-command"), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stdout="", stderr="")) as run:
            prepared = self.mod.prepare_macos_exact_sha_source(bundle_dir, "mac", "./tool --flag", source_request)
        self.assertEqual(prepared["prepared_state"], "clean")
        self.assertEqual(prepared["launch_command"], "prepared-command")
        self.assertEqual(prepared["launch_cwd"], str(self.root / "prepared"))
        self.assertEqual(prepared["prepare_log"], None)
        self.assertEqual(run.call_args.args[0][:3], ["git", "worktree", "add"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.mod.ROOT)
        reset_worktree.assert_called_once_with(self.root / "prepared")

        with mock.patch.object(self.mod, "desktop_source_root", return_value=self.root / "prepared"), \
             mock.patch.object(self.mod, "_local_worktree_matches", return_value=True), \
             mock.patch.object(self.mod, "_reset_local_worktree") as reset_worktree, \
             mock.patch.object(self.mod, "run_logged_command") as logged_command, \
             mock.patch.object(self.mod, "rewrite_launch_command_for_source_root", return_value="reused-command"):
            reused = self.mod.prepare_macos_exact_sha_source(bundle_dir, "mac", "./tool", source_request)
        self.assertEqual(reused["prepared_state"], "reused")
        self.assertEqual(reused["launch_command"], "reused-command")
        self.assertEqual(reused["prepared_root"], str(self.root / "prepared"))
        self.assertEqual(reused["prepare_log"], None)
        reset_worktree.assert_not_called()
        logged_command.assert_not_called()

        with mock.patch.object(self.mod, "desktop_source_root", return_value=self.root / "prepared"), \
             mock.patch.object(self.mod, "_local_worktree_matches", return_value=False), \
             mock.patch.object(self.mod, "_reset_local_worktree"), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stdout="", stderr="")), \
             mock.patch.object(self.mod, "run_logged_command", return_value={"timed_out": True, "returncode": 0}):
            with self.assertRaisesRegex(RuntimeError, "Timed out preparing"):
                self.mod.prepare_macos_exact_sha_source(bundle_dir, "mac", "./tool", source_request)

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("source.bundle", "refs/bundle")), \
             mock.patch.object(self.mod, "git_origin_clone_url", return_value="https://example/pulp.git"), \
             mock.patch.object(self.mod, "desktop_source_cache_key", return_value="cache-key"), \
             mock.patch.object(self.mod, "rewrite_launch_command_for_posix_root", return_value="remote-command"), \
             mock.patch.object(self.mod, "fetch_ssh_artifact", return_value=True) as fetch_artifact, \
             mock.patch.object(self.mod.subprocess, "run", side_effect=[
                 subprocess.CompletedProcess([], 0, stdout="/home/dev\n", stderr=""),
                 subprocess.CompletedProcess([], 0, stdout="__PULP_PREPARED__:reused\n", stderr=""),
             ]) as run:
            linux = self.mod.prepare_linux_exact_sha_source(bundle_dir, "ubuntu", "host", "./tool", source_request)
        self.assertEqual(linux["prepared_state"], "reused")
        self.assertEqual(linux["prepared_root"], "/home/dev/.local/state/pulp/desktop-source/ubuntu/cache-key")
        self.assertEqual(linux["prepared_root_display"], "~/.local/state/pulp/desktop-source/ubuntu/cache-key")
        self.assertEqual(linux["launch_command"], "remote-command")
        self.assertEqual(linux["prepare_log"], None)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[0].args[0][:2], ["ssh", "host"])
        self.assertIn("PULP_REQUIRE_PREPARE_STAMP", run.call_args_list[1].args[0][-1])
        fetch_artifact.assert_called_once()

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("source.bundle", "refs/bundle")), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, stdout="", stderr="no home")):
            with self.assertRaisesRegex(RuntimeError, "no home"):
                self.mod.prepare_linux_exact_sha_source(bundle_dir, "ubuntu", "host", "./tool", source_request)

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("source.bundle", "refs/bundle")), \
             mock.patch.object(self.mod, "git_origin_clone_url", return_value=""), \
             mock.patch.object(self.mod, "desktop_source_cache_key", return_value="cache-key"), \
             mock.patch.object(self.mod, "split_windows_prepare_commands", return_value=["echo prepare"]), \
             mock.patch.object(self.mod, "validate_windows_prepare_commands") as validate_commands, \
             mock.patch.object(self.mod, "rewrite_launch_command_for_windows_root", return_value="win-command"), \
             mock.patch.object(self.mod, "windows_ssh_fetch_file", return_value=True) as fetch_file, \
             mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=subprocess.CompletedProcess([], 0, stdout="__PULP_PREPARED__:clean\n", stderr="")) as run_ps:
            windows = self.mod.prepare_windows_exact_sha_source(bundle_dir, "windows", "win", r".\tool.exe", source_request)
        self.assertEqual(windows["prepared_state"], "clean")
        self.assertEqual(windows["prepared_root"], r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache-key")
        self.assertEqual(windows["launch_command"], "win-command")
        self.assertEqual(windows["prepare_log"], None)
        self.assertIn("PULP_REQUIRE_PREPARE_STAMP", run_ps.call_args.args[1])
        self.assertIn("@echo off", run_ps.call_args.args[1])
        self.assertEqual(run_ps.call_args.args[0], "win")
        validate_commands.assert_called_once_with(["echo prepare"])
        fetch_file.assert_called_once()

        copied = self.root / "artifacts" / "remote.txt"
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            side_effect=lambda *_args, **_kwargs: (copied.write_text("payload"), subprocess.CompletedProcess([], 0, stdout="", stderr=""))[1],
        ):
            self.assertTrue(self.mod.fetch_ssh_artifact("host", "/tmp/remote.txt", copied))
        self.assertEqual(copied.read_text(), "payload")
        self.assertTrue(copied.parent.is_dir())

        with mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, stdout="", stderr="missing")):
            self.assertFalse(self.mod.fetch_ssh_artifact("host", "/tmp/missing.txt", self.root / "optional.txt", optional=True))
            self.assertFalse((self.root / "optional.txt").exists())
            with self.assertRaisesRegex(RuntimeError, "missing"):
                self.mod.fetch_ssh_artifact("host", "/tmp/missing.txt", self.root / "required.txt")

    def test_desktop_publish_report_rollup_edges(self) -> None:
        config = {
            "desktop_automation": {
                "artifact_root": str(self.root / "desktop-artifacts"),
                "publish_mode": "local",
                "publish_branch": "dev-artifacts",
            }
        }
        with self.assertRaisesRegex(ValueError, "at least one run manifest"):
            self.mod.stage_desktop_publish_report(config, [])

        bundle = self.root / "bundle"
        bundle.mkdir()
        (bundle / "manifest.json").write_text('{"label":"bundle-copy"}\n')
        stdout_log = bundle / "stdout.log"
        stdout_log.write_text("hello\n")
        manifest = {
            "target": "mac<>",
            "action": "inspect",
            "label": "UI & Smoke",
            "completed_at": "2026-05-22T12:00:00+00:00",
            "artifacts": {
                "bundle_dir": str(bundle),
                "stdout": str(stdout_log),
                "screenshot": str(bundle / "missing.png"),
                "image_change": {"changed": False},
            },
            "interaction": {"mode": "dom"},
        }

        output_dir = self.root / "desktop-artifacts" / "_published" / "20260522-gallery"
        report = self.mod.stage_desktop_publish_report(config, [manifest], output_dir=output_dir, label="Gallery <One>")

        self.assertEqual(report["label"], "Gallery <One>")
        self.assertEqual(report["run_count"], 1)
        self.assertTrue((output_dir / "index.html").is_file())
        self.assertTrue((output_dir / "index.json").is_file())
        payload = json.loads((output_dir / "index.json").read_text())
        published_run = payload["runs"][0]
        self.assertEqual(payload["publish_mode"], "local")
        self.assertEqual(payload["publish_branch"], "dev-artifacts")
        self.assertEqual(published_run["target"], "mac<>")
        self.assertEqual(published_run["interaction_mode"], "dom")
        self.assertIn("stdout", published_run["artifacts"])
        self.assertIn("manifest", published_run["artifacts"])
        self.assertNotIn("screenshot", published_run["artifacts"])
        self.assertEqual(published_run["artifacts"]["image_change"], {"changed": False})
        self.assertTrue((output_dir / published_run["artifacts"]["stdout"]).is_file())
        self.assertTrue((output_dir / published_run["artifacts"]["manifest"]).is_file())
        html_text = (output_dir / "index.html").read_text()
        self.assertIn("Gallery &lt;One&gt;", html_text)
        self.assertIn("mac&lt;&gt;/inspect", html_text)

        invalid = output_dir.parent / "zz-invalid"
        invalid.mkdir()
        (invalid / "index.json").write_text("{not json")
        reports = self.mod.desktop_publish_reports(config, limit=1)
        self.assertEqual(len(reports), 1)
        self.assertEqual(reports[0]["label"], "Gallery <One>")
        self.assertEqual(reports[0]["output_dir"], str(output_dir))
        self.assertTrue((output_dir.parent / "latest-report.json").is_file())
        self.assertTrue((output_dir.parent / "reports.jsonl").is_file())

    def test_remote_probe_wrappers_parse_mocked_outputs(self) -> None:
        win_success = subprocess.CompletedProcess(
            [],
            0,
            stdout='banner\n{"task_present": true, "interactive_user": "dev"}\n',
            stderr="",
        )
        with mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=win_success) as run_ps:
            session = self.mod.probe_windows_session_agent(
                "win",
                {
                    "task_name": "Pulp Agent",
                    "remote_root": r"%LOCALAPPDATA%\Pulp\agent",
                    "script_path": r"%LOCALAPPDATA%\Pulp\agent\agent.ps1",
                },
            )
            self.assertTrue(session["task_present"])
            self.assertEqual(session["interactive_user"], "dev")
            self.assertIn("Get-ScheduledTask", run_ps.call_args.args[1])

            tooling = self.mod.probe_windows_remote_tooling("win")
            self.assertTrue(tooling["task_present"])
            self.assertIn("Get-Command git", run_ps.call_args.args[1])

        win_failure = subprocess.CompletedProcess([], 7, stdout="", stderr="powershell failed")
        with mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=win_failure):
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.probe_windows_session_agent("win", {"task_name": "task", "remote_root": "root"})
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.probe_windows_remote_tooling("win")
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.install_windows_remote_tool("win", "Git.Git", timeout=1)

        linux_success = subprocess.CompletedProcess(
            [],
            0,
            stdout="ignored\nmode=display\ndisplay=:2\nxdg_runtime_dir=/run/user/501\n",
            stderr="",
        )
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_success) as ssh_run:
            backend = self.mod.probe_linux_launch_backend("ubuntu")
            self.assertEqual(backend["mode"], "display")
            self.assertEqual(backend["display"], ":2")
            self.assertIn("xvfb-run", ssh_run.call_args.args[1])

        linux_tooling = subprocess.CompletedProcess(
            [],
            0,
            stdout="git_found=true\ngit_path=/usr/bin/git\ngit_version=git version 2.49\nwmctrl_found=false\n",
            stderr="",
        )
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_tooling):
            probe = self.mod.probe_linux_remote_tooling("ubuntu")
            self.assertEqual(probe["git_version"], "git version 2.49")
            self.assertEqual(probe["wmctrl_found"], "false")

        linux_failure = subprocess.CompletedProcess([], 2, stdout="", stderr="ssh failed")
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_failure):
            with self.assertRaisesRegex(RuntimeError, "ssh failed"):
                self.mod.probe_linux_launch_backend("ubuntu")
            with self.assertRaisesRegex(RuntimeError, "ssh failed"):
                self.mod.probe_linux_remote_tooling("ubuntu")

        command = self.mod.build_linux_window_driver_remote_command(
            "/repo",
            "bundle",
            "pulp-ui",
            launch_backend={"mode": "display", "display": ":2", "xdg_runtime_dir": "/run/user/501"},
            launch_cwd="$HOME/repo",
            click_point="10,20",
            capture_before=True,
            settle_secs=0.25,
        )
        self.assertIn("export DISPLAY=:2", command)
        self.assertIn("xdotool click 1", command)
        self.assertIn("sleep 0.250", command)

    def test_webdriver_probe_parses_status_shapes_and_errors(self) -> None:
        self.assertEqual(self.mod.webdriver_status_url("http://127.0.0.1:4444"), "http://127.0.0.1:4444/status")
        self.assertEqual(self.mod.webdriver_status_url("http://host/wd/hub"), "http://host/wd/hub/status")
        self.assertEqual(self.mod.webdriver_status_url("http://host/status?old=1#frag"), "http://host/status")
        with self.assertRaisesRegex(ValueError, "scheme and host"):
            self.mod.webdriver_status_url("localhost:4444")

        class FakeResponse:
            def __init__(self, payload: str) -> None:
                self.payload = payload

            def __enter__(self):
                return self

            def __exit__(self, *_exc):
                return False

            def read(self) -> bytes:
                return self.payload.encode("utf-8")

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            return_value=FakeResponse('{"value":{"ready":true,"message":" ok "}}'),
        ) as urlopen:
            probe = self.mod.probe_webdriver_endpoint("http://driver")
            self.assertEqual(probe["status_url"], "http://driver/status")
            self.assertTrue(probe["ready"])
            self.assertEqual(probe["message"], "ok")
            self.assertEqual(urlopen.call_args.kwargs["timeout"], 5.0)

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            return_value=FakeResponse('{"ready":false,"message":"not ready"}'),
        ):
            probe = self.mod.probe_webdriver_endpoint("http://driver/status", timeout=1.5)
            self.assertFalse(probe["ready"])
            self.assertEqual(probe["message"], "not ready")

        http_error = self.mod.urllib.error.HTTPError(
            "http://driver/status",
            500,
            "boom",
            {},
            mock.Mock(read=lambda: b"server body"),
        )
        with mock.patch.object(self.mod.urllib.request, "urlopen", side_effect=http_error):
            with self.assertRaisesRegex(RuntimeError, "HTTP 500: server body"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(self.mod.urllib.request, "urlopen", return_value=FakeResponse("{bad")):
            with self.assertRaisesRegex(RuntimeError, "invalid JSON response"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            side_effect=self.mod.urllib.error.URLError("connection refused"),
        ):
            with self.assertRaisesRegex(RuntimeError, "connection refused"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(self.mod.urllib.request, "urlopen", return_value=FakeResponse("[]")):
            probe = self.mod.probe_webdriver_endpoint("http://driver")
        self.assertIsNone(probe["ready"])
        self.assertEqual(probe["message"], "")
        self.assertEqual(probe["payload"], [])

    def test_remote_detail_helpers_cover_missing_and_partial_states(self) -> None:
        self.assertEqual(self.mod.windows_desktop_session_user(None), "")
        self.assertEqual(self.mod.windows_desktop_session_user({"logged_on_user": " alice "}), "alice")
        self.assertEqual(self.mod.windows_desktop_session_state({"session_state": " Active "}), "Active")
        self.assertEqual(self.mod.windows_tooling_detail({"git_found": True, "git_path": "C:/Git/bin/git.exe"}, "git"), "C:/Git/bin/git.exe")
        self.assertEqual(self.mod.windows_tooling_detail({}, "git", missing_hint="install git"), "install git")
        self.assertTrue(self.mod.windows_remote_tooling_ready({"git_found": True}))
        self.assertFalse(self.mod.windows_remote_tooling_ready({}))

        self.assertEqual(self.mod.windows_repo_checkout_detail(None, fallback_path=r"C:\\Pulp"), r"C:\\Pulp")
        self.assertIn("not a git checkout", self.mod.windows_repo_checkout_detail({"repo_path": r"C:\\Pulp", "repo_exists": True}))
        self.assertIn("empty git repo", self.mod.windows_repo_checkout_detail({"git_dir_exists": True, "repo_path": r"C:\\Pulp"}))
        self.assertIn(
            "checkout incomplete",
            self.mod.windows_repo_checkout_detail({"git_dir_exists": True, "head_exists": True, "repo_path": r"C:\\Pulp"}),
        )
        self.assertEqual(
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\\Pulp", "origin_url": "https://example/repo.git"}),
            r"C:\\Pulp (https://example/repo.git)",
        )

        self.assertEqual(
            self.mod.linux_tooling_detail({"git_lfs_found": False, "git_lfs_hint": "PATH missing"}, "git_lfs"),
            "PATH missing",
        )
        self.assertEqual(self.mod.linux_tooling_detail({}, "xauth", missing_hint="install xauth"), "install xauth")
        self.assertEqual(self.mod.linux_tooling_detail({"xvfb_run_found": True, "xvfb_run_path": "/usr/bin/xvfb-run"}, "xvfb_run"), "/usr/bin/xvfb-run")
        self.assertFalse(self.mod.linux_remote_tooling_ready({"git_found": True, "git_lfs_found": True}))

    def test_linux_launch_backend_and_windows_tool_install_edges(self) -> None:
        linux_probe = subprocess.CompletedProcess(
            [],
            0,
            stdout="mode=display\ndisplay=:7\nignored-line\nxdg_runtime_dir=/run/user/501\n",
            stderr="",
        )
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_probe) as ssh_result:
            backend = self.mod.probe_linux_launch_backend("ubuntu")
        self.assertEqual(backend["mode"], "display")
        self.assertEqual(backend["display"], ":7")
        self.assertEqual(backend["xdg_runtime_dir"], "/run/user/501")
        self.assertEqual(ssh_result.call_args.kwargs["timeout"], 30)
        self.assertEqual(ssh_result.call_args.args[0], "ubuntu")
        self.assertIn("xvfb-run", ssh_result.call_args.args[1])

        empty_probe = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        with mock.patch.object(self.mod, "ssh_command_result", return_value=empty_probe):
            self.assertEqual(self.mod.probe_linux_launch_backend("ubuntu"), {"mode": "missing"})

        failed_probe = subprocess.CompletedProcess([], 7, stdout="", stderr="ssh denied")
        with mock.patch.object(self.mod, "ssh_command_result", return_value=failed_probe):
            with self.assertRaisesRegex(RuntimeError, "ssh denied"):
                self.mod.probe_linux_launch_backend("ubuntu")

        probes = [
            {"git_found": False, "winget_found": True},
            {"git_found": True, "winget_found": True, "gh_found": False},
            {"git_found": True, "winget_found": True, "gh_found": True},
        ]
        with mock.patch.object(self.mod, "probe_windows_remote_tooling", side_effect=probes), \
             mock.patch.object(self.mod, "install_windows_remote_tool", side_effect=[None, RuntimeError("optional failed")]) as install:
            ensured = self.mod.ensure_windows_remote_tooling("win", install_optional=True)
        self.assertEqual(ensured["installed"], ["git"])
        self.assertTrue(ensured["probe"]["git_found"])
        self.assertTrue(ensured["probe"]["winget_found"])
        self.assertEqual(install.call_count, 2)
        self.assertEqual(install.call_args_list[0].args[0], "win")
        self.assertEqual(install.call_args_list[0].args[1], self.mod.WINDOWS_REQUIRED_REMOTE_TOOLS["git"]["winget_id"])

        with mock.patch.object(self.mod, "probe_windows_remote_tooling", return_value={"git_found": False, "winget_found": False}), \
             mock.patch.object(self.mod, "install_windows_remote_tool") as install:
            with self.assertRaisesRegex(RuntimeError, "winget"):
                self.mod.ensure_windows_remote_tooling("win")
        install.assert_not_called()

    def test_desktop_bundle_and_prune_helpers_cover_edge_filters(self) -> None:
        config = {"desktop_automation": {"artifact_root": str(self.root / "artifacts")}}
        run_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        publish_bundle = self.mod.create_desktop_publish_bundle(config)
        self.assertTrue((run_bundle / "screenshots").is_dir())
        self.assertEqual(run_bundle.parents[1].name, "mac")
        self.assertTrue((publish_bundle / "assets").is_dir())
        self.assertEqual(publish_bundle.parent, self.root / "artifacts" / "_published")

        bundle_a = self.root / "bundle-a"
        bundle_b = self.root / "bundle-b"
        bundle_a.mkdir()
        bundle_b.mkdir()
        manifests = [
            {"completed_at": "bad-date", "artifacts": {"bundle_dir": str(bundle_a)}},
            {"completed_at": "2000-01-01T00:00:00Z", "artifacts": {"bundle_dir": str(bundle_a)}},
            {"started_at": "2000-01-02T00:00:00Z", "artifacts": {"bundle_dir": str(bundle_b)}},
            {"completed_at": "2999-01-01T00:00:00Z", "artifacts": {"bundle_dir": str(self.root / "missing")}},
        ]
        with mock.patch.object(self.mod, "desktop_run_manifests", return_value=manifests):
            self.assertEqual(self.mod.prune_desktop_run_manifests(config, older_than_days=1), [bundle_a, bundle_b])
            self.assertEqual(self.mod.prune_desktop_run_manifests(config, keep_last=2), [bundle_b])

    def test_filesystem_and_git_wrappers_cover_fallbacks(self) -> None:
        src = self.root / "src"
        dest = self.root / "dest"
        src.mkdir()
        (src / "nested").mkdir()
        (src / "nested" / "file.txt").write_text("nested")
        (src / "top.txt").write_text("top")
        self.mod._copy_directory_contents(src, dest)
        self.assertEqual((dest / "nested" / "file.txt").read_text(), "nested")
        self.assertEqual((dest / "top.txt").read_text(), "top")

        keep_git = dest / ".git"
        keep_git.mkdir()
        self.mod._clear_directory_contents(dest)
        self.assertTrue(keep_git.exists())
        self.assertFalse((dest / "nested").exists())
        self.assertFalse((dest / "top.txt").exists())

        ok = subprocess.CompletedProcess(["git"], 0, stdout="ok", stderr="")
        fail = subprocess.CompletedProcess(["git"], 2, stdout="", stderr="bad ref")
        with mock.patch.object(self.mod.subprocess, "run", return_value=ok) as run:
            self.assertIs(self.mod._run_git(["status"], cwd=self.root), ok)
            self.assertEqual(run.call_args.args[0], ["git", "status"])
        with mock.patch.object(self.mod.subprocess, "run", return_value=fail):
            self.assertIs(self.mod._run_git(["status"], cwd=self.root, check=False), fail)
            with self.assertRaisesRegex(RuntimeError, "bad ref"):
                self.mod._run_git(["status"], cwd=self.root)

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr="no remote"),
        ):
            self.assertIsNone(self.mod.git_origin_http_url(self.root))
            self.assertIsNone(self.mod.git_origin_clone_url(self.root))

    def test_scheduler_submission_and_target_resolution_edges(self) -> None:
        config = {"targets": {}, "defaults": {}}
        self.assertIs(self.mod.config_for_job_execution({"submission": {}}, config), config)

        missing_config = self.root / "missing.json"
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertIs(
                self.mod.config_for_job_execution({"submission": {"config_path": str(missing_config)}}, config),
                config,
            )
        self.assertIn("failed to load submission config", buf.getvalue())

        submitted_config = self.root / "submitted.json"
        submitted_config.write_text(json.dumps({"targets": {"mac": {"enabled": True}}, "defaults": {"targets": "mac"}}))
        loaded = self.mod.config_for_job_execution({"submission": {"config_path": str(submitted_config)}}, config)
        self.assertEqual(loaded["defaults"]["targets"], "mac")
        self.assertEqual(loaded["targets"]["mac"]["enabled"], True)

        self.assertEqual(self.mod.submission_target_state({"submission": {"target_hosts": {"mac": "bad"}}}, "mac"), {})
        self.assertEqual(
            self.mod.submission_target_state({"submission": {"target_hosts": {"mac": {"status": "primary-up"}}}}, "mac"),
            {"status": "primary-up"},
        )

        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="live-host") as ensure:
            self.assertEqual(
                self.mod.resolve_ssh_target_execution(
                    {"submission": {"target_hosts": {"ubuntu": {"status": "primary-up", "resolved_host": "u1"}}}},
                    "ubuntu",
                    {"host": "u0", "repo_path": "/repo"},
                    {},
                ),
                ("u1", "/repo"),
            )
            self.assertEqual(
                self.mod.resolve_ssh_target_execution(
                    {"submission": {"target_hosts": {"ubuntu": {"status": "unreachable", "repo_path": "/submitted"}}}},
                    "ubuntu",
                    {"host": "u0", "repo_path": "/repo"},
                    {},
                ),
                (None, "/submitted"),
            )
            self.assertEqual(
                self.mod.resolve_ssh_target_execution(
                    {"submission": {"target_hosts": {"ubuntu": {"status": "utm-fallback-pending", "configured_host": "fallback"}}}},
                    "ubuntu",
                    {"host": "u0", "repo_path": "/repo"},
                    {"ssh_timeout": 3},
                ),
                ("live-host", "/repo"),
            )
            self.assertEqual(ensure.call_args.args[1]["host"], "fallback")
            self.assertEqual(
                self.mod.resolve_ssh_target_execution({}, "ubuntu", {"host": "u0", "repo_path": "/repo"}, {}),
                ("live-host", "/repo"),
            )

    def test_scheduler_builds_unreachable_and_no_target_results(self) -> None:
        config = {
            "targets": {
                "mac": {"enabled": False},
                "ubuntu": {"enabled": True, "host": "ubuntu", "repo_path": "/repo"},
                "windows": {"enabled": True, "host": "win", "repo_path": r"C:\Repo"},
            },
            "defaults": {},
        }
        job = self.mod.make_job("feature/offline", "d" * 40, "normal", ["mac", "ubuntu", "windows"], "run", "full")

        reporters: dict[str, object] = {}
        with mock.patch.object(self.mod, "resolve_ssh_target_execution", return_value=(None, "/repo")):
            tasks = self.mod._build_target_tasks(job, config, progress_factory=lambda name: reporters.setdefault(name, name))

        self.assertEqual([name for name, _fn in tasks], ["ubuntu", "windows"])
        ubuntu_result = tasks[0][1]()
        windows_result = tasks[1][1]()
        self.assertEqual(ubuntu_result["status"], "unreachable")
        self.assertEqual(ubuntu_result["stderr_tail"], "Host unreachable")
        self.assertEqual(windows_result["target"], "windows")
        self.assertEqual(windows_result["exit_code"], -1)
        self.assertEqual(reporters, {})

        no_targets_job = self.mod.make_job("feature/none", "e" * 40, "low", ["mac"], "run", "smoke")
        result = self.mod.process_job(no_targets_job, {"targets": {"mac": {"enabled": False}}, "defaults": {}})
        self.assertEqual(result["overall"], "pass")
        self.assertEqual(result["results"], [])
        self.assertNotIn("validation", result)
        self.assertEqual(result["targets"], ["mac"])
        self.assertIsInstance(result["provenance"], dict)

    def test_scheduler_process_job_and_drain_failure_edges(self) -> None:
        job = self.mod.make_job("feature/error", "f" * 40, "normal", ["mac"], "run", "full")
        failing_task = [("mac", lambda: (_ for _ in ()).throw(RuntimeError("boom")))]
        progress_snapshots = []

        with mock.patch.object(self.mod, "_build_target_tasks", return_value=failing_task), \
             mock.patch.object(self.mod, "update_runner_active_targets", side_effect=lambda _job_id, state: progress_snapshots.append(state)), \
             mock.patch.object(self.mod, "update_job_active_targets"):
            result = self.mod.process_job(job, {"targets": {"mac": {"enabled": True}}, "defaults": {}})

        self.assertEqual(result["overall"], "fail")
        self.assertEqual(result["results"][0]["target"], "mac")
        self.assertEqual(result["results"][0]["status"], "error")
        self.assertIn("boom", result["results"][0]["stderr_tail"])
        self.assertEqual(progress_snapshots[0]["mac"]["phase"], "starting")
        self.assertEqual(progress_snapshots[-1]["mac"]["status"], "error")

        with mock.patch.object(self.mod, "file_lock", side_effect=self.mod.LockBusyError("busy")):
            self.assertEqual(self.mod.drain_pending_jobs({"targets": {}}, blocking=False), (False, False))

        queue = [self.mod.make_job("feature/boom", "a" * 40, "normal", ["mac"], "run", "full")]
        queue[0]["id"] = "job-boom"
        saved_results = []
        with mock.patch.object(self.mod, "claim_next_job", side_effect=[queue[0], None]), \
             mock.patch.object(self.mod, "process_job", side_effect=RuntimeError("explode")), \
             mock.patch.object(self.mod, "save_result", side_effect=lambda result: saved_results.append(result) or (self.root / "result.json")), \
             mock.patch.object(self.mod, "finalize_job") as finalize, \
             mock.patch.object(self.mod, "write_runner_info"), \
             mock.patch.object(self.mod, "clear_runner_info"), \
             mock.patch.object(self.mod, "reclaim_stale_remote_validators"), \
             mock.patch.object(self.mod, "print_result"):
            self.assertEqual(self.mod.drain_pending_jobs({"targets": {}}, blocking=True), (True, True))

        self.assertEqual(saved_results[0]["overall"], "fail")
        self.assertEqual(saved_results[0]["results"][0]["target"], "scheduler")
        self.assertIn("explode", saved_results[0]["results"][0]["stderr_tail"])
        self.assertEqual(finalize.call_args.args[0], "job-boom")

    def test_scheduler_cli_error_paths_report_context(self) -> None:
        with mock.patch.object(self.mod, "load_config", side_effect=FileNotFoundError("missing config")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_drain(Namespace()), 1)
        self.assertIn("missing config", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"targets": {}}), \
             mock.patch.object(self.mod, "drain_pending_jobs", return_value=(False, False)), \
             mock.patch.object(self.mod, "current_runner_info", return_value={"active_job_id": "abc123", "active_branch": "feature/live"}):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_drain(Namespace()), 0)
        self.assertIn("[abc123] feature/live", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", side_effect=ValueError("bad targets")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_enqueue(Namespace(branch=None, sha=None, targets=None, priority=None, smoke=False)), 1)
        self.assertIn("bad targets", buf.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
