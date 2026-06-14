#!/usr/bin/env python3
"""Tests for PR ship command dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("local_ci_pr_ship_command_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LocalCiPrShipCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_ci_pr_ship_command_dependencies_bind_facade_dependencies(self) -> None:
        bindings = {
            "ROOT": Path("/repo"),
            "subprocess": types.SimpleNamespace(run=object()),
        }
        names = [
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
        ]
        for name in names:
            bindings[name] = object()

        deps = self.mod.local_ci_pr_ship_command_dependencies(bindings)

        self.assertIs(deps["root"], bindings["ROOT"])
        self.assertIs(deps["run_fn"], bindings["subprocess"].run)
        for name in names:
            self.assertIs(deps[f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
