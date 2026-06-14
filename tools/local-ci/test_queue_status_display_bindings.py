#!/usr/bin/env python3
"""Tests for queue status display facade bindings."""

from module_test_utils import load_module_from_path
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_status_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStatusDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_status_display_exports_match_facade_helpers(self):
        expected = (
            *self.mod.QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS[:2],
            *self.mod.QUEUE_STATUS_TARGET_DISPLAY_EXPORTS,
            self.mod.QUEUE_STATUS_ACTIVE_DISPLAY_EXPORTS[2],
            *self.mod.QUEUE_STATUS_RECENT_DISPLAY_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_STATUS_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))


if __name__ == "__main__":
    unittest.main()
