#!/usr/bin/env python3
from __future__ import annotations

from argparse import Namespace
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_setup_command_format.py")


class DesktopSetupCommandFormatTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_install_lines_render_local_and_windows_remote_state(self):
        local_lines = self.mod.desktop_install_lines(
            target_name="mac",
            target={"adapter": "macos-local", "bootstrap": "launchagent", "target_type": "local"},
            artifact_root=Path("/tmp/artifacts"),
            remote_bootstrap_ready=True,
            remote_tooling_ready=True,
            tooling_installed=[],
            tooling_probe=None,
            repo_checkout_probe=None,
            contract={},
            windows_tooling_detail_fn=lambda *_args, **_kwargs: "",
        )
        self.assertEqual(local_lines[-1], "  remote bootstrap: not required for local target")

        windows_lines = self.mod.desktop_install_lines(
            target_name="windows",
            target={"adapter": "windows-session-agent", "bootstrap": "scheduled-task", "target_type": "ssh"},
            artifact_root=Path("/tmp/artifacts"),
            remote_bootstrap_ready=True,
            remote_tooling_ready=True,
            tooling_installed=["git"],
            tooling_probe={"git_version": "git version 2.49.0", "git_path": r"C:\Git\git.exe"},
            repo_checkout_probe={"repo_path": r"C:\Users\daniel\pulp-validate"},
            contract={"task_name": "PulpDesktopAutomationAgent-windows", "remote_root": r"C:\RemoteRoot"},
            windows_tooling_detail_fn=lambda probe, tool_name, **_kwargs: f"{probe[tool_name + '_version']} ({probe[tool_name + '_path']})",
        )
        self.assertIn("  remote bootstrap: ready", windows_lines)
        self.assertIn(r"  remote tooling: ready (git version 2.49.0 (C:\Git\git.exe))", windows_lines)
        self.assertIn("  remote tooling installed: git", windows_lines)
        self.assertIn(r"  remote repo checkout: C:\Users\daniel\pulp-validate", windows_lines)
        self.assertIn("  task_name: PulpDesktopAutomationAgent-windows", windows_lines)
        self.assertIn(r"  remote_root: C:\RemoteRoot", windows_lines)

    def test_doctor_payload_and_lines_render_check_statuses(self):
        args = Namespace(target="ubuntu")
        target = {"adapter": "linux-xvfb", "bootstrap": "xvfb-run"}
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "optional", "ok": False, "required": False, "detail": "missing optional"},
            {"name": "ssh", "ok": False, "detail": "down"},
        ]

        self.assertEqual(
            self.mod.desktop_doctor_payload(args, target=target, checks=checks, all_ok=False),
            {
                "target": "ubuntu",
                "adapter": "linux-xvfb",
                "bootstrap": "xvfb-run",
                "ok": False,
                "checks": checks,
            },
        )
        self.assertEqual(
            self.mod.desktop_doctor_lines(args, target=target, checks=checks),
            [
                "Desktop doctor for `ubuntu`",
                "  adapter: linux-xvfb",
                "  bootstrap: xvfb-run",
                "  PASS  receipt: installed",
                "  WARN  optional: missing optional",
                "  FAIL  ssh: down",
            ],
        )


if __name__ == "__main__":
    unittest.main()
