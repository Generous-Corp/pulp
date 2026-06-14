#!/usr/bin/env python3
"""Tests for locked queue claim lifecycle helpers."""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_claim_lifecycle.py", module_name="pulp_queue_claim_lifecycle")


@contextmanager
def fake_lock(_path, *, blocking):
    assert blocking is True
    yield


class QueueClaimLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_claim_next_job_saves_claimed_queue_and_normalizes(self) -> None:
        queue = [{"id": "job1", "status": "pending"}]
        saved: list[list[dict]] = []

        def claim(loaded_queue, *, runner):
            loaded_queue[0]["status"] = "running"
            loaded_queue[0]["runner"] = runner
            return loaded_queue[0]

        result = self.mod.claim_next_job_locked(
            root="/repo",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            reconcile_running_jobs_unlocked_fn=lambda loaded_queue: (loaded_queue, False),
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(job) for job in saved_queue]),
            claim_next_job_unlocked_fn=claim,
            normalize_job_fn=lambda job: {**job, "normalized": True},
            pid_fn=lambda: 4321,
        )

        self.assertEqual(
            result,
            {"id": "job1", "status": "running", "runner": {"pid": 4321, "root": "/repo"}, "normalized": True},
        )
        self.assertEqual(saved, [[{"id": "job1", "status": "running", "runner": {"pid": 4321, "root": "/repo"}}]])

    def test_claim_next_job_preserves_reconcile_save_when_no_job_claimed(self) -> None:
        queue = [{"id": "stale", "status": "pending"}]
        saved: list[list[dict]] = []

        def reconcile(loaded_queue):
            loaded_queue[0]["requeued_at"] = "now"
            return loaded_queue, True

        result = self.mod.claim_next_job_locked(
            root="/repo",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            reconcile_running_jobs_unlocked_fn=reconcile,
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(job) for job in saved_queue]),
            claim_next_job_unlocked_fn=lambda _queue, *, runner: None,
            normalize_job_fn=lambda job: job,
            pid_fn=lambda: 4321,
        )

        self.assertIsNone(result)
        self.assertEqual(saved, [[{"id": "stale", "status": "pending", "requeued_at": "now"}]])


if __name__ == "__main__":
    unittest.main()
