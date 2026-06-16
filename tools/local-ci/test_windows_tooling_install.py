#!/usr/bin/env python3
"""Tests for Windows remote tooling install helper."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("windows_tooling_install.py", add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsToolingInstallTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_install_windows_remote_tool_uses_winget_package_id(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(returncode=7, stdout="out", stderr="err")

        with self.assertRaisesRegex(RuntimeError, "err"):
            self.mod.install_windows_remote_tool(
                "win",
                "Git.Git",
                timeout=5,
                run_windows_ssh_powershell_fn=fake_run,
            )
        self.assertIn("$PackageId = 'Git.Git'", scripts[-1]["script"])
        self.assertIn("'--id'", scripts[-1]["script"])
        self.assertIn("& $Winget.Source @InstallArgs", scripts[-1]["script"])
        self.assertEqual(scripts[-1]["timeout"], 5)


if __name__ == "__main__":
    unittest.main()
