#!/usr/bin/env python3
"""Tests for PR ship command dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from pathlib import Path
from unittest import mock



def load_module():
    return load_local_ci_module("local_ci_pr_ship_command_bindings.py")


class LocalCiPrShipCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        bindings = {
            "_local_ci_commands_cli": types.SimpleNamespace(cmd_ship=runner),
            "ROOT": Path("/repo"),
            "subprocess": types.SimpleNamespace(run=object()),
        }
        for name in [
            "resolve_submission_options",
            "gh_available",
            "print_submission_metadata",
            "gh_pr_create",
            "enqueue_job",
            "summarize_job",
            "wait_for_job",
            "gh_pr_comment",
            "format_ci_comment",
            "gh_pr_merge",
            "notify",
        ]:
            bindings[name] = object()
        return bindings

    def test_ship_exports_match_wrappers(self):
        self.assertEqual(self.mod.LOCAL_CI_PR_SHIP_COMMAND_EXPORTS, ("cmd_ship",))
        self.assertTrue(callable(self.mod.cmd_ship))

    def test_cmd_ship_delegates_with_assembled_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        bindings = self._bindings(runner)
        args_obj = object()
        deps = {"resolve_submission_options_fn": object(), "notify_fn": object()}
        with mock.patch.object(self.mod, "local_ci_pr_ship_command_dependencies", return_value=deps):
            self.assertEqual(self.mod.cmd_ship(bindings, args_obj), 5)

        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["resolve_submission_options_fn"], deps["resolve_submission_options_fn"])
        self.assertIs(captured["kwargs"]["notify_fn"], deps["notify_fn"])

    def test_install_ship_helpers_wires_named_exports(self):
        calls = []

        def runner(*args, **kwargs):
            calls.append((args, kwargs))
            return 7

        bindings = self._bindings(runner)
        self.mod.install_local_ci_pr_ship_command_helpers(bindings)

        args_obj = object()
        self.assertEqual(bindings["cmd_ship"](args_obj), 7)
        self.assertEqual(calls[0][0], (args_obj,))
        self.assertEqual(bindings["cmd_ship"].__name__, "cmd_ship")


if __name__ == "__main__":
    unittest.main()
