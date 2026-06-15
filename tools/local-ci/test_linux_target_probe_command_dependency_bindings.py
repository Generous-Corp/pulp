#!/usr/bin/env python3
"""Tests for Linux target probe command dependency assembly."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("linux_target_probe_command_dependency_bindings.py")


class LinuxTargetProbeCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_dependencies_preserve_ssh_command_seam(self) -> None:
        bindings = {"ssh_command_result": object()}

        deps = self.mod.linux_target_probe_command_dependencies(bindings)

        self.assertIs(deps["ssh_command_result_fn"], bindings["ssh_command_result"])


if __name__ == "__main__":
    unittest.main()
