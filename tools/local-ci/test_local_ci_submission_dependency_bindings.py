#!/usr/bin/env python3
"""Tests for shared local-CI submission dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("local_ci_submission_dependency_bindings.py")


class LocalCiSubmissionDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_local_ci_submission_dependencies_bind_facade_dependencies(self) -> None:
        bindings = {}
        names = [
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
        ]
        for name in names:
            bindings[name] = object()

        deps = self.mod.local_ci_submission_dependencies(bindings)

        for name in names:
            self.assertIs(deps[f"{name}_fn"], bindings[name])


if __name__ == "__main__":
    unittest.main()
