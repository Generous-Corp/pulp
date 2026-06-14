#!/usr/bin/env python3
"""Tests for queue terminal/completion helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_completion.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_queue_completion", add_module_dir=True)


class QueueCompletionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_complete_superseded_job_saves_result_and_marks_completed(self) -> None:
        job = {"id": "old", "status": "running"}
        events: list[tuple[str, object]] = []

        def make_result(current_job, superseded_by, reason):
            events.append(("result", (current_job["id"], superseded_by, reason)))
            return {"job_id": current_job["id"], "overall": "superseded", "completed_at": "done"}

        def save(result):
            events.append(("save", dict(result)))
            return Path("old-result.json")

        def complete(current_job, result, result_path):
            events.append(("complete", (current_job["id"], result["overall"], result_path)))
            current_job["status"] = "completed"
            current_job["result_file"] = str(result_path)

        result_path = self.mod.complete_superseded_job_unlocked(
            job,
            "new",
            "newer_sha_queued",
            supersedence_result_fn=make_result,
            save_result_fn=save,
            complete_job_with_result_unlocked_fn=complete,
        )

        self.assertEqual(result_path, Path("old-result.json"))
        self.assertEqual(job, {"id": "old", "status": "completed", "result_file": "old-result.json"})
        self.assertEqual(
            events,
            [
                ("result", ("old", "new", "newer_sha_queued")),
                ("save", {"job_id": "old", "overall": "superseded", "completed_at": "done"}),
                ("complete", ("old", "superseded", Path("old-result.json"))),
            ],
        )

    def test_complete_canceled_job_saves_result_and_marks_completed(self) -> None:
        job = {"id": "job1", "status": "pending"}
        events: list[tuple[str, object]] = []

        def make_result(current_job, reason):
            events.append(("result", (current_job["id"], reason)))
            return {"job_id": current_job["id"], "overall": "canceled", "completed_at": "done"}

        def save(result):
            events.append(("save", dict(result)))
            return "job1-result.json"

        def complete(current_job, result, result_path):
            events.append(("complete", (current_job["id"], result["overall"], result_path)))
            current_job["status"] = "completed"
            current_job["result_file"] = result_path

        result_path = self.mod.complete_canceled_job_unlocked(
            job,
            "operator_canceled",
            cancellation_result_fn=make_result,
            save_result_fn=save,
            complete_job_with_result_unlocked_fn=complete,
        )

        self.assertEqual(result_path, "job1-result.json")
        self.assertEqual(job, {"id": "job1", "status": "completed", "result_file": "job1-result.json"})
        self.assertEqual(
            events,
            [
                ("result", ("job1", "operator_canceled")),
                ("save", {"job_id": "job1", "overall": "canceled", "completed_at": "done"}),
                ("complete", ("job1", "canceled", "job1-result.json")),
            ],
        )


if __name__ == "__main__":
    unittest.main()
