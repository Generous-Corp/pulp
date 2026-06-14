#!/usr/bin/env python3
"""Tests for stale Windows validator cleanup candidate selection."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cleanup_stale_windows_candidates.py", add_module_dir=True)


class CleanupStaleWindowsCandidatesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_collect_stale_windows_cleanup_candidates_marks_queue_state(self) -> None:
        queue = [
            {
                "id": "job1",
                "status": "running",
                "active_targets": {
                    "windows": {
                        "host": "win",
                        "validator_pid": "123",
                        "validator_started_at": "2026-05-01T00:00:00Z",
                    }
                },
            },
            {
                "id": "job2",
                "status": "running",
                "active_targets": {
                    "windows": {
                        "host": "win",
                        "validator_pid": 456,
                        "validator_started_at": "2026-05-01T00:01:00Z",
                        "cleanup_requested_at": "already",
                    }
                },
            },
        ]

        candidates = self.mod.collect_stale_windows_cleanup_candidates_unlocked(
            queue,
            stale_running_jobs_fn=lambda jobs: jobs,
            now_fn=lambda: "now",
        )

        self.assertEqual(candidates[0]["validator_pid"], 123)
        self.assertEqual(queue[0]["active_targets"]["windows"]["cleanup_status"], "requested")
        self.assertEqual(queue[1]["active_targets"]["windows"]["cleanup_requested_at"], "already")


if __name__ == "__main__":
    unittest.main()
