#!/usr/bin/env python3
"""Tests for Windows session-agent probe helper."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_session_agent_probe.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsSessionAgentProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_windows_session_agent_reports_task_and_interactive_state(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(stdout='{"task_present":true,"interactive_user":"dev"}\n')

        session = self.mod.probe_windows_session_agent(
            "win",
            {
                "task_name": "PulpTask",
                "remote_root": r"%LOCALAPPDATA%\Pulp",
                "script_path": r"%LOCALAPPDATA%\Pulp\agent.ps1",
            },
            run_windows_ssh_powershell_fn=fake_run,
        )

        self.assertTrue(session["task_present"])
        self.assertEqual(session["interactive_user"], "dev")
        self.assertIn("Get-ScheduledTask", scripts[-1]["script"])
        self.assertIn("quser", scripts[-1]["script"])
        self.assertEqual(scripts[-1]["timeout"], 60)

    def test_probe_windows_session_agent_reports_remote_errors(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "probe failed"):
            self.mod.probe_windows_session_agent(
                "win",
                {"task_name": "PulpTask", "remote_root": "root"},
                run_windows_ssh_powershell_fn=lambda *_args, **_kwargs: completed(
                    returncode=1,
                    stderr="probe failed",
                ),
            )


if __name__ == "__main__":
    unittest.main()
