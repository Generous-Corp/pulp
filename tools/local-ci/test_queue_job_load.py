#!/usr/bin/env python3
"""Tests for locked single-job queue loading."""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_job_load.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_queue_job_load", add_module_dir=True)


@contextmanager
def fake_lock(_path, *, blocking):
    assert blocking is True
    yield


class QueueJobLoadTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
