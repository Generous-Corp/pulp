#!/usr/bin/env python3
"""Tests for local validation runner facade bindings."""

from __future__ import annotations

import builtins
import types
import unittest
from pathlib import Path

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("execution_runner_local_bindings.py")


class ExecutionRunnerLocalBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_runner_exports_match_wrappers(self) -> None:
        expected = ("run_local_validation",)

        self.assertEqual(self.mod.EXECUTION_RUNNER_LOCAL_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def _bindings(self, runner):
        execution = types.SimpleNamespace(run_local_validation=runner)
        bindings = {"_execution": execution, "ROOT": Path("/repo"), "print": object()}
        for name in [
            "short_sha",
            "prepare_target_log",
            "now_iso",
            "local_validation_command",
            "run_logged_command",
            "validation_result_from_run",
        ]:
            bindings[name] = object()
        return bindings

    def test_run_local_validation_binds_facade_dependencies(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"target": "mac"}

        bindings = self._bindings(runner)
        progress = object()

        result = self.mod.run_local_validation(bindings, {"id": "job"}, "slow", progress)

        self.assertEqual(result, {"target": "mac"})
        self.assertEqual(captured["args"], ({"id": "job"}, "slow", progress))
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["print_fn"], bindings["print"])
        self.assertIs(captured["kwargs"]["local_validation_command_fn"], bindings["local_validation_command"])
        self.assertIs(captured["kwargs"]["run_logged_command_fn"], bindings["run_logged_command"])
        self.assertIs(captured["kwargs"]["validation_result_from_run_fn"], bindings["validation_result_from_run"])

    def test_run_local_validation_uses_builtin_print_when_globals_lack_print(self) -> None:
        captured = {}

        def runner(*_args, **kwargs):
            captured["kwargs"] = kwargs
            return {"target": "mac"}

        bindings = self._bindings(runner)
        del bindings["print"]

        result = self.mod.run_local_validation(bindings, {"id": "job"})

        self.assertEqual(result, {"target": "mac"})
        self.assertIs(captured["kwargs"]["print_fn"], builtins.print)

if __name__ == "__main__":
    unittest.main()
