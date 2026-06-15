#!/usr/bin/env python3
"""Tests for CLI parser dependency assembly."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("cli_parser_dependency_bindings.py")


class CliParserDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_build_parser_dependencies_preserve_facade_inputs(self) -> None:
        bindings = {
            "PRIORITY_VALUES": {"normal", "high"},
            "KEEP_COMPLETED_JOBS": 17,
            "__doc__": "usage text",
        }

        deps = self.mod.build_parser_dependencies(bindings)

        self.assertIs(deps["priority_values"], bindings["PRIORITY_VALUES"])
        self.assertEqual(deps["keep_completed_jobs"], 17)
        self.assertEqual(deps["epilog"], "usage text")


if __name__ == "__main__":
    unittest.main()
