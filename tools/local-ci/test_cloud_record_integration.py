#!/usr/bin/env python3
"""Facade-level cloud billing and record integration tests."""

from __future__ import annotations

import importlib
import unittest
from datetime import datetime, timezone
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("local_ci.py", module_name="pulp_local_ci_cloud_record_integration", add_module_dir=True)


class CloudRecordIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.cloud = importlib.import_module("cloud")

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



if __name__ == "__main__":
    unittest.main()
