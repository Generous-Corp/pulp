#!/usr/bin/env python3
"""Tests for PR check command dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("local_ci_pr_check_command_dependency_bindings.py")


class LocalCiPrCheckCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_ci_pr_check_command_dependencies_bind_facade_dependencies(self) -> None:
        bindings = {}
        names = [
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
        ]
        for name in names:
            bindings[name] = object()

        deps = self.mod.local_ci_pr_check_command_dependencies(bindings)

        for name in names:
            self.assertIs(deps[f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
