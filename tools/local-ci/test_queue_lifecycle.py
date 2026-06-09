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
            self.assertEqual(job_id, "job1")
            self.assertEqual(result, {"overall": "pass"})
            self.assertEqual(result_path, Path("result.json"))
            loaded_queue[0]["status"] = "completed"
            loaded_queue[0]["overall"] = "pass"

        def trim(loaded_queue):
            events.append(("trim", lock_active))
            return [loaded_queue[0]], {"old"}

        def save(retained_queue):
            events.append(("save", lock_active))
            self.assertEqual(retained_queue, [{"id": "job1", "status": "completed", "overall": "pass"}])

        def collect(retained_queue, *, keep_results, keep_logs, keep_bundles, include_prepared):
            events.append(("collect", lock_active))
            self.assertEqual(retained_queue, [{"id": "job1", "status": "completed", "overall": "pass"}])
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

    def test_wait_for_job_returns_error_when_job_missing(self) -> None:
        messages: list[str] = []

        result = self.mod.wait_for_job_completion(
            "missing",
            {},
            load_job_fn=lambda job_id: None,
            load_result_fn=lambda path: self.fail(f"unexpected load_result({path})"),
            drain_pending_jobs_fn=lambda config, *, blocking: self.fail("unexpected drain"),
            current_runner_info_fn=lambda: self.fail("unexpected runner lookup"),
            sleep_fn=lambda seconds: self.fail(f"unexpected sleep({seconds})"),
            poll_secs=0.25,
            print_fn=messages.append,
        )

        self.assertEqual(result, (None, 1))
        self.assertEqual(messages, ["Job not found: missing"])

    def test_wait_for_job_loads_completed_result_and_exit_status(self) -> None:
        loaded_paths: list[Path] = []

        result = self.mod.wait_for_job_completion(
            "job1",
            {"targets": ["macos"]},
            load_job_fn=lambda job_id: {
                "id": job_id,
                "status": "completed",
                "result_file": "result.json",
            },
            load_result_fn=lambda path: (loaded_paths.append(path) or {"overall": "pass"}),
            drain_pending_jobs_fn=lambda config, *, blocking: self.fail("unexpected drain"),
            current_runner_info_fn=lambda: self.fail("unexpected runner lookup"),
            sleep_fn=lambda seconds: self.fail(f"unexpected sleep({seconds})"),
            poll_secs=0.25,
            print_fn=lambda message: self.fail(f"unexpected print({message})"),
        )

        self.assertEqual(result, ({"overall": "pass"}, 0))
        self.assertEqual(loaded_paths, [Path("result.json")])

    def test_wait_for_job_reports_completed_job_without_result_file(self) -> None:
        messages: list[str] = []

        result = self.mod.wait_for_job_completion(
            "job1",
            {},
            load_job_fn=lambda job_id: {"id": job_id, "status": "completed"},
            load_result_fn=lambda path: self.fail(f"unexpected load_result({path})"),
            drain_pending_jobs_fn=lambda config, *, blocking: self.fail("unexpected drain"),
            current_runner_info_fn=lambda: self.fail("unexpected runner lookup"),
            sleep_fn=lambda seconds: self.fail(f"unexpected sleep({seconds})"),
            poll_secs=0.25,
            print_fn=messages.append,
        )

        self.assertEqual(result, (None, 1))
        self.assertEqual(messages, ["Job completed without a result file: job1"])

    def test_wait_for_job_continues_immediately_when_drain_acquired(self) -> None:
        jobs = [
            {"id": "job1", "status": "pending"},
            {"id": "job1", "status": "completed", "result_file": "result.json"},
        ]
        drain_calls: list[tuple[dict, bool]] = []

        result = self.mod.wait_for_job_completion(
            "job1",
            {"config": True},
            load_job_fn=lambda job_id: jobs.pop(0),
            load_result_fn=lambda path: {"overall": "fail"},
            drain_pending_jobs_fn=lambda config, *, blocking: drain_calls.append((config, blocking))
            or (True, False),
            current_runner_info_fn=lambda: self.fail("unexpected runner lookup"),
            sleep_fn=lambda seconds: self.fail(f"unexpected sleep({seconds})"),
            poll_secs=0.25,
            print_fn=lambda message: self.fail(f"unexpected print({message})"),
        )

        self.assertEqual(result, ({"overall": "fail"}, 1))
        self.assertEqual(drain_calls, [({"config": True}, False)])

    def test_wait_for_job_waits_with_active_runner_message_once(self) -> None:
        jobs = [
            {"id": "job1", "status": "pending"},
            {"id": "job1", "status": "pending"},
            {"id": "job1", "status": "completed", "result_file": "result.json"},
        ]
        messages: list[str] = []
        sleeps: list[float] = []

        result = self.mod.wait_for_job_completion(
            "job1",
            {},
            load_job_fn=lambda job_id: jobs.pop(0),
            load_result_fn=lambda path: {"overall": "pass"},
            drain_pending_jobs_fn=lambda config, *, blocking: (False, False),
            current_runner_info_fn=lambda: {
                "active_job_id": "runner-job",
                "active_branch": "feature/queue",
            },
            sleep_fn=sleeps.append,
            poll_secs=0.25,
            print_fn=messages.append,
        )

        self.assertEqual(result, ({"overall": "pass"}, 0))
        self.assertEqual(sleeps, [0.25, 0.25])
        self.assertEqual(
            messages,
            ["Another local CI runner is active [runner-job] feature/queue; waiting for job1..."],
        )


if __name__ == "__main__":
    unittest.main()
