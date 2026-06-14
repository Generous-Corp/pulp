#!/usr/bin/env python3
"""Tests for PR check command dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_pr_check_command_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiPrCheckCommandBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner):
        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(cmd_check=runner)}
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
            bindings[name] = object()
        return bindings

    def test_check_exports_match_wrappers(self):
        self.assertEqual(self.mod.LOCAL_CI_PR_CHECK_COMMAND_EXPORTS, ("cmd_check",))
        self.assertTrue(callable(self.mod.cmd_check))

    def test_cmd_check_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return 3

        bindings = self._bindings(runner)
        args_obj = object()
        self.assertEqual(self.mod.cmd_check(bindings, args_obj), 3)

        self.assertEqual(captured["args"], (args_obj,))
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

    def test_install_check_helpers_wires_named_exports(self):
        calls = []

        def runner(*args, **kwargs):
            calls.append((args, kwargs))
            return 4

        bindings = self._bindings(runner)
        self.mod.install_local_ci_pr_check_command_helpers(bindings)

        args_obj = object()
        self.assertEqual(bindings["cmd_check"](args_obj), 4)
        self.assertEqual(calls[0][0], (args_obj,))
        self.assertEqual(bindings["cmd_check"].__name__, "cmd_check")


if __name__ == "__main__":
    unittest.main()
