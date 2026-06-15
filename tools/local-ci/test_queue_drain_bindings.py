#!/usr/bin/env python3
"""Tests for queue drain and runner lifecycle facade bindings."""

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_drain_bindings.py")


class QueueDrainBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_drain_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.QUEUE_CLAIM_EXPORTS,
            *self.mod.QUEUE_FINALIZE_EXPORTS,
            *self.mod.QUEUE_WAIT_DRAIN_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_DRAIN_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_drain_helpers_routes_focused_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_claim_helpers") as install_claim,
            mock.patch.object(self.mod, "install_queue_finalize_helpers") as install_finalize,
            mock.patch.object(self.mod, "install_queue_wait_drain_helpers") as install_wait,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_drain_helpers(bindings, ("claim_next_job", "finalize_job", "wait_for_job", "custom"))

        install_claim.assert_called_once_with(bindings, ("claim_next_job",))
        install_finalize.assert_called_once_with(bindings, ("finalize_job",))
        install_wait.assert_called_once_with(bindings, ("wait_for_job",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
