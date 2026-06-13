#!/usr/bin/env python3
"""Tests for PR-oriented local-CI command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_pr_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiPrCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        bindings = {
            "_local_ci_commands_cli": types.SimpleNamespace(**{runner_name: runner}),
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
            "gh_pr_head",
            "short_sha",
            "load_config",
            "resolve_targets",
            "parse_targets_arg",
            "normalize_priority",
            "default_priority_for",
            "normalize_validation_mode",
            "build_submission_metadata",
            "gh_pr_list_open",
            "open_pr_list_lines",
        ]:
            bindings[name] = object()
        return bindings

    def test_cmd_ship_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 5

        bindings = self._bindings("cmd_ship", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_ship(bindings, args_obj), 5)

        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["resolve_submission_options_fn"], bindings["resolve_submission_options"])
        self.assertIs(captured["kwargs"]["gh_available_fn"], bindings["gh_available"])
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["run_fn"], bindings["subprocess"].run)
        self.assertIs(captured["kwargs"]["gh_pr_merge_fn"], bindings["gh_pr_merge"])

    def test_cmd_check_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 3

        bindings = self._bindings("cmd_check", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_check(bindings, args_obj), 3)

        for name in [
            "gh_available",
            "gh_pr_head",
            "short_sha",
            "load_config",
            "resolve_targets",
            "parse_targets_arg",
            "normalize_priority",
            "default_priority_for",
            "normalize_validation_mode",
            "build_submission_metadata",
            "print_submission_metadata",
            "enqueue_job",
            "summarize_job",
            "wait_for_job",
            "gh_pr_comment",
            "format_ci_comment",
            "notify",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_cmd_list_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 9

        bindings = self._bindings("cmd_list", runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_list(bindings, args_obj), 9)
        self.assertIs(captured["kwargs"]["gh_available_fn"], bindings["gh_available"])
        self.assertIs(captured["kwargs"]["gh_pr_list_open_fn"], bindings["gh_pr_list_open"])
        self.assertIs(captured["kwargs"]["open_pr_list_lines_fn"], bindings["open_pr_list_lines"])

    def test_pr_command_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.LOCAL_CI_PR_SHIP_COMMAND_EXPORTS,
            *self.mod.LOCAL_CI_PR_CHECK_COMMAND_EXPORTS,
            *self.mod.LOCAL_CI_PR_LIST_COMMAND_EXPORTS,
        )

        self.assertEqual(self.mod.LOCAL_CI_PR_COMMAND_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_pr_command_helpers_routes_named_exports(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 11

        bindings = self._bindings("cmd_list", runner)
        args_obj = object()
        self.mod.install_local_ci_pr_command_helpers(bindings, ("cmd_list",))

        self.assertEqual(bindings["cmd_list"](args_obj), 11)
        self.assertEqual(captured["args"], (args_obj,))
        self.assertIs(captured["kwargs"]["gh_available_fn"], bindings["gh_available"])
        self.assertIs(captured["kwargs"]["gh_pr_list_open_fn"], bindings["gh_pr_list_open"])
        self.assertIs(captured["kwargs"]["open_pr_list_lines_fn"], bindings["open_pr_list_lines"])


if __name__ == "__main__":
    unittest.main()
