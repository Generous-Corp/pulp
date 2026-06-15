#!/usr/bin/env python3
"""Tests for desktop publish report command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_report_publish_command_dependency_bindings.py")


class DesktopReportPublishCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_publish_dependencies_preserve_report_seams(self) -> None:
        bindings = {
            "_desktop_cli": types.SimpleNamespace(desktop_publish_lines=object()),
            "load_config": object(),
            "desktop_run_manifests": object(),
            "stage_desktop_publish_report": object(),
        }

        deps = self.mod.desktop_report_publish_command_dependencies(bindings)

        self.assertIs(deps["load_config_fn"], bindings["load_config"])
        self.assertIs(deps["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(deps["stage_desktop_publish_report_fn"], bindings["stage_desktop_publish_report"])
        self.assertIs(deps["desktop_publish_lines_fn"], bindings["_desktop_cli"].desktop_publish_lines)


if __name__ == "__main__":
    unittest.main()
