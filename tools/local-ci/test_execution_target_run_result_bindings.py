#!/usr/bin/env python3
"""Tests for validation run/error result dependency bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("execution_target_run_result_bindings.py")


class ExecutionTargetRunResultBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_run_result_exports_match_helpers(self) -> None:
        expected = (
            "validation_result_from_run",
            "validation_error_result",
        )

        self.assertEqual(self.mod.EXECUTION_TARGET_RUN_RESULT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_run_result_helpers_delegate_arguments(self) -> None:
        execution = types.SimpleNamespace(
            validation_result_from_run=lambda *args, **kwargs: {"validation": args, **kwargs},
            validation_error_result=lambda *args, **kwargs: {"error": args, **kwargs},
        )
        bindings = {"_execution": execution}

        self.assertEqual(
            self.mod.validation_result_from_run(
                bindings,
                "mac",
                {"exit_code": 0},
                log_path=Path("/log"),
                validation="full",
                transport_mode="local",
            )["timeout_secs"],
            3600,
        )
        self.assertEqual(
            self.mod.validation_error_result(
                bindings,
                "mac",
                "detail",
                log_path=Path("/log"),
                transport_mode="local",
            )["transport_mode"],
            "local",
        )


if __name__ == "__main__":
    unittest.main()
