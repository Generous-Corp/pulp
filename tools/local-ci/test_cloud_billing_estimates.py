#!/usr/bin/env python3
"""Tests for cloud billing cost-estimation helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud_billing_estimates.py", add_module_dir=True)


class CloudBillingEstimatesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_infer_job_os(self) -> None:
        self.assertEqual(self.mod.infer_job_os("build", "Windows (x64)"), "windows")
        self.assertEqual(self.mod.infer_job_os("build", "macOS (ARM64)"), "macos")
        self.assertEqual(self.mod.infer_job_os("build", "Ubuntu tests"), "linux")
        self.assertEqual(self.mod.infer_job_os("docs-check", "Validate docs"), "linux")
        self.assertEqual(self.mod.infer_job_os("build", "Resolve provider"), "")

    def test_estimate_github_hosted_cost_skips_unrated_jobs(self) -> None:
        billing = {
            "currency": "USD",
            "github_hosted_job_os_rates_per_minute": {
                "linux": 0.01,
                "windows": 0.02,
            },
        }
        record = {
            "workflow_key": "build",
            "jobs": [
                {"name": "resolve-provider", "started_at": "2026-04-04T12:00:00Z", "completed_at": "2026-04-04T12:30:00Z"},
                {"name": "linux", "started_at": "2026-04-04T12:00:00Z", "completed_at": "2026-04-04T12:10:00Z"},
                {"name": "windows", "started_at": "2026-04-04T12:00:00Z", "completed_at": "2026-04-04T12:05:00Z"},
                {"name": "macos", "started_at": "2026-04-04T12:00:00Z", "completed_at": "2026-04-04T12:05:00Z"},
            ],
        }

        cost = self.mod.estimate_github_hosted_cost(record, billing)

        self.assertEqual(cost["status"], "estimated")
        self.assertEqual(cost["estimated_total"], 0.2)

    def test_estimate_namespace_cost_prefers_profile_then_shape(self) -> None:
        billing = {
            "currency": "USD",
            "namespace_profile_tag_rates_per_hour": {"fast": 4.0},
            "namespace_machine_shape_rates_per_hour": [
                {"os": "linux", "arch": "arm64", "virtual_cpu": 4, "memory_megabytes": 8192, "rate": 2.0}
            ],
        }
        record = {
            "provider_metadata": {
                "namespace_instances": [
                    {"profile_tag": "fast", "duration_secs": 1800},
                    {"os": "linux", "arch": "arm64", "virtual_cpu": 4, "memory_megabytes": 8192, "duration_secs": 1800},
                ]
            },
        }

        cost = self.mod.estimate_namespace_cost(record, billing)

        self.assertEqual(cost["status"], "estimated")
        self.assertEqual(cost["estimated_total"], 3.0)

    def test_estimate_cloud_record_cost_dispatches_by_provider(self) -> None:
        config = {
            "telemetry": {
                "billing": {
                    "github_hosted_job_os_rates_per_minute": {"linux": 0.01},
                    "namespace_profile_tag_rates_per_hour": {"fast": 2.0},
                }
            }
        }
        github_record = {
            "dispatch_id": "github",
            "workflow_key": "build",
            "provider_requested": "github-hosted",
            "jobs": [{"name": "linux", "started_at": "2026-04-04T12:00:00Z", "completed_at": "2026-04-04T12:10:00Z"}],
        }
        namespace_record = {
            "dispatch_id": "namespace",
            "provider_requested": "namespace",
            "provider_metadata": {"namespace_instances": [{"profile_tag": "fast", "duration_secs": 1800}]},
        }

        self.assertEqual(self.mod.estimate_cloud_record_cost(github_record, config)["estimated_total"], 0.1)
        self.assertEqual(self.mod.estimate_cloud_record_cost(namespace_record, config)["estimated_total"], 1.0)
        self.assertIn(
            "no estimator",
            self.mod.estimate_cloud_record_cost({"dispatch_id": "other", "provider_requested": "other"}, config)["reason"],
        )


if __name__ == "__main__":
    unittest.main()
