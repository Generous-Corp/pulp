#!/usr/bin/env python3
"""Tests for queue result display facade bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("queue_result_display_bindings.py")


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

    def test_install_queue_result_display_helpers_wires_named_exports(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_queue_result_display_helpers(bindings, ("result_overall_line", "custom_result_display"))

        self.assertEqual(
            install_local.call_args_list,
            [
                mock.call(bindings, self.mod.__dict__, ("result_overall_line",)),
                mock.call(bindings, self.mod.__dict__, ("custom_result_display",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
