#!/usr/bin/env python3
"""Tests for queue result display facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("queue_result_display_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class QueueResultDisplayBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_result_display_exports_match_facade_helpers(self):
        expected = (
            "result_validation_line",
            "result_execution_line",
            "target_result_line",
            "result_target_lines",
            "result_overall_line",
        )

        self.assertEqual(self.mod.QUEUE_RESULT_DISPLAY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_result_display_bindings_delegate_to_orchestrator(self):
        orchestrator = types.SimpleNamespace(
            result_validation_line=lambda result: "validation",
            result_execution_line=lambda result: "execution",
            target_result_line=lambda item: "target",
            result_target_lines=lambda result: ["target"],
            result_overall_line=lambda result: "overall",
        )
        bindings = {"_queue_orchestrator": orchestrator}

        self.assertEqual(self.mod.result_validation_line(bindings, {"validation": "smoke"}), "validation")
        self.assertEqual(self.mod.result_execution_line(bindings, {"overall": "pass"}), "execution")
        self.assertEqual(self.mod.target_result_line(bindings, {"target": "mac"}), "target")
        self.assertEqual(self.mod.result_target_lines(bindings, {"targets": []}), ["target"])
        self.assertEqual(self.mod.result_overall_line(bindings, {"overall": "pass"}), "overall")


if __name__ == "__main__":
    unittest.main()
