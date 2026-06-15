#!/usr/bin/env python3
"""Tests for desktop status command dependency assembly."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_status_command_dependency_bindings.py")


class DesktopStatusCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_status_dependencies_preserve_status_display_seams(self) -> None:
        bindings = {
            "_desktop_cli": types.SimpleNamespace(desktop_status_lines=object()),
            "load_config": object(),
            "desktop_receipt_for": object(),
            "desktop_capabilities_for": object(),
            "desktop_optional_capabilities": object(),
            "desktop_run_manifests": object(),
            "desktop_run_summary": object(),
            "desktop_proof_summaries": object(),
            "normalize_desktop_optional_config": object(),
            "desktop_target_contract": object(),
            "desktop_publish_reports": object(),
            "short_sha": object(),
            "windows_tooling_detail": object(),
            "windows_repo_checkout_detail": object(),
        }

        deps = self.mod.desktop_status_command_dependencies(bindings)

        for name in [
            "load_config",
            "desktop_receipt_for",
            "desktop_capabilities_for",
            "desktop_optional_capabilities",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "normalize_desktop_optional_config",
            "desktop_target_contract",
            "desktop_publish_reports",
            "short_sha",
            "windows_tooling_detail",
            "windows_repo_checkout_detail",
        ]:
            self.assertIs(deps[f"{name}_fn"], bindings[name])
        self.assertIs(deps["desktop_status_lines_fn"], bindings["_desktop_cli"].desktop_status_lines)


if __name__ == "__main__":
    unittest.main()
