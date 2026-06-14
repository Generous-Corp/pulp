#!/usr/bin/env python3
"""Tests for queue supersedence result bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("queue_supersedence_result_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueSupersedenceResultBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_result_exports_match_helpers(self) -> None:
        expected = (
            "supersedence_result",
            "cancellation_result",
        )

        self.assertEqual(self.mod.QUEUE_SUPERSEDENCE_RESULT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_result_bindings_delegate_with_assembled_dependencies(self) -> None:
        captured = {}

        def supersedence_result(job, superseded_by, reason, *, now_iso_fn):
            captured["supersedence_result"] = (job, superseded_by, reason, now_iso_fn)
            return {"overall": "superseded"}

        def cancellation_result(job, reason, *, now_iso_fn):
            captured["cancellation_result"] = (job, reason, now_iso_fn)
            return {"overall": "canceled"}

        orchestrator = types.SimpleNamespace(
            supersedence_result=supersedence_result,
            cancellation_result=cancellation_result,
        )
        bindings = {
            "_queue_orchestrator": orchestrator,
            "now_iso": object(),
        }
        deps = {"now_iso_fn": object()}

        with mock.patch.object(self.mod, "queue_supersedence_result_dependencies", return_value=deps):
            self.assertEqual(
                self.mod.supersedence_result(bindings, {"id": "old"}, "new", "newer_sha"),
                {"overall": "superseded"},
            )
        self.assertIs(captured["supersedence_result"][3], deps["now_iso_fn"])
        with mock.patch.object(self.mod, "queue_supersedence_result_dependencies", return_value=deps):
            self.assertEqual(self.mod.cancellation_result(bindings, {"id": "old"}, "operator"), {"overall": "canceled"})
        self.assertIs(captured["cancellation_result"][2], deps["now_iso_fn"])


if __name__ == "__main__":
    unittest.main()
