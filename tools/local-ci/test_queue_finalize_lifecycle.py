#!/usr/bin/env python3
"""Tests for locked queue finalization lifecycle helpers."""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_finalize_lifecycle.py", module_name="pulp_queue_finalize_lifecycle")


class QueueFinalizeLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_finalize_job_completes_trims_saves_then_cleans_after_lock(self) -> None:
        queue = [{"id": "job1", "status": "running"}, {"id": "old", "status": "completed"}]
        events: list[tuple[str, bool]] = []
        lock_active = False

        @contextmanager
        def tracked_lock(_path, *, blocking):
            nonlocal lock_active
            assert blocking is True
            lock_active = True
            try:
                yield
            finally:
                lock_active = False

        def complete(loaded_queue, job_id, result, result_path):
            events.append(("complete", lock_active))
            loaded_queue[0]["status"] = "completed"
            loaded_queue[0]["overall"] = result["overall"]

        def trim(loaded_queue):
            events.append(("trim", lock_active))
            return [loaded_queue[0]], {"old"}

        def save(retained_queue):
            events.append(("save", lock_active))
            self.assertEqual(retained_queue, [{"id": "job1", "status": "completed", "overall": "pass"}])

        def collect(retained_queue, *, keep_results, keep_logs, keep_bundles, include_prepared):
            events.append(("collect", lock_active))
            self.assertEqual((keep_results, keep_logs, keep_bundles, include_prepared), (5, 6, 0, False))
            return {"cleanup": True}

        def apply(plan):
            events.append(("apply", lock_active))
            self.assertEqual(plan, {"cleanup": True})
            return {"deleted": []}

        self.mod.finalize_job_locked(
            "job1",
            {"overall": "pass"},
            Path("result.json"),
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=tracked_lock,
            load_queue_unlocked_fn=lambda: queue,
            complete_job_unlocked_fn=complete,
            trim_completed_jobs_with_removed_ids_fn=trim,
            save_queue_unlocked_fn=save,
            collect_local_ci_cleanup_plan_fn=collect,
            apply_local_ci_cleanup_plan_fn=apply,
            keep_results=5,
            keep_logs=6,
        )

        self.assertEqual(
            events,
            [
                ("complete", True),
                ("trim", True),
                ("save", True),
                ("collect", False),
                ("apply", False),
            ],
        )


if __name__ == "__main__":
    unittest.main()
