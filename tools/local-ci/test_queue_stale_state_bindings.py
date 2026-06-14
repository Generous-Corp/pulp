#!/usr/bin/env python3
"""Tests for queue stale/target-state facade bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("queue_stale_state_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueStaleStateBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_stale_state_exports_compose_focused_groups(self):
        expected = (
            *self.mod.QUEUE_STALE_RECONCILE_EXPORTS,
            *self.mod.QUEUE_TARGET_UPDATE_EXPORTS,
            *self.mod.QUEUE_STALE_RECLAIM_EXPORTS,
        )

        self.assertEqual(self.mod.QUEUE_STALE_STATE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))


if __name__ == "__main__":
    unittest.main()
