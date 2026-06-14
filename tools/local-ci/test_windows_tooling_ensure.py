#!/usr/bin/env python3
"""Tests for Windows remote tooling ensure policy."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_tooling_ensure.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


class WindowsToolingEnsureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

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

    def test_ensure_remote_tooling_rejects_missing_required_without_winget(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "winget.*unavailable"):
            self.mod.ensure_windows_remote_tooling(
                "win",
                install_optional=False,
                required_tools={"git": {"winget_id": "Git.Git", "required": True}},
                optional_tools={},
                probe_windows_remote_tooling_fn=lambda _host: {"git_found": False, "winget_found": False},
                install_windows_remote_tool_fn=lambda _host, _package_id: None,
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
