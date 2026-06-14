#!/usr/bin/env python3
"""Tests for locked queue state update helpers."""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_state_updates.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_queue_state_updates", add_module_dir=True)


@contextmanager
def fake_lock(_path, *, blocking):
    assert blocking is True
    yield


class QueueStateUpdatesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_update_job_active_targets_saves_only_when_changed(self) -> None:
        queue = [{"id": "job1", "active_targets": {}}]
        saved: list[list[dict]] = []

        def upsert(loaded_queue, job_id, active_targets):
            self.assertEqual((job_id, active_targets), ("job1", {"mac": {"status": "running"}}))
            loaded_queue[0]["active_targets"] = active_targets
            return True

        self.mod.update_job_active_targets_locked(
            "job1",
            {"mac": {"status": "running"}},
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            upsert_job_active_targets_unlocked_fn=upsert,
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(job) for job in saved_queue]),
        )

        self.assertEqual(
            saved,
            [[{"id": "job1", "active_targets": {"mac": {"status": "running"}}}]],
        )

        self.mod.update_job_active_targets_locked(
            "job1",
            {"mac": {"status": "running"}},
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            upsert_job_active_targets_unlocked_fn=lambda loaded_queue, job_id, active_targets: False,
            save_queue_unlocked_fn=lambda saved_queue: saved.append(saved_queue),
        )

        self.assertEqual(len(saved), 1)

    def test_update_job_target_state_saves_only_when_changed(self) -> None:
        queue = [{"id": "job1", "target_state": {}}]
        saved: list[list[dict]] = []

        def update(loaded_queue, job_id, target_name, fields):
            self.assertEqual((job_id, target_name), ("job1", "windows"))
            self.assertEqual(fields, {"status": "pass", "detail": "ok"})
            loaded_queue[0]["target_state"] = {target_name: dict(fields)}
            return True

        self.mod.update_job_target_state_locked(
            "job1",
            "windows",
            {"status": "pass", "detail": "ok"},
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            update_job_target_state_unlocked_fn=update,
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(job) for job in saved_queue]),
        )

        self.assertEqual(
            saved,
            [[{"id": "job1", "target_state": {"windows": {"status": "pass", "detail": "ok"}}}]],
        )

        self.mod.update_job_target_state_locked(
            "job1",
            "windows",
            {"status": "pass"},
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            update_job_target_state_unlocked_fn=lambda loaded_queue, job_id, target_name, fields: False,
            save_queue_unlocked_fn=lambda saved_queue: saved.append(saved_queue),
        )

        self.assertEqual(len(saved), 1)


if __name__ == "__main__":
    unittest.main()
