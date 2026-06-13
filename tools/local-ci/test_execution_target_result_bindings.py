#!/usr/bin/env python3
"""Tests for validation target result dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("execution_target_result_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ExecutionTargetResultBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_target_result_helpers_delegate_arguments(self):
        execution = types.SimpleNamespace(
            validation_result_from_run=lambda *args, **kwargs: {"validation": args, **kwargs},
            validation_error_result=lambda *args, **kwargs: {"error": args, **kwargs},
            unreachable_target_result=lambda target, detail="Host unreachable": {"target": target, "detail": detail},
            target_exception_result=lambda target, exc: {"target": target, "error": str(exc)},
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
        self.assertEqual(self.mod.unreachable_target_result(bindings, "ubuntu"), {"target": "ubuntu", "detail": "Host unreachable"})
        self.assertEqual(self.mod.target_exception_result(bindings, "mac", RuntimeError("boom")), {"target": "mac", "error": "boom"})


if __name__ == "__main__":
    unittest.main()
