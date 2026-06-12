#!/usr/bin/env python3
"""Tests for Namespace provider usage helpers."""

from __future__ import annotations

import io
import pathlib
import unittest
from contextlib import redirect_stdout

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_namespace_usage.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudNamespaceUsageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_normalize_instance_uses_destroyed_time_or_now_fallback(self) -> None:
        instance = self.mod.normalize_namespace_instance(
            {
                "cluster_id": "cluster",
                "created": "2026-04-30T00:00:00Z",
                "shape": {"os": "linux", "machine_arch": "arm64", "virtual_cpu": 4, "memory_megabytes": 8192},
                "user_label": {"nsc.runner-profile-tag": "coverage-linux", "nsc.runner-profile-id": "profile"},
                "github_workflow": {"repository": "danielraffel/pulp", "run_id": 123, "workflow": "Build"},
            },
            now_iso_fn=lambda: "2026-04-30T00:30:00Z",
        )

        self.assertEqual(instance["cluster_id"], "cluster")
        self.assertEqual(instance["created_at"], "2026-04-30T00:00:00Z")
        self.assertEqual(instance["arch"], "arm64")
        self.assertEqual(instance["profile_tag"], "coverage-linux")
        self.assertEqual(instance["run_id"], 123)
        self.assertEqual(instance["duration_secs"], 1800.0)

    def test_summarize_usage_groups_shapes_and_runtime(self) -> None:
        instances = [
            {
                "os": "linux",
                "arch": "arm64",
                "virtual_cpu": 4,
                "memory_megabytes": 8192,
                "profile_tag": "coverage-linux",
                "duration_secs": 1800.0,
            },
            {
                "os": "linux",
                "arch": "arm64",
                "virtual_cpu": 4,
                "memory_megabytes": 8192,
                "profile_tag": "coverage-linux",
                "duration_secs": 900.0,
            },
        ]

        usage = self.mod.summarize_namespace_usage(instances)

        self.assertEqual(usage["instances_count"], 2)
        self.assertEqual(usage["provider_runtime_secs"], 2700.0)
        self.assertEqual(usage["machine_shapes"][0]["count"], 2)
        self.assertEqual(usage["machine_shapes"][0]["duration_secs"], 2700.0)

    def test_print_namespace_usage_summary_includes_cost_edges(self) -> None:
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.mod.print_namespace_usage_summary(
                {
                    "usage_summary": {
                        "instances_count": 2,
                        "provider_runtime_secs": 2700,
                        "machine_shapes": [
                            {
                                "os": "linux",
                                "arch": "arm64",
                                "virtual_cpu": 4,
                                "memory_megabytes": 8192,
                                "profile_tag": "",
                                "count": 2,
                                "duration_secs": 2700,
                            }
                        ],
                    },
                    "cost_summary": {
                        "status": "estimated",
                        "estimated_total": 1.2,
                        "currency": "EUR",
                        "reason": "",
                    },
                }
            )
            self.mod.print_namespace_usage_summary(
                {
                    "usage_summary": {"instances_count": 1},
                    "cost_summary": {"status": "unavailable", "reason": "missing rates"},
                }
            )

        output = buf.getvalue()
        self.assertIn("provider usage: 2 Namespace instance(s) runtime=45m00s", output)
        self.assertIn("unlabeled: linux/arm64 4 vCPU 8 GB x2 runtime=45m00s", output)
        self.assertIn("cost: est EUR 1.20; estimated; verify provider pricing", output)
        self.assertIn("cost: unavailable (missing rates)", output)


if __name__ == "__main__":
    unittest.main()
