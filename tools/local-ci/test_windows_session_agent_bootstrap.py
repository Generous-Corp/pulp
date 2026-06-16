#!/usr/bin/env python3
"""Tests for Windows session-agent bootstrap helper."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("windows_session_agent_bootstrap.py", add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsSessionAgentBootstrapTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.tmp_path = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_bootstrap_windows_session_agent_uses_callbacks(self) -> None:
        template = self.tmp_path / "windows_session_agent.ps1"
        template.write_text("agent body")
        contract = {
            "task_name": "PulpDesktopAutomationAgent-win",
            "remote_root": r"%LOCALAPPDATA%\Pulp\agent",
            "script_path": r"%LOCALAPPDATA%\Pulp\agent\agent.ps1",
        }
        writes: list[tuple[str, str, str]] = []
        scripts: list[str] = []

        def fake_write(host, remote_path, content):
            writes.append((host, remote_path, content))

        def fake_run(_host, script, *, timeout=0):
            scripts.append(script)
            return completed(stdout='{"task_present":true,"task_name":"PulpDesktopAutomationAgent-win"}\n')

        result = self.mod.bootstrap_windows_session_agent(
            "win",
            contract,
            windows_session_agent_template_path_fn=lambda: template,
            windows_ssh_write_text_fn=fake_write,
            run_windows_ssh_powershell_fn=fake_run,
        )

        self.assertEqual(result["task_present"], True)
        self.assertEqual(writes, [("win", contract["script_path"], "agent body")])
        self.assertIn("Register-ScheduledTask", scripts[-1])
        self.assertIn('-File "{0}" -RemoteRoot "{1}"', scripts[-1])
        self.assertIn("PulpDesktopAutomationAgent-win", scripts[-1])

    def test_bootstrap_missing_template_fails_before_remote_write(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "template missing"):
            self.mod.bootstrap_windows_session_agent(
                "win",
                {"task_name": "Pulp", "remote_root": "root", "script_path": "agent.ps1"},
                windows_session_agent_template_path_fn=lambda: self.tmp_path / "missing.ps1",
                windows_ssh_write_text_fn=lambda *_args: self.fail("should not write"),
                run_windows_ssh_powershell_fn=lambda *_args, **_kwargs: completed(),
            )


if __name__ == "__main__":
    unittest.main()
