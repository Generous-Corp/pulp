#!/usr/bin/env python3
"""Tests for stale Windows validator cleanup reclaim dispatch."""

from __future__ import annotations

import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cleanup_stale_windows_reclaim.py", add_module_dir=True)


class CleanupStaleWindowsReclaimTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_reclaim_stale_remote_validator_candidates_updates_targets(self) -> None:
        candidates = [
            {
                "job_id": "job1",
                "target": "windows",
                "host": "win",
                "validator_pid": 123,
                "validator_started_at": "2026-05-01T00:00:00Z",
            }
        ]
        updates = []

        reclaimed = self.mod.reclaim_stale_remote_validator_candidates(
            candidates,
            cleanup_validator_fn=mock.Mock(return_value={"found": True, "matched": True, "killed": True, "pid": 123}),
            update_job_target_state_fn=lambda job_id, target, **fields: updates.append((job_id, target, fields)),
            now_fn=lambda: "done",
            trim_line_fn=lambda line: line,
        )

        self.assertEqual(reclaimed, 1)
        self.assertEqual(updates[0][0], "job1")
        self.assertEqual(updates[0][2]["cleanup_status"], "killed")


if __name__ == "__main__":
    unittest.main()
