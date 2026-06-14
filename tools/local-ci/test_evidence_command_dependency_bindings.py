#!/usr/bin/env python3
"""Tests for evidence command dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("evidence_command_dependency_bindings.py")


class EvidenceCommandDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_evidence_command_dependencies_preserve_facade_seams(self) -> None:
        bindings = {
            "current_branch": object(),
            "evidence_scope_header_line": object(),
            "print_evidence_summary": object(),
            "evidence_empty_line": object(),
        }

        deps = self.mod.evidence_command_dependencies(bindings)

        self.assertIs(deps["current_branch_fn"], bindings["current_branch"])
        self.assertIs(deps["evidence_scope_header_line_fn"], bindings["evidence_scope_header_line"])
        self.assertIs(deps["print_evidence_summary_fn"], bindings["print_evidence_summary"])
        self.assertIs(deps["evidence_empty_line_fn"], bindings["evidence_empty_line"])


if __name__ == "__main__":
    unittest.main()
