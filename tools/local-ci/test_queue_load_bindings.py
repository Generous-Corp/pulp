#!/usr/bin/env python3
"""Tests for locked queue load dependency bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_load_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueLoadBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_load_exports_match_wrappers(self):
        expected = ("load_queue",)

        self.assertEqual(self.mod.QUEUE_LOAD_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_load_queue_locks_reconciles_and_saves_only_when_changed(self):
        events = []
        queue_path = Path("/state/queue.lock")
        original_queue = [{"id": "job1", "status": "running"}]
        reconciled_queue = [{"id": "job1", "status": "pending"}]

        class Lock:
            def __enter__(self):
                events.append("enter")

            def __exit__(self, exc_type, exc, tb):
                events.append("exit")

        def file_lock(path, *, blocking):
            events.append(("lock", path, blocking))
            return Lock()

        def load_queue_unlocked():
            events.append("load")
            return original_queue

        def reconcile(queue):
            events.append(("reconcile", queue))
            return reconciled_queue, True

        def save(queue):
            events.append(("save", queue))

        bindings = {
            "queue_lock_path": lambda: queue_path,
            "file_lock": file_lock,
            "load_queue_unlocked": load_queue_unlocked,
            "reconcile_running_jobs_unlocked": reconcile,
            "save_queue_unlocked": save,
        }

        self.assertEqual(self.mod.load_queue(bindings), reconciled_queue)
        self.assertEqual(
            events,
            [
                ("lock", queue_path, True),
                "enter",
                "load",
                ("reconcile", original_queue),
                ("save", reconciled_queue),
                "exit",
            ],
        )

        events.clear()
        bindings["reconcile_running_jobs_unlocked"] = lambda queue: (queue, False)

        self.assertEqual(self.mod.load_queue(bindings), original_queue)
        self.assertNotIn(("save", original_queue), events)


if __name__ == "__main__":
    unittest.main()
