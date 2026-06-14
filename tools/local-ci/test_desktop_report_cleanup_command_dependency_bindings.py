#!/usr/bin/env python3
"""Tests for desktop cleanup report command dependency assembly."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_report_cleanup_command_dependency_bindings.py")


class DesktopReportCleanupCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_dependencies_preserve_report_seams(self) -> None:
        bindings = {
            "_desktop_cli": types.SimpleNamespace(
                desktop_cleanup_empty_line=object(),
                desktop_cleanup_lines=object(),
            ),
            "load_config": object(),
            "prune_desktop_run_manifests": object(),
            "write_desktop_run_rollups": object(),
        }

        deps = self.mod.desktop_report_cleanup_command_dependencies(bindings)

        self.assertIs(deps["load_config_fn"], bindings["load_config"])
        self.assertIs(deps["prune_desktop_run_manifests_fn"], bindings["prune_desktop_run_manifests"])
        self.assertIs(deps["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])
        self.assertIs(deps["desktop_cleanup_empty_line_fn"], bindings["_desktop_cli"].desktop_cleanup_empty_line)
        self.assertIs(deps["desktop_cleanup_lines_fn"], bindings["_desktop_cli"].desktop_cleanup_lines)


if __name__ == "__main__":
    unittest.main()
