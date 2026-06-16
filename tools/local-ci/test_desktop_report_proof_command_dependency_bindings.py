#!/usr/bin/env python3
"""Tests for desktop proof report command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_report_proof_command_dependency_bindings.py")


class DesktopReportProofCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_proof_dependencies_preserve_report_seams(self) -> None:
        bindings = {
            "_desktop_cli": types.SimpleNamespace(
                desktop_proof_empty_line=object(),
                desktop_proof_lines=object(),
            ),
            "load_config": object(),
            "desktop_proof_summaries": object(),
            "short_sha": object(),
        }

        deps = self.mod.desktop_report_proof_command_dependencies(bindings)

        self.assertIs(deps["load_config_fn"], bindings["load_config"])
        self.assertIs(deps["desktop_proof_summaries_fn"], bindings["desktop_proof_summaries"])
        self.assertIs(deps["desktop_proof_empty_line_fn"], bindings["_desktop_cli"].desktop_proof_empty_line)
        self.assertIs(deps["desktop_proof_lines_fn"], bindings["_desktop_cli"].desktop_proof_lines)
        self.assertIs(deps["short_sha_fn"], bindings["short_sha"])


if __name__ == "__main__":
    unittest.main()
