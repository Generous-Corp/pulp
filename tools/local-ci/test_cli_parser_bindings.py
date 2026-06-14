#!/usr/bin/env python3
"""Tests for local_ci facade parser binding wiring."""

from __future__ import annotations

from module_test_utils import load_module_from_path
import pathlib
import types
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("cli_parser_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class CliParserBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_build_parser_delegates_with_assembled_dependencies(self) -> None:
        build_local_ci_parser = mock.Mock(return_value="parser")
        cli_parser = types.SimpleNamespace(build_local_ci_parser=build_local_ci_parser)
        bindings = {"_cli_parser": cli_parser}
        deps = {
            "priority_values": {"normal", "high"},
            "keep_completed_jobs": 17,
            "epilog": "usage text",
        }

        with mock.patch.object(self.mod, "build_parser_dependencies", return_value=deps):
            result = self.mod.build_parser(bindings)

        self.assertEqual(result, "parser")
        build_local_ci_parser.assert_called_once_with(**deps)

    def test_build_local_ci_parser_delegates_to_parser_module(self) -> None:
        build_local_ci_parser = mock.Mock(return_value="parser")
        bindings = {"_cli_parser": types.SimpleNamespace(build_local_ci_parser=build_local_ci_parser)}

        result = self.mod.build_local_ci_parser(
            bindings,
            priority_values={"normal"},
            keep_completed_jobs=9,
            epilog="docs",
        )

        self.assertEqual(result, "parser")
        build_local_ci_parser.assert_called_once_with(
            priority_values={"normal"},
            keep_completed_jobs=9,
            epilog="docs",
        )

    def test_install_cli_parser_helpers_wires_named_exports(self) -> None:
        build_local_ci_parser = mock.Mock(return_value="parser")
        bindings = {
            "_cli_parser": types.SimpleNamespace(build_local_ci_parser=build_local_ci_parser),
            "PRIORITY_VALUES": {"normal"},
            "KEEP_COMPLETED_JOBS": 9,
            "__doc__": "docs",
        }

        self.mod.install_cli_parser_helpers(bindings, ("build_parser",))

        self.assertEqual(bindings["build_parser"](), "parser")
        build_local_ci_parser.assert_called_once_with(
            priority_values={"normal"},
            keep_completed_jobs=9,
            epilog="docs",
        )


if __name__ == "__main__":
    unittest.main()
