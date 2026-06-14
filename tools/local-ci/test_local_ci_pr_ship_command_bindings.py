#!/usr/bin/env python3
"""Tests for PR ship command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_pr_ship_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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

    def test_cmd_ship_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        bindings = self._bindings(runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_ship(bindings, args_obj), 5)

        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["resolve_submission_options_fn"], bindings["resolve_submission_options"])
        self.assertIs(captured["kwargs"]["gh_available_fn"], bindings["gh_available"])
        self.assertIs(captured["kwargs"]["print_submission_metadata_fn"], bindings["print_submission_metadata"])
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["run_fn"], bindings["subprocess"].run)
        self.assertIs(captured["kwargs"]["gh_pr_create_fn"], bindings["gh_pr_create"])
        self.assertIs(captured["kwargs"]["enqueue_job_fn"], bindings["enqueue_job"])
        self.assertIs(captured["kwargs"]["summarize_job_fn"], bindings["summarize_job"])
        self.assertIs(captured["kwargs"]["wait_for_job_fn"], bindings["wait_for_job"])
        self.assertIs(captured["kwargs"]["gh_pr_comment_fn"], bindings["gh_pr_comment"])
        self.assertIs(captured["kwargs"]["format_ci_comment_fn"], bindings["format_ci_comment"])
        self.assertIs(captured["kwargs"]["gh_pr_merge_fn"], bindings["gh_pr_merge"])
        self.assertIs(captured["kwargs"]["notify_fn"], bindings["notify"])

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
