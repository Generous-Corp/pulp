#!/usr/bin/env python3
"""Tests for queue runner-info dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("queue_runner_info_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueRunnerInfoBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_runner_info_exports_match_facade_helpers(self):
        expected = (
            "read_runner_info",
            "pid_alive",
            "current_runner_info",
            "write_runner_info",
            "clear_runner_info",
        )

        self.assertEqual(self.mod.QUEUE_RUNNER_INFO_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_runner_info_helpers_delegate_to_runner_state(self):
        calls = []
        runner_state = types.SimpleNamespace(
            read_runner_info=lambda: calls.append("read") or {"pid": 123},
            pid_alive=lambda pid: calls.append(("pid", pid)) or True,
            current_runner_info=lambda: calls.append("current") or {"pid": 456},
            write_runner_info=lambda info: calls.append(("write", info)),
            clear_runner_info=lambda: calls.append("clear"),
        )
        bindings = {"_runner_state": runner_state}

        self.assertEqual(self.mod.read_runner_info(bindings), {"pid": 123})
        self.assertTrue(self.mod.pid_alive(bindings, 789))
        self.assertEqual(self.mod.current_runner_info(bindings), {"pid": 456})
        self.mod.write_runner_info(bindings, {"pid": 1})
        self.mod.clear_runner_info(bindings)

        self.assertEqual(calls, ["read", ("pid", 789), "current", ("write", {"pid": 1}), "clear"])


if __name__ == "__main__":
    unittest.main()
