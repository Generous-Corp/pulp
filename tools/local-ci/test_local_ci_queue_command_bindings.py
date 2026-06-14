#!/usr/bin/env python3
"""Tests for queue-oriented local-CI command compatibility bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_queue_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiQueueCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_command_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.LOCAL_CI_ENQUEUE_COMMAND_EXPORTS,
            *self.mod.LOCAL_CI_DRAIN_COMMAND_EXPORTS,
            *self.mod.LOCAL_CI_RUN_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LOCAL_CI_QUEUE_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))


if __name__ == "__main__":
    unittest.main()
