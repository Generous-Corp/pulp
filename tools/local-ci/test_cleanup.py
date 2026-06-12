#!/usr/bin/env python3
"""No-network tests for local-ci stale validator cleanup helpers."""

from __future__ import annotations

import pathlib
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("cleanup.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CleanupTests(unittest.TestCase):
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

        self.assertEqual(
            candidates,
            [
                {
                    "job_id": "job1",
                    "target": "windows",
                    "host": "win",
                    "validator_pid": 123,
                    "validator_started_at": "2026-05-01T00:00:00Z",
                }
            ],
        )
        windows_state = queue[0]["active_targets"]["windows"]
        self.assertEqual(windows_state["cleanup_requested_at"], "now")
        self.assertEqual(windows_state["cleanup_status"], "requested")
        self.assertEqual(windows_state["cleanup_reason"], "stale_runner_recovery")
        self.assertEqual(queue[0]["last_progress_at"], "now")
        self.assertEqual(queue[1]["active_targets"]["windows"]["cleanup_requested_at"], "already")

    def test_cleanup_stale_windows_validator_parses_remote_json(self) -> None:
        captured = {}

        def fake_run_logged_command(command, **kwargs):
            captured["command"] = command
            captured["input_text"] = kwargs.get("input_text", "")
            captured["timeout"] = kwargs.get("timeout")
            return {
                "returncode": 0,
                "output": 'banner\n{"found":true,"matched":true,"killed":true,"children":[456]}\n',
            }

        result = self.mod.cleanup_stale_windows_validator(
            "win",
            123,
            "2026-05-01T00:00:00Z",
            ps_literal_fn=lambda value: value,
            run_logged_command_fn=fake_run_logged_command,
            windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
            trim_line_fn=lambda line: line[:100],
        )

        self.assertTrue(result["killed"])
        self.assertEqual(result["children"], [456])
        self.assertEqual(captured["command"], ["ssh", "win"])
        self.assertEqual(captured["timeout"], 120)
        self.assertIn("$PidToKill = 123", captured["input_text"])
        self.assertIn("$ExpectedStart = '2026-05-01T00:00:00Z'", captured["input_text"])

    def test_cleanup_stale_windows_validator_reports_non_json_error(self) -> None:
        result = self.mod.cleanup_stale_windows_validator(
            "win",
            123,
            "",
            ps_literal_fn=lambda value: value,
            run_logged_command_fn=lambda _command, **_kwargs: {"returncode": 7, "output": "not json\n"},
            windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
            trim_line_fn=lambda line: line[:8],
        )

        self.assertEqual(result["error"], "not json")

    def test_stale_windows_validator_status_and_update_fields(self) -> None:
        candidate = {
            "validator_pid": 123,
            "validator_started_at": "2026-05-01T00:00:00Z",
        }

        self.assertEqual(self.mod.stale_windows_validator_cleanup_status({"killed": True}), "killed")
        self.assertEqual(self.mod.stale_windows_validator_cleanup_status({"found": False}), "not-found")
        self.assertEqual(
            self.mod.stale_windows_validator_cleanup_status({"found": True, "matched": False}),
            "mismatch",
        )
        self.assertEqual(self.mod.stale_windows_validator_cleanup_status({"error": "boom"}), "error")
        self.assertEqual(self.mod.stale_windows_validator_cleanup_status({"found": True, "matched": True}), "checked")

        killed = self.mod.stale_windows_validator_update_fields(
            candidate,
            {"found": True, "matched": True, "killed": True, "pid": 123},
            now_fn=lambda: "done",
            trim_line_fn=lambda line: line,
        )
        self.assertEqual(killed["cleanup_completed_at"], "done")
        self.assertEqual(killed["cleanup_status"], "killed")
        self.assertIsNone(killed["validator_pid"])
        self.assertIsNone(killed["validator_started_at"])

        mismatch = self.mod.stale_windows_validator_update_fields(
            candidate,
            {"found": True, "matched": False, "pid": 123},
            now_fn=lambda: "done",
            trim_line_fn=lambda line: line,
        )
        self.assertEqual(mismatch["cleanup_status"], "mismatch")
        self.assertEqual(mismatch["validator_pid"], 123)
        self.assertEqual(mismatch["validator_started_at"], "2026-05-01T00:00:00Z")

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
        self.assertEqual(updates[0][1], "windows")
        self.assertEqual(updates[0][2]["cleanup_status"], "killed")
        self.assertIsNone(updates[0][2]["validator_pid"])


if __name__ == "__main__":
    unittest.main()
