#!/usr/bin/env python3
"""Tests for GitHub Actions run snapshot helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cloud_run_snapshot.py", add_module_dir=True)


class CloudRunSnapshotTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_summarize_cloud_timing_uses_job_bounds_for_completed_run(self) -> None:
        timing = self.mod.summarize_cloud_timing(
            {
                "createdAt": "2026-04-30T00:00:00Z",
                "updatedAt": "2026-04-30T00:05:00Z",
                "status": "completed",
                "jobs": [
                    {
                        "startedAt": "2026-04-30T00:01:00Z",
                        "completedAt": "2026-04-30T00:03:00Z",
                    }
                ],
            }
        )

        self.assertEqual(timing["started_at"], "2026-04-30T00:01:00Z")
        self.assertEqual(timing["completed_at"], "2026-04-30T00:03:00Z")
        self.assertEqual(timing["queue_delay_secs"], 60.0)
        self.assertEqual(timing["duration_secs"], 120.0)

    def test_summarize_cloud_timing_uses_latest_step_for_in_progress_run(self) -> None:
        timing = self.mod.summarize_cloud_timing(
            {
                "createdAt": "2026-04-30T00:00:00Z",
                "updatedAt": "2026-04-30T00:00:05Z",
                "status": "in_progress",
                "jobs": [
                    {
                        "startedAt": "2026-04-30T00:00:10Z",
                        "steps": [
                            {"startedAt": "2026-04-30T00:00:15Z"},
                            {"completedAt": "2026-04-30T00:00:25Z"},
                        ],
                    }
                ],
            }
        )

        self.assertEqual(timing["started_at"], "2026-04-30T00:00:10Z")
        self.assertEqual(timing["completed_at"], "")
        self.assertEqual(timing["duration_secs"], 15.0)

    def test_snapshot_jobs_normalizes_timestamps(self) -> None:
        jobs = self.mod.snapshot_jobs(
            {
                "jobs": [
                    {
                        "name": "linux",
                        "status": "completed",
                        "conclusion": "success",
                        "startedAt": "2026-04-30T00:00:00+00:00",
                        "completedAt": "0001-01-01T00:00:00Z",
                    }
                ]
            }
        )

        self.assertEqual(
            jobs,
            [
                {
                    "name": "linux",
                    "status": "completed",
                    "conclusion": "success",
                    "started_at": "2026-04-30T00:00:00+00:00",
                    "completed_at": "",
                }
            ],
        )

    def test_update_cloud_record_from_run_updates_record_and_jobs(self) -> None:
        updated = self.mod.update_cloud_record_from_run(
            {"dispatch_id": "abc", "provider_requested": "namespace"},
            {
                "databaseId": 7,
                "workflowName": "Build",
                "headBranch": "feature/cloud",
                "headSha": "a" * 40,
                "status": "completed",
                "conclusion": "success",
                "url": "https://example.test/runs/7",
                "createdAt": "2026-04-30T00:00:00Z",
                "updatedAt": "2026-04-30T00:02:00Z",
                "jobs": [
                    {
                        "name": "linux",
                        "status": "completed",
                        "conclusion": "success",
                        "startedAt": "2026-04-30T00:01:00Z",
                        "completedAt": "2026-04-30T00:02:00Z",
                    }
                ],
            },
            provider_resolved="namespace",
            now_iso_fn=lambda: "unused",
        )

        self.assertEqual(updated["run_id"], 7)
        self.assertEqual(updated["workflow_name"], "Build")
        self.assertEqual(updated["provider_resolved"], "namespace")
        self.assertEqual(updated["matched_at"], "2026-04-30T00:00:00Z")
        self.assertEqual(updated["completed_at"], "2026-04-30T00:02:00Z")
        self.assertEqual(updated["queue_delay_secs"], 60.0)
        self.assertEqual(updated["duration_secs"], 60.0)
        self.assertEqual(updated["jobs"][0]["name"], "linux")

    def test_update_cloud_record_from_run_preserves_newer_record_and_uses_now_fallback(self) -> None:
        stale = self.mod.update_cloud_record_from_run(
            {"dispatch_id": "abc", "updated_at": "2026-04-30T00:05:00Z"},
            {"databaseId": 7, "updatedAt": "2026-04-30T00:04:00Z"},
            now_iso_fn=lambda: "unused",
        )
        self.assertEqual(stale["updated_at"], "2026-04-30T00:05:00Z")
        self.assertIsNone(stale["run_id"])

        fallback = self.mod.update_cloud_record_from_run(
            {"dispatch_id": "abc"},
            {"databaseId": 8, "status": "completed"},
            now_iso_fn=lambda: "2026-04-30T00:06:00Z",
        )
        self.assertEqual(fallback["updated_at"], "2026-04-30T00:06:00Z")
        self.assertEqual(fallback["completed_at"], "2026-04-30T00:06:00Z")


if __name__ == "__main__":
    unittest.main()
