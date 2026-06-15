#!/usr/bin/env python3
"""Tests for locked queue command mutation helpers."""

from __future__ import annotations

from contextlib import contextmanager
import unittest
from pathlib import Path

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_command_lifecycle.py", module_name="pulp_queue_command_lifecycle", add_module_dir=True)


@contextmanager
def fake_lock(_path, *, blocking):
    assert blocking is True
    yield


class QueueCommandLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_bump_queue_command_job_updates_pending_job_and_saves(self) -> None:
        job = {"id": "job1", "status": "pending", "priority": "normal"}
        queue = [job]
        saved: list[list[dict]] = []

        def find(loaded_queue, job_ref):
            self.assertIs(loaded_queue, queue)
            self.assertEqual(job_ref, "job1")
            return job

        def set_priority(current_job, requested_priority):
            self.assertIs(current_job, job)
            self.assertEqual(requested_priority, "high")
            current_job["priority"] = requested_priority
            current_job["bumped_at"] = "now"
            return True

        result = self.mod.bump_queue_command_job_locked(
            "job1",
            "high",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            find_queue_command_job_unlocked_fn=find,
            set_pending_job_priority_unlocked_fn=set_priority,
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(item) for item in saved_queue]),
            summarize_job_fn=lambda current_job: f"summary:{current_job['id']}:{current_job['priority']}",
        )

        self.assertEqual(result, {"status": "updated", "summary": "summary:job1:high"})
        self.assertEqual(saved, [[{"id": "job1", "status": "pending", "priority": "high", "bumped_at": "now"}]])

    def test_bump_queue_command_job_reports_missing_and_non_pending_without_save(self) -> None:
        saved: list[list[dict]] = []
        missing = self.mod.bump_queue_command_job_locked(
            "missing",
            "high",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: [],
            find_queue_command_job_unlocked_fn=lambda queue, job_ref: None,
            set_pending_job_priority_unlocked_fn=lambda job, priority: self.fail("unexpected priority update"),
            save_queue_unlocked_fn=lambda queue: saved.append(queue),
            summarize_job_fn=lambda job: self.fail("unexpected summary"),
        )
        self.assertEqual(missing, {"status": "missing", "job_ref": "missing"})

        running = {"id": "job2", "status": "running", "priority": "normal"}
        not_pending = self.mod.bump_queue_command_job_locked(
            "job2",
            "low",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: [running],
            find_queue_command_job_unlocked_fn=lambda queue, job_ref: running,
            set_pending_job_priority_unlocked_fn=lambda job, priority: False,
            save_queue_unlocked_fn=lambda queue: saved.append(queue),
            summarize_job_fn=lambda job: self.fail("unexpected summary"),
        )

        self.assertEqual(not_pending, {"status": "not_pending", "job_status": "running"})
        self.assertEqual(saved, [])

    def test_cancel_queue_command_job_cancels_pending_job_trims_and_saves(self) -> None:
        job = {"id": "job1", "status": "pending", "priority": "normal"}
        queue = [job, {"id": "old", "status": "completed"}]
        saved: list[list[dict]] = []
        events: list[str] = []

        def cancel(current_job):
            events.append(f"cancel:{current_job['id']}")
            current_job["status"] = "completed"
            current_job["overall"] = "canceled"

        def trim(loaded_queue):
            events.append("trim")
            self.assertIs(loaded_queue, queue)
            return [job]

        result = self.mod.cancel_queue_command_job_locked(
            "job1",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            find_queue_command_job_unlocked_fn=lambda loaded_queue, job_ref: job,
            cancel_job_unlocked_fn=cancel,
            trim_completed_jobs_fn=trim,
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(item) for item in saved_queue]),
            summarize_job_fn=lambda current_job: f"summary:{current_job['id']}:{current_job['status']}",
        )

        self.assertEqual(result, {"status": "canceled", "summary": "summary:job1:completed"})
        self.assertEqual(events, ["cancel:job1", "trim"])
        self.assertEqual(saved, [[{"id": "job1", "status": "completed", "priority": "normal", "overall": "canceled"}]])

    def test_cancel_queue_command_job_reports_missing_and_non_pending_without_save(self) -> None:
        saved: list[list[dict]] = []
        missing = self.mod.cancel_queue_command_job_locked(
            "missing",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: [],
            find_queue_command_job_unlocked_fn=lambda queue, job_ref: None,
            cancel_job_unlocked_fn=lambda job: self.fail("unexpected cancel"),
            trim_completed_jobs_fn=lambda queue: self.fail("unexpected trim"),
            save_queue_unlocked_fn=lambda queue: saved.append(queue),
            summarize_job_fn=lambda job: self.fail("unexpected summary"),
        )
        self.assertEqual(missing, {"status": "missing", "job_ref": "missing"})

        running = {"id": "job2", "status": "running", "priority": "normal"}
        not_pending = self.mod.cancel_queue_command_job_locked(
            "job2",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: [running],
            find_queue_command_job_unlocked_fn=lambda queue, job_ref: running,
            cancel_job_unlocked_fn=lambda job: self.fail("unexpected cancel"),
            trim_completed_jobs_fn=lambda queue: self.fail("unexpected trim"),
            save_queue_unlocked_fn=lambda queue: saved.append(queue),
            summarize_job_fn=lambda job: self.fail("unexpected summary"),
        )

        self.assertEqual(not_pending, {"status": "not_pending", "job_status": "running"})
        self.assertEqual(saved, [])


if __name__ == "__main__":
    unittest.main()
