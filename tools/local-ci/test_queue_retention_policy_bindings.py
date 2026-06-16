#!/usr/bin/env python3
"""Tests for queue retention and selection policy compatibility bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_retention_policy_bindings.py")


class QueueRetentionPolicyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_retention_policy_exports_match_facade_helpers(self) -> None:
        expected = (
            *self.mod.QUEUE_RETENTION_TRIM_EXPORTS,
            *self.mod.QUEUE_SELECTION_POLICY_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_RETENTION_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_retention_policy_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_retention_trim_helpers") as install_trim,
            mock.patch.object(self.mod, "install_queue_selection_policy_helpers") as install_selection,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_retention_policy_helpers(
                bindings,
                ("trim_completed_jobs", "find_job_unlocked", "custom_retention_policy"),
            )

        install_trim.assert_called_once_with(bindings, ("trim_completed_jobs",))
        install_selection.assert_called_once_with(bindings, ("find_job_unlocked",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_retention_policy",))


if __name__ == "__main__":
    unittest.main()
