#!/usr/bin/env python3
"""Tests for shared local-CI submission dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("local_ci_submission_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiSubmissionBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_resolve_submission_options_delegates_with_assembled_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return ({}, "branch", "sha", ["mac"], "normal", "full", {})

        bindings = {"_local_ci_commands_cli": types.SimpleNamespace(resolve_submission_options=runner)}
        deps = {"load_config_fn": object(), "current_branch_fn": object()}

        args_obj = object()
        with mock.patch.object(self.mod, "local_ci_submission_dependencies", return_value=deps):
            result = self.mod.resolve_submission_options(bindings, args_obj, "run")

        self.assertEqual(result[1], "branch")
        self.assertEqual(captured["args"], (args_obj, "run"))
        self.assertIs(captured["kwargs"]["load_config_fn"], deps["load_config_fn"])
        self.assertIs(captured["kwargs"]["current_branch_fn"], deps["current_branch_fn"])


if __name__ == "__main__":
    unittest.main()
