#!/usr/bin/env python3
"""Tests for queue retention and selection policy facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_retention_policy_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueRetentionPolicyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_retention_policy_exports_match_facade_helpers(self) -> None:
        expected = (
            "trim_completed_jobs_with_removed_ids",
            "trim_completed_jobs",
            "job_sort_key",
            "queue_status_groups",
            "recent_completed_jobs_for_status",
            "find_job_unlocked",
        )

        self.assertEqual(self.mod.QUEUE_RETENTION_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_queue_retention_policy_bindings_delegate_to_orchestrator(self) -> None:
        captured = {}

        def trim_removed(queue, *, keep_completed_jobs):
            captured["trim_removed"] = (queue, keep_completed_jobs)
            return queue, {"old"}

        def trim(queue, *, keep_completed_jobs):
            captured["trim"] = (queue, keep_completed_jobs)
            return queue

        orchestrator = types.SimpleNamespace(
            trim_completed_jobs_with_removed_ids=trim_removed,
            trim_completed_jobs=trim,
            job_sort_key=lambda job: (0, job["created_at"], job["id"]),
            queue_status_groups=lambda queue: (queue[:1], queue[1:2], queue[2:]),
            recent_completed_jobs_for_status=lambda jobs, *, limit: jobs[:limit],
            find_job_unlocked=lambda queue, job_ref, statuses=None: queue[0],
        )
        bindings = {
            "_queue_orchestrator": orchestrator,
            "KEEP_COMPLETED_JOBS": 7,
        }

        self.assertEqual(self.mod.trim_completed_jobs_with_removed_ids(bindings, [{"id": "old"}]), ([{"id": "old"}], {"old"}))
        self.assertEqual(captured["trim_removed"][1], 7)
        self.assertEqual(self.mod.trim_completed_jobs(bindings, [{"id": "old"}]), [{"id": "old"}])
        self.assertEqual(captured["trim"][1], 7)
        self.assertEqual(self.mod.job_sort_key(bindings, {"id": "job", "created_at": "now"}), (0, "now", "job"))
        self.assertEqual(self.mod.queue_status_groups(bindings, [{"id": "a"}, {"id": "b"}, {"id": "c"}]), ([{"id": "a"}], [{"id": "b"}], [{"id": "c"}]))
        self.assertEqual(self.mod.recent_completed_jobs_for_status(bindings, [{"id": "a"}, {"id": "b"}], limit=1), [{"id": "a"}])
        self.assertEqual(self.mod.find_job_unlocked(bindings, [{"id": "a"}], "a", {"pending"}), {"id": "a"})

    def test_install_queue_retention_policy_helpers_wires_named_exports(self) -> None:
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_retention_policy_helpers(bindings, ("find_job_unlocked", "custom_retention_policy"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("find_job_unlocked",)),
                mock.call(bindings, self.mod.__dict__, ("custom_retention_policy",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
