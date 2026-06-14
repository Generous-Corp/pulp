#!/usr/bin/env python3
"""Tests for cloud provider comparison helpers."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_compare.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudCompareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_filter_records_and_median_edges(self) -> None:
        records = [
            {"dispatch_id": "a", "workflow_key": "build", "provider_requested": "namespace"},
            {"dispatch_id": "b", "workflow_key": "docs", "provider_requested": "github-hosted"},
            {"dispatch_id": "c", "workflow_key": "build", "provider_resolved": "github-hosted"},
        ]

        filtered = self.mod.filter_cloud_records(records, workflow_key="build", provider="github-hosted")

        self.assertEqual([item["dispatch_id"] for item in filtered], ["c"])
        self.assertIsNone(self.mod.median_or_none([]))
        self.assertEqual(self.mod.median_or_none([3, "", None, 1, 2]), 2.0)
        self.assertEqual(self.mod.median_or_none([0.11114, 0.11116], digits=4), 0.1111)

    def test_compare_cloud_providers_summarizes_success_timing_and_cost(self) -> None:
        config = {
            "telemetry": {
                "billing": {
                    "github_hosted_job_os_rates_per_minute": {"linux": 0.01},
                    "namespace_profile_tag_rates_per_hour": {"namespace-profile-default": 0.5},
                }
            }
        }
        records = [
            {
                "dispatch_id": "ns",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:00:00Z",
                "duration_secs": 120,
                "queue_delay_secs": 5,
                "usage_summary": {"provider_runtime_secs": 3600},
                "provider_metadata": {
                    "namespace_instances": [
                        {"profile_tag": "namespace-profile-default", "duration_secs": 3600}
                    ]
                },
            },
            {
                "dispatch_id": "gh",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "failure",
                "updated_at": "2026-04-04T12:10:00Z",
                "duration_secs": 180,
                "queue_delay_secs": 15,
                "jobs": [
                    {
                        "name": "Linux",
                        "started_at": "2026-04-04T12:00:00Z",
                        "completed_at": "2026-04-04T12:03:00Z",
                    }
                ],
            },
            {"dispatch_id": "running", "workflow_key": "build", "provider_requested": "github-hosted", "status": "in_progress"},
            {"dispatch_id": "other", "workflow_key": "docs", "provider_requested": "github-hosted", "status": "completed"},
        ]

        summaries = self.mod.compare_cloud_providers(records, config, workflow_key="build")

        self.assertEqual([summary["provider"] for summary in summaries], ["github-hosted", "namespace"])
        github, namespace = summaries
        self.assertEqual(github["completed_count"], 1)
        self.assertEqual(github["success_count"], 0)
        self.assertEqual(github["median_duration_secs"], 180.0)
        self.assertEqual(github["median_estimated_cost"], 0.03)
        self.assertEqual(github["latest_completed_at"], "2026-04-04T12:10:00Z")
        self.assertEqual(namespace["success_count"], 1)
        self.assertEqual(namespace["median_provider_runtime_secs"], 3600.0)
        self.assertEqual(namespace["median_estimated_cost"], 0.5)
        self.assertEqual(namespace["latest_success_at"], "2026-04-04T12:00:00Z")

    def test_recommend_provider_edges_and_cost_tradeoff(self) -> None:
        self.assertEqual(
            self.mod.recommend_cloud_provider([], None, workflow_key="build"),
            (None, "no successful runs recorded yet"),
        )
        one_provider = [
            {
                "dispatch_id": "ns",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "status": "completed",
                "conclusion": "success",
            }
        ]
        self.assertEqual(
            self.mod.recommend_cloud_provider(one_provider, None, workflow_key="build"),
            ("namespace", "only measured provider"),
        )

        config = {
            "telemetry": {
                "billing": {
                    "github_hosted_job_os_rates_per_minute": {"linux": 0.01},
                    "namespace_profile_tag_rates_per_hour": {"namespace-profile-default": 2.0},
                }
            }
        }
        records = [
            {
                "dispatch_id": "ns",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "duration_secs": 100,
                "provider_metadata": {
                    "namespace_instances": [
                        {"profile_tag": "namespace-profile-default", "duration_secs": 3600}
                    ]
                },
            },
            {
                "dispatch_id": "gh",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "duration_secs": 110,
                "jobs": [
                    {
                        "name": "Linux",
                        "started_at": "2026-04-04T12:00:00Z",
                        "completed_at": "2026-04-04T12:10:00Z",
                    }
                ],
            },
        ]

        self.assertEqual(
            self.mod.recommend_cloud_provider(records, config, workflow_key="build"),
            ("github-hosted", "lower estimated cost with similar timing"),
        )


if __name__ == "__main__":
    unittest.main()
