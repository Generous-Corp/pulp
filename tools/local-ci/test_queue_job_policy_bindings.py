#!/usr/bin/env python3
"""Tests for queue job policy compatibility bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_job_policy_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueJobPolicyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_job_policy_exports_match_facade_helpers(self) -> None:
        expected = (
            *self.mod.QUEUE_JOB_IDENTITY_POLICY_EXPORTS[:2],
            *self.mod.QUEUE_JOB_FACTORY_EXPORTS,
            self.mod.QUEUE_JOB_IDENTITY_POLICY_EXPORTS[2],
        )

        self.assertEqual(self.mod.QUEUE_JOB_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_job_policy_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_job_identity_policy_helpers") as install_identity,
            mock.patch.object(self.mod, "install_queue_job_factory_helpers") as install_factory,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_job_policy_helpers(
                bindings,
                ("default_priority_for", "make_job", "validate_ci_branch_name", "custom_job_policy"),
            )

        install_identity.assert_called_once_with(bindings, ("default_priority_for", "validate_ci_branch_name"))
        install_factory.assert_called_once_with(bindings, ("make_job",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_job_policy",))


if __name__ == "__main__":
    unittest.main()
