#!/usr/bin/env python3
"""Tests for queue cancel utility command dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("utility_queue_cancel_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class UtilityQueueCancelCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_utility_queue_cancel_command_dependencies_bind_facade_dependencies(self) -> None:
        bindings = {
            "cancel_queue_command_job": object(),
            "cancel_queue_command_result_line": object(),
        }

        deps = self.mod.utility_queue_cancel_command_dependencies(bindings)

        self.assertIs(deps["cancel_queue_command_job_fn"], bindings["cancel_queue_command_job"])
        self.assertIs(deps["cancel_queue_command_result_line_fn"], bindings["cancel_queue_command_result_line"])


if __name__ == "__main__":
    unittest.main()
