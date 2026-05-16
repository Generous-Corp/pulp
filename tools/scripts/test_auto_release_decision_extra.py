#!/usr/bin/env python3
"""Additional edge coverage for auto_release_decision.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import unittest
from unittest import mock


HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "auto_release_decision.py"

spec = importlib.util.spec_from_file_location("auto_release_decision", SCRIPT)
ard = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(ard)


class ParseVersionEdgeTests(unittest.TestCase):
    def test_parse_version_returns_tuple_for_exact_semver(self) -> None:
        self.assertEqual(ard.parse_version("10.2.300"), (10, 2, 300))

    def test_parse_version_rejects_wrong_part_counts(self) -> None:
        self.assertIsNone(ard.parse_version("1.2"))
        self.assertIsNone(ard.parse_version("1.2.3.4"))

    def test_parse_version_rejects_non_numeric_parts(self) -> None:
        self.assertIsNone(ard.parse_version("1.two.3"))


class MainCliEdgeTests(unittest.TestCase):
    def test_main_prints_json_decision_for_plugin_surface(self) -> None:
        argv = [
            str(SCRIPT),
            "--head-version",
            "2.0.0",
            "--tag-version",
            "1.9.9",
            "--bump-commit-has-skip",
            "0",
            "--surface",
            "plugin",
        ]
        output = io.StringIO()
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(output):
            self.assertEqual(ard.main(), 0)

        payload = json.loads(output.getvalue())
        self.assertEqual(payload["should_tag"], 1)
        self.assertEqual(payload["surface"], "plugin")
        self.assertIn("tagging", payload["reason"])

    def test_script_entrypoint_handles_empty_head_version(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(SCRIPT), "--surface", "sdk"],
            check=True,
            text=True,
            capture_output=True,
        )
        payload = json.loads(completed.stdout)
        self.assertEqual(payload["should_tag"], 0)
        self.assertIn("no sdk version", payload["reason"])

    def test_script_entrypoint_rejects_invalid_skip_flag(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--head-version",
                "1.0.0",
                "--bump-commit-has-skip",
                "2",
            ],
            text=True,
            capture_output=True,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("invalid choice", completed.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
