#!/usr/bin/env python3
"""Tests for queue job identity and priority policy bindings."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_job_identity_policy_bindings.py")


class QueueJobIdentityPolicyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_identity_policy_exports_match_helpers(self) -> None:
        expected = (
            "default_priority_for",
            "make_fingerprint",
            "validate_ci_branch_name",
        )

        self.assertEqual(self.mod.QUEUE_JOB_IDENTITY_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_identity_policy_bindings_delegate_to_orchestrator(self) -> None:
        orchestrator = types.SimpleNamespace(
            default_priority_for=lambda command, config: f"{command}:{config['priority']}",
            make_fingerprint=lambda branch, sha, targets, validation: "|".join([branch, sha, ",".join(targets), validation]),
            validate_ci_branch_name=lambda branch: branch.strip(),
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.default_priority_for(bindings, "ship", {"priority": "high"}), "ship:high")
        self.assertEqual(self.mod.make_fingerprint(bindings, "b", "s", ["mac"], "full"), "b|s|mac|full")
        self.assertEqual(self.mod.validate_ci_branch_name(bindings, " branch "), "branch")


if __name__ == "__main__":
    unittest.main()
