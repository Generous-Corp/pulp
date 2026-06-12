#!/usr/bin/env python3
"""Tests for cloud billing period helpers."""

from __future__ import annotations

import pathlib
import unittest
from datetime import datetime, timezone

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_billing_period.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudBillingPeriodTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

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

        summary = self.mod.estimate_billing_period_totals(
            records,
            config,
            provider="github-hosted",
            period_window_func=lambda _day: (
                datetime(2026, 4, 1, tzinfo=timezone.utc),
                datetime(2026, 5, 1, tzinfo=timezone.utc),
            ),
        )

        self.assertEqual(summary["matched_runs"], 1)
        self.assertEqual(summary["estimated_runs"], 1)
        self.assertEqual(summary["estimated_total"], 0.1)

    def test_estimate_billing_period_totals_uses_injected_estimator(self) -> None:
        calls = []

        def estimate_cost(record: dict, config: dict | None) -> dict:
            calls.append((record["dispatch_id"], config))
            return {"status": "estimated", "estimated_total": 1.23456}

        config = {"telemetry": {"billing": {"currency": "eur"}}}
        summary = self.mod.estimate_billing_period_totals(
            [
                {
                    "dispatch_id": "inside",
                    "workflow_key": "build",
                    "provider_requested": "namespace",
                    "completed_at": "2026-04-10T12:00:00Z",
                },
                {
                    "dispatch_id": "wrong-provider",
                    "workflow_key": "build",
                    "provider_requested": "github-hosted",
                    "completed_at": "2026-04-10T12:00:00Z",
                },
            ],
            config,
            provider="namespace",
            period_window_func=lambda _day: (
                datetime(2026, 4, 1, tzinfo=timezone.utc),
                datetime(2026, 5, 1, tzinfo=timezone.utc),
            ),
            estimate_cloud_record_cost_fn=estimate_cost,
        )

        self.assertEqual(calls, [("inside", config)])
        self.assertEqual(summary["currency"], "EUR")
        self.assertEqual(summary["matched_runs"], 1)
        self.assertEqual(summary["estimated_runs"], 1)
        self.assertEqual(summary["estimated_total"], 1.2346)


if __name__ == "__main__":
    unittest.main()
