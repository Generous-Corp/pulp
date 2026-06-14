#!/usr/bin/env python3
"""Tests for stale Windows validator cleanup result helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cleanup_stale_windows_result.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CleanupStaleWindowsResultTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_stale_windows_validator_status_and_update_fields(self) -> None:
        candidate = {"validator_pid": 123, "validator_started_at": "2026-05-01T00:00:00Z"}

        self.assertEqual(self.mod.stale_windows_validator_cleanup_status({"killed": True}), "killed")
        self.assertEqual(self.mod.stale_windows_validator_cleanup_status({"found": False}), "not-found")
        self.assertEqual(self.mod.stale_windows_validator_cleanup_status({"found": True, "matched": False}), "mismatch")

        killed = self.mod.stale_windows_validator_update_fields(
            candidate,
            {"found": True, "matched": True, "killed": True, "pid": 123},
            now_fn=lambda: "done",
            trim_line_fn=lambda line: line,
        )
        self.assertEqual(killed["cleanup_status"], "killed")
        self.assertIsNone(killed["validator_pid"])


if __name__ == "__main__":
    unittest.main()
