#!/usr/bin/env python3
"""Tests for state lock path facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("state_path_lock_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class StatePathLockBindingsTests(unittest.TestCase):
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
            queue_lock_path=make_runner("queue_lock_path", Path("/state/queue.lock")),
            evidence_lock_path=make_runner("evidence_lock_path", Path("/state/evidence.lock")),
            drain_lock_path=make_runner("drain_lock_path", Path("/state/drain.lock")),
            runner_info_path=make_runner("runner_info_path", Path("/state/runner.json")),
        )
        return {"_state_paths": paths}, calls

    def test_lock_path_helpers_delegate_to_state_paths_module(self) -> None:
        bindings, calls = self._bindings()
        helpers = [
            "queue_lock_path",
            "evidence_lock_path",
            "drain_lock_path",
            "runner_info_path",
        ]

        for name in helpers:
            with self.subTest(name=name):
                self.assertIsInstance(getattr(self.mod, name)(bindings), Path)

        self.assertEqual([call[0] for call in calls], helpers)
        self.assertTrue(all(call[1] == () and call[2] == {} for call in calls))

    def test_install_state_path_lock_helpers_wires_named_exports(self) -> None:
        bindings, calls = self._bindings()

        self.mod.install_state_path_lock_helpers(bindings, ("queue_lock_path", "runner_info_path"))

        self.assertEqual(bindings["queue_lock_path"](), Path("/state/queue.lock"))
        self.assertEqual(bindings["runner_info_path"](), Path("/state/runner.json"))
        self.assertEqual(bindings["queue_lock_path"].__name__, "queue_lock_path")
        self.assertEqual([call[0] for call in calls], ["queue_lock_path", "runner_info_path"])


if __name__ == "__main__":
    unittest.main()
