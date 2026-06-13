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

    def test_build_parser_wires_facade_parser_inputs(self) -> None:
        build_local_ci_parser = mock.Mock(return_value="parser")
        cli_parser = types.SimpleNamespace(build_local_ci_parser=build_local_ci_parser)
        bindings = {
            "_cli_parser": cli_parser,
            "PRIORITY_VALUES": {"normal", "high"},
            "KEEP_COMPLETED_JOBS": 17,
            "__doc__": "usage text",
        }

        result = self.mod.build_parser(bindings)

        self.assertEqual(result, "parser")
        build_local_ci_parser.assert_called_once_with(
            priority_values=bindings["PRIORITY_VALUES"],
            keep_completed_jobs=17,
            epilog="usage text",
        )

    def test_build_local_ci_parser_delegates_to_parser_module(self) -> None:
        build_local_ci_parser = mock.Mock(return_value="parser")
        bindings = {"_cli_parser": types.SimpleNamespace(build_local_ci_parser=build_local_ci_parser)}

        result = self.mod.build_local_ci_parser(
            bindings,
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

    def test_install_cli_parser_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_cli_parser_helper = lambda _bindings: "future"

        self.mod.install_cli_parser_helpers(bindings, ("future_cli_parser_helper",))

        self.assertEqual(bindings["future_cli_parser_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
