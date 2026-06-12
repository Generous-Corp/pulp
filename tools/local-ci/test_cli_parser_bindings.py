#!/usr/bin/env python3
"""Tests for local_ci facade parser binding wiring."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("cli_parser_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("cli_parser_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class CliParserBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_build_parser_wires_facade_parser_inputs(self) -> None:
        build_local_ci_parser = mock.Mock(return_value="parser")
        bindings = {
            "build_local_ci_parser": build_local_ci_parser,
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


if __name__ == "__main__":
    unittest.main()
