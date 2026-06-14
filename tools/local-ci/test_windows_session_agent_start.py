#!/usr/bin/env python3
"""Tests for Windows session-agent scheduled-task start helper."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_session_agent_start.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsSessionAgentStartTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_start_windows_session_agent_task_runs_scheduled_task(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(stdout='{"started":true,"task_name":"PulpTask"}\n')

        self.mod.start_windows_session_agent_task(
            "win",
            {"task_name": "PulpTask"},
            run_windows_ssh_powershell_fn=fake_run,
        )

        self.assertIn("Start-ScheduledTask", scripts[-1]["script"])
        self.assertIn("PulpTask", scripts[-1]["script"])
        self.assertEqual(scripts[-1]["timeout"], 30)

    def test_start_windows_session_agent_task_reports_remote_errors(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "start failed"):
            self.mod.start_windows_session_agent_task(
                "win",
                {"task_name": "PulpTask"},
                run_windows_ssh_powershell_fn=lambda *_args, **_kwargs: completed(
                    returncode=1,
                    stderr="start failed",
                ),
            )


if __name__ == "__main__":
    unittest.main()
