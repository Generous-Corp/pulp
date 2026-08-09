#!/usr/bin/env python3
"""Focused tests for the portable CI timeout helper."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import unittest
from unittest import mock

import run_with_timeout


SCRIPT = pathlib.Path(__file__).with_name("run_with_timeout.py")


class RunWithTimeoutTests(unittest.TestCase):
    def run_helper(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            text=True,
            capture_output=True,
            timeout=10,
        )

    def test_propagates_command_success_and_failure(self) -> None:
        self.assertEqual(
            self.run_helper("2", sys.executable, "-c", "raise SystemExit(0)").returncode,
            0,
        )
        self.assertEqual(
            self.run_helper("2", sys.executable, "-c", "raise SystemExit(7)").returncode,
            7,
        )

    def test_timeout_returns_124(self) -> None:
        result = self.run_helper("0.05", sys.executable, "-c", "import time; time.sleep(5)")
        self.assertEqual(result.returncode, 124)
        self.assertIn("terminating process group", result.stderr)

    def test_rejects_invalid_invocations(self) -> None:
        self.assertEqual(self.run_helper().returncode, 2)
        self.assertEqual(self.run_helper("nope", "true").returncode, 2)
        self.assertEqual(self.run_helper("0", "true").returncode, 2)

    def test_darwin_permission_race_waits_for_group_leader(self) -> None:
        process = mock.Mock(pid=1234)
        process.poll.return_value = None
        process.wait.return_value = -15

        with mock.patch.object(
            run_with_timeout.os,
            "killpg",
            side_effect=[None, PermissionError(1, "Operation not permitted")],
        ):
            run_with_timeout.terminate_process_group(process)

        process.wait.assert_called_once_with(
            timeout=run_with_timeout.TERMINATION_GRACE_SECONDS
        )


if __name__ == "__main__":
    unittest.main()
