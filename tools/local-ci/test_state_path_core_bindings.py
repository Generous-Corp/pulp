#!/usr/bin/env python3
"""Tests for core state path facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("state_path_core_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class StatePathCoreBindingsTests(unittest.TestCase):
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
            queue_path=make_runner("queue_path", Path("/state/queue.json")),
            results_dir=make_runner("results_dir", Path("/state/results")),
            cloud_runs_dir=make_runner("cloud_runs_dir", Path("/state/cloud-runs")),
            evidence_path=make_runner("evidence_path", Path("/state/evidence.json")),
            logs_dir=make_runner("logs_dir", Path("/state/logs")),
            ensure_state_dirs=make_runner("ensure_state_dirs", None),
        )
        return {"_state_paths": paths}, calls

    def test_core_path_helpers_delegate_to_state_paths_module(self) -> None:
        bindings, calls = self._bindings()
        helpers = [
            "state_dir",
            "config_path",
            "worktree_config_path",
            "shared_config_path",
            "queue_path",
            "results_dir",
            "cloud_runs_dir",
            "evidence_path",
            "logs_dir",
        ]

        for name in helpers:
            with self.subTest(name=name):
                self.assertIsInstance(getattr(self.mod, name)(bindings), Path)

        self.mod.ensure_state_dirs(bindings)
        self.assertEqual([call[0] for call in calls], [*helpers, "ensure_state_dirs"])
        self.assertTrue(all(call[1] == () and call[2] == {} for call in calls))

    def test_core_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.STATE_PATH_CONFIG_EXPORTS,
            *self.mod.STATE_PATH_STORE_EXPORTS,
        )

        self.assertEqual(self.mod.STATE_PATH_CORE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_state_path_core_helpers_wires_named_exports(self) -> None:
        bindings, calls = self._bindings()

        self.mod.install_state_path_core_helpers(bindings, ("state_dir", "logs_dir"))

        self.assertEqual(bindings["state_dir"](), Path("/state"))
        self.assertEqual(bindings["logs_dir"](), Path("/state/logs"))
        self.assertEqual(bindings["state_dir"].__name__, "state_dir")
        self.assertEqual(bindings["logs_dir"].__name__, "logs_dir")
        self.assertEqual([call[0] for call in calls], ["state_dir", "logs_dir"])

    def test_install_state_path_core_helpers_routes_focused_exports(self) -> None:
        bindings, calls = self._bindings()

        self.mod.install_state_path_core_helpers(bindings, ("config_path", "queue_path"))

        self.assertEqual(bindings["config_path"](), Path("/state/config.json"))
        self.assertEqual(bindings["queue_path"](), Path("/state/queue.json"))
        self.assertEqual([call[0] for call in calls], ["config_path", "queue_path"])

    def test_install_state_path_core_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_state_path_core_helper = lambda _bindings: "future"

        self.mod.install_state_path_core_helpers(bindings, ("future_state_path_core_helper",))

        self.assertEqual(bindings["future_state_path_core_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
