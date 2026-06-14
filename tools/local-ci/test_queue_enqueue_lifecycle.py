#!/usr/bin/env python3
"""Tests for locked queue enqueue lifecycle helpers."""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_enqueue_lifecycle.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_queue_enqueue_lifecycle")


@contextmanager
def fake_lock(_path, *, blocking):
    assert blocking is True
    yield


class QueueEnqueueLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_enqueue_reconciles_saves_bumps_existing_and_normalizes(self) -> None:
        queue = [{"id": "job1", "fingerprint": "fp-feature/a", "priority": "low", "status": "running"}]
        saved: list[list[dict]] = []

        def reconcile(loaded_queue):
            loaded_queue[0]["status"] = "pending"
            return loaded_queue, True

        result = self.mod.enqueue_job_locked(
            "feature/a",
            "1" * 40,
            "HIGH",
            ["mac"],
            "enqueue",
            "SMOKE",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            reconcile_running_jobs_unlocked_fn=reconcile,
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(job) for job in saved_queue]),
            normalize_priority_fn=lambda priority: priority.lower(),
            normalize_validation_mode_fn=lambda validation: validation.lower(),
            make_fingerprint_fn=lambda branch, sha, targets, validation: f"fp-{branch}",
            find_active_job_by_fingerprint_unlocked_fn=lambda loaded_queue, fingerprint: loaded_queue[0],
            bump_pending_job_priority_unlocked_fn=lambda existing, requested_priority: existing.update(
                priority=requested_priority
            )
            is None,
            make_job_fn=lambda *args, **kwargs: self.fail("unexpected make_job"),
            pending_supersedence_candidates_unlocked_fn=lambda loaded_queue, job: self.fail(
                "unexpected supersedence lookup"
            ),
            supersede_job_unlocked_fn=lambda existing, job_id, reason: self.fail("unexpected supersede"),
            trim_completed_jobs_fn=lambda loaded_queue: self.fail("unexpected trim"),
            normalize_job_fn=lambda job: {**job, "normalized": True},
        )

        self.assertEqual(result[0]["priority"], "high")
        self.assertEqual(result[0]["normalized"], True)
        self.assertEqual(len(saved), 2)

    def test_enqueue_appends_supersedes_trims_and_saves_new_job(self) -> None:
        old_job = {"id": "old", "branch": "feature/a", "status": "pending", "priority": "normal"}
        new_job = {"id": "new", "branch": "feature/a", "status": "pending", "priority": "high"}
        queue = [old_job]
        superseded: list[tuple[str, str, str]] = []
        saved: list[list[dict]] = []

        result = self.mod.enqueue_job_locked(
            "feature/a",
            "2" * 40,
            "HIGH",
            ["mac"],
            "ship",
            "FULL",
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: queue,
            reconcile_running_jobs_unlocked_fn=lambda loaded_queue: (loaded_queue, False),
            save_queue_unlocked_fn=lambda saved_queue: saved.append([dict(job) for job in saved_queue]),
            normalize_priority_fn=lambda priority: priority.lower(),
            normalize_validation_mode_fn=lambda validation: validation.lower(),
            make_fingerprint_fn=lambda branch, sha, targets, validation: "new-fp",
            find_active_job_by_fingerprint_unlocked_fn=lambda loaded_queue, fingerprint: None,
            bump_pending_job_priority_unlocked_fn=lambda existing, requested_priority: self.fail("unexpected bump"),
            make_job_fn=lambda branch, sha, priority, targets, mode, validation, *, submission: new_job,
            pending_supersedence_candidates_unlocked_fn=lambda loaded_queue, job: [(old_job, "newer_sha")],
            supersede_job_unlocked_fn=lambda existing, job_id, reason: (
                superseded.append((existing["id"], job_id, reason)),
                existing.update(status="completed"),
            ),
            trim_completed_jobs_fn=lambda loaded_queue: [job for job in loaded_queue if job.get("id") == "new"],
            normalize_job_fn=lambda job: self.fail("unexpected normalize"),
        )

        self.assertEqual(result, (new_job, True))
        self.assertEqual(superseded, [("old", "new", "newer_sha")])
        self.assertEqual(saved, [[new_job]])


if __name__ == "__main__":
    unittest.main()
