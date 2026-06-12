#!/usr/bin/env python3
"""Tests for Windows remote tooling helpers."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_tooling.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsToolingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_and_install_windows_remote_tooling_use_injected_runner(self) -> None:
        scripts: list[dict] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            if "Get-Command git" in script:
                return completed(stdout='{"git_found":true,"winget_found":true}\n')
            return completed(returncode=7, stdout="out", stderr="err")

        tooling = self.mod.probe_windows_remote_tooling("win", run_windows_ssh_powershell_fn=fake_run)
        self.assertTrue(tooling["git_found"])
        self.assertIn("Get-Command git", scripts[-1]["script"])
        self.assertIn("auth status", scripts[-1]["script"])
        self.assertEqual(scripts[-1]["timeout"], 60)

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

    def test_ensure_remote_tooling_required_and_optional_install_flow(self) -> None:
        probes = iter(
            [
                {"git_found": False, "winget_found": True},
                {"git_found": True, "gh_found": False, "winget_found": True},
                {"git_found": True, "gh_found": False, "winget_found": True},
            ]
        )
        installs: list[str] = []

        def fake_install(_host, package_id):
            installs.append(package_id)
            if package_id == "GitHub.cli":
                raise RuntimeError("optional failed")

        ensured = self.mod.ensure_windows_remote_tooling(
            "win",
            install_optional=True,
            required_tools={"git": {"winget_id": "Git.Git", "required": True}},
            optional_tools={"gh": {"winget_id": "GitHub.cli", "required": False}},
            probe_windows_remote_tooling_fn=lambda _host: next(probes),
            install_windows_remote_tool_fn=fake_install,
        )
        self.assertEqual(installs, ["Git.Git", "GitHub.cli"])
        self.assertEqual(ensured["installed"], ["git"])

        with self.assertRaisesRegex(RuntimeError, "winget.*unavailable"):
            self.mod.ensure_windows_remote_tooling(
                "win",
                install_optional=False,
                required_tools={"git": {"winget_id": "Git.Git", "required": True}},
                optional_tools={},
                probe_windows_remote_tooling_fn=lambda _host: {"git_found": False, "winget_found": False},
                install_windows_remote_tool_fn=fake_install,
            )

    def test_ensure_remote_tooling_verifies_required_install_result(self) -> None:
        probes = iter(
            [
                {"git_found": False, "winget_found": True},
                {"git_found": False, "winget_found": True},
            ]
        )

        with self.assertRaisesRegex(RuntimeError, "still missing"):
            self.mod.ensure_windows_remote_tooling(
                "win",
                install_optional=False,
                required_tools={"git": {"winget_id": "Git.Git", "required": True}},
                optional_tools={},
                probe_windows_remote_tooling_fn=lambda _host: next(probes),
                install_windows_remote_tool_fn=lambda _host, _package_id: None,
            )


if __name__ == "__main__":
    unittest.main()
