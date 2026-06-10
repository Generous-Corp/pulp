#!/usr/bin/env python3
"""Tests for queue_orchestrator policy helpers."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_orchestrator.py")


def load_module():
    script_dir = str(MODULE_PATH.parent)
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
    spec = importlib.util.spec_from_file_location("pulp_queue_orchestrator", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class QueueOrchestratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_make_job_normalizes_identity_and_submission(self) -> None:
        job = self.mod.make_job(
            " feature/queue-policy ",
            "a" * 40,
            " HIGH ",
            ["windows", "mac"],
            "run",
            " smoke ",
            submission={
                "submitted_root": "/tmp/submitted",
                "provenance": {
                    "execution_kind": "hosted",
                    "hosted_orchestrator": "github-actions",
                    "runner_provider": "github-hosted",
                },
            },
            now_iso_fn=lambda: "2026-06-09T00:00:00+00:00",
            uuid_hex_fn=lambda: "1234567890abcdef",
            root="/tmp/root",
        )

        self.assertEqual(job["id"], "1234567890ab")
        self.assertEqual(job["branch"], "feature/queue-policy")
        self.assertEqual(job["priority"], "high")
        self.assertEqual(job["targets"], ["mac", "windows"])
        self.assertEqual(job["validation"], "smoke")
        self.assertEqual(job["queued_at"], "2026-06-09T00:00:00+00:00")
        self.assertEqual(job["submitted_root"], "/tmp/submitted")
        self.assertEqual(job["provenance"]["execution_kind"], "hosted")
        self.assertEqual(
            job["fingerprint"],
            self.mod.make_fingerprint("feature/queue-policy", "a" * 40, ["windows", "mac"], "smoke"),
        )

        with self.assertRaisesRegex(ValueError, "Unsupported branch"):
            self.mod.make_job("feature/bad;rm", "a" * 40, "normal", ["mac"], "run", "full")

    def test_priority_summary_and_sorting_helpers(self) -> None:
        self.assertEqual(self.mod.default_priority_for("ship", {"defaults": {}}), "high")
        self.assertEqual(
            self.mod.default_priority_for("run", {"defaults": {"priority": "low"}}),
            "low",
        )
        self.assertEqual(
            self.mod.summarize_job(
                {
                    "id": "job123",
                    "branch": "feature/q",
                    "sha": "abcdef1234567890",
                    "priority": "normal",
                    "targets": ["mac"],
                    "validation": "smoke",
                }
            ),
            "[job123] feature/q @ abcdef123456 priority=normal targets=mac validation=smoke",
        )
        self.assertEqual(
            self.mod.summarize_active_targets(
                {"windows": {"status": "running"}, "mac": {"status": "pass"}},
                preferred_order=["mac"],
            ),
            "mac=pass, windows=running",
        )
        self.assertEqual(
            self.mod.status_active_targets(
                {"id": "job123", "active_targets": {"mac": {"status": "pass"}}},
                {"active_job_id": "job123", "active_targets": {"mac": {"status": "running"}}},
            ),
            {"mac": {"status": "pass"}},
        )
        self.assertEqual(
            self.mod.status_active_targets(
                {"id": "job123"},
                {"active_job_id": "job123", "active_targets": {"windows": {"status": "running"}}},
            ),
            {"windows": {"status": "running"}},
        )
        self.assertIsNone(
            self.mod.status_active_targets(
                {"id": "job123"},
                {"active_job_id": "other", "active_targets": {"mac": {"status": "running"}}},
            )
        )
        self.assertEqual(
            self.mod.status_target_states(
                {"targets": ["windows", "mac", "linux", "ios"]},
                {
                    "mac": {"status": "pass"},
                    "windows": {"status": "running"},
                    "linux": {},
                    "extra": {"status": "ignored"},
                },
            ),
            [
                ("windows", {"status": "running"}),
                ("mac", {"status": "pass"}),
            ],
        )
        self.assertEqual(self.mod.status_target_states({"targets": ["mac"]}, None), [])
        self.assertEqual(
            self.mod.status_submission_lines(
                {
                    "submission": {
                        "submitted_root": "/tmp/pulp",
                        "config_path": "/tmp/pulp/.pulp-ci.json",
                        "config_source": "worktree",
                        "provenance": {
                            "execution_kind": "hosted",
                            "hosted_orchestrator": "github-actions",
                            "runner_provider": "github-hosted",
                            "runner_selector": "ubuntu-latest",
                            "run_id": "12345",
                        },
                    }
                }
            ),
            [
                "submission: root=/tmp/pulp config=/tmp/pulp/.pulp-ci.json (worktree)",
                "provenance: hosted via github-actions/github-hosted selector=ubuntu-latest run=12345",
            ],
        )
        self.assertEqual(self.mod.status_submission_lines({"submission": {}}), [])
        self.assertEqual(
            self.mod.target_state_detail_parts(
                {
                    "phase": "build",
                    "validation_mode": "smoke",
                    "transport_mode": "ssh",
                    "test_policy": "smoke",
                    "prepared_state": "ready",
                    "wait_reason": "queue",
                    "cleanup_status": "done",
                    "last_output_at": "2026-06-09T00:00:00Z",
                    "last_heartbeat_at": "2026-06-09T00:01:00Z",
                    "quiet_for_secs": 3,
                    "liveness": "alive",
                    "log_path": "/tmp/pulp/logs/windows.log",
                }
            ),
            [
                "phase=build",
                "mode=smoke",
                "transport=ssh",
                "tests=smoke",
                "prepared=ready",
                "wait=queue",
                "cleanup=done",
                "output=2026-06-09T00:00:00Z",
                "heartbeat=2026-06-09T00:01:00Z",
                "idle=3s",
                "liveness=alive",
                "log=windows.log",
            ],
        )
        self.assertEqual(
            self.mod.status_target_detail_lines(
                {"targets": ["windows"]},
                {
                    "windows": {
                        "phase": "cleanup",
                        "cleanup_status": "done",
                        "last_line": "last output",
                        "cleanup_result": "terminated pid 123",
                    }
                },
            ),
            [
                "windows: phase=cleanup, cleanup=done",
                "  last output",
                "  cleanup: terminated pid 123",
            ],
        )
        self.assertEqual(self.mod.status_runner_line(None), "Runner: idle")
        self.assertEqual(
            self.mod.status_runner_line(
                {
                    "pid": 123,
                    "active_job_id": "job123",
                    "active_branch": "feature/q",
                }
            ),
            "Runner: pid=123 active=[job123] feature/q",
        )
        self.assertEqual(
            self.mod.status_runner_line({"pid": 123}),
            "Runner: pid=123 active=[?] ?",
        )
        self.assertEqual(
            self.mod.recent_completed_status_line(
                {
                    "id": "done123",
                    "branch": "feature/q",
                    "sha": "abcdef1234567890",
                },
                {
                    "overall": "pass",
                    "results": [
                        {"target": "mac", "status": "pass"},
                        {"target": "linux", "status": "skip"},
                    ],
                    "provenance": {
                        "execution_kind": "direct",
                        "direct_backend": "local-ci",
                    },
                },
            ),
            "[done123] feature/q @ abcdef123456 PASS [mac=pass, linux=skip] via direct via local-ci",
        )
        self.assertEqual(
            self.mod.recent_completed_missing_result_line(
                {
                    "id": "missing123",
                    "branch": "feature/q",
                    "sha": "abcdef1234567890",
                    "priority": "normal",
                    "targets": ["mac"],
                }
            ),
            "[missing123] feature/q @ abcdef123456 priority=normal targets=mac (result file missing)",
        )
        self.assertLess(
            self.mod.job_sort_key({"id": "high", "priority": "high", "queued_at": "2"}),
            self.mod.job_sort_key({"id": "low", "priority": "low", "queued_at": "1"}),
        )
        pending, running, completed = self.mod.queue_status_groups(
            [
                {"id": "running", "status": "running"},
                {"id": "pending-low", "status": "pending", "priority": "low", "queued_at": "1"},
                {"id": "completed", "status": "completed"},
                {"id": "pending-high", "status": "pending", "priority": "high", "queued_at": "2"},
            ]
        )
        self.assertEqual([job["id"] for job in pending], ["pending-high", "pending-low"])
        self.assertEqual([job["id"] for job in running], ["running"])
        self.assertEqual([job["id"] for job in completed], ["completed"])
        self.assertEqual(
            [job["id"] for job in self.mod.recent_completed_jobs_for_status([{"id": str(i)} for i in range(7)])],
            ["2", "3", "4", "5", "6"],
        )
        self.assertEqual(
            [job["id"] for job in self.mod.recent_completed_jobs_for_status([{"id": str(i)} for i in range(7)], limit=2)],
            ["5", "6"],
        )
        self.assertEqual(self.mod.recent_completed_jobs_for_status([{"id": "completed"}], limit=0), [])

    def test_result_display_line_helpers(self) -> None:
        result = {
            "validation": "smoke",
            "provenance": {
                "execution_kind": "direct",
                "direct_backend": "local-ci",
            },
            "results": [
                {"target": "mac", "status": "pass", "duration_secs": 12.5},
                {"target": "windows", "status": "fail"},
            ],
            "overall": "fail",
        }

        self.assertEqual(self.mod.result_validation_line(result), "  validation  smoke")
        self.assertIsNone(self.mod.result_validation_line({"validation": "full"}))
        self.assertEqual(self.mod.result_execution_line(result), "  execution   direct via local-ci")
        self.assertEqual(
            self.mod.result_target_lines(result),
            [
                "  mac         PASS          12.5s",
                "  windows     FAIL          0s",
            ],
        )
        self.assertEqual(
            self.mod.target_result_line({"target": "ios", "status": "skip"}),
            "  ios         SKIP          0s",
        )
        self.assertEqual(self.mod.result_overall_line(result), "  overall     FAIL")

    def test_enqueue_duplicate_lookup_and_priority_bump_helpers(self) -> None:
        queue = [
            {"id": "completed", "fingerprint": "same", "status": "completed", "priority": "low"},
            {"id": "pending", "fingerprint": "same", "status": "pending", "priority": "normal"},
            {"id": "running", "fingerprint": "other", "status": "running", "priority": "low"},
        ]

        existing = self.mod.find_active_job_by_fingerprint_unlocked(queue, "same")
        self.assertIs(existing, queue[1])
        self.assertIs(self.mod.find_active_job_by_fingerprint_unlocked(queue, "missing"), None)

        bumped = self.mod.bump_pending_job_priority_unlocked(
            existing,
            "high",
            now_iso_fn=lambda: "2026-06-09T00:01:00+00:00",
        )
        self.assertTrue(bumped)
        self.assertEqual(existing["priority"], "high")
        self.assertEqual(existing["bumped_at"], "2026-06-09T00:01:00+00:00")

        self.assertFalse(self.mod.bump_pending_job_priority_unlocked(existing, "normal"))
        self.assertFalse(self.mod.bump_pending_job_priority_unlocked(queue[2], "high"))
        self.assertNotIn("bumped_at", queue[2])

    def test_queue_command_lookup_and_priority_mutation_helpers(self) -> None:
        queue = [
            {"id": "pending", "branch": "feature/pending", "status": "pending", "priority": "normal"},
            {"id": "running", "branch": "feature/running", "status": "running", "priority": "low"},
            {"id": "completed", "branch": "feature/completed", "status": "completed", "priority": "high"},
        ]

        self.assertIs(
            self.mod.find_queue_command_job_unlocked(queue, "feature/pending"),
            queue[0],
        )
        self.assertIs(
            self.mod.find_queue_command_job_unlocked(queue, "running"),
            queue[1],
        )
        self.assertIsNone(self.mod.find_queue_command_job_unlocked(queue, "feature/completed"))
        self.assertIsNone(self.mod.find_queue_command_job_unlocked(queue, "missing"))

        updated = self.mod.set_pending_job_priority_unlocked(
            queue[0],
            "low",
            now_iso_fn=lambda: "2026-06-09T00:06:00+00:00",
        )

        self.assertTrue(updated)
        self.assertEqual(queue[0]["priority"], "low")
        self.assertEqual(queue[0]["bumped_at"], "2026-06-09T00:06:00+00:00")
        self.assertFalse(self.mod.set_pending_job_priority_unlocked(queue[1], "high"))
        self.assertEqual(queue[1]["priority"], "low")
        self.assertNotIn("bumped_at", queue[1])

    def test_queue_command_result_line_helpers(self) -> None:
        self.assertEqual(
            self.mod.bump_queue_command_result_line({"status": "missing"}, "missing"),
            (1, "No active job matches 'missing'."),
        )
        self.assertEqual(
            self.mod.bump_queue_command_result_line(
                {"status": "not_pending", "job_status": "running"},
                "running",
            ),
            (1, "Job is already running; only pending jobs can be reprioritized."),
        )
        self.assertEqual(
            self.mod.bump_queue_command_result_line({"status": "updated", "summary": "summary"}, "job"),
            (0, "Updated priority: summary"),
        )
        self.assertEqual(
            self.mod.cancel_queue_command_result_line({"status": "missing"}, "missing"),
            (1, "No active job matches 'missing'."),
        )
        self.assertEqual(
            self.mod.cancel_queue_command_result_line(
                {"status": "not_pending", "job_status": "running"},
                "running",
            ),
            (1, "Job is already running; only pending jobs can be canceled safely."),
        )
        self.assertEqual(
            self.mod.cancel_queue_command_result_line({"status": "canceled", "summary": "summary"}, "job"),
            (0, "Canceled: summary"),
        )

    def test_runner_info_active_target_update_matches_active_job_only(self) -> None:
        info = {
            "active_job_id": "job123",
            "active_targets": {"mac": {"status": "running"}},
            "updated_at": "2026-06-09T00:00:00+00:00",
        }

        updated = self.mod.update_runner_info_active_targets(
            info,
            "job123",
            {"mac": {"status": "pass"}, "windows": {"status": "running"}},
            now_iso_fn=lambda: "2026-06-09T00:01:00+00:00",
        )

        self.assertTrue(updated)
        self.assertEqual(info["active_targets"]["mac"], {"status": "pass"})
        self.assertEqual(info["active_targets"]["windows"], {"status": "running"})
        self.assertEqual(info["updated_at"], "2026-06-09T00:01:00+00:00")

        cleared = self.mod.update_runner_info_active_targets(
            info,
            "job123",
            None,
            now_iso_fn=lambda: "2026-06-09T00:02:00+00:00",
        )

        self.assertTrue(cleared)
        self.assertNotIn("active_targets", info)
        self.assertEqual(info["updated_at"], "2026-06-09T00:02:00+00:00")

        unchanged = dict(info)
        self.assertFalse(
            self.mod.update_runner_info_active_targets(
                info,
                "other-job",
                {"mac": {"status": "fail"}},
            )
        )
        self.assertEqual(info, unchanged)

    def test_supersedence_and_terminal_result_payloads(self) -> None:
        older = {
            "id": "older",
            "branch": "feature/q",
            "sha": "a" * 40,
            "fingerprint": "old",
            "targets": ["mac", "windows"],
            "priority": "normal",
            "validation": "full",
            "queued_at": "2026-06-09T00:00:00+00:00",
        }
        newer_sha = dict(older, id="newer", sha="b" * 40, fingerprint="new")
        narrower = dict(older, id="narrow", fingerprint="narrow", targets=["mac"])

        self.assertEqual(self.mod.supersedence_reason(newer_sha, older), "newer_sha_queued")
        self.assertEqual(self.mod.supersedence_reason(narrower, older), "narrower_scope_queued")
        self.assertIsNone(self.mod.supersedence_reason(older, older))
        completed_older = dict(older, id="completed", fingerprint="completed", status="completed")
        older["status"] = "pending"
        self.assertEqual(
            self.mod.pending_supersedence_candidates_unlocked(
                [older, completed_older, newer_sha],
                newer_sha,
            ),
            [(older, "newer_sha_queued")],
        )

        superseded = self.mod.supersedence_result(
            older,
            "newer",
            "newer_sha_queued",
            now_iso_fn=lambda: "2026-06-09T00:01:00+00:00",
        )
        self.assertEqual(superseded["overall"], "superseded")
        self.assertEqual(superseded["completed_at"], "2026-06-09T00:01:00+00:00")

        canceled = self.mod.cancellation_result(
            older,
            "operator_canceled",
            now_iso_fn=lambda: "2026-06-09T00:02:00+00:00",
        )
        self.assertEqual(canceled["overall"], "canceled")
        self.assertEqual(canceled["canceled_reason"], "operator_canceled")

    def test_stale_running_replacement_prefers_newest_superseding_job(self) -> None:
        stale = {
            "id": "stale",
            "branch": "feature/q",
            "sha": "a" * 40,
            "fingerprint": "stale",
            "targets": ["mac", "windows"],
            "priority": "normal",
            "validation": "full",
            "queued_at": "2026-06-09T00:00:00+00:00",
            "status": "running",
        }
        older_replacement = dict(
            stale,
            id="older-replacement",
            sha="b" * 40,
            fingerprint="older-replacement",
            queued_at="2026-06-09T00:01:00+00:00",
            status="pending",
        )
        newer_replacement = dict(
            stale,
            id="newer-replacement",
            sha="c" * 40,
            fingerprint="newer-replacement",
            queued_at="2026-06-09T00:02:00+00:00",
            status="running",
        )
        completed_replacement = dict(
            stale,
            id="completed-replacement",
            sha="d" * 40,
            fingerprint="completed-replacement",
            queued_at="2026-06-09T00:03:00+00:00",
            status="completed",
        )

        replacement, reason = self.mod.find_stale_running_replacement_unlocked(
            [stale, older_replacement, newer_replacement, completed_replacement],
            stale,
        )

        self.assertIs(replacement, newer_replacement)
        self.assertEqual(reason, "newer_sha_queued")

        requeue_only = dict(
            stale,
            id="requeue-only",
            branch="feature/other",
            fingerprint="requeue-only",
            queued_at="2026-06-09T00:04:00+00:00",
        )
        actions = self.mod.stale_running_reconciliation_actions_unlocked(
            [stale, older_replacement, newer_replacement, completed_replacement, requeue_only],
            [stale, requeue_only],
        )

        self.assertEqual(actions[0]["action"], "supersede")
        self.assertIs(actions[0]["job"], stale)
        self.assertIs(actions[0]["replacement"], newer_replacement)
        self.assertEqual(actions[0]["reason"], "newer_sha_queued")
        self.assertEqual(actions[1], {"action": "requeue", "job": requeue_only})

    def test_stale_running_jobs_for_runner_skips_current_runner_pid(self) -> None:
        current = {"id": "current", "status": "running", "runner": {"pid": 123}}
        other = {"id": "other", "status": "running", "runner": {"pid": 456}}
        missing_runner = {"id": "missing-runner", "status": "running"}
        pending = {"id": "pending", "status": "pending", "runner": {"pid": 456}}

        self.assertEqual(
            self.mod.stale_running_jobs_for_runner_unlocked(
                [current, other, missing_runner, pending],
                123,
            ),
            [other, missing_runner],
        )

    def test_stale_running_jobs_for_runner_marks_all_running_without_runner_pid(self) -> None:
        running = {"id": "running", "status": "running", "runner": {"pid": 123}}
        running_without_pid = {"id": "running-without-pid", "status": "running", "runner": {}}
        completed = {"id": "completed", "status": "completed", "runner": {"pid": 123}}

        self.assertEqual(
            self.mod.stale_running_jobs_for_runner_unlocked(
                [running, running_without_pid, completed],
                None,
            ),
            [running, running_without_pid],
        )

    def test_requeue_stale_running_job_preserves_progress_snapshot(self) -> None:
        job = {
            "id": "stale",
            "status": "running",
            "started_at": "2026-06-09T00:00:00+00:00",
            "runner": {"pid": 123},
            "active_targets": {"mac": {"status": "running"}},
            "last_progress_at": "2026-06-09T00:01:00+00:00",
        }

        self.mod.requeue_stale_running_job_unlocked(
            job,
            now_iso_fn=lambda: "2026-06-09T00:02:00+00:00",
        )

        self.assertEqual(job["status"], "pending")
        self.assertEqual(job["requeued_at"], "2026-06-09T00:02:00+00:00")
        self.assertNotIn("started_at", job)
        self.assertNotIn("runner", job)
        self.assertEqual(job["active_targets"], {"mac": {"status": "running"}})
        self.assertEqual(job["last_progress_at"], "2026-06-09T00:01:00+00:00")

    def test_terminal_result_completion_updates_job_state(self) -> None:
        job = {
            "id": "old",
            "branch": "feature/q",
            "sha": "a" * 40,
            "priority": "normal",
            "targets": ["mac"],
            "queued_at": "2026-06-09T00:00:00+00:00",
            "status": "running",
            "runner": {"pid": 123},
            "active_targets": {"mac": {"status": "running"}},
            "last_progress_at": "2026-06-09T00:00:30+00:00",
        }
        result = self.mod.supersedence_result(
            job,
            "newer",
            "newer_sha_queued",
            now_iso_fn=lambda: "2026-06-09T00:01:00+00:00",
        )

        self.mod.complete_job_with_result_unlocked(job, result, "/tmp/result.json")

        self.assertEqual(job["status"], "completed")
        self.assertEqual(job["completed_at"], "2026-06-09T00:01:00+00:00")
        self.assertEqual(job["result_file"], "/tmp/result.json")
        self.assertEqual(job["overall"], "superseded")
        self.assertEqual(job["superseded_by"], "newer")
        self.assertEqual(job["superseded_reason"], "newer_sha_queued")
        self.assertNotIn("runner", job)
        self.assertNotIn("active_targets", job)
        self.assertNotIn("last_progress_at", job)

    def test_completed_queue_trimming_keeps_newest_completed_and_running(self) -> None:
        queue = [
            {"id": "old", "status": "completed", "completed_at": "2026-06-09T00:00:00+00:00"},
            {"id": "new", "status": "completed", "completed_at": "2026-06-09T00:01:00+00:00"},
            {"id": "running", "status": "running"},
        ]

        trimmed, removed = self.mod.trim_completed_jobs_with_removed_ids(queue, keep_completed_jobs=1)
        self.assertEqual({job["id"] for job in trimmed}, {"new", "running"})
        self.assertEqual(removed, {"old"})
        self.assertEqual(
            self.mod.trim_completed_jobs(queue, keep_completed_jobs=1),
            trimmed,
        )

    def test_job_lookup_and_active_target_updates(self) -> None:
        queue = [
            {"id": "abc111", "branch": "feature/a", "status": "pending"},
            {"id": "abc222", "branch": "feature/a", "status": "completed"},
            {"id": "def333", "branch": "feature/b", "status": "running"},
        ]

        self.assertEqual(self.mod.find_job_unlocked(queue, "def333")["branch"], "feature/b")
        self.assertEqual(self.mod.find_job_unlocked(queue, "def")["id"], "def333")
        self.assertEqual(self.mod.find_job_unlocked(queue, "feature/a", {"pending"})["id"], "abc111")
        self.assertIsNone(self.mod.find_job_unlocked(queue, "missing"))
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.mod.find_job_unlocked(queue, "abc")
        with self.assertRaisesRegex(ValueError, "Multiple jobs match branch"):
            self.mod.find_job_unlocked(queue, "feature/a")

        self.assertEqual(self.mod.select_job_for_logs(queue, None, "def333")["branch"], "feature/b")
        self.assertEqual(
            self.mod.select_job_for_logs(queue, {"active_job_id": "def333"}, None)["id"],
            "def333",
        )
        self.assertEqual(self.mod.select_job_for_logs(queue, None, None)["id"], "abc222")
        self.assertIsNone(self.mod.select_job_for_logs([{"id": "pending", "status": "pending"}], None, None))

        updated = self.mod.upsert_job_active_targets_unlocked(
            queue,
            "def333",
            {"mac": {"status": "running"}},
            now_iso_fn=lambda: "2026-06-09T00:03:00+00:00",
        )
        self.assertTrue(updated)
        self.assertEqual(queue[2]["active_targets"]["mac"]["status"], "running")
        self.assertEqual(queue[2]["last_progress_at"], "2026-06-09T00:03:00+00:00")

        cleared = self.mod.upsert_job_active_targets_unlocked(queue, "def333", None)
        self.assertTrue(cleared)
        self.assertNotIn("active_targets", queue[2])
        self.assertNotIn("last_progress_at", queue[2])
        self.assertFalse(self.mod.upsert_job_active_targets_unlocked(queue, "unknown", {}))

        target_updated = self.mod.update_job_target_state_unlocked(
            queue,
            "def333",
            "windows",
            {"status": "running", "pid": 42},
            now_iso_fn=lambda: "2026-06-09T00:04:00+00:00",
        )
        self.assertTrue(target_updated)
        self.assertEqual(queue[2]["active_targets"]["windows"]["status"], "running")
        self.assertEqual(queue[2]["active_targets"]["windows"]["pid"], 42)
        self.assertEqual(queue[2]["last_progress_at"], "2026-06-09T00:04:00+00:00")

        self.mod.update_job_target_state_unlocked(
            queue,
            "def333",
            "windows",
            {"status": "pass", "pid": None},
            now_iso_fn=lambda: "2026-06-09T00:05:00+00:00",
        )
        self.assertEqual(queue[2]["active_targets"]["windows"], {"status": "pass"})
        self.assertEqual(queue[2]["last_progress_at"], "2026-06-09T00:05:00+00:00")

        self.mod.update_job_target_state_unlocked(queue, "def333", "windows", {"status": None})
        self.assertNotIn("active_targets", queue[2])
        self.assertNotIn("last_progress_at", queue[2])
        self.assertFalse(
            self.mod.update_job_target_state_unlocked(queue, "unknown", "mac", {"status": "running"})
        )

    def test_complete_job_marks_terminal_state_and_clears_runner_progress(self) -> None:
        queue = [
            {
                "id": "running",
                "branch": "feature/running",
                "status": "running",
                "runner": {"pid": 123},
                "active_targets": {"mac": {"status": "running"}},
                "last_progress_at": "2026-06-09T00:04:00+00:00",
            },
            {"id": "pending", "branch": "feature/pending", "status": "pending"},
        ]

        completed = self.mod.complete_job_unlocked(
            queue,
            "running",
            {"overall": "pass"},
            Path("/tmp/local-ci/results/running.json"),
            now_iso_fn=lambda: "2026-06-09T00:05:00+00:00",
        )

        self.assertTrue(completed)
        self.assertEqual(queue[0]["status"], "completed")
        self.assertEqual(queue[0]["completed_at"], "2026-06-09T00:05:00+00:00")
        self.assertEqual(queue[0]["result_file"], "/tmp/local-ci/results/running.json")
        self.assertEqual(queue[0]["overall"], "pass")
        self.assertNotIn("runner", queue[0])
        self.assertNotIn("active_targets", queue[0])
        self.assertNotIn("last_progress_at", queue[0])
        self.assertEqual(queue[1]["status"], "pending")
        self.assertFalse(
            self.mod.complete_job_unlocked(
                queue,
                "missing",
                {"overall": "fail"},
                "/tmp/missing.json",
            )
        )

    def test_claim_next_job_marks_highest_priority_pending_running(self) -> None:
        queue = [
            {
                "id": "low",
                "branch": "feature/low",
                "status": "pending",
                "priority": "low",
                "queued_at": "2026-06-09T00:00:00+00:00",
            },
            {"id": "running", "branch": "feature/running", "status": "running"},
            {
                "id": "high",
                "branch": "feature/high",
                "status": "pending",
                "priority": "high",
                "queued_at": "2026-06-09T00:01:00+00:00",
                "active_targets": {"mac": {"status": "stale"}},
                "last_progress_at": "2026-06-09T00:02:00+00:00",
            },
        ]

        claimed = self.mod.claim_next_job_unlocked(
            queue,
            runner={"pid": 123, "root": "/tmp/pulp"},
            now_iso_fn=lambda: "2026-06-09T00:03:00+00:00",
        )
        self.assertEqual(claimed["id"], "high")
        self.assertEqual(queue[2]["status"], "running")
        self.assertEqual(queue[2]["started_at"], "2026-06-09T00:03:00+00:00")
        self.assertEqual(queue[2]["runner"], {"pid": 123, "root": "/tmp/pulp"})
        self.assertNotIn("active_targets", queue[2])
        self.assertNotIn("last_progress_at", queue[2])

        next_claimed = self.mod.claim_next_job_unlocked(
            queue,
            runner={"pid": 124, "root": "/tmp/pulp"},
            now_iso_fn=lambda: "2026-06-09T00:04:00+00:00",
        )
        self.assertEqual(next_claimed["id"], "low")
        self.assertIsNone(
            self.mod.claim_next_job_unlocked(
                queue,
                runner={"pid": 125, "root": "/tmp/pulp"},
            )
        )


if __name__ == "__main__":
    unittest.main()
