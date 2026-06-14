#!/usr/bin/env python3
"""Tests for PR list command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("local_ci_pr_list_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiPrListCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        return {"_local_ci_commands_cli": types.SimpleNamespace(cmd_list=runner)}

    def test_list_exports_match_wrappers(self):
        self.assertEqual(self.mod.LOCAL_CI_PR_LIST_COMMAND_EXPORTS, ("cmd_list",))
        self.assertTrue(callable(self.mod.cmd_list))

    def test_cmd_list_delegates_with_assembled_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = self._bindings(runner)
        deps = {
            "gh_available_fn": object(),
            "gh_pr_list_open_fn": object(),
            "open_pr_list_lines_fn": object(),
        }
        args_obj = object()
        with mock.patch.object(self.mod, "local_ci_pr_list_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_list(bindings, args_obj), 9)

        self.assertEqual(captured["args"], (args_obj,))
        self.assertEqual(captured["kwargs"], deps)

    def test_install_list_helpers_wires_named_exports(self):
        calls = []

        def runner(*args, **kwargs):
            calls.append((args, kwargs))
            return 10

        bindings = self._bindings(runner)
        bindings["gh_available"] = object()
        bindings["gh_pr_list_open"] = object()
        bindings["open_pr_list_lines"] = object()
        self.mod.install_local_ci_pr_list_command_helpers(bindings)

        args_obj = object()
        self.assertEqual(bindings["cmd_list"](args_obj), 10)
        self.assertEqual(calls[0][0], (args_obj,))
        self.assertEqual(bindings["cmd_list"].__name__, "cmd_list")


if __name__ == "__main__":
    unittest.main()
