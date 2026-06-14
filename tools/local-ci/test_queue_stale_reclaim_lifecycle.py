#!/usr/bin/env python3
"""Tests for locked stale remote-validator reclaim orchestration."""

from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_stale_reclaim_lifecycle.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_queue_stale_reclaim_lifecycle", add_module_dir=True)


@contextmanager
def fake_lock(_path, *, blocking):
    assert blocking is True
    yield


class QueueStaleReclaimLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_reclaim_stale_remote_validators_saves_candidates_then_reclaims_after_lock(self) -> None:
        queue = [{"id": "job1", "status": "running"}]
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

        def collect(loaded_queue):
            events.append(("collect", lock_active))
            loaded_queue[0]["target_state"] = {"windows": {"stale_cleanup": "pending"}}
            return [{"job_id": "job1", "target": "windows"}]

        def save(saved_queue):
            events.append(("save", lock_active))
            self.assertEqual(
                saved_queue,
                [{"id": "job1", "status": "running", "target_state": {"windows": {"stale_cleanup": "pending"}}}],
            )

        def reclaim(candidates, *, cleanup_validator_fn, update_job_target_state_fn, now_fn, trim_line_fn):
            events.append(("reclaim", lock_active))
            self.assertEqual(candidates, [{"job_id": "job1", "target": "windows"}])
            self.assertEqual(cleanup_validator_fn("host", 123, "started"), {"status": "cleaned"})
            update_job_target_state_fn("job1", "windows", stale_cleanup="complete")
            self.assertEqual(now_fn(), "now")
            self.assertEqual(trim_line_fn(" done "), "done")
            return 1

        updates: list[tuple[str, str, dict]] = []
        result = self.mod.reclaim_stale_remote_validators_locked(
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=tracked_lock,
            load_queue_unlocked_fn=lambda: queue,
            collect_stale_windows_cleanup_candidates_unlocked_fn=collect,
            save_queue_unlocked_fn=save,
            reclaim_stale_remote_validator_candidates_fn=reclaim,
            cleanup_validator_fn=lambda host, pid, started_at: {"status": "cleaned"},
            update_job_target_state_fn=lambda job_id, target, **fields: updates.append((job_id, target, fields)),
            now_fn=lambda: "now",
            trim_line_fn=lambda line: line.strip(),
        )

        self.assertEqual(result, 1)
        self.assertEqual(events, [("collect", True), ("save", True), ("reclaim", False)])
        self.assertEqual(updates, [("job1", "windows", {"stale_cleanup": "complete"})])

    def test_reclaim_stale_remote_validators_skips_save_without_candidates(self) -> None:
        events: list[str] = []

        result = self.mod.reclaim_stale_remote_validators_locked(
            queue_lock_path_fn=lambda: Path("queue.lock"),
            file_lock_fn=fake_lock,
            load_queue_unlocked_fn=lambda: [{"id": "job1"}],
            collect_stale_windows_cleanup_candidates_unlocked_fn=lambda queue: events.append("collect") or [],
            save_queue_unlocked_fn=lambda queue: self.fail("unexpected save"),
            reclaim_stale_remote_validator_candidates_fn=lambda candidates, **kwargs: (
                events.append(f"reclaim:{len(candidates)}") or 0
            ),
            cleanup_validator_fn=lambda host, pid, started_at: self.fail("unexpected cleanup"),
            update_job_target_state_fn=lambda *args, **kwargs: self.fail("unexpected target update"),
            now_fn=lambda: "now",
            trim_line_fn=lambda line: line.strip(),
        )

        self.assertEqual(result, 0)
        self.assertEqual(events, ["collect", "reclaim:0"])


if __name__ == "__main__":
    unittest.main()
