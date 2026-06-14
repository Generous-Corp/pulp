#!/usr/bin/env python3
"""Tests for Windows remote tooling probe helper."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_tooling_probe.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsToolingProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_windows_remote_tooling_uses_injected_runner(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(stdout='{"git_found":true,"winget_found":true}\n')

        tooling = self.mod.probe_windows_remote_tooling("win", run_windows_ssh_powershell_fn=fake_run)

        self.assertTrue(tooling["git_found"])
        self.assertIn("Get-Command git", scripts[-1]["script"])
        self.assertIn("auth status", scripts[-1]["script"])
        self.assertEqual(scripts[-1]["timeout"], 60)


if __name__ == "__main__":
    unittest.main()
