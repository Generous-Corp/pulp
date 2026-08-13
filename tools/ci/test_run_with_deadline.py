#!/usr/bin/env python3
"""Negative and exit-propagation tests for run-with-deadline.py."""

import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


RUNNER = Path(__file__).with_name("run-with-deadline.py")
RUNNER_SPEC = importlib.util.spec_from_file_location("run_with_deadline", RUNNER)
RUNNER_MODULE = importlib.util.module_from_spec(RUNNER_SPEC)
RUNNER_SPEC.loader.exec_module(RUNNER_MODULE)


def process_is_live(pid):
    result = subprocess.run(
        ["ps", "-o", "stat=", "-p", str(pid)],
        check=False,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0 and not result.stdout.lstrip().startswith("Z")


class ProcessDeadlineTests(unittest.TestCase):
    def test_permission_denied_group_probe_still_escalates(self):
        process = mock.Mock(pid=123)
        process.poll.return_value = None
        killpg_calls = [
            mock.call(123, signal.SIGTERM),
            mock.call(123, 0),
            mock.call(123, signal.SIGKILL),
        ]

        with (
            mock.patch.object(
                RUNNER_MODULE.os,
                "killpg",
                side_effect=[None, PermissionError, ProcessLookupError],
            ) as killpg,
            mock.patch.object(
                RUNNER_MODULE.time, "monotonic", side_effect=[10, 10, 11]
            ),
            mock.patch.object(RUNNER_MODULE.time, "sleep"),
        ):
            RUNNER_MODULE.terminate_group(process, grace_seconds=0.1)

        self.assertEqual(killpg.call_args_list, killpg_calls)
        process.wait.assert_called_once_with()

    def test_exit_code_is_propagated(self):
        result = subprocess.run(
            [
                sys.executable,
                str(RUNNER),
                "--timeout",
                "5",
                "--",
                sys.executable,
                "-c",
                "raise SystemExit(7)",
            ],
            check=False,
        )
        self.assertEqual(result.returncode, 7)

    def test_timeout_kills_command_and_descendant(self):
        with tempfile.TemporaryDirectory() as directory:
            pid_file = Path(directory) / "pids"
            child_code = (
                "import os, pathlib, subprocess, sys, time; "
                "child=subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
                "pathlib.Path(sys.argv[1]).write_text(f'{os.getpid()} {child.pid}'); "
                "time.sleep(60)"
            )
            started = time.monotonic()
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--timeout",
                    "0.5",
                    "--",
                    sys.executable,
                    "-c",
                    child_code,
                    str(pid_file),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 124, result.stderr)
            self.assertLess(time.monotonic() - started, 6)
            parent_pid, child_pid = map(
                int, pid_file.read_text(encoding="utf-8").split()
            )
            for _ in range(30):
                if not process_is_live(parent_pid) and not process_is_live(child_pid):
                    break
                time.sleep(0.1)
            self.assertFalse(process_is_live(parent_pid))
            self.assertFalse(process_is_live(child_pid))


if __name__ == "__main__":
    unittest.main()
