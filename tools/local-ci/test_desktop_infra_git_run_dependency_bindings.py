#!/usr/bin/env python3
"""Tests for desktop git command execution dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_infra_git_run_dependency_bindings.py")


class DesktopInfraGitRunDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_git_dependencies_preserve_subprocess_runner(self) -> None:
        bindings = {"subprocess": types.SimpleNamespace(run=object())}

        deps = self.mod.run_git_dependencies(bindings)

        self.assertIs(deps["run_fn"], bindings["subprocess"].run)


if __name__ == "__main__":
    unittest.main()
