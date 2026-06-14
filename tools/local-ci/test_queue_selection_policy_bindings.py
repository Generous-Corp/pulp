#!/usr/bin/env python3
"""Tests for queue selection and status grouping bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_selection_policy_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueSelectionPolicyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_selection_policy_exports_match_helpers(self) -> None:
        expected = (
            "job_sort_key",
            "queue_status_groups",
            "recent_completed_jobs_for_status",
            "find_job_unlocked",
        )

        self.assertEqual(self.mod.QUEUE_SELECTION_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_selection_policy_bindings_delegate_to_orchestrator(self) -> None:
        orchestrator = types.SimpleNamespace(
            job_sort_key=lambda job: (0, job["created_at"], job["id"]),
            queue_status_groups=lambda queue: (queue[:1], queue[1:2], queue[2:]),
            recent_completed_jobs_for_status=lambda jobs, *, limit: jobs[:limit],
            find_job_unlocked=lambda queue, job_ref, statuses=None: queue[0],
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.job_sort_key(bindings, {"id": "job", "created_at": "now"}), (0, "now", "job"))
        self.assertEqual(
            self.mod.queue_status_groups(bindings, [{"id": "a"}, {"id": "b"}, {"id": "c"}]),
            ([{"id": "a"}], [{"id": "b"}], [{"id": "c"}]),
        )
        self.assertEqual(self.mod.recent_completed_jobs_for_status(bindings, [{"id": "a"}, {"id": "b"}], limit=1), [{"id": "a"}])
        self.assertEqual(self.mod.find_job_unlocked(bindings, [{"id": "a"}], "a", {"pending"}), {"id": "a"})


if __name__ == "__main__":
    unittest.main()
