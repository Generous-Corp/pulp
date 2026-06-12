#!/usr/bin/env python3
"""Tests for pure cloud billing helpers."""

from __future__ import annotations

import io
import pathlib
import unittest
from contextlib import redirect_stdout
from datetime import datetime, timezone

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_billing.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudBillingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_resolve_billing_settings_normalizes_rates(self) -> None:
        settings = self.mod.resolve_billing_settings(
            {
                "telemetry": {
                    "billing": {
                        "currency": "eur",
                        "billing_period_start_day": "12",
                        "enable_provider_reported_totals": True,
                        "github_hosted_job_os_rates_per_minute": {
                            " macOS ": "0.16",
                            "linux": -1,
                            "": 1,
                        },
                        "namespace_profile_tag_rates_per_hour": {
                            "large": "1.5",
                            "bad": "nope",
                        },
                        "namespace_machine_shape_rates_per_hour": [
                            {"os": "Linux", "arch": "ARM64", "virtual_cpu": "4", "memory_megabytes": "8192", "rate": "2.25"},
                            {"os": "windows", "rate": ""},
                        ],
                    }
                }
            }
        )

        self.assertEqual(settings["currency"], "EUR")
        self.assertEqual(settings["billing_period_start_day"], 12)
        self.assertTrue(settings["enable_provider_reported_totals"])
        self.assertEqual(settings["github_hosted_job_os_rates_per_minute"], {"macos": 0.16})
        self.assertEqual(settings["namespace_profile_tag_rates_per_hour"], {"large": 1.5})
        self.assertEqual(
            settings["namespace_machine_shape_rates_per_hour"],
            [{"os": "linux", "arch": "arm64", "virtual_cpu": 4, "memory_megabytes": 8192, "rate": 2.25}],
        )

    def test_billing_period_helpers(self) -> None:
        start, end = self.mod.billing_period_window(
            15,
            now_dt=datetime(2026, 1, 10, tzinfo=timezone.utc),
        )

        self.assertEqual(start.isoformat(), "2025-12-15T00:00:00+00:00")
        self.assertEqual(end.isoformat(), "2026-01-15T00:00:00+00:00")
        self.assertEqual(self.mod.iter_year_months(start, end), [(2025, 12), (2026, 1)])
        self.assertEqual(self.mod.parse_iso_date("2026-01-10").isoformat(), "2026-01-10")
        self.assertIsNone(self.mod.parse_iso_date("bad"))

    def test_estimate_github_hosted_cost_skips_unrated_jobs(self) -> None:
        billing = self.mod.resolve_billing_settings(
            {
                "telemetry": {
                    "billing": {
                        "github_hosted_job_os_rates_per_minute": {
                            "linux": 0.01,
                            "windows": 0.02,
                        }
                    }
                }
            }
        )
        record = {
            "workflow_key": "build",
            "provider_requested": "github-hosted",
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
        billing = self.mod.resolve_billing_settings(
            {
                "telemetry": {
                    "billing": {
                        "namespace_profile_tag_rates_per_hour": {"fast": 4.0},
                        "namespace_machine_shape_rates_per_hour": [
                            {"os": "linux", "arch": "arm64", "virtual_cpu": 4, "memory_megabytes": 8192, "rate": 2.0}
                        ],
                    }
                }
            }
        )
        record = {
            "provider_requested": "namespace",
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

    def test_estimate_billing_period_totals_filters_provider_and_period(self) -> None:
        config = {
            "telemetry": {
                "billing": {
                    "github_hosted_job_os_rates_per_minute": {"linux": 0.01}
                }
            }
        }
        records = [
            {
                "dispatch_id": "in",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "completed_at": "2026-04-10T12:00:00Z",
                "jobs": [{"name": "linux", "started_at": "2026-04-10T12:00:00Z", "completed_at": "2026-04-10T12:10:00Z"}],
            },
            {
                "dispatch_id": "out",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "completed_at": "2026-05-10T12:00:00Z",
                "jobs": [{"name": "linux", "started_at": "2026-05-10T12:00:00Z", "completed_at": "2026-05-10T12:10:00Z"}],
            },
        ]
        original_window = self.mod.billing_period_window
        self.mod.billing_period_window = lambda _day: (
            datetime(2026, 4, 1, tzinfo=timezone.utc),
            datetime(2026, 5, 1, tzinfo=timezone.utc),
        )
        try:
            summary = self.mod.estimate_billing_period_totals(records, config, provider="github-hosted")
        finally:
            self.mod.billing_period_window = original_window

        self.assertEqual(summary["matched_runs"], 1)
        self.assertEqual(summary["estimated_runs"], 1)
        self.assertEqual(summary["estimated_total"], 0.1)

    def test_print_helpers(self) -> None:
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.mod.print_github_repo_billing_summary({"status": "actual", "actual_total": 1.25}, indent="")
            self.mod.print_github_repo_billing_summary({"status": "unavailable", "reason": "denied"}, indent="")
            self.mod.print_billing_period_summary({"status": "estimated", "estimated_total": 2.5, "estimated_runs": 3}, indent="")
            self.mod.print_billing_period_summary({"status": "unavailable", "reason": "no rates"}, indent="")

        output = buf.getvalue()
        self.assertIn("github repo billing: actual $1.25 current period", output)
        self.assertIn("github repo billing: unavailable (denied)", output)
        self.assertIn("period cost: est $2.50 over 3 run(s)", output)
        self.assertIn("period cost: unavailable (no rates)", output)


if __name__ == "__main__":
    unittest.main()
