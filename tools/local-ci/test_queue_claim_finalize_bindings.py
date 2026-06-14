#!/usr/bin/env python3
"""Tests for queue claim/finalize facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_claim_finalize_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueClaimFinalizeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_claim_finalize_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_CLAIM_EXPORTS,
            *self.mod.QUEUE_FINALIZE_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_CLAIM_FINALIZE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_claim_finalize_facade_reexports_focused_bindings(self):
        self.assertEqual(self.mod.claim_next_job.__module__, "queue_claim_bindings")
        self.assertEqual(self.mod.finalize_job.__module__, "queue_finalize_bindings")

    def test_install_claim_finalize_helpers_routes_selected_groups_and_unknown_exports(self):
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_claim_helpers") as claim,
            mock.patch.object(self.mod, "install_queue_finalize_helpers") as finalize,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_claim_finalize_helpers(bindings, ("claim_next_job", "custom"))

        claim.assert_called_once_with(bindings, ("claim_next_job",))
        finalize.assert_called_once_with(bindings, ())
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom",))


if __name__ == "__main__":
    unittest.main()
