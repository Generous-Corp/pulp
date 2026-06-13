#!/usr/bin/env python3
"""Tests for validation result I/O facade bindings."""

from __future__ import annotations

import types
import unittest
from pathlib import Path

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("execution_result_io_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionResultIoBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_save_and_print_result_bind_facade_dependencies(self) -> None:
        captured = {}

        def save_runner(*args, **kwargs):
            captured["save"] = (args, kwargs)
            return Path("/state/result.json")

        def print_runner(*args, **kwargs):
            captured["print"] = (args, kwargs)

        bindings = {
            "_execution": types.SimpleNamespace(save_result=save_runner, print_result=print_runner),
            "datetime": types.SimpleNamespace(now=object()),
            "print": object(),
        }
        for name in [
            "ensure_state_dirs",
            "results_dir",
            "update_evidence_index",
            "normalize_result",
            "result_validation_line",
            "result_execution_line",
            "result_target_lines",
            "result_overall_line",
        ]:
            bindings[name] = object()
        result_payload = {"job_id": "job"}

        self.assertEqual(self.mod.save_result(bindings, result_payload), Path("/state/result.json"))
        self.assertEqual(captured["save"][0], (result_payload,))
        self.assertIs(captured["save"][1]["ensure_state_dirs_fn"], bindings["ensure_state_dirs"])
        self.assertIs(captured["save"][1]["results_dir_fn"], bindings["results_dir"])
        self.assertIs(captured["save"][1]["update_evidence_index_fn"], bindings["update_evidence_index"])
        self.assertIs(captured["save"][1]["now_fn"], bindings["datetime"].now)

        result_path = Path("/state/result.json")
        self.mod.print_result(bindings, result_payload, result_path)

        self.assertEqual(captured["print"][0], (result_payload, result_path))
        self.assertIs(captured["print"][1]["normalize_result_fn"], bindings["normalize_result"])
        self.assertIs(captured["print"][1]["result_validation_line_fn"], bindings["result_validation_line"])
        self.assertIs(captured["print"][1]["result_execution_line_fn"], bindings["result_execution_line"])
        self.assertIs(captured["print"][1]["result_target_lines_fn"], bindings["result_target_lines"])
        self.assertIs(captured["print"][1]["result_overall_line_fn"], bindings["result_overall_line"])
        self.assertIs(captured["print"][1]["print_fn"], bindings["print"])

    def test_result_io_exports_match_wrappers(self) -> None:
        expected = (
            "save_result",
            "print_result",
        )

        self.assertEqual(self.mod.EXECUTION_RESULT_IO_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_install_result_io_helpers_wires_named_exports(self) -> None:
        captured = {}

        def save_runner(*args, **kwargs):
            captured["save"] = (args, kwargs)
            return Path("/state/result.json")

        bindings = {
            "_execution": types.SimpleNamespace(save_result=save_runner),
            "datetime": types.SimpleNamespace(now=object()),
            "ensure_state_dirs": object(),
            "results_dir": object(),
            "update_evidence_index": object(),
        }
        self.mod.install_execution_result_io_helpers(bindings, ("save_result",))

        self.assertEqual(bindings["save_result"]({"job_id": "job"}), Path("/state/result.json"))
        self.assertIs(captured["save"][1]["results_dir_fn"], bindings["results_dir"])
        self.assertEqual(bindings["save_result"].__name__, "save_result")


if __name__ == "__main__":
    unittest.main()
