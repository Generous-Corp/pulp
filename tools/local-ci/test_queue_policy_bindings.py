#!/usr/bin/env python3
"""Tests for queue policy facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_policy_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueuePolicyBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_policy_exports_match_focused_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_JOB_POLICY_EXPORTS[:3],
            *self.mod.QUEUE_SUPERSEDENCE_POLICY_EXPORTS,
            *self.mod.QUEUE_RETENTION_POLICY_EXPORTS,
            self.mod.QUEUE_JOB_POLICY_EXPORTS[3],
        )

        self.assertEqual(self.mod.QUEUE_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_queue_policy_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_job_policy_helpers") as job,
            mock.patch.object(self.mod, "install_queue_supersedence_policy_helpers") as supersedence,
            mock.patch.object(self.mod, "install_queue_retention_policy_helpers") as retention,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_policy_helpers(
                bindings,
                ("make_job", "supersedence_result", "find_job_unlocked", "unknown_helper"),
            )

        job.assert_called_once_with(bindings, ("make_job",))
        supersedence.assert_called_once_with(bindings, ("supersedence_result",))
        retention.assert_called_once_with(bindings, ("find_job_unlocked",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
