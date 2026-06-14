#!/usr/bin/env python3
"""Tests for remote stale Windows validator cleanup execution."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cleanup_stale_windows_remote.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class CleanupStaleWindowsRemoteTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_stale_windows_validator_parses_remote_json(self) -> None:
        captured = {}

        def fake_run_logged_command(command, **kwargs):
            captured["command"] = command
            captured["input_text"] = kwargs.get("input_text", "")
            captured["timeout"] = kwargs.get("timeout")
            return {"returncode": 0, "output": 'banner\n{"found":true,"matched":true,"killed":true,"children":[456]}\n'}

        result = self.mod.cleanup_stale_windows_validator(
            "win",
            123,
            "2026-05-01T00:00:00Z",
            ps_literal_fn=lambda value: value,
            run_logged_command_fn=fake_run_logged_command,
            windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
            trim_line_fn=lambda line: line[:100],
        )

        self.assertTrue(result["killed"])
        self.assertEqual(result["children"], [456])
        self.assertEqual(captured["command"], ["ssh", "win"])
        self.assertEqual(captured["timeout"], 120)
        self.assertIn("$PidToKill = 123", captured["input_text"])

    def test_cleanup_stale_windows_validator_reports_non_json_error(self) -> None:
        result = self.mod.cleanup_stale_windows_validator(
            "win",
            123,
            "",
            ps_literal_fn=lambda value: value,
            run_logged_command_fn=lambda _command, **_kwargs: {"returncode": 7, "output": "not json\n"},
            windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
            trim_line_fn=lambda line: line[:8],
        )

        self.assertEqual(result["error"], "not json")


if __name__ == "__main__":
    unittest.main()
