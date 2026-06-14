#!/usr/bin/env python3
"""Tests for queue supersedence policy facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_supersedence_policy_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueSupersedencePolicyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_supersedence_policy_exports_match_facade_helpers(self) -> None:
        expected = (
            "supersedence_result",
            "cancellation_result",
            "supersedence_key",
            "supersedence_identity_key",
            "jobs_share_supersedence_scope",
            "job_has_narrower_same_identity_scope",
            "supersedence_reason",
        )

        self.assertEqual(self.mod.QUEUE_SUPERSEDENCE_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_queue_supersedence_policy_bindings_delegate_to_orchestrator(self) -> None:
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
            supersedence_key=lambda job: ("branch", ("mac",), "full"),
            supersedence_identity_key=lambda job: ("branch", "sha", "full"),
            jobs_share_supersedence_scope=lambda newer, older: newer["branch"] == older["branch"],
            job_has_narrower_same_identity_scope=lambda newer, older: newer["targets"] != older["targets"],
            supersedence_reason=lambda newer, older: "newer_sha",
        )
        bindings = {
            "_queue_orchestrator": orchestrator,
            "now_iso": object(),
        }

        self.assertEqual(self.mod.supersedence_result(bindings, {"id": "old"}, "new", "newer_sha"), {"overall": "superseded"})
        self.assertIs(captured["supersedence_result"][3], bindings["now_iso"])
        self.assertEqual(self.mod.cancellation_result(bindings, {"id": "old"}, "operator"), {"overall": "canceled"})
        self.assertIs(captured["cancellation_result"][2], bindings["now_iso"])
        self.assertEqual(self.mod.supersedence_key(bindings, {"id": "job"}), ("branch", ("mac",), "full"))
        self.assertEqual(self.mod.supersedence_identity_key(bindings, {"id": "job"}), ("branch", "sha", "full"))
        self.assertTrue(self.mod.jobs_share_supersedence_scope(bindings, {"branch": "b"}, {"branch": "b"}))
        self.assertTrue(self.mod.job_has_narrower_same_identity_scope(bindings, {"targets": ["mac"]}, {"targets": ["mac", "linux"]}))
        self.assertEqual(self.mod.supersedence_reason(bindings, {"id": "new"}, {"id": "old"}), "newer_sha")


if __name__ == "__main__":
    unittest.main()
