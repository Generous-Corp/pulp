#!/usr/bin/env python3
"""Tests for validation target failure result dependency bindings."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("execution_target_failure_result_bindings.py")


class ExecutionTargetFailureResultBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_failure_result_exports_match_helpers(self) -> None:
        expected = (
            "unreachable_target_result",
            "target_exception_result",
        )

        self.assertEqual(self.mod.EXECUTION_TARGET_FAILURE_RESULT_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_failure_result_helpers_delegate_arguments(self) -> None:
        execution = types.SimpleNamespace(
            unreachable_target_result=lambda target, detail="Host unreachable": {"target": target, "detail": detail},
            target_exception_result=lambda target, exc: {"target": target, "error": str(exc)},
        )
        bindings = {"_execution": execution}

        self.assertEqual(
            self.mod.unreachable_target_result(bindings, "ubuntu"),
            {"target": "ubuntu", "detail": "Host unreachable"},
        )
        self.assertEqual(
            self.mod.target_exception_result(bindings, "mac", RuntimeError("boom")),
            {"target": "mac", "error": "boom"},
        )


if __name__ == "__main__":
    unittest.main()
