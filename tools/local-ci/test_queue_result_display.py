#!/usr/bin/env python3
"""Tests for queue result display helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("queue_result_display.py", module_name="pulp_queue_result_display")


class QueueResultDisplayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_result_validation_line_omits_full_validation(self) -> None:
        self.assertIsNone(self.mod.result_validation_line({"validation": "full"}))
        self.assertEqual(self.mod.result_validation_line({"validation": "smoke"}), "  validation  smoke")

    def test_result_target_and_overall_lines(self) -> None:
        result = {"results": [{"target": "mac", "status": "pass", "duration_secs": 12}], "overall": "pass"}

        self.assertEqual(self.mod.target_result_line(result["results"][0]), "  mac         PASS          12s")
        self.assertEqual(self.mod.result_target_lines(result), ["  mac         PASS          12s"])
        self.assertEqual(self.mod.result_overall_line(result), "  overall     PASS")


if __name__ == "__main__":
    unittest.main()
