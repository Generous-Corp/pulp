#!/usr/bin/env python3
"""Facade-level remote detail and tooling helper integration tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module(
        "local_ci.py",
        module_name="pulp_local_ci_remote_detail_tooling_integration",
        add_module_dir=True,
    )


class RemoteDetailToolingIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_remote_detail_helpers_cover_missing_and_partial_states(self) -> None:
        self.assertEqual(self.mod.windows_desktop_session_user(None), "")
        self.assertEqual(self.mod.windows_desktop_session_user({"logged_on_user": " alice "}), "alice")
        self.assertEqual(self.mod.windows_desktop_session_state({"session_state": " Active "}), "Active")
        self.assertEqual(
            self.mod.windows_tooling_detail({"git_found": True, "git_path": "C:/Git/bin/git.exe"}, "git"),
            "C:/Git/bin/git.exe",
        )
        self.assertEqual(self.mod.windows_tooling_detail({}, "git", missing_hint="install git"), "install git")
        self.assertTrue(self.mod.windows_remote_tooling_ready({"git_found": True}))
        self.assertFalse(self.mod.windows_remote_tooling_ready({}))

        self.assertEqual(self.mod.windows_repo_checkout_detail(None, fallback_path=r"C:\\Pulp"), r"C:\\Pulp")
        self.assertIn(
            "not a git checkout",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\\Pulp", "repo_exists": True}),
        )
        self.assertIn(
            "empty git repo",
            self.mod.windows_repo_checkout_detail({"git_dir_exists": True, "repo_path": r"C:\\Pulp"}),
        )
        self.assertIn(
            "checkout incomplete",
            self.mod.windows_repo_checkout_detail(
                {"git_dir_exists": True, "head_exists": True, "repo_path": r"C:\\Pulp"}
            ),
        )
        self.assertEqual(
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\\Pulp", "origin_url": "https://example/repo.git"}),
            r"C:\\Pulp (https://example/repo.git)",
        )

        self.assertEqual(
            self.mod.linux_tooling_detail({"git_lfs_found": False, "git_lfs_hint": "PATH missing"}, "git_lfs"),
            "PATH missing",
        )
        self.assertEqual(self.mod.linux_tooling_detail({}, "xauth", missing_hint="install xauth"), "install xauth")
        self.assertEqual(
            self.mod.linux_tooling_detail({"xvfb_run_found": True, "xvfb_run_path": "/usr/bin/xvfb-run"}, "xvfb_run"),
            "/usr/bin/xvfb-run",
        )
        self.assertFalse(self.mod.linux_remote_tooling_ready({"git_found": True, "git_lfs_found": True}))

    def test_linux_launch_backend_and_windows_tool_install_edges(self) -> None:
        linux_probe = subprocess.CompletedProcess(
            [],
            0,
            stdout="mode=display\ndisplay=:7\nignored-line\nxdg_runtime_dir=/run/user/501\n",
            stderr="",
        )
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_probe) as ssh_result:
            backend = self.mod.probe_linux_launch_backend("ubuntu")
        self.assertEqual(backend["mode"], "display")
        self.assertEqual(backend["display"], ":7")
        self.assertEqual(backend["xdg_runtime_dir"], "/run/user/501")
        self.assertEqual(ssh_result.call_args.kwargs["timeout"], 30)
        self.assertEqual(ssh_result.call_args.args[0], "ubuntu")
        self.assertIn("xvfb-run", ssh_result.call_args.args[1])

        empty_probe = subprocess.CompletedProcess([], 0, stdout="", stderr="")
        with mock.patch.object(self.mod, "ssh_command_result", return_value=empty_probe):
            self.assertEqual(self.mod.probe_linux_launch_backend("ubuntu"), {"mode": "missing"})

        failed_probe = subprocess.CompletedProcess([], 7, stdout="", stderr="ssh denied")
        with mock.patch.object(self.mod, "ssh_command_result", return_value=failed_probe):
            with self.assertRaisesRegex(RuntimeError, "ssh denied"):
                self.mod.probe_linux_launch_backend("ubuntu")

        probes = [
            {"git_found": False, "winget_found": True},
            {"git_found": True, "winget_found": True, "gh_found": False},
            {"git_found": True, "winget_found": True, "gh_found": True},
        ]
        with mock.patch.object(self.mod, "probe_windows_remote_tooling", side_effect=probes), \
             mock.patch.object(self.mod, "install_windows_remote_tool", side_effect=[None, RuntimeError("optional failed")]) as install:
            ensured = self.mod.ensure_windows_remote_tooling("win", install_optional=True)
        self.assertEqual(ensured["installed"], ["git"])
        self.assertTrue(ensured["probe"]["git_found"])
        self.assertTrue(ensured["probe"]["winget_found"])
        self.assertEqual(install.call_count, 2)
        self.assertEqual(install.call_args_list[0].args[0], "win")
        self.assertEqual(install.call_args_list[0].args[1], self.mod.WINDOWS_REQUIRED_REMOTE_TOOLS["git"]["winget_id"])

        with mock.patch.object(self.mod, "probe_windows_remote_tooling", return_value={"git_found": False, "winget_found": False}), \
             mock.patch.object(self.mod, "install_windows_remote_tool") as install:
            with self.assertRaisesRegex(RuntimeError, "winget"):
                self.mod.ensure_windows_remote_tooling("win")
        install.assert_not_called()

    def test_remote_probe_wrappers_parse_mocked_outputs(self) -> None:
        win_success = subprocess.CompletedProcess(
            [],
            0,
            stdout='banner\n{"task_present": true, "interactive_user": "dev"}\n',
            stderr="",
        )
        with mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=win_success) as run_ps:
            session = self.mod.probe_windows_session_agent(
                "win",
                {
                    "task_name": "Pulp Agent",
                    "remote_root": r"%LOCALAPPDATA%\Pulp\agent",
                    "script_path": r"%LOCALAPPDATA%\Pulp\agent\agent.ps1",
                },
            )
            self.assertTrue(session["task_present"])
            self.assertEqual(session["interactive_user"], "dev")
            self.assertIn("Get-ScheduledTask", run_ps.call_args.args[1])

            tooling = self.mod.probe_windows_remote_tooling("win")
            self.assertTrue(tooling["task_present"])
            self.assertIn("Get-Command git", run_ps.call_args.args[1])

        win_failure = subprocess.CompletedProcess([], 7, stdout="", stderr="powershell failed")
        with mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=win_failure):
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.probe_windows_session_agent("win", {"task_name": "task", "remote_root": "root"})
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.probe_windows_remote_tooling("win")
            with self.assertRaisesRegex(RuntimeError, "powershell failed"):
                self.mod.install_windows_remote_tool("win", "Git.Git", timeout=1)

        linux_success = subprocess.CompletedProcess(
            [],
            0,
            stdout="ignored\nmode=display\ndisplay=:2\nxdg_runtime_dir=/run/user/501\n",
            stderr="",
        )
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_success) as ssh_run:
            backend = self.mod.probe_linux_launch_backend("ubuntu")
            self.assertEqual(backend["mode"], "display")
            self.assertEqual(backend["display"], ":2")
            self.assertIn("xvfb-run", ssh_run.call_args.args[1])

        linux_tooling = subprocess.CompletedProcess(
            [],
            0,
            stdout="git_found=true\ngit_path=/usr/bin/git\ngit_version=git version 2.49\nwmctrl_found=false\n",
            stderr="",
        )
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_tooling):
            probe = self.mod.probe_linux_remote_tooling("ubuntu")
            self.assertEqual(probe["git_version"], "git version 2.49")
            self.assertEqual(probe["wmctrl_found"], "false")

        linux_failure = subprocess.CompletedProcess([], 2, stdout="", stderr="ssh failed")
        with mock.patch.object(self.mod, "ssh_command_result", return_value=linux_failure):
            with self.assertRaisesRegex(RuntimeError, "ssh failed"):
                self.mod.probe_linux_launch_backend("ubuntu")
            with self.assertRaisesRegex(RuntimeError, "ssh failed"):
                self.mod.probe_linux_remote_tooling("ubuntu")

        command = self.mod.build_linux_window_driver_remote_command(
            "/repo",
            "bundle",
            "pulp-ui",
            launch_backend={"mode": "display", "display": ":2", "xdg_runtime_dir": "/run/user/501"},
            launch_cwd="$HOME/repo",
            click_point="10,20",
            capture_before=True,
            settle_secs=0.25,
        )
        self.assertIn("export DISPLAY=:2", command)
        self.assertIn("xdotool click 1", command)
        self.assertIn("sleep 0.250", command)


if __name__ == "__main__":
    unittest.main()
