#!/usr/bin/env python3
"""Tests for Windows remote repository checkout probe helper."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_repo_checkout_probe.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsRepoCheckoutProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_windows_repo_checkout_uses_injected_runner_and_safety_policy(self) -> None:
        scripts: list[dict] = []
        unsafe_calls: list[tuple[str | None, str | None]] = []

        def fake_run(_host, script, *, timeout=0):
            scripts.append({"script": script, "timeout": timeout})
            return completed(
                stdout='{"home_dir":"C:\\\\Users\\\\dev","repo_path":"C:\\\\Pulp","head_exists":true,"setup_exists":true}\n'
            )

        def fake_unsafe(repo_path, home_dir):
            unsafe_calls.append((repo_path, home_dir))
            return repo_path == r"C:\Users\dev"

        probe = self.mod.probe_windows_repo_checkout(
            "win",
            "Owner's Repo",
            run_windows_ssh_powershell_fn=fake_run,
            windows_repo_path_is_unsafe_fn=fake_unsafe,
        )

        self.assertFalse(probe["repo_path_unsafe"])
        self.assertIn("$RepoRaw = 'Owner''s Repo'", scripts[0]["script"])
        self.assertIn("git -C $Repo remote 2>$null", scripts[0]["script"])
        self.assertIn("Where-Object { $_ -eq 'origin' }", scripts[0]["script"])
        self.assertIn("git -C $Repo rev-parse --verify --quiet HEAD 2>$null", scripts[0]["script"])
        self.assertEqual(scripts[0]["timeout"], 60)
        self.assertEqual(unsafe_calls, [(r"C:\Pulp", r"C:\Users\dev")])


if __name__ == "__main__":
    unittest.main()
