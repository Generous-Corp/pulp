#!/usr/bin/env python3
"""Tests for locked queue lifecycle helpers."""

from __future__ import annotations

from contextlib import contextmanager
import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_lifecycle.py")


def load_module():
    script_dir = str(MODULE_PATH.parent)
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
    spec = importlib.util.spec_from_file_location("pulp_queue_lifecycle", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


@contextmanager
def fake_lock(_path, *, blocking):
    assert blocking is True
    yield


class QueueLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_load_job_reconciles_saves_and_normalizes_match(self) -> None:
        queue = [{"id": "job1", "status": "running"}]
        saved: list[list[dict]] = []

        def reconcile(loaded_queue):
            loaded_queue[0]["status"] = "pending"
            return loaded_queue, True

        result = self.mod.load_job_locked(
            "job1",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            reconcile_running_jobs_unlocked_fn=reconcile,
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(job) for job in saved_queue]),
            find_job_unlocked_fn=lambda loaded_queue, job_id: next(
                (job for job in loaded_queue if job["id"] == job_id),
                None,
            ),
            normalize_job_fn=lambda job: {**job, "normalized": True},
        )

        self.assertEqual(result, {"id": "job1", "status": "pending", "normalized": True})
        self.assertEqual(saved, [[{"id": "job1", "status": "pending"}]])

    def test_load_job_returns_none_without_save_when_unchanged_missing(self) -> None:
        saved: list[list[dict]] = []

        result = self.mod.load_job_locked(
            "missing",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: [{"id": "job1"}],
            reconcile_running_jobs_unlocked_fn=lambda queue: (queue, False),
            save_queue_unlocked_fn=lambda queue: saved.append(queue),
            find_job_unlocked_fn=lambda _queue, _job_id: None,
            normalize_job_fn=lambda job: job,
        )

        self.assertIsNone(result)
        self.assertEqual(saved, [])

    def test_claim_next_job_saves_claimed_queue_and_normalizes(self) -> None:
        queue = [{"id": "job1", "status": "pending"}]
        saved: list[list[dict]] = []

        def claim(loaded_queue, *, runner):
            self.assertEqual(runner, {"pid": 4321, "root": "/repo"})
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
            {
                "id": "job1",
                "status": "running",
                "runner": {"pid": 4321, "root": "/repo"},
                "normalized": True,
            },
        )
        self.assertEqual(
            saved,
            [[{"id": "job1", "status": "running", "runner": {"pid": 4321, "root": "/repo"}}]],
        )

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
