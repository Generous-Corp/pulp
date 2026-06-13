#!/usr/bin/env python3
"""Tests for PR list command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_pr_list_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiPrListCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(cmd_list=runner)}
        for name in [
            "gh_available",
            "gh_pr_list_open",
            "open_pr_list_lines",
        ]:
            bindings[name] = object()
        return bindings

    def test_list_exports_match_wrappers(self):
        self.assertEqual(self.mod.LOCAL_CI_PR_LIST_COMMAND_EXPORTS, ("cmd_list",))
        self.assertTrue(callable(self.mod.cmd_list))

    def test_cmd_list_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = self._bindings(runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_list(bindings, args_obj), 9)

        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["gh_available_fn"], bindings["gh_available"])
        self.assertIs(captured["kwargs"]["gh_pr_list_open_fn"], bindings["gh_pr_list_open"])
        self.assertIs(captured["kwargs"]["open_pr_list_lines_fn"], bindings["open_pr_list_lines"])

    def test_install_list_helpers_wires_named_exports(self):
        calls = []

        def runner(*args, **kwargs):
            calls.append((args, kwargs))
            return 10

        bindings = self._bindings(runner)
        self.mod.install_local_ci_pr_list_command_helpers(bindings)

        args_obj = object()
        self.assertEqual(bindings["cmd_list"](args_obj), 10)
        self.assertEqual(calls[0][0], (args_obj,))
        self.assertEqual(bindings["cmd_list"].__name__, "cmd_list")

    def test_install_local_ci_pr_list_command_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_local_ci_pr_list_command_helper = lambda _bindings: "future"

        self.mod.install_local_ci_pr_list_command_helpers(bindings, ("future_local_ci_pr_list_command_helper",))

        self.assertEqual(bindings["future_local_ci_pr_list_command_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
