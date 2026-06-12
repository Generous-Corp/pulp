#!/usr/bin/env python3
"""No-network tests for local-ci Linux desktop target helpers."""

from __future__ import annotations

from pathlib import Path
import subprocess
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("linux_target.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_launch_backend_probe_parses_display_missing_and_errors(self) -> None:
        calls: list[tuple[str, str, int | None]] = []

        def ssh_success(host: str, remote_cmd: str, *, timeout: int | None = None):
            calls.append((host, remote_cmd, timeout))
            return subprocess.CompletedProcess(
                [],
                0,
                stdout="ignored\nmode=display\ndisplay=:7\nxdg_runtime_dir=/run/user/501\n",
                stderr="",
            )

        backend = self.mod.probe_linux_launch_backend("ubuntu", ssh_command_result_fn=ssh_success)
        self.assertEqual(backend["mode"], "display")
        self.assertEqual(backend["display"], ":7")
        self.assertEqual(backend["xdg_runtime_dir"], "/run/user/501")
        self.assertEqual(calls[0][0], "ubuntu")
        self.assertEqual(calls[0][2], 30)
        self.assertIn("xvfb-run", calls[0][1])

        def ssh_empty(_host: str, _remote_cmd: str, *, timeout: int | None = None):
            return subprocess.CompletedProcess([], 0, stdout="", stderr="")

        self.assertEqual(
            self.mod.probe_linux_launch_backend("ubuntu", ssh_command_result_fn=ssh_empty),
            {"mode": "missing"},
        )

        def ssh_failure(_host: str, _remote_cmd: str, *, timeout: int | None = None):
            return subprocess.CompletedProcess([], 7, stdout="", stderr="ssh denied")

        with self.assertRaisesRegex(RuntimeError, "ssh denied"):
            self.mod.probe_linux_launch_backend("ubuntu", ssh_command_result_fn=ssh_failure)

    def test_remote_tooling_probe_and_detail_helpers(self) -> None:
        def ssh_tooling(_host: str, _remote_cmd: str, *, timeout: int | None = None):
            return subprocess.CompletedProcess(
                [],
                0,
                stdout=(
                    "git_found=true\n"
                    "git_path=/usr/bin/git\n"
                    "git_version=git version 2.49\n"
                    "git_lfs_found=false\n"
                    "git_lfs_hint=PATH missing\n"
                    "wmctrl_found=false\n"
                ),
                stderr="",
            )

        probe = self.mod.probe_linux_remote_tooling("ubuntu", ssh_command_result_fn=ssh_tooling)
        self.assertEqual(probe["git_version"], "git version 2.49")
        self.assertEqual(probe["wmctrl_found"], "false")
        self.assertEqual(self.mod.linux_tooling_detail(probe, "git"), "git version 2.49 (/usr/bin/git)")
        self.assertEqual(
            self.mod.linux_tooling_detail({"git_lfs_found": False, "git_lfs_hint": "PATH missing"}, "git_lfs"),
            "PATH missing",
        )
        self.assertEqual(self.mod.linux_tooling_detail({}, "xauth", missing_hint="install xauth"), "install xauth")
        self.assertFalse(self.mod.linux_remote_tooling_ready({"git_found": True, "git_lfs_found": True}))
        self.assertTrue(self.mod.linux_remote_tooling_ready({"git_found": True}, required_tools={"git": {}}))

    def test_remote_bundle_relpath_and_pulp_app_command_builder(self) -> None:
        relpath = self.mod.remote_linux_bundle_relpath("ubuntu", "smoke", Path("/tmp/run-1"))
        self.assertEqual(relpath, ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run-1")

        remote_cmd = self.mod.build_linux_xvfb_remote_command(
            "/tmp/pulp",
            relpath,
            "./build/ui-preview",
            launch_backend={"mode": "display", "display": ":0", "xdg_runtime_dir": "/run/user/1000"},
            launch_cwd="$HOME/.local/state/pulp/desktop-source/ubuntu/abc123",
            capture_ui_snapshot=True,
            click_point="10,20",
            click_view_id="root",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=True,
            settle_secs=0.5,
        )

        self.assertIn("launch_cwd=$HOME/.local/state/pulp/desktop-source/ubuntu/abc123", remote_cmd)
        self.assertIn("export DISPLAY=:0", remote_cmd)
        self.assertIn("export XDG_RUNTIME_DIR=/run/user/1000", remote_cmd)
        self.assertIn("export PULP_VIEW_TREE_OUT=", remote_cmd)
        self.assertIn("export PULP_AUTOMATION_CLICK_VIEW_ID=root", remote_cmd)
        self.assertNotIn("xvfb-run -a", remote_cmd)

    def test_window_driver_command_builder_uses_parser_and_display_backend(self) -> None:
        calls: list[tuple[str, str]] = []

        def parse_point(value: str, *, flag_name: str):
            calls.append((value, flag_name))
            return 10, 20

        command = self.mod.build_linux_window_driver_remote_command(
            "/repo",
            "bundle",
            "pulp-ui",
            launch_backend={"mode": "display", "display": ":2", "xdg_runtime_dir": "/run/user/501"},
            launch_cwd="$HOME/repo",
            click_point="10,20",
            capture_before=True,
            settle_secs=0.25,
            parse_coordinate_pair_fn=parse_point,
        )

        self.assertEqual(calls, [("10,20", "--click")])
        self.assertIn("export DISPLAY=:2", command)
        self.assertIn("xdotool click 1", command)
        self.assertIn("sleep 0.250", command)
        self.assertTrue(command.startswith("bash -lc "))


if __name__ == "__main__":
    unittest.main()
