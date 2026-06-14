#!/usr/bin/env python3
"""Tests for shared local-CI submission dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("local_ci_submission_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiSubmissionBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_resolve_submission_options_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return ({}, "branch", "sha", ["mac"], "normal", "full", {})

        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(resolve_submission_options=runner)}
        for name in [
            "load_config",
            "current_branch",
            "resolve_git_ref_sha",
            "current_sha",
            "resolve_targets",
            "parse_targets_arg",
            "normalize_priority",
            "default_priority_for",
            "normalize_validation_mode",
            "build_submission_metadata",
        ]:
            bindings[name] = object()

        args_obj = object()
        result = self.mod.resolve_submission_options(bindings, args_obj, "run")

        self.assertEqual(result[1], "branch")
        self.assertEqual(captured["args"], (args_obj, "run"))
        for name in [
            "load_config",
            "current_branch",
            "resolve_git_ref_sha",
            "current_sha",
            "resolve_targets",
            "parse_targets_arg",
            "normalize_priority",
            "default_priority_for",
            "normalize_validation_mode",
            "build_submission_metadata",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
