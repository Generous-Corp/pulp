#!/usr/bin/env python3
"""Tests for cloud compare output line helpers."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_compare_format.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudCompareFormatTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cloud_compare_summary_line_renders_timing_cost_and_latest_success(self) -> None:
        line = self.mod.cloud_compare_summary_line(
            {
                "provider": "namespace",
                "runs_count": 3,
                "success_count": 2,
                "completed_count": 2,
                "median_duration_secs": 125,
                "median_queue_delay_secs": 7,
                "median_provider_runtime_secs": 3600,
                "median_estimated_cost": 0.5,
                "currency": "USD",
                "latest_success_at": "2026-04-04T12:00:00Z",
            }
        )

        self.assertEqual(
            line,
            "  namespace: runs=3 success=2/2 median_elapsed=2m05s median_queue=7s "
            "median_provider_time=1h00m00s median_cost=est $0.50 latest_success=2026-04-04T12:00:00Z",
        )

    def test_cloud_compare_summary_line_falls_back_to_run_count_and_latest_completed(self) -> None:
        line = self.mod.cloud_compare_summary_line(
            {
                "provider": "github-hosted",
                "runs_count": 4,
                "success_count": 0,
                "completed_count": 0,
                "median_estimated_cost": None,
                "latest_completed_at": "2026-04-04T12:10:00Z",
            }
        )

        self.assertEqual(
            line,
            "  github-hosted: runs=4 success=0/4 latest=2026-04-04T12:10:00Z",
        )


if __name__ == "__main__":
    unittest.main()
