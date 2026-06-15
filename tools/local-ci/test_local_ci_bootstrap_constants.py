#!/usr/bin/env python3
"""Tests for local_ci bootstrap constant installation."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module


bootstrap_constants = load_local_ci_module(
    "local_ci_bootstrap_constants.py",
    module_name="local_ci_bootstrap_constants",
    add_module_dir=True,
)


class LocalCiBootstrapConstantsTests(unittest.TestCase):
    def test_install_bootstrap_constants_delegates_to_focused_binding_modules(self) -> None:
        bindings = {"sentinel": object()}
        execution_timing = types.SimpleNamespace(
            heartbeat_interval_secs=lambda received: ("heartbeat", received["sentinel"]),
            stuck_idle_secs=lambda received: ("stuck", received["sentinel"]),
        )
        windows_target = types.SimpleNamespace(
            windows_required_remote_tools=lambda received: ("win-required", received["sentinel"]),
            windows_optional_remote_tools=lambda received: ("win-optional", received["sentinel"]),
            windows_default_remote_repo_dirname=lambda received: ("win-repo", received["sentinel"]),
        )
        linux_target = types.SimpleNamespace(
            linux_required_remote_tools=lambda received: ("linux-required", received["sentinel"]),
            linux_optional_remote_tools=lambda received: ("linux-optional", received["sentinel"]),
        )
        normalize = types.SimpleNamespace(
            priority_values=lambda received: ("priority", received["sentinel"]),
        )
        github_workflow = types.SimpleNamespace(
            github_actions_defaults=lambda received: ("defaults", received["sentinel"]),
            builtin_github_workflows=lambda received: ("workflows", received["sentinel"]),
            repo_variable_fallbacks=lambda received: ("fallbacks", received["sentinel"]),
        )

        bootstrap_constants.install_bootstrap_constants(
            bindings,
            execution_timing_bindings=execution_timing,
            windows_target_bindings=windows_target,
            linux_target_bindings=linux_target,
            normalize_bindings=normalize,
            github_workflow_bindings=github_workflow,
        )

        self.assertEqual(bindings["HEARTBEAT_INTERVAL_SECS"], ("heartbeat", bindings["sentinel"]))
        self.assertEqual(bindings["STUCK_IDLE_SECS"], ("stuck", bindings["sentinel"]))
        self.assertEqual(bindings["WINDOWS_REQUIRED_REMOTE_TOOLS"], ("win-required", bindings["sentinel"]))
        self.assertEqual(bindings["WINDOWS_OPTIONAL_REMOTE_TOOLS"], ("win-optional", bindings["sentinel"]))
        self.assertEqual(bindings["WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME"], ("win-repo", bindings["sentinel"]))
        self.assertEqual(bindings["LINUX_REQUIRED_REMOTE_TOOLS"], ("linux-required", bindings["sentinel"]))
        self.assertEqual(bindings["LINUX_OPTIONAL_REMOTE_TOOLS"], ("linux-optional", bindings["sentinel"]))
        self.assertEqual(bindings["PRIORITY_VALUES"], ("priority", bindings["sentinel"]))
        self.assertEqual(bindings["GITHUB_ACTIONS_DEFAULTS"], ("defaults", bindings["sentinel"]))
        self.assertEqual(bindings["BUILTIN_GITHUB_WORKFLOWS"], ("workflows", bindings["sentinel"]))
        self.assertEqual(bindings["REPO_VARIABLE_FALLBACKS"], ("fallbacks", bindings["sentinel"]))


if __name__ == "__main__":
    unittest.main()
