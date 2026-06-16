#!/usr/bin/env python3
"""Tests for pure cloud-run record helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud_records.py")


class CloudRecordTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_normalize_cloud_record_defaults_and_container_types(self) -> None:
        record = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "abc",
                "dispatch_fields": "bad",
                "jobs": {},
                "provider_metadata": [],
                "usage_summary": [],
                "cost_summary": [],
            }
        )

        self.assertEqual(record["kind"], "github-actions-run")
        self.assertEqual(record["dispatch_id"], "abc")
        self.assertEqual(record["provider_requested"], "github-hosted")
        self.assertEqual(record["status"], "unresolved")
        self.assertEqual(record["dispatch_fields"], {})
        self.assertEqual(record["jobs"], [])
        self.assertEqual(record["provider_metadata"], {})
        self.assertEqual(record["usage_summary"], {})
        self.assertEqual(record["cost_summary"], {})

    def test_find_cloud_record_latest_prefix_run_id_and_ambiguity(self) -> None:
        records = [
            {"dispatch_id": "abcdef", "run_id": 11},
            {"dispatch_id": "abzzzz", "run_id": 12},
            {"dispatch_id": "unique", "run_id": 13},
        ]

        self.assertIs(self.mod.find_cloud_record(records, None), records[0])
        self.assertIs(self.mod.find_cloud_record(records, "latest"), records[0])
        self.assertIs(self.mod.find_cloud_record(records, "unique"), records[2])
        self.assertIs(self.mod.find_cloud_record(records, "uni"), records[2])
        self.assertIs(self.mod.find_cloud_record(records, "13"), records[2])
        self.assertIsNone(self.mod.find_cloud_record(records, "missing"))
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.mod.find_cloud_record(records, "ab")
        with self.assertRaisesRegex(ValueError, "matched multiple"):
            self.mod.find_cloud_record([{"dispatch_id": "a", "run_id": 1}, {"dispatch_id": "b", "run_id": 1}], "1")

    def test_selector_timestamp_duration_and_memory_helpers(self) -> None:
        self.assertEqual(self.mod.summarize_runner_selector('"macos-15"'), "macos-15")
        self.assertEqual(self.mod.summarize_runner_selector('["self-hosted","macOS"]'), "self-hosted,macOS")
        self.assertEqual(self.mod.summarize_runner_selector("{bad"), "{bad")
        self.assertEqual(self.mod.render_selector_value('"ubuntu-latest"'), "ubuntu-latest")

        self.assertEqual(self.mod.normalize_github_timestamp("0001-01-01T00:00:00Z"), "")
        self.assertEqual(
            self.mod.duration_between("2026-04-04T12:00:00Z", "2026-04-04T12:01:01.4Z"),
            61.4,
        )
        self.assertIsNone(self.mod.duration_between("bad", "2026-04-04T12:01:01Z"))
        self.assertEqual(self.mod.format_duration_secs(3661), "1h01m01s")
        self.assertEqual(self.mod.format_duration_secs(61), "1m01s")
        self.assertEqual(self.mod.format_duration_secs(1.25), "1.2s")
        self.assertEqual(self.mod.format_duration_secs(-1), "")
        self.assertEqual(self.mod.format_memory_megabytes(2048), "2 GB")
        self.assertEqual(self.mod.format_memory_megabytes(0), "")

    def test_cloud_record_sort_key_prefers_latest_available_timestamp(self) -> None:
        self.assertEqual(
            self.mod.cloud_record_sort_key({"dispatch_id": "a", "completed_at": "4", "updated_at": "3"}),
            ("4", "a"),
        )
        self.assertEqual(
            self.mod.cloud_record_sort_key({"dispatch_id": "b", "updated_at": "3", "matched_at": "2"}),
            ("3", "b"),
        )


if __name__ == "__main__":
    unittest.main()
