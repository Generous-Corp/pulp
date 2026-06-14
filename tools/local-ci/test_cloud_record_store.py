#!/usr/bin/env python3
"""Tests for cloud-run record persistence helpers."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cloud_record_store.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CloudRecordStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_save_load_list_records_and_skip_invalid_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cloud_dir = pathlib.Path(tmp)
            calls = []

            def record_path(dispatch_id: str) -> pathlib.Path:
                return cloud_dir / f"{dispatch_id}.json"

            saved_path = self.mod.save_cloud_record(
                {"dispatch_id": "newer", "updated_at": "2026-04-04T12:01:00+00:00"},
                ensure_state_dirs_fn=lambda: calls.append("ensure"),
                cloud_run_path_fn=record_path,
            )
            self.assertEqual(saved_path, cloud_dir / "newer.json")
            self.assertEqual(calls, ["ensure"])
            self.assertEqual(self.mod.load_cloud_record(saved_path)["dispatch_id"], "newer")

            (cloud_dir / "older.json").write_text(
                json.dumps({"dispatch_id": "older", "updated_at": "2026-04-04T12:00:00+00:00"}) + "\n"
            )
            (cloud_dir / "invalid.json").write_text("{bad")

            records = self.mod.list_cloud_records(
                ensure_state_dirs_fn=lambda: None,
                cloud_runs_dir_fn=lambda: cloud_dir,
            )
            self.assertEqual([record["dispatch_id"] for record in records], ["newer", "older"])
            limited = self.mod.list_cloud_records(
                limit=1,
                ensure_state_dirs_fn=lambda: None,
                cloud_runs_dir_fn=lambda: cloud_dir,
            )
            self.assertEqual([record["dispatch_id"] for record in limited], ["newer"])
            empty = self.mod.list_cloud_records(
                limit=0,
                ensure_state_dirs_fn=lambda: None,
                cloud_runs_dir_fn=lambda: cloud_dir,
            )
            self.assertEqual(empty, [])

    def test_load_result_normalizes_result_payload(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "result.json"
            path.write_text(json.dumps({"target": "mac", "submission": {}}) + "\n")
            result = self.mod.load_result(path)
            self.assertEqual(result["target"], "mac")
            self.assertEqual(result["provenance"]["execution_kind"], "direct")

    def test_cloud_record_summary_includes_selector_timing_provider_time_and_cost(self) -> None:
        summary = self.mod.cloud_record_summary(
            {
                "dispatch_id": "abc123",
                "workflow_key": "build",
                "head_branch": "feature/cloud",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "runner_selector_json": '["self-hosted","Linux"]',
                "run_id": 42,
                "duration_secs": 61,
                "usage_summary": {"provider_runtime_secs": 120},
            },
            {"billing": True},
            estimate_cloud_record_cost_fn=lambda _record, _config: {
                "status": "estimated",
                "estimated_total": 1.25,
                "currency": "USD",
            },
            format_currency_amount_fn=lambda amount, currency="USD": f"{currency} {amount:.2f}",
        )

        self.assertEqual(
            summary,
            "[abc123] build ref=feature/cloud provider=namespace COMPLETED/SUCCESS "
            "selector=self-hosted,Linux gha#42 duration=1m01s provider_time=2m00s cost=est USD 1.25",
        )

    def test_cloud_record_summary_omits_unestimated_cost(self) -> None:
        summary = self.mod.cloud_record_summary(
            {"dispatch_id": "abc123", "workflow_key": "build", "requested_ref": "main"},
            {},
            estimate_cloud_record_cost_fn=lambda _record, _config: {"status": "unavailable"},
        )
        self.assertEqual(summary, "[abc123] build ref=main provider=github-hosted UNRESOLVED")


if __name__ == "__main__":
    unittest.main()
