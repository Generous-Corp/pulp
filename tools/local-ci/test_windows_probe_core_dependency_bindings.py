#!/usr/bin/env python3
"""Tests for Windows probe core dependency assembly."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("windows_probe_core_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsProbeCoreDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_windows_ssh_powershell_dependencies_preserve_runner(self) -> None:
        bindings = {"run_ssh_subprocess": object()}

        deps = self.mod.run_windows_ssh_powershell_dependencies(bindings)

        self.assertIs(deps["run_ssh_subprocess_fn"], bindings["run_ssh_subprocess"])

    def test_windows_contract_expand_expression_dependencies_preserve_literal(self) -> None:
        bindings = {"ps_literal": object()}

        deps = self.mod.windows_contract_expand_expression_dependencies(bindings)

        self.assertIs(deps["ps_literal_fn"], bindings["ps_literal"])

    def test_windows_session_agent_template_path_dependencies_preserve_script_dir(self) -> None:
        bindings = {"SCRIPT_DIR": Path("/repo/tools/local-ci")}

        deps = self.mod.windows_session_agent_template_path_dependencies(bindings)

        self.assertIs(deps["script_dir"], bindings["SCRIPT_DIR"])


if __name__ == "__main__":
    unittest.main()
