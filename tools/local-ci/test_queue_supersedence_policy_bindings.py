#!/usr/bin/env python3
"""Tests for queue supersedence policy compatibility bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("queue_supersedence_policy_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueSupersedencePolicyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_queue_supersedence_policy_exports_match_facade_helpers(self) -> None:
        expected = (
            *self.mod.QUEUE_SUPERSEDENCE_RESULT_EXPORTS,
            *self.mod.QUEUE_SUPERSEDENCE_SCOPE_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_SUPERSEDENCE_POLICY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_queue_supersedence_policy_helpers_routes_each_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_queue_supersedence_result_helpers") as install_result,
            mock.patch.object(self.mod, "install_queue_supersedence_scope_helpers") as install_scope,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_queue_supersedence_policy_helpers(
                bindings,
                ("supersedence_result", "supersedence_key", "custom_supersedence_policy"),
            )

        install_result.assert_called_once_with(bindings, ("supersedence_result",))
        install_scope.assert_called_once_with(bindings, ("supersedence_key",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("custom_supersedence_policy",))


if __name__ == "__main__":
    unittest.main()
