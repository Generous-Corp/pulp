#!/usr/bin/env python3
"""Tests for queue completed-job retention bindings."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_retention_trim_bindings.py")


class QueueRetentionTrimBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_retention_trim_exports_match_helpers(self) -> None:
        expected = (
            "trim_completed_jobs_with_removed_ids",
            "trim_completed_jobs",
        )

        self.assertEqual(self.mod.QUEUE_RETENTION_TRIM_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_retention_trim_bindings_delegate_with_keep_count(self) -> None:
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
        )
        bindings = {
            "_queue_orchestrator": orchestrator,
            "KEEP_COMPLETED_JOBS": 7,
        }

        self.assertEqual(
            self.mod.trim_completed_jobs_with_removed_ids(bindings, [{"id": "old"}]),
            ([{"id": "old"}], {"old"}),
        )
        self.assertEqual(captured["trim_removed"][1], 7)
        self.assertEqual(self.mod.trim_completed_jobs(bindings, [{"id": "old"}]), [{"id": "old"}])
        self.assertEqual(captured["trim"][1], 7)


if __name__ == "__main__":
    unittest.main()
