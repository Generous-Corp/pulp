#!/usr/bin/env python3
"""Tests for local_ci bootstrap private module aliases."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module


module_aliases = load_local_ci_module(
    "local_ci_bootstrap_module_aliases.py",
    module_name="local_ci_bootstrap_module_aliases",
    add_module_dir=True,
)


class LocalCiBootstrapModuleAliasesTests(unittest.TestCase):
    def test_bootstrap_module_aliases_include_expected_compatibility_names(self) -> None:
        expected_names = {
            "_state_paths",
            "_footprint",
            "_cleanup",
            "_cleanup_cli",
            "_cli_dispatch",
            "_cli_parser",
            "_cloud",
            "_desktop_action_commands_cli",
            "_desktop_actions",
            "_desktop_artifacts",
            "_desktop_commands_cli",
            "_desktop_cli",
            "_desktop_doctor",
            "_desktop_setup_commands_cli",
            "_evidence_cli",
            "_execution",
            "_git_helpers",
            "_github_workflows",
            "_io_utils",
            "_job_queue",
            "_linux_desktop_action",
            "_linux_target",
            "_local_ci_commands_cli",
            "_logs_cli",
            "_macos_desktop",
            "_macos_desktop_action",
            "_notifications",
            "_normalize",
            "_provenance",
            "_queue_commands_cli",
            "_queue_lifecycle",
            "_queue_orchestrator",
            "_reporting",
            "_runner_state",
            "_source_prep",
            "_ssh_bundle",
            "_ssh_subprocess",
            "_target_preflight",
            "_targets",
            "_windows_desktop_action",
            "_windows_probe",
            "_windows_target",
            "evidence_index_module",
            "LockBusyError",
        }

        self.assertEqual(set(module_aliases.BOOTSTRAP_MODULE_ALIASES), expected_names)

    def test_install_bootstrap_module_aliases_copies_from_module_globals(self) -> None:
        bindings = {}

        module_aliases.install_bootstrap_module_aliases(bindings)

        for binding_name, module in module_aliases.BOOTSTRAP_MODULE_ALIASES.items():
            self.assertIs(bindings[binding_name], module)


if __name__ == "__main__":
    unittest.main()
