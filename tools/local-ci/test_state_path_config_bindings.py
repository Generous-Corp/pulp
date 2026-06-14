#!/usr/bin/env python3
"""Tests for state/config path facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("state_path_config_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class StatePathConfigBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        paths = types.SimpleNamespace(
            state_dir=make_runner("state_dir", Path("/state")),
            config_path=make_runner("config_path", Path("/state/config.json")),
            worktree_config_path=make_runner("worktree_config_path", Path("/repo/config.json")),
            shared_config_path=make_runner("shared_config_path", Path("/state/config.json")),
        )
        return {"_state_paths": paths}, calls

    def test_config_path_exports_match_facade_helpers(self) -> None:
        expected = (
            "state_dir",
            "config_path",
            "worktree_config_path",
            "shared_config_path",
        )

        self.assertEqual(self.mod.STATE_PATH_CONFIG_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_config_path_helpers_delegate_to_state_paths_module(self) -> None:
        bindings, calls = self._bindings()

        self.assertEqual(self.mod.state_dir(bindings), Path("/state"))
        self.assertEqual(self.mod.config_path(bindings), Path("/state/config.json"))
        self.assertEqual(self.mod.worktree_config_path(bindings), Path("/repo/config.json"))
        self.assertEqual(self.mod.shared_config_path(bindings), Path("/state/config.json"))

        self.assertEqual(
            [call[0] for call in calls],
            ["state_dir", "config_path", "worktree_config_path", "shared_config_path"],
        )
        self.assertTrue(all(call[1] == () and call[2] == {} for call in calls))


if __name__ == "__main__":
    unittest.main()
