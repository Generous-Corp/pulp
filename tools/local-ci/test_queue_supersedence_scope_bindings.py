#!/usr/bin/env python3
"""Tests for queue supersedence key and scope bindings."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_supersedence_scope_bindings.py")


class QueueSupersedenceScopeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_scope_exports_match_helpers(self) -> None:
        expected = (
            "supersedence_key",
            "supersedence_identity_key",
            "jobs_share_supersedence_scope",
            "job_has_narrower_same_identity_scope",
            "supersedence_reason",
        )

        self.assertEqual(self.mod.QUEUE_SUPERSEDENCE_SCOPE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_scope_bindings_delegate_to_orchestrator(self) -> None:
        orchestrator = types.SimpleNamespace(
            supersedence_key=lambda job: ("branch", ("mac",), "full"),
            supersedence_identity_key=lambda job: ("branch", "sha", "full"),
            jobs_share_supersedence_scope=lambda newer, older: newer["branch"] == older["branch"],
            job_has_narrower_same_identity_scope=lambda newer, older: newer["targets"] != older["targets"],
            supersedence_reason=lambda newer, older: "newer_sha",
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.supersedence_key(bindings, {"id": "job"}), ("branch", ("mac",), "full"))
        self.assertEqual(self.mod.supersedence_identity_key(bindings, {"id": "job"}), ("branch", "sha", "full"))
        self.assertTrue(self.mod.jobs_share_supersedence_scope(bindings, {"branch": "b"}, {"branch": "b"}))
        self.assertTrue(
            self.mod.job_has_narrower_same_identity_scope(
                bindings,
                {"targets": ["mac"]},
                {"targets": ["mac", "linux"]},
            )
        )
        self.assertEqual(self.mod.supersedence_reason(bindings, {"id": "new"}, {"id": "old"}), "newer_sha")


if __name__ == "__main__":
    unittest.main()
