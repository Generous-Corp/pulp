#!/usr/bin/env python3
"""Tests for desktop recent report command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_report_recent_command_dependency_bindings.py")


class DesktopReportRecentCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_recent_dependencies_preserve_report_seams(self) -> None:
        bindings = {
            "_desktop_cli": types.SimpleNamespace(desktop_recent_lines=object()),
            "load_config": object(),
            "desktop_run_manifests": object(),
            "desktop_run_summary": object(),
            "short_sha": object(),
        }

        deps = self.mod.desktop_report_recent_command_dependencies(bindings)

        self.assertIs(deps["load_config_fn"], bindings["load_config"])
        self.assertIs(deps["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(deps["desktop_run_summary_fn"], bindings["desktop_run_summary"])
        self.assertIs(deps["desktop_recent_lines_fn"], bindings["_desktop_cli"].desktop_recent_lines)
        self.assertIs(deps["short_sha_fn"], bindings["short_sha"])


if __name__ == "__main__":
    unittest.main()
